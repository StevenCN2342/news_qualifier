#pragma once

#include "qualifier_types.h"

namespace chongming::qualifier {

/** 第四步多智能体德尔菲陪审团融合器。 */
class Jury {
public:
    /**
     * 根据信任分和智能体置信度加权投票。
     * 总有效票重为零时返回 has_valid_votes=false，不伪造一致性分数。
     */
    JuryEvaluation deliberate(const std::vector<JurorOpinion>& opinions) const;
};

}  // namespace chongming::qualifier
