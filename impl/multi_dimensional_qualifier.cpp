#include "multi_dimensional_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace chongming::qualifier {
namespace {

double requireProbability(double value, const char* field_name) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument(std::string(field_name) +
                                    " must be finite and within [0, 1]");
    }
    return value;
}

std::optional<double> checkedProbability(const std::optional<double>& value,
                                         const char* field_name) {
    if (!value) return std::nullopt;
    return requireProbability(*value, field_name);
}

/** 对可用值求平均；没有可用值时保留 nullopt，而不是伪造中性分。 */
template <typename Range, typename Projection>
std::optional<double> averageAvailable(const Range& values,
                                       Projection projection) {
    double sum = 0.0;
    std::size_t count = 0;
    for (const auto& value : values) {
        const std::optional<double> projected = projection(value);
        if (!projected) continue;
        sum += *projected;
        ++count;
    }
    if (count == 0) return std::nullopt;
    return sum / static_cast<double>(count);
}

/** 按优化规范计算一个 claim 的来源质量与来源独立性。 */
std::pair<double, double> sourceDimensions(const ClaimEvaluation& claim) {
    double source = 0.0;
    if (!claim.evidence_clusters.empty()) {
        for (const auto& cluster : claim.evidence_clusters) {
            const double support =
                requireProbability(cluster.support, "cluster.support");
            const double refute =
                requireProbability(cluster.refute, "cluster.refute");
            const double neutral =
                requireProbability(cluster.neutral, "cluster.neutral");
            source += std::max({support, refute, neutral});
        }
        source /= static_cast<double>(claim.evidence_clusters.size());
    }

    // evidence_id 在 JSON 层已要求同一 claim 内唯一；这里仍显式去重，保证
    // 直接使用 C++ API 构造输入时也符合 E_i^unique 的定义。
    std::unordered_set<std::string> unique_evidence_ids;
    for (const auto& evidence : claim.evidence) {
        unique_evidence_ids.insert(evidence.evidence_id);
    }

    double independence = 0.0;
    if (!unique_evidence_ids.empty()) {
        independence = static_cast<double>(claim.evidence_clusters.size()) /
                       static_cast<double>(unique_evidence_ids.size());
        independence = std::clamp(independence, 0.0, 1.0);
    }
    return {source, independence};
}

}  // namespace

MultiDimensionalQualifier::MultiDimensionalQualifier(
    MultiDimensionalConfig config)
    : config_(config) {
    const double weights[] = {
        config_.content_weight, config_.logic_weight,
        config_.sufficiency_weight, config_.jury_weight,
        config_.source_weight};
    double sum = 0.0;
    for (double weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
            throw std::invalid_argument(
                "multi-dimensional weights must be finite and within [0, 1]");
        }
        sum += weight;
    }
    if (std::abs(sum - 1.0) > 1e-9) {
        throw std::invalid_argument(
            "multi-dimensional weights must sum to one");
    }
    if (config_.logic_weight > 0.8) {
        throw std::invalid_argument(
            "logic_weight must not exceed 0.8");
    }
}

