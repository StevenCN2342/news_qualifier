#pragma once

#include <optional>
#include <string>
#include <vector>

namespace chongming::qualifier {

/** README 第七层使用的错误预算和四条显式安全阈值。 */
struct RejectionTriggerConfig {
    double error_budget{0.05};
    double sufficiency_threshold{0.30};
    double consensus_threshold{0.40};
    double independence_threshold{0.30};
    double calibrated_probability_threshold{0.35};
    double low_confidence_sufficiency_threshold{0.50};
};

/** 拒答可能由多个条件同时触发，因此原因使用数组而不是单一枚举。 */
enum class RejectionReason {
    ConformalThresholdExceeded,
    InsufficientEvidence,
    JuryDisagreement,
    SourceDependency,
    LowCalibratedProbability,
    InsufficientEvidenceForLowConfidence
};

/** 第七层需要的全部输入；任一字段未定义时尚不能作出完整拒答决定。 */
struct RejectionInput {
    std::optional<double> nonconformity_score;
    std::optional<double> conformal_threshold;
    std::optional<double> sufficiency;
    std::optional<double> consensus;
    std::optional<double> independence;
    std::optional<double> calibrated_probability;
};

/** 一次完整的回答/拒答决定以及可审计的触发原因。 */
struct RejectionDecision {
    bool should_reject{false};
    std::vector<RejectionReason> reasons;
    std::string suggested_message;
};

class RejectionTrigger {
public:
    /** 配置不合法时抛出 std::invalid_argument。 */
    explicit RejectionTrigger(
        RejectionTriggerConfig config = RejectionTriggerConfig{});

    /**
     * 根据校准集计算 tau=s_(k)。空校准集没有阈值，返回 nullopt。
     * 非一致性分数必须位于 [0,1]。
     */
    std::optional<double> conformalThreshold(
        const std::vector<double>& calibration_nonconformity_scores) const;

    /**
     * 应用保形阈值和 README 的四条安全规则。输入未完整时返回 nullopt，
     * 以区分“尚未计算”和“已计算且允许回答”。
     */
    std::optional<RejectionDecision> decide(const RejectionInput& input) const;

private:
    RejectionTriggerConfig config_;
};

/** 返回拒答原因的稳定中文说明。 */
const char* toString(RejectionReason reason) noexcept;

}  // namespace chongming::qualifier
