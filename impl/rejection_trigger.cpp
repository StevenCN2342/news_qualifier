#include "rejection_trigger.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chongming::qualifier {
namespace {

void requireProbability(double value, const char* field_name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument(std::string(field_name) +
                                    " must be finite and within [0, 1]");
    }
}

void requireThreshold(double value, const char* field_name) {
    requireProbability(value, field_name);
}

}  // namespace

RejectionTrigger::RejectionTrigger(RejectionTriggerConfig config)
    : config_(config) {
    if (!std::isfinite(config_.error_budget) || config_.error_budget <= 0.0 ||
        config_.error_budget >= 1.0) {
        throw std::invalid_argument("error_budget must be finite and within (0, 1)");
    }
    requireThreshold(config_.sufficiency_threshold, "sufficiency_threshold");
    requireThreshold(config_.consensus_threshold, "consensus_threshold");
    requireThreshold(config_.independence_threshold, "independence_threshold");
    requireThreshold(config_.calibrated_probability_threshold,
                     "calibrated_probability_threshold");
    requireThreshold(config_.low_confidence_sufficiency_threshold,
                     "low_confidence_sufficiency_threshold");
}

std::optional<double> RejectionTrigger::conformalThreshold(
    const std::vector<double>& calibration_nonconformity_scores) const {
    if (calibration_nonconformity_scores.empty()) return std::nullopt;

    std::vector<double> sorted = calibration_nonconformity_scores;
    for (double score : sorted) {
        requireProbability(score, "calibration nonconformity score");
    }
    std::sort(sorted.begin(), sorted.end());

    // README 中 k 从 1 开始；C++ 数组从 0 开始，因此最终读取 k-1。
    const std::size_t n = sorted.size();
    const double raw_rank =
        std::ceil(static_cast<double>(n + 1) * (1.0 - config_.error_budget));
    const std::size_t k = std::min(n, static_cast<std::size_t>(raw_rank));
    return sorted[k - 1];
}

std::optional<RejectionDecision> RejectionTrigger::decide(
    const RejectionInput& input) const {
    if (!input.nonconformity_score || !input.conformal_threshold ||
        !input.sufficiency || !input.consensus || !input.independence ||
        !input.calibrated_probability) {
        return std::nullopt;
    }

    requireProbability(*input.nonconformity_score, "nonconformity_score");
    requireProbability(*input.conformal_threshold, "conformal_threshold");
    requireProbability(*input.sufficiency, "sufficiency");
    requireProbability(*input.consensus, "consensus");
    requireProbability(*input.independence, "independence");
    requireProbability(*input.calibrated_probability, "calibrated_probability");

    RejectionDecision result;
    if (*input.nonconformity_score > *input.conformal_threshold) {
        result.reasons.push_back(RejectionReason::ConformalThresholdExceeded);
    }
    if (*input.sufficiency < config_.sufficiency_threshold) {
        result.reasons.push_back(RejectionReason::InsufficientEvidence);
    }
    if (*input.consensus < config_.consensus_threshold) {
        result.reasons.push_back(RejectionReason::JuryDisagreement);
    }
    if (*input.independence < config_.independence_threshold) {
        result.reasons.push_back(RejectionReason::SourceDependency);
    }
    if (*input.calibrated_probability <
            config_.calibrated_probability_threshold &&
        *input.sufficiency < config_.low_confidence_sufficiency_threshold) {
        // README 的第四条仍是一个 AND 条件。条件成立时分别记录两个原子原因，
        // 便于每个配置阈值与一个原因枚举对应，同时不扩大原有拒答范围。
        result.reasons.push_back(RejectionReason::LowCalibratedProbability);
        result.reasons.push_back(
            RejectionReason::InsufficientEvidenceForLowConfidence);
    }

    result.should_reject = !result.reasons.empty();
    if (!result.should_reject) return result;

    // 文案只提供 README 建议的上层展示值；完整原因仍保存在 reasons 中。
    const bool evidence_related =
        std::find(result.reasons.begin(), result.reasons.end(),
                  RejectionReason::InsufficientEvidence) != result.reasons.end() ||
        std::find(result.reasons.begin(), result.reasons.end(),
                  RejectionReason::InsufficientEvidenceForLowConfidence) !=
            result.reasons.end();
    const bool needs_manual_review =
        std::find(result.reasons.begin(), result.reasons.end(),
                  RejectionReason::JuryDisagreement) != result.reasons.end() ||
        std::find(result.reasons.begin(), result.reasons.end(),
                  RejectionReason::SourceDependency) != result.reasons.end();
    if (evidence_related) {
        result.suggested_message = "证据不足";
    } else if (needs_manual_review) {
        result.suggested_message = "建议人工复核";
    } else {
        result.suggested_message = "暂不能确认";
    }
    return result;
}

const char* toString(RejectionReason reason) noexcept {
    switch (reason) {
        case RejectionReason::ConformalThresholdExceeded:
            return "非一致性分数超过保形阈值";
        case RejectionReason::InsufficientEvidence:
            return "证据充分性低于阈值";
        case RejectionReason::JuryDisagreement:
            return "陪审团一致性低于阈值";
        case RejectionReason::SourceDependency:
            return "来源独立性低于阈值";
        case RejectionReason::LowCalibratedProbability:
            return "校准概率低于阈值";
        case RejectionReason::InsufficientEvidenceForLowConfidence:
            return "证据充分性低于低置信度联合阈值";
    }
    return "未知拒答原因";
}

}  // namespace chongming::qualifier
