#pragma once

#include "qualifier_types.h"

#define MULTI_DIMENSIONAL_QUALIFIER_CONTENT_WEIGHT 0.20
#define MULTI_DIMENSIONAL_QUALIFIER_LOGIC_WEIGHT 0.20
#define MULTI_DIMENSIONAL_QUALIFIER_SUFFICIENCY_WEIGHT 0.20
#define MULTI_DIMENSIONAL_QUALIFIER_JURY_WEIGHT 0.20
#define MULTI_DIMENSIONAL_QUALIFIER_SOURCE_WEIGHT 0.20

namespace chongming::qualifier {

/**
 * 第五层 MC_i 的五项权重配置。
 *
 * 五项之和必须为 1，logic_weight 还必须不大于 0.8，以保证没有逻辑边时
 * 删除逻辑项后分母仍为正。这里的等权默认值只是 fit 前的合法初值，最终值
 * 应由训练/验证流程冻结后传入。
 */
struct MultiDimensionalConfig {
    double content_weight{MULTI_DIMENSIONAL_QUALIFIER_CONTENT_WEIGHT};
    double logic_weight{MULTI_DIMENSIONAL_QUALIFIER_LOGIC_WEIGHT};
    double sufficiency_weight{MULTI_DIMENSIONAL_QUALIFIER_SUFFICIENCY_WEIGHT};
    double jury_weight{MULTI_DIMENSIONAL_QUALIFIER_JURY_WEIGHT};
    double source_weight{MULTI_DIMENSIONAL_QUALIFIER_SOURCE_WEIGHT};
};

/** 汇总前四层结果，输出逐声明可解释维度和 MC_i；标签留给第六层。 */
class MultiDimensionalQualifier {
public:
    /** 权重非法、总和不为 1 或 logic_weight>0.8 时抛出 std::invalid_argument。 */
    explicit MultiDimensionalQualifier(
        MultiDimensionalConfig config = MultiDimensionalConfig{});

    /**
     * claim_logic_results 使用第三层 aggregateByClaim 的输出；jury_results 按
     * claim 下标对齐。无逻辑边时删除逻辑项并重新归一化权重；无陪审团输出
     * 时按第四层边界规则使用 jury=0.5、consensus=0。
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
