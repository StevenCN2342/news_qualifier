#pragma once

#include "evidence_authenticity_qualifier.h"

namespace chongming::qualifier {

/** 第一步内容事实性 sigmoid 的参数。 */
struct AtomicContentConfig {
    double sigmoid_alpha{4.0};
    double sigmoid_beta{2.0};
    double epsilon{1e-8};
};

/** 聚合支持/反驳证据，生成一条原子声明的内容分和充分性状态。 */
class AtomicContentQualifier {
public:
    /** 配置非法时抛出 std::invalid_argument。 */
    explicit AtomicContentQualifier(
        AtomicContentConfig content_config = AtomicContentConfig{},
        EvidenceAuthenticityConfig evidence_config = EvidenceAuthenticityConfig{});

    /** 评估单条原子声明；无有效支持或反驳证据时内容分回退为 0.5。 */
    ClaimEvaluation evaluate(const AtomicClaim& claim) const;

private:
    AtomicContentConfig config_;
    EvidenceAuthenticityQualifier evidence_qualifier_;
};

}  // namespace chongming::qualifier
