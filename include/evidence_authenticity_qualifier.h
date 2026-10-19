#pragma once

#include "qualifier_types.h"

#include <unordered_map>

#define EVIDENCE_AUTHENTICITY_QUALIFIER_TIME_DECAY_PER_DAY 0.01
#define EVIDENCE_AUTHENTICITY_QUALIFIER_SUFFICIENCY_GAMMA 0.50
#define EVIDENCE_AUTHENTICITY_QUALIFIER_REFERENCE_CLUSTER_COUNT 3.0
#define EVIDENCE_AUTHENTICITY_QUALIFIER_KEYWORD_SIMILARITY_ZERO_BOUND 0.60
#define EVIDENCE_AUTHENTICITY_QUALIFIER_KEYWORD_SIMILARITY_IDENTITY_BOUND 0.80

#define EVIDENCE_AUTHENTICITY_QUALIFIER_OFFICIAL_REGISTRY 0.90
#define EVIDENCE_AUTHENTICITY_QUALIFIER_ACADEMIC 0.88
#define EVIDENCE_AUTHENTICITY_QUALIFIER_OPEN_WEB 0.72
#define EVIDENCE_AUTHENTICITY_QUALIFIER_SOCIAL_MEDIA 0.72
#define EVIDENCE_AUTHENTICITY_QUALIFIER_NEWS_MEDIA 0.68
#define EVIDENCE_AUTHENTICITY_QUALIFIER_VIDEO_OR_GITHUB 0.65
#define EVIDENCE_AUTHENTICITY_QUALIFIER_MARKET_DATA 0.60

namespace chongming::qualifier {

/** 第一、二步的可调参数以及来源可靠性基线表。 */
struct EvidenceAuthenticityConfig {
    double time_decay_per_day{EVIDENCE_AUTHENTICITY_QUALIFIER_TIME_DECAY_PER_DAY};
    double sufficiency_gamma{EVIDENCE_AUTHENTICITY_QUALIFIER_SUFFICIENCY_GAMMA};
    double reference_cluster_count{
        EVIDENCE_AUTHENTICITY_QUALIFIER_REFERENCE_CLUSTER_COUNT};
    // 低于 zero_bound 的相似度计0；从 zero_bound 到 identity_bound 线性过渡；
    // 达到 identity_bound 后保留 Agent 给出的原始相似度。
    double keyword_similarity_zero_bound{
        EVIDENCE_AUTHENTICITY_QUALIFIER_KEYWORD_SIMILARITY_ZERO_BOUND};
    double keyword_similarity_identity_bound{
        EVIDENCE_AUTHENTICITY_QUALIFIER_KEYWORD_SIMILARITY_IDENTITY_BOUND};
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
     * 将逐条证据评估合并为来源簇；每簇的支持、反驳和中立分别取最大值。
     */
    std::vector<EvidenceClusterAssessment> summarizeClusters(
        const std::vector<EvidenceAssessment>& assessments) const;

    /** 按 1-exp(-gamma/kappa*sum(max(s_g,r_g,n_g))) 计算充分性。 */
    double sufficiencyScore(
        const std::vector<EvidenceClusterAssessment>& clusters) const;

private:
    EvidenceAuthenticityConfig config_;
};

}  // namespace chongming::qualifier
