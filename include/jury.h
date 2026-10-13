#pragma once

#include "qualifier_types.h"

namespace chongming::qualifier {

/** 第四步多智能体德尔菲陪审团融合器。 */
class Jury {
public:
    /**
     * 根据信任分和智能体置信度进行带符号加权投票。
     * 支持为 +1、反驳为 -1、未知为 0；未知票不会增加真实性方向。
     * 总有效票重为零时显式返回 jury_score=0.5、consensus_score=0。
     */
    JuryEvaluation deliberate(
        const std::vector<JurorOpinion>& opinions) const;
};

}  // namespace chongming::qualifier
