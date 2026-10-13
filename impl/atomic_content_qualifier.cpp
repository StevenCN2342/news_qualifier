#include "atomic_content_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace chongming::qualifier {
namespace {

double stableSigmoid(double value) {
    if (value >= 0.0) {
        return 1.0 / (1.0 + std::exp(-value));
    }
    const double exponential = std::exp(value);
    return exponential / (1.0 + exponential);
}

}  // namespace

AtomicContentQualifier::AtomicContentQualifier(
    AtomicContentConfig content_config,
    EvidenceAuthenticityConfig evidence_config)
    : config_(content_config), evidence_qualifier_(std::move(evidence_config)) {
    if (!std::isfinite(config_.sigmoid_alpha) ||
        config_.sigmoid_alpha <= 0.0 || !std::isfinite(config_.epsilon) ||
        config_.epsilon <= 0.0) {
        throw std::invalid_argument("invalid atomic-content configuration");
    }
}

ClaimEvaluation AtomicContentQualifier::evaluate(const AtomicClaim& claim) const {
    ClaimEvaluation result;
    result.claim_id = claim.id;
    result.evidence = evidence_qualifier_.assess(claim);
    result.evidence_clusters =
        evidence_qualifier_.summarizeClusters(result.evidence);
    double support_strength = 0.0;
    double refute_strength = 0.0;

    for (const auto& cluster : result.evidence_clusters) {
        // 同一来源簇的转载不重复累计；每簇只贡献最强支持和最强反驳。
        support_strength += cluster.support;
        refute_strength += cluster.refute;
    }

    result.support_strength = support_strength;
    result.refute_strength = refute_strength;
    result.direction =
        (support_strength - refute_strength) /
        (support_strength + refute_strength + config_.epsilon);
    result.direction =
        std::clamp(result.direction.value_or(0.0), -1.0, 1.0);
    result.content_score =
        stableSigmoid(config_.sigmoid_alpha * result.direction.value());
    result.content_score =
        std::clamp(result.content_score.value_or(0.5), 0.0, 1.0);
    result.sufficiency_score =
        evidence_qualifier_.sufficiencyScore(result.evidence_clusters);
    // 新规范不再用硬阈值改写内容方向；这里只保留“是否完全没有有效证据”
    // 的诊断状态，最终信息不足标签由第六层四类概率决定。
    result.status = result.sufficiency_score.value_or(0.0) > 0.0
                        ? ClaimStatus::Determinate
                        : ClaimStatus::InsufficientInformation;
    return result;
}

}  // namespace chongming::qualifier
