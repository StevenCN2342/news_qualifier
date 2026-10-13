#include "rejection_trigger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace chongming::qualifier {
namespace {

double requireProbability(double value, const char* field_name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument(std::string(field_name) +
                                    " must be finite and within [0, 1]");
    }
    return value;
}

std::size_t labelIndex(CredibilityLabel label) {
    switch (label) {
        case CredibilityLabel::ConfirmedTrue:
            return 0;
        case CredibilityLabel::MostlyTrueWithFlaws:
            return 1;
        case CredibilityLabel::InsufficientInformation:
            return 2;
        case CredibilityLabel::ConfirmedFalse:
            return 3;
    }
    throw std::invalid_argument("unsupported credibility label");
}

std::array<double, 4> probabilityArray(
    const FourClassProbabilities& probabilities) {
    if (!probabilities.confirmed_true || !probabilities.mostly_true ||
        !probabilities.insufficient_information ||
        !probabilities.confirmed_false) {
        throw std::invalid_argument(
            "all four class probabilities must be defined");
    }
    std::array<double, 4> result{
        requireProbability(*probabilities.confirmed_true, "confirmed_true"),
        requireProbability(*probabilities.mostly_true, "mostly_true"),
        requireProbability(*probabilities.insufficient_information,
                           "insufficient_information"),
        requireProbability(*probabilities.confirmed_false, "confirmed_false")};
    const double sum = result[0] + result[1] + result[2] + result[3];
    if (std::abs(sum - 1.0) > 1e-9) {
        throw std::invalid_argument(
            "four class probabilities must sum to one");
    }
    return result;
}

CredibilityLabel labelAt(std::size_t index) {
    static constexpr std::array<CredibilityLabel, 4> labels{
        CredibilityLabel::ConfirmedTrue,
        CredibilityLabel::MostlyTrueWithFlaws,
        CredibilityLabel::InsufficientInformation,
        CredibilityLabel::ConfirmedFalse};
    return labels.at(index);
}

}  // namespace

RejectionTrigger::RejectionTrigger(RejectionTriggerConfig config)
    : config_(config) {
    if (!std::isfinite(config_.error_budget) || config_.error_budget <= 0.0 ||
        config_.error_budget >= 1.0) {
        throw std::invalid_argument(
            "error_budget must be finite and within (0, 1)");
    }
}

std::optional<ConformalThreshold> RejectionTrigger::conformalThreshold(
    const std::vector<ConformalCalibrationSample>& calibration) const {
    if (calibration.empty()) return std::nullopt;

    // 同一 family 的阈值分数取族内所有 claim 真标签非一致性分数的最大值。
    std::unordered_map<std::string, double> family_scores;
    for (const auto& sample : calibration) {
        if (sample.family_id.empty()) {
            throw std::invalid_argument("family_id must not be empty");
        }
        const auto probabilities = probabilityArray(sample.probabilities);
        const double nonconformity =
            1.0 - probabilities[labelIndex(sample.true_label)];
        auto [iterator, inserted] =
            family_scores.emplace(sample.family_id, nonconformity);
        if (!inserted) iterator->second = std::max(iterator->second, nonconformity);
    }

    std::vector<double> sorted;
    sorted.reserve(family_scores.size());
    for (const auto& item : family_scores) sorted.push_back(item.second);
    std::sort(sorted.begin(), sorted.end());

    ConformalThreshold result;
    result.family_count = sorted.size();
    const double raw_rank =
        static_cast<double>(result.family_count + 1) *
        (1.0 - config_.error_budget);
    // 减去仅用于浮点边界的微小容差，避免数学整数 19 被表示成
    // 19.000000000000004 后错误地向上取整为 20。
    result.rank = static_cast<std::size_t>(std::ceil(raw_rank - 1e-12));
    if (result.rank > result.family_count) {
        result.value = std::numeric_limits<double>::infinity();
        result.is_infinite = true;
    } else {
        result.value = sorted[result.rank - 1];
    }
    return result;
}

std::vector<CredibilityLabel> RejectionTrigger::predictionSet(
    const FourClassProbabilities& probabilities,
    const ConformalThreshold& threshold) const {
    const auto values = probabilityArray(probabilities);
    if (threshold.family_count == 0 || threshold.rank == 0) {
        throw std::invalid_argument(
            "conformal threshold must come from a non-empty calibration set");
    }
    if (threshold.is_infinite) {
        if (!std::isinf(threshold.value) || threshold.value < 0.0) {
            throw std::invalid_argument(
                "an infinite conformal threshold must have value +infinity");
        }
        if (threshold.rank <= threshold.family_count) {
            throw std::invalid_argument(
                "an infinite threshold requires rank greater than family_count");
        }
    } else {
        requireProbability(threshold.value, "conformal_threshold");
        if (threshold.rank > threshold.family_count) {
            throw std::invalid_argument(
                "a finite threshold rank cannot exceed family_count");
        }
    }

    std::vector<CredibilityLabel> result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (threshold.is_infinite ||
            1.0 - values[index] <= threshold.value) {
            result.push_back(labelAt(index));
        }
    }
    return result;
}

RejectionDecision RejectionTrigger::decide(
    const FourClassProbabilities& probabilities,
    const ConformalThreshold& threshold) const {
    RejectionDecision result;
    result.prediction_set = predictionSet(probabilities, threshold);
    if (result.prediction_set.size() == 1) {
        result.accepted_label = result.prediction_set.front();
        return result;
    }

    result.should_reject = true;
    if (result.prediction_set.empty()) {
        result.reasons.push_back(RejectionReason::EmptyPredictionSet);
        result.suggested_message = "暂不能确认";
    } else {
        result.reasons.push_back(RejectionReason::MultipleCandidateLabels);
        result.suggested_message = "建议人工复核";
    }
    return result;
}

const char* toString(RejectionReason reason) noexcept {
    switch (reason) {
        case RejectionReason::EmptyPredictionSet:
            return "保形预测集为空";
        case RejectionReason::MultipleCandidateLabels:
            return "保形预测集包含多个候选标签";
    }
    return "未知拒答原因";
}

}  // namespace chongming::qualifier
