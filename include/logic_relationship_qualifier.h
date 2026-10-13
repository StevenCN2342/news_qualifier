#pragma once

#include "qualifier_types.h"

#define LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_TIME_WEIGHT 0.20
#define LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_MECHANISM_WEIGHT 0.20
#define LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_COUNTERFACTUAL_WEIGHT 0.20
#define LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_CONFOUNDER_WEIGHT 0.20
#define LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_DOSE_RESPONSE_WEIGHT 0.20

namespace chongming::qualifier {

struct LogicRelationshipConfig {
    double causal_time_weight{LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_TIME_WEIGHT};
    double causal_mechanism_weight{LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_MECHANISM_WEIGHT};
    double causal_counterfactual_weight{LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_COUNTERFACTUAL_WEIGHT};
    double causal_confounder_weight{LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_CONFOUNDER_WEIGHT};
    double causal_dose_response_weight{LOGIC_RELATIONSHIP_QUALIFIER_CAUSAL_DOSE_RESPONSE_WEIGHT};
};

/** 第三层语义关系与逻辑聚合评分器。 */
class LogicRelationshipQualifier {
public:
    /**
     * 创建逻辑关系评分器。
     * 五个因果权重必须是非负有限数，并且总和必须为 1。
     */
    explicit LogicRelationshipQualifier(
        LogicRelationshipConfig config = LogicRelationshipConfig{});

    /**
     * 计算一条语义关系的逻辑分数。
     * 下标无效、解析失败或 NLI 分数无效时返回 logic_available=false。
     */
    LogicEvaluation evaluate(
        const RelationInput& relation,
        const std::vector<ClaimEvaluation>& claims) const;

    /** 对 AND/OR/NOT/IMPLICATION/CAUSAL 执行确定性的逻辑聚合。 */
    LogicEvaluation evaluate(
        const AggregationInput& aggregation,
        const std::vector<ClaimEvaluation>& claims) const;

    /**
     * 按 claim 汇总所有可用关联边；没有可用边的 claim 保持 not_applicable。
     * 二元边分别计入左右声明，自环只计入一次，NOT 只计入左声明。
     */
    std::vector<ClaimLogicEvaluation> aggregateByClaim(
        std::size_t claim_count,
        const std::vector<LogicEvaluation>& edge_results) const;

private:
    LogicRelationshipConfig config_;

    /** 五个因果子维度是否都已经由上游模型给出。 */
    static bool hasCompleteCausalFactors(const CausalFactorScores& factors);

    /** 按优化规范计算 R_ij = sum(w_k * S_ijk)。 */
    double causalRelationConfidence(const CausalFactorScores& factors) const;
};

}  // namespace chongming::qualifier
