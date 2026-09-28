#pragma once

#include "qualifier_types.h"

namespace chongming::qualifier {

/** 第五步 MCScore 的四项权重；四项之和必须为 1。 */
struct MultiDimensionalConfig {
    double content_weight{0.35};
    double logic_weight{0.25};
    double sufficiency_weight{0.20};
    double source_quality_weight{0.20};
};

/** 汇总前四步结果并输出可解释维度、MCScore 和四级标签。 */
class MultiDimensionalQualifier {
public:
    /** 权重为负、非有限数或总和不为 1 时抛出 std::invalid_argument。 */
    explicit MultiDimensionalQualifier(
        MultiDimensionalConfig config = MultiDimensionalConfig{});

    /**
     * jury_results 按下标与 claims 对齐；缺失或无效的陪审团结果按零一致性处理。
     * 执行后所有维度和 MCScore 均有值：空方向集合回退为 0.5，空质量集合回退为 0。
     */
    MultiDimensionalEvaluation evaluate(
        const std::vector<ClaimEvaluation>& claims,
        const std::vector<LogicEvaluation>& relations,
        const std::vector<JuryEvaluation>& jury_results) const;

private:
    MultiDimensionalConfig config_;
};

/** 返回稳定的中文标签文本；返回指针指向静态字符串。 */
const char* toString(CredibilityLabel label) noexcept;

}  // namespace chongming::qualifier
