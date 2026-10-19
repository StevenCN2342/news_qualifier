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

double effectiveKeywordSimilarity(double similarity,
                                  double zero_bound,
                                  double identity_bound) {
    const double normalized = clamp01(similarity);
    if (normalized < zero_bound) return 0.0;
    if (normalized >= identity_bound) return normalized;

    // 连接 (zero_bound, 0) 与 (identity_bound, identity_bound) 的直线。
    // 默认边界为0.6和0.8时，该式等价于 y=4x-2.4。
    return identity_bound * (normalized - zero_bound) /
           (identity_bound - zero_bound);
}

double keywordRelevance(const std::unordered_set<std::string>& claim_keywords,
                        const Evidence& evidence,
                        double zero_bound,
                        double identity_bound) {
    // 相关度衡量“声明关键词被证据覆盖的比例”，因此分母固定为声明关键词数。
    if (claim_keywords.empty()) {
        return 0.0;
    }

    std::unordered_map<std::string, double> best_match;
    for (const auto& claim_keyword : claim_keywords) {
        best_match.emplace(claim_keyword,
                           evidence.keywords.count(claim_keyword) > 0 ? 1.0 : 0.0);
    }

    // 多个证据关键词可能指向同一个声明关键词；每个声明关键词只取最高值，
    // 防止同一含义被重复计分。中间区间先经过连续分段函数压缩。
    for (const auto& match : evidence.keyword_matches) {
        const double effective = effectiveKeywordSimilarity(
            match.similarity, zero_bound, identity_bound);
        if (effective <= 0.0) continue;
        const auto position = best_match.find(match.claim_keyword);
        if (position != best_match.end()) {
            position->second = std::max(position->second, effective);
        }
    }

    double covered = 0.0;
    for (const auto& [keyword, score] : best_match) {
        static_cast<void>(keyword);
        covered += score;
    }
    return covered / static_cast<double>(claim_keywords.size());
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
        config_.reference_cluster_count <= 0.0 ||
        !std::isfinite(config_.keyword_similarity_zero_bound) ||
        !std::isfinite(config_.keyword_similarity_identity_bound) ||
        config_.keyword_similarity_zero_bound < 0.0 ||
        config_.keyword_similarity_identity_bound > 1.0 ||
        config_.keyword_similarity_zero_bound >=
            config_.keyword_similarity_identity_bound) {
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
        item.relevance = keywordRelevance(
            claim.keywords, evidence,
            config_.keyword_similarity_zero_bound,
            config_.keyword_similarity_identity_bound);
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
