#include "evidence_authenticity_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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
    // 来源可靠性默认值统一由头文件宏定义提供，避免出现两套默认参数。
    : source_baselines{
          {SourceType::OfficialRegistry,
           EVIDENCE_AUTHENTICITY_QUALIFIER_OFFICIAL_REGISTRY},

          {SourceType::Academic,
           EVIDENCE_AUTHENTICITY_QUALIFIER_ACADEMIC},

          {SourceType::OpenWeb,
           EVIDENCE_AUTHENTICITY_QUALIFIER_OPEN_WEB},

          {SourceType::SocialMedia,
           EVIDENCE_AUTHENTICITY_QUALIFIER_SOCIAL_MEDIA},

          {SourceType::NewsMedia,
           EVIDENCE_AUTHENTICITY_QUALIFIER_NEWS_MEDIA},

          {SourceType::VideoOrGitHub,
           EVIDENCE_AUTHENTICITY_QUALIFIER_VIDEO_OR_GITHUB},

          {SourceType::MarketData,
           EVIDENCE_AUTHENTICITY_QUALIFIER_MARKET_DATA}} {}

EvidenceAuthenticityQualifier::EvidenceAuthenticityQualifier(
    EvidenceAuthenticityConfig config)
    : config_(std::move(config)) {
    if (!std::isfinite(config_.time_decay_per_day) ||
        config_.time_decay_per_day < 0.0 ||
        !std::isfinite(config_.sufficiency_gamma) ||
        config_.sufficiency_gamma <= 0.0 ||
        !std::isfinite(config_.reference_cluster_count) ||
        config_.reference_cluster_count <= 0.0) {
        throw std::invalid_argument("invalid evidence-authenticity configuration");
    }
    for (const auto& [source_type, baseline] : config_.source_baselines) {
        static_cast<void>(source_type);
        if (!std::isfinite(baseline) || baseline < 0.0 || baseline > 1.0) {
            throw std::invalid_argument(
                "source reliability baselines must be within [0, 1]");
        }
    }
}

std::vector<EvidenceAssessment> EvidenceAuthenticityQualifier::assess(
    const AtomicClaim& claim) const {
    // 第一遍先统计原始来源簇规模，第二遍才能为簇中每条证据计算 1/n。
    // 对全部证据取该值的平均，恰好得到 |G_i|/|E_i^unique|。
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
        item.cluster_id = evidence.original_source_id.empty()
                              ? std::string{"__evidence__"} + evidence.id
                              : evidence.original_source_id;
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
        item.independence_discount =
            1.0 / static_cast<double>(cluster_sizes.at(item.cluster_id));
        item.nli_label = evidence.nli_label;
        item.nli_confidence = clamp01(evidence.nli_confidence);
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<EvidenceClusterAssessment>
EvidenceAuthenticityQualifier::summarizeClusters(
    const std::vector<EvidenceAssessment>& assessments) const {
    std::vector<EvidenceClusterAssessment> clusters;
    std::unordered_map<std::string, std::size_t> cluster_indexes;
    for (const auto& item : assessments) {
        const auto [position, inserted] =
            cluster_indexes.emplace(item.cluster_id, clusters.size());
        if (inserted) {
            EvidenceClusterAssessment cluster;
            cluster.cluster_id = item.cluster_id;
            clusters.push_back(std::move(cluster));
        }

        auto& cluster = clusters[position->second];
        const double effective_weight = clamp01(item.reliability.value_or(0.0));
        switch (item.nli_label) {
            case NliLabel::Support:
                cluster.support = std::max(cluster.support, effective_weight);
                break;
            case NliLabel::Refute:
                cluster.refute = std::max(cluster.refute, effective_weight);
                break;
            case NliLabel::Neutral:
                cluster.neutral = std::max(cluster.neutral, effective_weight);
                break;
        }
    }
    return clusters;
}

double EvidenceAuthenticityQualifier::sufficiencyScore(
    const std::vector<EvidenceClusterAssessment>& clusters) const {
    double cluster_effective_sum = 0.0;
    for (const auto& cluster : clusters) {
        cluster_effective_sum +=
            std::max({cluster.support, cluster.refute, cluster.neutral});
    }
    const double normalized_evidence =
        cluster_effective_sum / config_.reference_cluster_count;
    return clamp01(
        1.0 - std::exp(-config_.sufficiency_gamma * normalized_evidence));
}

}  // namespace chongming::qualifier
