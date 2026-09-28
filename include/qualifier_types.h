#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

// 本文件只定义评分流水线共享的数据模型，不包含任何计算逻辑。
// 外部系统（例如 ChongMing Electron 应用）的 JSON 字段应在适配层转换为这些类型。
// 结果结构中的 optional 分数为 nullopt 表示“尚未计算”，不是数值零。
namespace chongming::qualifier {

/** 证据来源类型；用于查询来源可靠性基线。 */
enum class SourceType {
    OfficialRegistry,
    Academic,
    OpenWeb,
    SocialMedia,
    NewsMedia,
    VideoOrGitHub,
    MarketData,
};

/** NLI 模型对“证据是否支持声明”的三分类输出。 */
enum class NliLabel { Support, Refute, Neutral };

/** 单条声明是否已拥有足够证据进入确定性判断。 */
enum class ClaimStatus { Determinate, InsufficientInformation };

/** README 第三步支持的逻辑关系类型。 */
enum class RelationType {
    Temporal,
    Conditional,
    Contrast,
    Parallel,
    And,
    Or,
    Not,
    Implication,
    Causal
};

/** 陪审团智能体可提交的三种裁决。 */
enum class JuryVerdict { Support, Refute, Uncertain };

/** MCScore 映射得到的四级可信度标签。 */
enum class CredibilityLabel {
    ConfirmedTrue,
    MostlyTrueWithFlaws,
    InsufficientInformation,
    ConfirmedFalse
};

/**
 * 一条与原子声明相关的证据。
 *
 * event_day 使用调用方自选纪元的“整日序号”；只要声明和证据使用相同纪元即可。
 * nli_confidence 的合法区间为 [0, 1]，实现会对越界输入做裁剪。
 */
struct Evidence {
    std::string id;
    std::string original_source_id;
    // 未填写来源时保持 nullopt，不能默认成某个真实来源类型，否则会错误获得其基线分。
    std::optional<SourceType> source_type;
    std::unordered_set<std::string> keywords;
    std::optional<std::int64_t> event_day;
    NliLabel nli_label{NliLabel::Neutral};
    double nli_confidence{0.0};
};

/** 待评分的原子声明及其候选证据集合。 */
struct AtomicClaim {
    std::string id;
    std::string text;
    std::unordered_set<std::string> keywords;
    std::optional<std::int64_t> event_day;
    bool time_sensitive{false};
    std::vector<Evidence> evidence;
};

/**
 * 第一、二步对单条证据计算出的可审计中间量。
 * reliability = base_reliability * relevance * timeliness。
 */
struct EvidenceAssessment {
    std::string evidence_id;
    std::optional<double> base_reliability;
    std::optional<double> relevance;
    std::optional<double> timeliness;
    std::optional<double> reliability;
    std::optional<double> independence_discount;
    bool filtered_by_relevance{false};
    NliLabel nli_label{NliLabel::Neutral};
    double nli_confidence{0.0};
};

/** 第一、二步对一条原子声明的完整评分结果。 */
struct ClaimEvaluation {
    std::string claim_id;
    std::optional<double> support_strength;
    std::optional<double> refute_strength;
    std::optional<double> content_score;
    std::optional<double> sufficiency_score;
    ClaimStatus status{ClaimStatus::InsufficientInformation};
    std::vector<EvidenceAssessment> evidence;
};

/**
 * 一条待检查的声明关系。
 *
 * left_claim/right_claim 是 ClaimEvaluation 数组下标，不是业务 claim id。
 * Not 关系只读取 left_claim；普通关系使用 nli_score；因果关系额外使用
 * causal_probability 和 counterfactual_probability。
 */
struct RelationInput {
    std::size_t left_claim{0};
    std::size_t right_claim{0};
    RelationType type{RelationType::Temporal};
    double nli_score{0.0};
    double causal_probability{0.0};
    double counterfactual_probability{0.0};
    bool parsing_succeeded{true};
};

/** 第三步的逻辑评分结果；used_fallback 表示是否采用原子声明多数投票。 */
struct LogicEvaluation {
    RelationInput relation;
    std::optional<double> score;
    bool used_fallback{false};
};

/**
 * 单个陪审团智能体的意见。
 * completed_without_error=false 不会丢弃意见，但会降低该智能体的信任权重。
 */
struct JurorOpinion {
    JuryVerdict verdict{JuryVerdict::Uncertain};
    double hybrid_retrieval_score{0.0};
    double confidence{0.0};
    bool completed_without_error{false};
};

/**
 * 第四步陪审团融合结果。
 * vote_distribution 的顺序固定为 Support、Refute、Uncertain。
 * 当总有效票重为零时，四个 optional 结果均保持 nullopt。
 */
struct JuryEvaluation {
    bool has_valid_votes{false};
    std::optional<std::array<double, 3>> vote_distribution;
    std::optional<double> consensus_score;
    std::optional<double> jury_score;
    std::optional<double> uncertain_vote_share;
};

/**
 * README 第五步要求输出的七个可解释维度。
 * 默认构造时均未定义；MultiDimensionalQualifier::evaluate 执行后均有值。
 */
struct DimensionScores {
    std::optional<double> content;
    std::optional<double> logic;
    std::optional<double> sufficiency;
    std::optional<double> source;
    std::optional<double> independence;
    std::optional<double> consensus;
    std::optional<double> uncertainty;
};

/** 第五步的综合评分、标签及证据不足提示。 */
struct MultiDimensionalEvaluation {
    DimensionScores dimensions;
    std::optional<double> mc_score;
    std::optional<CredibilityLabel> label;
    bool has_insufficient_claims{false};
};

}  // namespace chongming::qualifier
