#include "multi_dimensional_qualifier.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace chongming::qualifier {
namespace {

double clamp01(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

template <typename Range, typename Projection>
double averageOr(const Range& values, double fallback, Projection projection) {
    // 每一种空集合的业务默认值由调用处显式给出，避免隐藏除零行为。
    if (values.empty()) {
        return fallback;
    }
    double sum = 0.0;
    for (const auto& value : values) {
        sum += clamp01(projection(value).value_or(fallback));
    }
    return sum / static_cast<double>(values.size());
}

CredibilityLabel labelFor(double score) {
    // 区间边界严格遵循 README 第五步的左闭右开定义。
    if (score >= 0.85) {
        return CredibilityLabel::ConfirmedTrue;
    }
    if (score >= 0.55) {
        return CredibilityLabel::MostlyTrueWithFlaws;
    }
    if (score >= 0.30) {
        return CredibilityLabel::InsufficientInformation;
    }
    return CredibilityLabel::ConfirmedFalse;
}

}  // namespace

MultiDimensionalQualifier::MultiDimensionalQualifier(
    MultiDimensionalConfig config)
    : config_(config) {
    const double weights[] = {config_.content_weight, config_.logic_weight,
                              config_.sufficiency_weight,
                              config_.source_quality_weight};
    double sum = 0.0;
    for (double weight : weights) {
        if (!std::isfinite(weight) || weight < 0.0) {
            throw std::invalid_argument("multi-dimensional weights must be non-negative");
        }
        sum += weight;
    }
    if (std::abs(sum - 1.0) > 1e-9) {
        throw std::invalid_argument("multi-dimensional weights must sum to one");
    }
}

MultiDimensionalEvaluation MultiDimensionalQualifier::evaluate(
    const std::vector<ClaimEvaluation>& claims,
    const std::vector<LogicEvaluation>& relations,
    const std::vector<JuryEvaluation>& jury_results) const {
    MultiDimensionalEvaluation result;
    // 没有声明或逻辑关系时保持中性 0.5；没有声明时充分性为 0。
    result.dimensions.content = averageOr(
        claims, 0.5, [](const ClaimEvaluation& value) { return value.content_score; });
    result.dimensions.logic = averageOr(
        relations, 0.5, [](const LogicEvaluation& value) { return value.score; });
    result.dimensions.sufficiency = averageOr(
        claims, 0.0,
        [](const ClaimEvaluation& value) { return value.sufficiency_score; });

    double reliability_sum = 0.0;
    double independence_sum = 0.0;
    std::size_t evidence_count = 0;
    // evaluate 已经开始执行；即使最终没有证据，这两个质量维度也应定义为 0。
    result.dimensions.source = 0.0;
    result.dimensions.independence = 0.0;
    for (const auto& claim : claims) {
        if (claim.status == ClaimStatus::InsufficientInformation) {
            result.has_insufficient_claims = true;
        }
        for (const auto& evidence : claim.evidence) {
            reliability_sum += clamp01(evidence.reliability.value_or(0.0));
            independence_sum +=
                clamp01(evidence.independence_discount.value_or(0.0));
            ++evidence_count;
        }
    }
    if (evidence_count > 0) {
        // independence 的公式等价于所有 delta_ij 的算术平均。
        result.dimensions.source =
            reliability_sum / static_cast<double>(evidence_count);
        result.dimensions.independence =
            independence_sum / static_cast<double>(evidence_count);
    }

    // 缺失或零票重的陪审团结果贡献零一致性。这是对 README 中 Z_i=0
    // 待确认项的保守处理，也确保此类声明会提高最终不确定性。
    // 没有声明或没有可用陪审团结果时，一致性质量定义为 0。
    result.dimensions.consensus = 0.0;
    if (!claims.empty()) {
        double consensus_sum = 0.0;
        const std::size_t paired = std::min(claims.size(), jury_results.size());
        for (std::size_t i = 0; i < paired; ++i) {
            if (jury_results[i].has_valid_votes) {
                consensus_sum +=
                    clamp01(jury_results[i].consensus_score.value_or(0.0));
            }
        }
        result.dimensions.consensus =
            consensus_sum / static_cast<double>(claims.size());
    }
    result.dimensions.uncertainty = clamp01(
        1.0 - result.dimensions.sufficiency.value_or(0.0) *
                  result.dimensions.consensus.value_or(0.0));

    // 来源质量采用可靠性与独立性的乘法耦合，防止同源转载虚增总分。
    result.mc_score = clamp01(
        config_.content_weight * result.dimensions.content.value_or(0.5) +
        config_.logic_weight * result.dimensions.logic.value_or(0.5) +
        config_.sufficiency_weight *
            result.dimensions.sufficiency.value_or(0.0) +
        config_.source_quality_weight *
            result.dimensions.source.value_or(0.0) *
            result.dimensions.independence.value_or(0.0));
    result.label = labelFor(result.mc_score.value_or(0.0));
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
