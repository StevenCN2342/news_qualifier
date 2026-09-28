#pragma once

#include "qualifier_types.h"

#include <unordered_map>

namespace chongming::qualifier {

/** 第一、二步的可调参数以及来源可靠性基线表。 */
struct EvidenceAuthenticityConfig {
    double relevance_threshold{0.20};
    double time_decay_per_day{0.01};
    double sufficiency_gamma{0.50};
    double sufficiency_threshold{0.30};
    std::unordered_map<SourceType, double> source_baselines;

    EvidenceAuthenticityConfig();
};

/**
 * 计算证据可靠性、主题相关性、时效性、来源独立性和证据充分性。
 * 本类不执行 NLI；NLI 标签及置信度由调用方放入 Evidence。
 */
class EvidenceAuthenticityQualifier {
public:
    /** 构造时校验阈值和衰减参数；非法配置会抛出 std::invalid_argument。 */
    explicit EvidenceAuthenticityQualifier(EvidenceAuthenticityConfig config = EvidenceAuthenticityConfig{});

    /** 对声明中的全部候选证据生成逐条评估结果；返回项中的各分数均已赋值。 */
    std::vector<EvidenceAssessment> assess(const AtomicClaim& claim) const;

    /**
     * 按 1-exp(-gamma*sum(delta*r)) 计算充分性。
     * 低于相关性阈值的证据不参与求和；中立 NLI 证据按 README 原公式参与。
     */
    double sufficiencyScore(const std::vector<EvidenceAssessment>& assessments) const;

    /** 返回用于标记“信息不足”的充分性阈值。 */
    double sufficiencyThreshold() const noexcept;

private:
    EvidenceAuthenticityConfig config_;
};

}  // namespace chongming::qualifier
