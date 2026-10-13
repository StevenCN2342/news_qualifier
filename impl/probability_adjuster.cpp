#include "probability_adjuster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

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

CredibilityLabel labelAt(std::size_t index) {
    static constexpr std::array<CredibilityLabel, 4> labels{
        CredibilityLabel::ConfirmedTrue,
        CredibilityLabel::MostlyTrueWithFlaws,
        CredibilityLabel::InsufficientInformation,
        CredibilityLabel::ConfirmedFalse};
    return labels.at(index);
}

FourClassProbabilities toProbabilities(const std::array<double, 4>& values) {
    FourClassProbabilities result;
    result.confirmed_true = values[0];
    result.mostly_true = values[1];
    result.insufficient_information = values[2];
    result.confirmed_false = values[3];
    return result;
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

struct BinAccumulator {
    std::size_t count{0};
    std::size_t correct_count{0};
    double confidence_sum{0.0};
};

}  // namespace

ProbabilityAdjuster::ProbabilityAdjuster(ProbabilityAdjusterConfig config)
    : config_(config) {
    if (!std::isfinite(config_.temperature) ||
        config_.temperature < PROBABILITY_ADJUSTER_MIN_TEMPERATURE ||
        config_.temperature > PROBABILITY_ADJUSTER_MAX_TEMPERATURE) {
        throw std::invalid_argument(
            "temperature must be finite and within [0.1, 5.0]");
    }
    if (!std::isfinite(config_.epsilon) || config_.epsilon <= 0.0 ||
        config_.epsilon >= 0.25) {
        throw std::invalid_argument(
            "epsilon must be finite and within (0, 0.25)");
    }
}

std::optional<ProbabilityEvaluation> ProbabilityAdjuster::evaluate(
    const ClaimEvaluation& claim,
    const ClaimMultiDimensionalEvaluation& multi_dimensional) const {
    if (!claim.claim_id.empty() && !multi_dimensional.claim_id.empty() &&
        claim.claim_id != multi_dimensional.claim_id) {
        throw std::invalid_argument(
            "claim and multi-dimensional result ids do not match");
    }
    if (!claim.direction || !multi_dimensional.mc_score ||
        !multi_dimensional.dimensions.sufficiency) {
        return std::nullopt;
    }
    if (!std::isfinite(*claim.direction) || *claim.direction < -1.0 ||
        *claim.direction > 1.0) {
        throw std::invalid_argument(
            "claim.direction must be finite and within [-1, 1]");
    }

    const double mc =
        requireProbability(*multi_dimensional.mc_score, "mc_score");
    const double sufficiency = requireProbability(
        *multi_dimensional.dimensions.sufficiency, "sufficiency");
    const double directional_sufficiency =
        sufficiency * std::abs(*claim.direction);

    double logic = 0.5;
    if (multi_dimensional.logic_available) {
        if (!multi_dimensional.dimensions.logic) {
            throw std::invalid_argument(
                "logic_available requires a logic score");
        }
        logic = requireProbability(*multi_dimensional.dimensions.logic,
                                   "logic_score");
    }

    // q_T + q_M + q_I + q_F = 1；无方向、纯中立或强冲突时 Q=0，I 占主导。
    const std::array<double, 4> raw{
        directional_sufficiency * mc * logic,
        directional_sufficiency * mc * (1.0 - logic),
        1.0 - directional_sufficiency,
        directional_sufficiency * (1.0 - mc)};

    std::array<double, 4> logits{};
    for (std::size_t index = 0; index < raw.size(); ++index) {
        logits[index] =
            std::log(raw[index] + config_.epsilon) / config_.temperature;
    }
    const double maximum = *std::max_element(logits.begin(), logits.end());
    std::array<double, 4> calibrated{};
    double denominator = 0.0;
    for (std::size_t index = 0; index < logits.size(); ++index) {
        calibrated[index] = std::exp(logits[index] - maximum);
        denominator += calibrated[index];
    }
    for (double& probability : calibrated) probability /= denominator;

    ProbabilityEvaluation result;
    result.uncalibrated_probabilities = toProbabilities(raw);
    result.probabilities = toProbabilities(calibrated);
    result.directional_sufficiency = directional_sufficiency;
    result.logic_for_mapping = logic;
    result.predicted_label = labelAt(static_cast<std::size_t>(
        std::distance(calibrated.begin(),
                      std::max_element(calibrated.begin(), calibrated.end()))));
    return result;
}

MulticlassCalibrationMetrics ProbabilityAdjuster::metrics(
    const std::vector<MulticlassCalibrationObservation>& observations,
    std::size_t bin_count) const {
    if (bin_count == 0) {
        throw std::invalid_argument("bin_count must be greater than zero");
    }

    MulticlassCalibrationMetrics result;
    if (observations.empty()) return result;

    std::array<double, 4> class_nll_sum{};
    std::array<std::size_t, 4> class_count{};
    std::vector<BinAccumulator> bins(bin_count);
    double brier_sum = 0.0;

    for (const auto& observation : observations) {
        const auto probabilities = probabilityArray(observation.probabilities);
        const std::size_t truth = labelIndex(observation.true_label);
        class_nll_sum[truth] +=
            -std::log(std::max(probabilities[truth], config_.epsilon));
        ++class_count[truth];

        for (std::size_t index = 0; index < probabilities.size(); ++index) {
            const double target = index == truth ? 1.0 : 0.0;
            const double difference = probabilities[index] - target;
            brier_sum += difference * difference;
        }

        const auto predicted_iterator =
            std::max_element(probabilities.begin(), probabilities.end());
        const std::size_t predicted = static_cast<std::size_t>(
            std::distance(probabilities.begin(), predicted_iterator));
        const double confidence = *predicted_iterator;
        const std::size_t bin_index = std::min(
            bin_count - 1,
            static_cast<std::size_t>(confidence * bin_count));
        auto& bin = bins[bin_index];
        ++bin.count;
        bin.correct_count += predicted == truth ? 1U : 0U;
        bin.confidence_sum += confidence;
    }

    bool all_classes_present = true;
    double macro_nll = 0.0;
    for (std::size_t index = 0; index < class_count.size(); ++index) {
        if (class_count[index] == 0) {
            all_classes_present = false;
            break;
        }
        macro_nll += class_nll_sum[index] /
                     static_cast<double>(class_count[index]);
    }
    if (all_classes_present) result.macro_negative_log_likelihood = macro_nll / 4.0;

    const double sample_count = static_cast<double>(observations.size());
    result.brier_score = brier_sum / sample_count;
    double ece = 0.0;
    for (const auto& bin : bins) {
        if (bin.count == 0) continue;
        const double count = static_cast<double>(bin.count);
        const double accuracy = static_cast<double>(bin.correct_count) / count;
        const double confidence = bin.confidence_sum / count;
        ece += count / sample_count * std::abs(accuracy - confidence);
    }
    result.expected_calibration_error = std::clamp(ece, 0.0, 1.0);
    return result;
}

}  // namespace chongming::qualifier
