#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
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

/** 原子声明拆分规则规定的六类声明。 */
enum class ClaimType { Numeric, Temporal, Identity, Location, Existence, Causal };

/**
 * 事件或原子声明的七要素。
 *
 * optional 为空表示拆分规则中的 JSON null；它与“字段未提供”是两种不同情况，
 * JsonReader 仍要求七个键完整出现。
 */
struct SevenElements {
    std::optional<std::string> subject;
    std::optional<std::string> action;
    std::optional<std::string> object;
    std::optional<std::string> time;
    std::optional<std::string> location;
    std::optional<std::string> quantity;
    std::optional<std::string> result;
};

/** README 第三步中由 NLI 或因果模型评分的声明间语义关系。 */
enum class RelationType {
    Temporal,
    Conditional,
    Contrast,
    Parallel,
    Causal
};

/** README 第三步中直接按声明分数计算的逻辑聚合运算。 */
enum class AggregationType { And, Or, Not, Implication, Causal };

/** 陪审团智能体可提交的三种裁决。 */
enum class JuryVerdict { Support, Refute, Uncertain };

/** 第一版 sandbox 约定的四种陪审员审查角色。 */
enum class JurorRole {
    StrictLegal,
    OpenWebPragmatist,
    CausalSkeptic,
    AntiManipulation
};

/** MCScore 映射得到的四级可信度标签。 */
enum class CredibilityLabel {
    ConfirmedTrue,
    MostlyTrueWithFlaws,
    InsufficientInformation,
    ConfirmedFalse
};

/**
 * 预处理 Agent 为一组证据关键词和声明关键词给出的方向敏感可替换相似度。
 *
 * similarity 只衡量两个短语在当前事实语境中能否保持方向不变地替换，
 * 不表示证据支持或反驳声明；证据方向仍由 Evidence::nli_label 表示。
 */
struct KeywordMatch {
    std::string evidence_keyword;
    std::string claim_keyword;
    double similarity{0.0};
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
    // 允许缺省以兼容旧输入；缺省时第一层仍执行完全字符串匹配。
    std::vector<KeywordMatch> keyword_matches;
    std::optional<std::int64_t> event_day;
    NliLabel nli_label{NliLabel::Neutral};
    double nli_confidence{0.0};
};

/** 待评分的原子声明及其候选证据集合。 */
struct AtomicClaim {
    std::string id;
    std::string text;
    SevenElements seven_elements;
    std::string check_question;
    ClaimType claim_type{ClaimType::Existence};
    std::vector<std::string> entities;
    std::vector<std::string> canon_refs;
    std::unordered_set<std::string> keywords;
    std::optional<std::int64_t> event_day;
    bool time_sensitive{false};
    std::vector<Evidence> evidence;
};

/** 逻辑边由预处理 agent 补齐的标准维度名。 */
enum class RelationDimension {
    Time,
    Mechanism,
    Counterfactual,
    Confounder,
    DoseResponse,
    Contrast,
    Coordinate
};

/**
 * 非因果关系维度的 agent 评分。因果边的五维评分仍存放在
 * CausalFactorScores 中，以便现有第三层直接计算。
 */
struct RelationDimensionScores {
    std::optional<double> temporal_order;
    std::optional<double> mechanism;
    std::optional<double> counterfactual_support;
    std::optional<double> contrast;
    std::optional<double> coordinate;
};

/**
 * 第一、二步对单条证据计算出的可审计中间量。
 * reliability = base_reliability * relevance * timeliness。
 */
struct EvidenceAssessment {
    std::string evidence_id;
    // 转载或同源证据共享同一个簇 ID；后续按簇去重和取最大值。
    std::string cluster_id;
    std::optional<double> base_reliability;
    std::optional<double> relevance;
    std::optional<double> timeliness;
    std::optional<double> reliability;
    std::optional<double> independence_discount;
    NliLabel nli_label{NliLabel::Neutral};
    double nli_confidence{0.0};
};

/**
 * 一个独立证据簇对声明的最强支持、反驳和中立有效量。
 * 同一簇内同方向的转载只保留最大值，不随转载条数线性累加。
 */
struct EvidenceClusterAssessment {
    std::string cluster_id;
    double support{0.0};
    double refute{0.0};
    double neutral{0.0};
};

/** 第一、二步对一条原子声明的完整评分结果。 */
struct ClaimEvaluation {
    std::string claim_id;
    std::optional<double> support_strength;
    std::optional<double> refute_strength;
    // 支持与反驳的带符号净方向，合法区间为 [-1, 1]。
    std::optional<double> direction;
    std::optional<double> content_score;
    std::optional<double> sufficiency_score;
    ClaimStatus status{ClaimStatus::InsufficientInformation};
    std::vector<EvidenceAssessment> evidence;
    std::vector<EvidenceClusterAssessment> evidence_clusters;
};

/**
 * 一条待检查的声明间语义关系。
 *
 * left_claim/right_claim 是 ClaimEvaluation 数组下标，不是业务 claim id。
 * 时间、条件、转折、并列和因果关系都使用 nli_score。因果系数属于
 * AggregationType::Causal 的聚合输入，不在语义关系输入中重复保存。
 */
struct RelationInput {
    std::size_t left_claim{0};
    std::size_t right_claim{0};
    RelationType type{RelationType::Temporal};
    double nli_score{0.0};
    bool parsing_succeeded{true};
    // 以下字段保留预处理协议的可审计信息；放在末尾以兼容旧聚合初始化顺序。
    std::string edge_id;
    std::vector<RelationDimension> required_dimensions;
    RelationDimensionScores dimension_scores;
};

