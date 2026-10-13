#include "jury.h"

#include <algorithm>
#include <cmath>

namespace chongming::qualifier {
namespace {

constexpr double kEpsilon = 1e-8;

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
        // 规范要求零票重使用中性真实性方向，但明确标记为缺失。
        result.direction = 0.0;
        result.consensus_score = 0.0;
        result.jury_score = 0.5;
        return result;
    }
    result.has_valid_votes = true;
    result.jury_missing = false;
    std::array<double, 3> distribution{{0.0, 0.0, 0.0}};
    for (std::size_t i = 0; i < votes.size(); ++i) {
        distribution[i] = votes[i] / total;
    }
    result.vote_distribution = distribution;
    // D_jury=(V_support-V_refute)/(V_total+epsilon)。未知票只进入分母，
    // 因此会稀释净方向，但不会把真实性向支持或反驳方向推动。
    result.direction = std::clamp(
        (votes[0] - votes[1]) / (total + kEpsilon), -1.0, 1.0);

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
    // S_jury=0.5+0.5*D_jury*C。反驳票会降低分数；全未知时 D=0，
    // 无论未知票内部多一致，真实性分数始终保持中性 0.5。
    result.jury_score = clamp01(
        0.5 + 0.5 * result.direction.value() *
                  result.consensus_score.value());
    return result;
}

}  // namespace chongming::qualifier