MultiDimensionalEvaluation MultiDimensionalQualifier::evaluate(
    const std::vector<ClaimEvaluation>& claims,
    const std::vector<ClaimLogicEvaluation>& claim_logic_results,
    const std::vector<JuryEvaluation>& jury_results) const {
    MultiDimensionalEvaluation result;

    // 第三层按 claim_index 关联结果；重复下标通常表示调用方错误，不能静默覆盖。
    std::vector<const ClaimLogicEvaluation*> logic_by_claim(claims.size(), nullptr);
    for (const auto& logic : claim_logic_results) {
        if (logic.claim_index >= claims.size()) {
            throw std::invalid_argument(
                "claim logic result references an invalid claim index");
        }
        if (logic_by_claim[logic.claim_index] != nullptr) {
            throw std::invalid_argument(
                "claim logic results contain a duplicate claim index");
        }
        logic_by_claim[logic.claim_index] = &logic;
    }

    result.claims.reserve(claims.size());
    for (std::size_t index = 0; index < claims.size(); ++index) {
        const ClaimEvaluation& claim = claims[index];
        ClaimMultiDimensionalEvaluation item;
        item.claim_id = claim.claim_id;
        item.dimensions.content =
            checkedProbability(claim.content_score, "claim.content_score");
        item.dimensions.sufficiency = checkedProbability(
            claim.sufficiency_score, "claim.sufficiency_score");

        const auto [source, independence] = sourceDimensions(claim);
        item.dimensions.source = source;
        item.dimensions.independence = independence;

        const ClaimLogicEvaluation* logic = logic_by_claim[index];
        if (logic && logic->logic_available) {
            if (!logic->score) {
                throw std::invalid_argument(
                    "an available claim logic result requires a score");
            }
            item.dimensions.logic =
                requireProbability(*logic->score, "claim_logic.score");
            item.logic_available = true;
        }

        // 第四层规定：没有有效投票时 jury=0.5、consensus=0，并保留缺失标志。
        item.dimensions.jury = 0.5;
        item.dimensions.consensus = 0.0;
        item.jury_missing = true;
        if (index < jury_results.size()) {
            const JuryEvaluation& jury = jury_results[index];
            item.jury_missing = jury.jury_missing;
            if (jury.jury_score) {
                item.dimensions.jury =
                    requireProbability(*jury.jury_score, "jury.jury_score");
            }
            if (jury.consensus_score) {
                item.dimensions.consensus = requireProbability(
                    *jury.consensus_score, "jury.consensus_score");
            }
        }

        if (item.dimensions.sufficiency && item.dimensions.consensus) {
            item.dimensions.uncertainty =
                1.0 - *item.dimensions.sufficiency *
                          *item.dimensions.consensus;
        }

        // MC_i 的必要方向和质量输入齐全后才计算；optional 缺失表示前层尚未运行。
        if (item.dimensions.content && item.dimensions.sufficiency &&
            item.dimensions.source && item.dimensions.independence &&
            item.dimensions.jury) {
            double numerator =
                config_.content_weight * *item.dimensions.content +
                config_.sufficiency_weight * *item.dimensions.sufficiency +
                config_.jury_weight * *item.dimensions.jury +
                config_.source_weight * *item.dimensions.source *
                    *item.dimensions.independence;
            double denominator =
                config_.content_weight + config_.sufficiency_weight +
                config_.jury_weight + config_.source_weight;

            // a_i=0 时同时删除逻辑分子和逻辑权重，不能把缺失逻辑当成 0.5。
            if (item.logic_available) {
                numerator += config_.logic_weight * *item.dimensions.logic;
                denominator += config_.logic_weight;
            }
            if (denominator <= 0.0) {
                throw std::logic_error(
                    "multi-dimensional denominator must be positive");
            }
            item.mc_score = std::clamp(numerator / denominator, 0.0, 1.0);
        }

        if (claim.status == ClaimStatus::InsufficientInformation) {
            result.has_insufficient_claims = true;
        }
        result.claims.push_back(std::move(item));
    }

    // 顶层字段是逐声明结果的事件级算术平均，便于保留新闻整体输出接口。
    result.dimensions.content = averageAvailable(
        result.claims, [](const auto& value) { return value.dimensions.content; });
    result.dimensions.logic = averageAvailable(
        result.claims, [](const auto& value) { return value.dimensions.logic; });
    result.dimensions.sufficiency = averageAvailable(
        result.claims,
        [](const auto& value) { return value.dimensions.sufficiency; });
    result.dimensions.source = averageAvailable(
        result.claims, [](const auto& value) { return value.dimensions.source; });
    result.dimensions.independence = averageAvailable(
        result.claims,
        [](const auto& value) { return value.dimensions.independence; });
    result.dimensions.jury = averageAvailable(
        result.claims, [](const auto& value) { return value.dimensions.jury; });
    result.dimensions.consensus = averageAvailable(
        result.claims,
        [](const auto& value) { return value.dimensions.consensus; });
    result.dimensions.uncertainty = averageAvailable(
        result.claims,
        [](const auto& value) { return value.dimensions.uncertainty; });

    // 任何 claim 未完成 MC_i 时，整体 MC 也保持未定义，避免部分平均掩盖缺失。
    if (!result.claims.empty()) {
        bool all_available = true;
        double sum = 0.0;
        for (const auto& item : result.claims) {
            if (!item.mc_score) {
                all_available = false;
                break;
            }
            sum += *item.mc_score;
        }
        if (all_available) {
            result.mc_score = sum / static_cast<double>(result.claims.size());
        }
    }
    return result;
}

const char* toString(CredibilityLabel label) noexcept {
    switch (label) {
        case CredibilityLabel::ConfirmedTrue:
            return "确认为真";
        case CredibilityLabel::MostlyTrueWithFlaws:
            return "大体为真但有瑕疵";
        case CredibilityLabel::InsufficientInformation:
            return "信息不足";
        case CredibilityLabel::ConfirmedFalse:
            return "确认为假";
    }
    return "未知";
}

}  // namespace chongming::qualifier
