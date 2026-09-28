#include "atomic_content_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chongming::qualifier {

AtomicContentQualifier::AtomicContentQualifier(
    AtomicContentConfig content_config,
    EvidenceAuthenticityConfig evidence_config)
    : config_(content_config), evidence_qualifier_(std::move(evidence_config)) {
    if (config_.sigmoid_alpha < 0.0 || config_.epsilon <= 0.0) {
        throw std::invalid_argument("invalid atomic-content configuration");
    }
}

ClaimEvaluation AtomicContentQualifier::evaluate(const AtomicClaim& claim) const {
    ClaimEvaluation result;
    result.claim_id = claim.id;
    result.evidence = evidence_qualifier_.assess(claim);
    double support_strength = 0.0;
    double refute_strength = 0.0;

    for (const auto& item : result.evidence) {
        // 相关性过滤发生在所有强度聚合之前。
        if (item.filtered_by_relevance) {
            continue;
        }
        // delta_ij * p_ij * r_ij；只有支持和反驳参与事实性分数。
        const double contribution = item.independence_discount.value_or(0.0) *
                                    item.nli_confidence *
                                    item.reliability.value_or(0.0);
        if (item.nli_label == NliLabel::Support) {
            support_strength += contribution;
        } else if (item.nli_label == NliLabel::Refute) {
            refute_strength += contribution;
        }
    }

    result.support_strength = support_strength;
    result.refute_strength = refute_strength;
    const double total = support_strength + refute_strength;
    if (total < config_.epsilon) {
        // 无方向性证据不等价于“假”，因此返回中性分 0.5。
        result.content_score = 0.5;
    } else {
        const double support_ratio = support_strength / total;
        const double x = config_.sigmoid_alpha * support_ratio -
                         config_.sigmoid_beta;
        result.content_score = 1.0 / (1.0 + std::exp(-x));
    }
    result.content_score =
        std::clamp(result.content_score.value_or(0.5), 0.0, 1.0);
    result.sufficiency_score =
        evidence_qualifier_.sufficiencyScore(result.evidence);
    result.status = result.sufficiency_score.value_or(0.0) <
                            evidence_qualifier_.sufficiencyThreshold()
                        ? ClaimStatus::InsufficientInformation
                        : ClaimStatus::Determinate;
    return result;
}

}  // namespace chongming::qualifier
