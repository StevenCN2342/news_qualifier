#include "logic_relationship_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace chongming::qualifier {
namespace {

double clamp01(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

bool indexExists(std::size_t index, const std::vector<ClaimEvaluation>& claims) {
    return index < claims.size();
}

bool isValidWeight(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool hasFiniteCausalFactors(const CausalFactorScores& factors) {
    return std::isfinite(factors.temporal_order.value()) &&
           std::isfinite(factors.mechanism.value()) &&
           std::isfinite(factors.counterfactual_support.value()) &&
           std::isfinite(factors.confounder_control.value()) &&
           std::isfinite(factors.dose_response.value());
}

}  // namespace

LogicRelationshipQualifier::LogicRelationshipQualifier(
    LogicRelationshipConfig config)
    : config_(std::move(config)) {
    const bool weights_valid =
        isValidWeight(config_.causal_time_weight) &&
        isValidWeight(config_.causal_mechanism_weight) &&
        isValidWeight(config_.causal_counterfactual_weight) &&
        isValidWeight(config_.causal_confounder_weight) &&
        isValidWeight(config_.causal_dose_response_weight);
    const double weight_sum =
        config_.causal_time_weight + config_.causal_mechanism_weight +
        config_.causal_counterfactual_weight +
        config_.causal_confounder_weight +
        config_.causal_dose_response_weight;

    // 优化规范要求 w_k >= 0 且五个权重之和为 1。使用微小容差避免
    // 浮点表示误差导致合法配置被拒绝。
    if (!weights_valid || !std::isfinite(weight_sum) ||
        std::abs(weight_sum - 1.0) > 1e-9) {
        throw std::invalid_argument(
            "causal relation weights must be finite, non-negative, and sum to 1");
    }
}

LogicEvaluation LogicRelationshipQualifier::evaluate(
    const RelationInput& relation,
    const std::vector<ClaimEvaluation>& claims) const {
    LogicEvaluation result;
    result.input = relation;
    const bool left_ok = indexExists(relation.left_claim, claims);
    const bool right_ok = indexExists(relation.right_claim, claims);
    if (!relation.parsing_succeeded || !left_ok || !right_ok ||
        !std::isfinite(relation.nli_score)) {
        // 无效输入必须显式保持不可用，不能用声明内容分伪造逻辑分数。
        return result;
    }

    // 五种语义关系均使用外部 NLI 模型提供的关系真实性分数。Causal 在
    // RelationType 中表示语义分类；因果系数计算属于 AggregationType::Causal。
    switch (relation.type) {
        case RelationType::Temporal:
        case RelationType::Conditional:
        case RelationType::Contrast:
        case RelationType::Parallel:
        case RelationType::Causal:
            result.score = clamp01(relation.nli_score);
            break;
    }
    result.score = clamp01(result.score.value_or(0.5));
    result.logic_available = true;
    return result;
}

LogicEvaluation LogicRelationshipQualifier::evaluate(
    const AggregationInput& aggregation,
    const std::vector<ClaimEvaluation>& claims) const {
    LogicEvaluation result;
    result.input = aggregation;
    const bool right_needed = aggregation.type != AggregationType::Not;
    const bool left_ok = indexExists(aggregation.left_claim, claims);
    const bool right_ok =
        !right_needed || indexExists(aggregation.right_claim, claims);
    const bool causal_input_incomplete =
        aggregation.type == AggregationType::Causal &&
        (!aggregation.nli_score.has_value() ||
         !hasCompleteCausalFactors(aggregation.causal_factors));
    const bool causal_input_invalid =
        aggregation.type == AggregationType::Causal &&
        !causal_input_incomplete &&
        (!std::isfinite(aggregation.nli_score.value()) ||
         !hasFiniteCausalFactors(aggregation.causal_factors));
    if (!aggregation.parsing_succeeded || !left_ok || !right_ok ||
        causal_input_incomplete || causal_input_invalid) {
        return result;
    }

    const bool content_scores_needed =
        aggregation.type != AggregationType::Causal;
    const auto& left_score = claims[aggregation.left_claim].content_score;
    const std::optional<double> right_score =
        right_needed ? claims[aggregation.right_claim].content_score
                     : std::optional<double>{};
    if (content_scores_needed &&
        (!left_score.has_value() || !std::isfinite(left_score.value()) ||
         (right_needed &&
          (!right_score.has_value() || !std::isfinite(right_score.value()))))) {
        return result;
    }

    const double left = content_scores_needed ? clamp01(left_score.value()) : 0.0;
    const double right = content_scores_needed && right_needed
                             ? clamp01(right_score.value())
                             : 0.0;
    switch (aggregation.type) {
        case AggregationType::And:
            result.score = std::min(left, right);
            break;
        case AggregationType::Or:
            result.score = std::max(left, right);
            break;
        case AggregationType::Not:
            result.score = 1.0 - left;
            break;
        case AggregationType::Implication:
            result.score = std::max(1.0 - left, right);
            break;
        case AggregationType::Causal: {
            // 优化规范第三层：先由五个子维度形成关系置信度 R_ij，再与
            // 外部 NLI 概率相乘。声明内容分数不再重复参与因果边计算。
            const double relation_confidence =
                causalRelationConfidence(aggregation.causal_factors);
            result.score =
                clamp01(aggregation.nli_score.value()) * relation_confidence;
            break;
        }
    }
    result.score = clamp01(result.score.value_or(0.5));
    result.logic_available = true;
    return result;
}

std::vector<ClaimLogicEvaluation>
LogicRelationshipQualifier::aggregateByClaim(
    std::size_t claim_count,
    const std::vector<LogicEvaluation>& edge_results) const {
    std::vector<double> sums(claim_count, 0.0);
    std::vector<std::size_t> counts(claim_count, 0);

    const auto add_edge = [&sums, &counts, claim_count](
                              std::size_t claim_index, double score) {
        if (claim_index < claim_count) {
            sums[claim_index] += score;
            ++counts[claim_index];
        }
    };

    for (const auto& edge : edge_results) {
        if (!edge.logic_available || !edge.score.has_value() ||
            !std::isfinite(edge.score.value())) {
            continue;
        }
        const double score = clamp01(edge.score.value());
        std::visit(
            [&](const auto& input) {
                add_edge(input.left_claim, score);
                bool right_needed = true;
                if constexpr (std::is_same_v<
                                  std::decay_t<decltype(input)>,
                                  AggregationInput>) {
                    right_needed = input.type != AggregationType::Not;
                }
                if (right_needed && input.right_claim != input.left_claim) {
                    add_edge(input.right_claim, score);
                }
            },
            edge.input);
    }

    std::vector<ClaimLogicEvaluation> result(claim_count);
    for (std::size_t index = 0; index < claim_count; ++index) {
        result[index].claim_index = index;
        result[index].contributing_edge_count = counts[index];
        if (counts[index] > 0) {
            result[index].score =
                clamp01(sums[index] / static_cast<double>(counts[index]));
            result[index].logic_available = true;
        }
    }
    return result;
}

bool LogicRelationshipQualifier::hasCompleteCausalFactors(
    const CausalFactorScores& factors) {
    return factors.temporal_order.has_value() &&
           factors.mechanism.has_value() &&
           factors.counterfactual_support.has_value() &&
           factors.confounder_control.has_value() &&
           factors.dose_response.has_value();
}

double LogicRelationshipQualifier::causalRelationConfidence(
    const CausalFactorScores& factors) const {
    // 调用者已检查五个 optional 均有值；各输入按统一约定裁剪到 [0,1]。
    const double confidence =
        config_.causal_time_weight * clamp01(factors.temporal_order.value()) +
        config_.causal_mechanism_weight * clamp01(factors.mechanism.value()) +
        config_.causal_counterfactual_weight *
            clamp01(factors.counterfactual_support.value()) +
        config_.causal_confounder_weight *
            clamp01(factors.confounder_control.value()) +
        config_.causal_dose_response_weight *
            clamp01(factors.dose_response.value());
    return clamp01(confidence);
}

}  // namespace chongming::qualifier
