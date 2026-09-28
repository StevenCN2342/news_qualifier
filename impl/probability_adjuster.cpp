#include "probability_adjuster.h"

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

// 分段计算 sigmoid，避免 scaled_logit 很小时 exp(-x) 上溢。
double stableSigmoid(double value) {
    if (value >= 0.0) {
        return 1.0 / (1.0 + std::exp(-value));
    }
    const double exponential = std::exp(value);
    return exponential / (1.0 + exponential);
}

struct BinAccumulator {
    std::size_t count{0};
    std::size_t correct_count{0};
    double probability_sum{0.0};
};

}  // namespace

ProbabilityAdjuster::ProbabilityAdjuster(ProbabilityAdjusterConfig config)
    : config_(config) {
    if (!std::isfinite(config_.temperature) || config_.temperature <= 0.0) {
        throw std::invalid_argument("temperature must be finite and greater than zero");
    }
    if (!std::isfinite(config_.epsilon) || config_.epsilon <= 0.0 ||
        config_.epsilon >= 0.5) {
        throw std::invalid_argument("epsilon must be finite and within (0, 0.5)");
    }
}

double ProbabilityAdjuster::calibrate(double mc_score) const {
    requireProbability(mc_score, "mc_score");

    // README 要求先裁剪，防止 log(0) 以及 logit 在边界处发散。
    const double clipped =
        std::clamp(mc_score, config_.epsilon, 1.0 - config_.epsilon);
    const double logit = std::log(clipped / (1.0 - clipped));
    return stableSigmoid(logit / config_.temperature);
}

std::optional<double> ProbabilityAdjuster::calibrate(
    std::optional<double> mc_score) const {
    if (!mc_score) return std::nullopt;
    return calibrate(*mc_score);
}

std::optional<double> ProbabilityAdjuster::expectedCalibrationError(
    const std::vector<CalibrationObservation>& observations,
    std::size_t bin_count) const {
    if (bin_count == 0) {
        throw std::invalid_argument("bin_count must be greater than zero");
    }
    if (observations.empty()) return std::nullopt;

    std::vector<BinAccumulator> bins(bin_count);
    for (const auto& observation : observations) {
        requireProbability(observation.probability, "observation.probability");

        // 区间采用 [b/m,(b+1)/m)，概率 1 特别归入最后一箱。
        const std::size_t index = std::min(
            bin_count - 1,
            static_cast<std::size_t>(observation.probability * bin_count));
        auto& bin = bins[index];
        ++bin.count;
        bin.correct_count += observation.correct ? 1U : 0U;
        bin.probability_sum += observation.probability;
    }

    double ece = 0.0;
    const double sample_count = static_cast<double>(observations.size());
    for (const auto& bin : bins) {
        if (bin.count == 0) continue;
        const double count = static_cast<double>(bin.count);
        const double accuracy = static_cast<double>(bin.correct_count) / count;
        const double confidence = bin.probability_sum / count;
        ece += (count / sample_count) * std::abs(accuracy - confidence);
    }
    return std::clamp(ece, 0.0, 1.0);
}

}  // namespace chongming::qualifier
