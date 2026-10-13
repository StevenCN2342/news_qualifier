#pragma once

#include "qualifier_types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#define REJECTION_TRIGGER_ERROR_BUDGET 0.05

namespace chongming::qualifier {

/** 用户预先指定的 family-level split conformal 错误预算。 */
struct RejectionTriggerConfig {
    double error_budget{REJECTION_TRIGGER_ERROR_BUDGET};
};

/** calib 中一条带 family 与真值标签的声明概率。 */
struct ConformalCalibrationSample {
    std::string family_id;
    FourClassProbabilities probabilities;
    CredibilityLabel true_label{CredibilityLabel::InsufficientInformation};
};

/**
 * family 级 conformal 分位点。
 * is_infinite=true 表示样本不足、q_conf=+infinity，此时预测集包含全部类别。
 */
struct ConformalThreshold {
    double value{0.0};
    bool is_infinite{false};
    std::size_t family_count{0};
    std::size_t rank{0};
};

enum class RejectionReason {
    EmptyPredictionSet,
    MultipleCandidateLabels
};

/** 单条声明的 conformal 预测集和最终回答/拒答决定。 */
struct RejectionDecision {
    bool should_reject{false};
    std::vector<CredibilityLabel> prediction_set;
    std::optional<CredibilityLabel> accepted_label;
    std::vector<RejectionReason> reasons;
    std::string suggested_message;
};

/** 第七层 family-level split conformal 拒答器。 */
class RejectionTrigger {
public:
    explicit RejectionTrigger(
        RejectionTriggerConfig config = RejectionTriggerConfig{});

    /**
     * 先在每个 family 内取真标签非一致性分数的最大值，再计算有限样本分位点。
     * calib 为空时返回 nullopt；样本不足时返回 is_infinite=true。
     */
    std::optional<ConformalThreshold> conformalThreshold(
        const std::vector<ConformalCalibrationSample>& calibration) const;

    /** 根据 Gamma={k:1-p_k<=q_conf} 构造预测集；顺序固定为 T/M/I/F。 */
    std::vector<CredibilityLabel> predictionSet(
        const FourClassProbabilities& probabilities,
        const ConformalThreshold& threshold) const;

    /** 预测集恰为单例时回答，否则拒答；不再追加独立硬阈值。 */
    RejectionDecision decide(
        const FourClassProbabilities& probabilities,
        const ConformalThreshold& threshold) const;

private:
    RejectionTriggerConfig config_;
};

const char* toString(RejectionReason reason) noexcept;

}  // namespace chongming::qualifier