/**
 * 因果逻辑边的五维评分输入。
 *
 * 五个 optional 字段分别对应优化规范中的 S_time、S_mech、S_counter、
 * S_confound 和 S_dose。nullopt 表示该维度尚未计算，不能按数值 0 处理。
 */
struct CausalFactorScores {
    std::optional<double> temporal_order;
    std::optional<double> mechanism;
    std::optional<double> counterfactual_support;
    std::optional<double> confounder_control;
    std::optional<double> dose_response;
};

/**
 * 一条待计算的逻辑聚合运算。
 *
 * Not 是一元运算，只读取 left_claim；其余运算读取左右两个声明下标。
 * Causal 使用外部输入的 p_NLI 和五个因果子维度计算逻辑边分数：
 * L_ij = clip(p_NLI) * sum(w_k * S_ijk)。
 */
struct AggregationInput {
    std::size_t left_claim{0};
    std::size_t right_claim{0};
    AggregationType type{AggregationType::And};
    std::optional<double> nli_score;
    CausalFactorScores causal_factors;
    bool parsing_succeeded{true};
    // 以下字段保留预处理协议的可审计信息；放在末尾以兼容旧聚合初始化顺序。
    std::string edge_id;
    std::vector<RelationDimension> required_dimensions;
};

/** 语义关系和逻辑聚合统一进入第三层评分，但保留各自独立的数据类型。 */
using LogicInput = std::variant<RelationInput, AggregationInput>;

/**
 * 第三步的单条逻辑边评分结果。
 * logic_available=false 表示该边没有产生规范定义的逻辑分数；此时第五层
 * 必须删除逻辑项，而不能把缺失值当作 0 或 0.5。
 */
struct LogicEvaluation {
    LogicInput input;
    std::optional<double> score;
    bool logic_available{false};
};

/** 第三层按 claim 汇总关联逻辑边后的结果。 */
struct ClaimLogicEvaluation {
    std::size_t claim_index{0};
    std::optional<double> score;
    bool logic_available{false};
    std::size_t contributing_edge_count{0};
};

/**
 * 单个陪审团智能体的意见。
 * completed_without_error=false 不会丢弃意见，但会降低该智能体的信任权重。
 */
struct JurorOpinion {
    // 以下审计字段作为一个可选字段组出现；不参与第四层数值公式。
    std::optional<std::string> agent_id;
    std::optional<JurorRole> agent_role;
    std::optional<std::string> model_id;
    std::optional<std::string> prompt_version;
    JuryVerdict verdict{JuryVerdict::Uncertain};
    double hybrid_retrieval_score{0.0};
    double confidence{0.0};
    bool completed_without_error{false};
};

/**
 * 第四步陪审团融合结果。
 * vote_distribution 的顺序固定为 Support、Refute、Uncertain。
 * 当总有效票重为零时，direction=0、consensus_score=0、jury_score=0.5，
 * 同时使用 jury_missing=true 明确记录投票缺失。
 */
struct JuryEvaluation {
    bool has_valid_votes{false};
    // 无投票或所有票权为零时为 true；不能与“已计算且得分为 0.5”混淆。
    bool jury_missing{true};
    std::optional<std::array<double, 3>> vote_distribution;
    // 支持为正、反驳为负、未知为零的带符号净方向，区间为 [-1, 1]。
    std::optional<double> direction;
    std::optional<double> consensus_score;
    std::optional<double> jury_score;
    std::optional<double> uncertain_vote_share;
};

/**
 * 优化规范第五层涉及的八个可解释维度。
 * 默认构造时均未定义；没有逻辑边时 logic 保持 nullopt，其余字段在对应
 * 前置层已经运行后由 MultiDimensionalQualifier::evaluate 赋值。
 */
struct DimensionScores {
    std::optional<double> content;
    std::optional<double> logic;
    std::optional<double> sufficiency;
    std::optional<double> source;
    std::optional<double> independence;
    std::optional<double> jury;
    std::optional<double> consensus;
    std::optional<double> uncertainty;
};

/** 第五层对单条原子声明计算出的可审计结果。 */
struct ClaimMultiDimensionalEvaluation {
    std::string claim_id;
    DimensionScores dimensions;
    bool logic_available{false};
    bool jury_missing{true};
    // 新第五层在 [-1,1] 上计算的可审计方向量。
    std::optional<double> content_direction;
    std::optional<double> content_logic_direction;
    std::optional<double> evidence_quality;
    std::optional<double> evidence_direction;
    std::optional<double> jury_direction;
    std::optional<double> final_direction;
    std::optional<double> mc_score;
};

/**
 * 第五层结果。
 *
 * claims 保存规范定义的逐声明 MC_i；顶层 dimensions 和 mc_score 是各声明
 * 可用结果的算术平均，供新闻事件级展示使用。四类标签由第六层概率映射产生。
 */
struct MultiDimensionalEvaluation {
    std::vector<ClaimMultiDimensionalEvaluation> claims;
    DimensionScores dimensions;
    std::optional<double> mc_score;
    bool has_insufficient_claims{false};
};

/** 第六层校准后的四类概率；四项均有值时总和应为 1。 */
struct FourClassProbabilities {
    std::optional<double> confirmed_true;
    std::optional<double> mostly_true;
    std::optional<double> insufficient_information;
    std::optional<double> confirmed_false;
};

/** 第六层四类概率输出、可审计中间量及其最大概率标签。 */
struct ProbabilityEvaluation {
    FourClassProbabilities uncalibrated_probabilities;
    FourClassProbabilities probabilities;
    std::optional<double> mc_direction;
    std::optional<double> positive_mass;
    std::optional<double> negative_mass;
    std::optional<double> uncertainty_mass;
    std::optional<double> truth_completeness;
    std::optional<CredibilityLabel> predicted_label;
};

}  // namespace chongming::qualifier
