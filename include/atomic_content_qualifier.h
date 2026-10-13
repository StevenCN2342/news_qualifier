#pragma once

#include "evidence_authenticity_qualifier.h"

#define ATOMIC_CONTENT_QUALIFIER_SIGMOID_ALPHA 4.0
#define ATOMIC_CONTENT_QUALIFIER_EPSILON 1e-8

namespace chongming::qualifier {

/** 第一步内容事实性 sigmoid 的参数。 */
struct AtomicContentConfig {
    double sigmoid_alpha{ATOMIC_CONTENT_QUALIFIER_SIGMOID_ALPHA};
    double epsilon{ATOMIC_CONTENT_QUALIFIER_EPSILON};
};

/** 聚合支持/反驳证据，生成一条原子声明的内容分和充分性状态。 */
class AtomicContentQualifier {
public:
    /** 配置非法时抛出 std::invalid_argument。 */
    explicit AtomicContentQualifier(
        AtomicContentConfig content_config = AtomicContentConfig{},
        EvidenceAuthenticityConfig evidence_config = EvidenceAuthenticityConfig{});

    /** 评估单条原子声明；无方向性证据时 D=0，因此内容分为 0.5。 */
    ClaimEvaluation evaluate(const AtomicClaim& claim) const;

private:
    AtomicContentConfig config_;
    EvidenceAuthenticityQualifier evidence_qualifier_;
};

}  // namespace chongming::qualifier
