#pragma once

#include "qualifier_types.h"

namespace chongming::qualifier {

/** 第三步逻辑关系评分器。 */
class LogicRelationshipQualifier {
public:
    /**
     * 计算一条关系的逻辑分数。
     * 当下标无效、解析失败或参与声明证据不足时，使用声明多数投票回退。
     */
    LogicEvaluation evaluate(
        const RelationInput& relation,
        const std::vector<ClaimEvaluation>& claims) const;

private:
    static double majorityFallback(
        const RelationInput& relation,
        const std::vector<ClaimEvaluation>& claims);
};

}  // namespace chongming::qualifier
