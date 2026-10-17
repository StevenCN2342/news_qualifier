#pragma once

#include "qualifier_types.h"

#define MULTI_DIMENSIONAL_QUALIFIER_CONTENT_LOGIC_WEIGHT (1.0 / 3.0)
#define MULTI_DIMENSIONAL_QUALIFIER_EVIDENCE_WEIGHT (1.0 / 3.0)
#define MULTI_DIMENSIONAL_QUALIFIER_JURY_WEIGHT (1.0 / 3.0)

namespace chongming::qualifier {

/**
 * 第五层三个有方向分量的权重配置。
 *
 * 三项之和必须为 1。默认等权值只是 fit 前的合法初值，最终值应由
 * sandbox 搜索、验证并冻结后通过 ModelConfig 注入。
 */
struct MultiDimensionalConfig {
    double content_logic_weight{
        MULTI_DIMENSIONAL_QUALIFIER_CONTENT_LOGIC_WEIGHT};
    double evidence_weight{MULTI_DIMENSIONAL_QUALIFIER_EVIDENCE_WEIGHT};
    double jury_weight{MULTI_DIMENSIONAL_QUALIFIER_JURY_WEIGHT};
};

/** 汇总前四层结果，输出逐声明可解释维度和 MC_i；标签留给第六层。 */
class MultiDimensionalQualifier {
public:
    /** 权重不是 [0,1] 内有限数或总和不为 1 时抛出 std::invalid_argument。 */
    explicit MultiDimensionalQualifier(
        MultiDimensionalConfig config = MultiDimensionalConfig{});

    /**
     * claim_logic_results 使用第三层 aggregateByClaim 的输出；jury_results 按
     * claim 下标对齐。逻辑确实不适用时 d_CL=d_C；陪审团缺失时 d_J=0，
     * 且剩余权重不重新归一化，使缺失信息自然把结果拉向中性点。
     */
    MultiDimensionalEvaluation evaluate(
        const std::vector<ClaimEvaluation>& claims,
        const std::vector<ClaimLogicEvaluation>& claim_logic_results,
        const std::vector<JuryEvaluation>& jury_results) const;

private:
    MultiDimensionalConfig config_;
};

/** 返回稳定的中文标签文本；返回指针指向静态字符串。 */
const char* toString(CredibilityLabel label) noexcept;

}  // namespace chongming::qualifier
