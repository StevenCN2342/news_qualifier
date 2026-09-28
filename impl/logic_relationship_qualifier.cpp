#include "logic_relationship_qualifier.h"

#include <algorithm>
#include <cmath>

namespace chongming::qualifier {
namespace {

double clamp01(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

bool indexExists(std::size_t index, const std::vector<ClaimEvaluation>& claims) {
    return index < claims.size();
}

}  // namespace

LogicEvaluation LogicRelationshipQualifier::evaluate(
    const RelationInput& relation,
    const std::vector<ClaimEvaluation>& claims) const {
    LogicEvaluation result;
    result.relation = relation;
    const bool left_ok = indexExists(relation.left_claim, claims);
    const bool right_needed = relation.type != RelationType::Not;
    const bool right_ok = !right_needed || indexExists(relation.right_claim, claims);
    const bool uncertain =
        !left_ok || !right_ok ||
        (left_ok && claims[relation.left_claim].status != ClaimStatus::Determinate) ||
        (right_needed && right_ok &&
         claims[relation.right_claim].status != ClaimStatus::Determinate);
    if (!relation.parsing_succeeded || uncertain) {
        // README 要求解析失败或声明不确定时回退到原子声明多数投票。
        result.score = majorityFallback(relation, claims);
        result.used_fallback = true;
        return result;
    }

    const double left =
        clamp01(claims[relation.left_claim].content_score.value_or(0.5));
    const double right = right_needed
                             ? clamp01(claims[relation.right_claim]
                                           .content_score.value_or(0.5))
                             : 0.5;
    switch (relation.type) {
        // 以下五种组合关系直接实现 README 中的真值聚合公式。
        case RelationType::And:
            result.score = std::min(left, right);
            break;
        case RelationType::Or:
            result.score = std::max(left, right);
            break;
        case RelationType::Not:
            result.score = 1.0 - left;
            break;
        case RelationType::Implication:
            result.score = std::max(1.0 - left, right);
            break;
        case RelationType::Causal: {
            // CausalConfidence = p_causal * (1 - p_counterfactual)。
            const double causal_confidence =
                clamp01(relation.causal_probability) *
                (1.0 - clamp01(relation.counterfactual_probability));
            result.score = std::min(left, right) * causal_confidence;
            break;
        }
        default:
            // 时间、条件、转折和并列关系使用外部 NLI 模型提供的关系分数。
            result.score = clamp01(relation.nli_score);
            break;
    }
    result.score = clamp01(result.score.value_or(0.5));
    return result;
}

double LogicRelationshipQualifier::majorityFallback(
    const RelationInput& relation,
    const std::vector<ClaimEvaluation>& claims) {
    double votes = 0.0;
    std::size_t count = 0;
    if (indexExists(relation.left_claim, claims)) {
        votes += claims[relation.left_claim].content_score.value_or(0.5) >= 0.5
                     ? 1.0
                     : 0.0;
        ++count;
    }
    if (relation.type != RelationType::Not &&
        relation.right_claim != relation.left_claim &&
        indexExists(relation.right_claim, claims)) {
        votes += claims[relation.right_claim].content_score.value_or(0.5) >= 0.5
                     ? 1.0
                     : 0.0;
        ++count;
    }
    // 没有任何合法声明可投票时保持中性；否则返回支持票比例。
    return count == 0 ? 0.5 : votes / static_cast<double>(count);
}

}  // namespace chongming::qualifier
