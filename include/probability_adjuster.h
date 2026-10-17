#pragma once

#include "qualifier_types.h"

#include <cstddef>
#include <optional>
#include <vector>

#define PROBABILITY_ADJUSTER_TEMPERATURE 1.0
#define PROBABILITY_ADJUSTER_MIN_TEMPERATURE 0.1
#define PROBABILITY_ADJUSTER_MAX_TEMPERATURE 5.0
#define PROBABILITY_ADJUSTER_EPSILON 1e-8
#define PROBABILITY_ADJUSTER_ECE_BIN_COUNT 10U

namespace chongming::qualifier {

/** 第六层四类温度缩放配置；temperature 应在 fit 的折外预测上拟合。 */
struct ProbabilityAdjusterConfig {
    double temperature{PROBABILITY_ADJUSTER_TEMPERATURE};
    double epsilon{PROBABILITY_ADJUSTER_EPSILON};
};

/** 用于报告四类概率校准指标的一条已标注观测。 */
struct MulticlassCalibrationObservation {
    FourClassProbabilities probabilities;
    CredibilityLabel true_label{CredibilityLabel::InsufficientInformation};
};

/** 优化规范要求报告的概率质量指标。 */
struct MulticlassCalibrationMetrics {
    std::optional<double> macro_negative_log_likelihood;
    std::optional<double> brier_score;
    std::optional<double> expected_calibration_error;
};

/**
 * 将第五层结果映射为 T/M/I/F 四类质量，并执行多类温度缩放。
 * 本类不拟合 temperature，只使用 fit 流程已经冻结的配置。
 */
class ProbabilityAdjuster {
public:
    /** temperature 不在 [0.1,5.0] 或 epsilon 不在 (0,0.25) 时抛出异常。 */
    explicit ProbabilityAdjuster(
        ProbabilityAdjusterConfig config = ProbabilityAdjusterConfig{});

    /**
     * 由第五层 MC-Score 计算四类概率。前层必要字段未定义时返回 nullopt。
     * 逻辑适用时 H=L；逻辑确实不适用时 H=1。
     */
    std::optional<ProbabilityEvaluation> evaluate(
        const ClaimMultiDimensionalEvaluation& multi_dimensional) const;

    /**
     * 计算 Macro-NLL、四类 Brier score 与基于最大类置信度的 ECE。
     * Macro-NLL 只有在四个真值类别均有样本时才定义。
     */
    MulticlassCalibrationMetrics metrics(
        const std::vector<MulticlassCalibrationObservation>& observations,
        std::size_t bin_count = PROBABILITY_ADJUSTER_ECE_BIN_COUNT) const;

private:
    ProbabilityAdjusterConfig config_;
};

}  // namespace chongming::qualifier
