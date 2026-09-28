#include "jury.h"

#include <algorithm>
#include <cmath>

namespace chongming::qualifier {
namespace {

double clamp01(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

std::size_t verdictIndex(JuryVerdict verdict) {
    switch (verdict) {
        case JuryVerdict::Support:
            return 0;
        case JuryVerdict::Refute:
            return 1;
        case JuryVerdict::Uncertain:
            return 2;
    }
    return 2;
}

}  // namespace

JuryEvaluation Jury::deliberate(
    const std::vector<JurorOpinion>& opinions) const {
    JuryEvaluation result;
    std::array<double, 3> votes{{0.0, 0.0, 0.0}};
    for (const auto& opinion : opinions) {
        const double confidence = clamp01(opinion.confidence);
        // trust_p = 0.4*s_hybrid + 0.4*c_p + 0.2*I_no_error。
        const double trust = 0.4 * clamp01(opinion.hybrid_retrieval_score) +
                             0.4 * confidence +
                             0.2 * (opinion.completed_without_error ? 1.0 : 0.0);
        votes[verdictIndex(opinion.verdict)] += trust * confidence;
    }

    const double total = votes[0] + votes[1] + votes[2];
    if (total <= 0.0) {
        // Z_i=0 时各评分保持 nullopt，并通过标志位交由后续安全策略处理。
        return result;
    }
    result.has_valid_votes = true;
    std::array<double, 3> distribution{{0.0, 0.0, 0.0}};
    for (std::size_t i = 0; i < votes.size(); ++i) {
        distribution[i] = votes[i] / total;
    }
    result.vote_distribution = distribution;

    double entropy = 0.0;
    for (double probability : distribution) {
        if (probability > 0.0) {
            entropy -= probability * std::log2(probability);
        }
    }
    // 以 log2(3) 归一化，使一致性严格落在 [0, 1]。
    result.consensus_score =
        clamp01(1.0 - entropy / std::log2(3.0));
    result.uncertain_vote_share = distribution[2];
    // 效用权重：支持=1、反驳=0、不确定=0.5。
    const double utility = distribution[0] + 0.5 * distribution[2];
    result.jury_score =
        clamp01(result.consensus_score.value_or(0.0) * utility);
    return result;
}

}  // namespace chongming::qualifier
