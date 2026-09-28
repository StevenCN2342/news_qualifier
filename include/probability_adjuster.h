#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace chongming::qualifier {

/** 第六层温度缩放参数。temperature 应由验证集学习后传入。 */
struct ProbabilityAdjusterConfig {
    double temperature{1.0};
    double epsilon{1e-8};
};

/** ECE 的一条验证样本：预测概率以及该预测最终是否正确。 */
struct CalibrationObservation {
    double probability{0.0};
    bool correct{false};
};

/**
 * 使用温度缩放校准 MCScore，并计算预期校准误差（ECE）。
 * 本类不训练温度参数，只使用外部验证流程已经确定的 temperature。
 */
class ProbabilityAdjuster {
public:
    /** temperature<=0 或 epsilon 不在 (0,0.5) 时抛出 std::invalid_argument。 */
    explicit ProbabilityAdjuster(
        ProbabilityAdjusterConfig config = ProbabilityAdjusterConfig{});

    /**
     * 按 sigmoid(logit(clip(mc_score))/temperature) 返回校准概率。
     * mc_score 必须位于 [0,1]，否则抛出 std::invalid_argument。
     */
    double calibrate(double mc_score) const;

    /** 未计算出 MCScore 时传播 nullopt；有值时与 double 重载行为一致。 */
    std::optional<double> calibrate(std::optional<double> mc_score) const;

    /**
     * 使用等宽置信度分箱计算 ECE。空验证集没有定义，返回 nullopt；
     * bin_count 必须大于 0。
     */
    std::optional<double> expectedCalibrationError(
        const std::vector<CalibrationObservation>& observations,
        std::size_t bin_count = 10) const;

private:
    ProbabilityAdjusterConfig config_;
};

}  // namespace chongming::qualifier
