#include "evidence_authenticity_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace chongming::qualifier {
namespace {

// 所有进入评分公式的概率都必须留在 [0, 1]；NaN/Inf 视为无效输入。
double clamp01(double value) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    return std::clamp(value, 0.0, 1.0);
}

double keywordRelevance(const std::unordered_set<std::string>& claim_keywords,
                        const std::unordered_set<std::string>& evidence_keywords) {
    // README 明确要求任一集合为空时返回 0，避免分母为零。
    if (claim_keywords.empty() || evidence_keywords.empty()) {
        return 0.0;
    }
    std::size_t intersection = 0;
    const auto& smaller = claim_keywords.size() <= evidence_keywords.size()
                              ? claim_keywords
                              : evidence_keywords;
    const auto& larger = claim_keywords.size() <= evidence_keywords.size()
                             ? evidence_keywords
                             : claim_keywords;
    for (const auto& keyword : smaller) {
        intersection += larger.count(keyword);
    }
    return static_cast<double>(intersection) /
           static_cast<double>(smaller.size());
}

}  // namespace

EvidenceAuthenticityConfig::EvidenceAuthenticityConfig()
    // README“来源可靠性”表中的建议初始值。
    : source_baselines{{SourceType::OfficialRegistry, 0.90},
                       {SourceType::Academic, 0.88},
                       {SourceType::OpenWeb, 0.72},
                       {SourceType::SocialMedia, 0.72},
                       {SourceType::NewsMedia, 0.68},
                       {SourceType::VideoOrGitHub, 0.65},
                       {SourceType::MarketData, 0.60}} {}

EvidenceAuthenticityQualifier::EvidenceAuthenticityQualifier(
    EvidenceAuthenticityConfig config)
    : config_(std::move(config)) {
    if (config_.relevance_threshold < 0.0 ||
        config_.relevance_threshold > 1.0 ||
        config_.time_decay_per_day < 0.0 ||
        config_.sufficiency_gamma < 0.0 ||
        config_.sufficiency_threshold < 0.0 ||
        config_.sufficiency_threshold > 1.0) {
        throw std::invalid_argument("invalid evidence-authenticity configuration");
    }
}

std::vector<EvidenceAssessment> EvidenceAuthenticityQualifier::assess(
    const AtomicClaim& claim) const {
    // 第一遍先统计原始来源簇规模，第二遍才能为簇中每条证据计算 1/sqrt(n)。
    std::unordered_map<std::string, std::size_t> cluster_sizes;
    for (const auto& evidence : claim.evidence) {
        // 没有原始来源 id 时，将证据自身视为独立来源，避免把所有缺失 id 的
        // 证据错误地归入同一个来源簇。
        const std::string key = evidence.original_source_id.empty()
                                    ? std::string{"__evidence__"} + evidence.id
                                    : evidence.original_source_id;
        ++cluster_sizes[key];
    }

    std::vector<EvidenceAssessment> result;
    result.reserve(claim.evidence.size());
    for (const auto& evidence : claim.evidence) {
        EvidenceAssessment item;
        item.evidence_id = evidence.id;
        item.base_reliability = 0.0;
        // 来源缺失或调用方未为该类型配置基线时，可靠性基线保持为 0。
        if (evidence.source_type) {
            const auto baseline = config_.source_baselines.find(*evidence.source_type);
            if (baseline != config_.source_baselines.end()) {
                item.base_reliability = clamp01(baseline->second);
            }
        }
        item.relevance = keywordRelevance(claim.keywords, evidence.keywords);
        item.timeliness = 1.0;
        if (claim.time_sensitive) {
            if (claim.event_day && evidence.event_day) {
                const double days = static_cast<double>(
                    std::llabs(*evidence.event_day - *claim.event_day));
                item.timeliness = std::exp(-config_.time_decay_per_day * days);
            } else {
                // 时效敏感但日期缺失时采用保守值，防止未知时间获得满时效分。
                item.timeliness = 0.0;
            }
        }
        item.reliability = clamp01(item.base_reliability.value_or(0.0) *
                                   item.relevance.value_or(0.0) *
                                   item.timeliness.value_or(0.0));
        const std::string key = evidence.original_source_id.empty()
                                    ? std::string{"__evidence__"} + evidence.id
                                    : evidence.original_source_id;
        item.independence_discount =
            1.0 / std::sqrt(static_cast<double>(cluster_sizes.at(key)));
        item.filtered_by_relevance =
            item.relevance.value_or(0.0) < config_.relevance_threshold;
        item.nli_label = evidence.nli_label;
        item.nli_confidence = clamp01(evidence.nli_confidence);
        result.push_back(std::move(item));
    }
    return result;
}

double EvidenceAuthenticityQualifier::sufficiencyScore(
    const std::vector<EvidenceAssessment>& assessments) const {
    double effective_reliability = 0.0;
    for (const auto& item : assessments) {
        if (!item.filtered_by_relevance) {
            // 严格遵循 README 当前公式：中立证据也计入充分性，但已过滤证据不计入。
            effective_reliability += item.independence_discount.value_or(0.0) *
                                     item.reliability.value_or(0.0);
        }
    }
    return clamp01(1.0 - std::exp(-config_.sufficiency_gamma * effective_reliability));
}

double EvidenceAuthenticityQualifier::sufficiencyThreshold() const noexcept {
    return config_.sufficiency_threshold;
}

}  // namespace chongming::qualifier
