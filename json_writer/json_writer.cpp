#include "json_writer.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace chongming::qualifier {
namespace {

/**
 * 很小的流式 JSON 生成器。
 *
 * 项目当前没有第三方 JSON 依赖，因此这里与 JsonReader 一样只实现协议所需
 * 功能。生成器集中处理逗号、缩进、字符串转义和有限数字检查，业务写出函数
 * 不直接拼接 JSON 文本。
 */
class Emitter {
public:
    Emitter(std::ostream& output, bool pretty)
        : output_(output), pretty_(pretty) {}

    void beginObject() {
        beforeValue();
        output_ << '{';
        stack_.push_back(Frame{Kind::Object});
    }

    void endObject() {
        requireFrame(Kind::Object, "object");
        const bool had_values = !stack_.back().first;
        stack_.pop_back();
        if (had_values) newlineAndIndent();
        output_ << '}';
    }

    void beginArray() {
        beforeValue();
        output_ << '[';
        stack_.push_back(Frame{Kind::Array});
    }

    void endArray() {
        requireFrame(Kind::Array, "array");
        const bool had_values = !stack_.back().first;
        stack_.pop_back();
        if (had_values) newlineAndIndent();
        output_ << ']';
    }

    void key(const std::string& name) {
        requireFrame(Kind::Object, "object key");
        Frame& frame = stack_.back();
        if (frame.awaiting_value) {
            throw JsonWriteError("a JSON object key has no value");
        }
        if (!frame.first) output_ << ',';
        newlineAndIndent();
        frame.first = false;
        writeEscaped(name);
        output_ << (pretty_ ? ": " : ":");
        frame.awaiting_value = true;
    }

    void nullValue() {
        beforeValue();
        output_ << "null";
    }

    void value(bool value) {
        beforeValue();
        output_ << (value ? "true" : "false");
    }

    void value(std::int64_t value) {
        beforeValue();
        output_ << value;
    }

    void value(std::size_t value) {
        beforeValue();
        output_ << value;
    }

    void value(double value) {
        if (!std::isfinite(value)) {
            throw JsonWriteError("JSON numbers must be finite");
        }
        beforeValue();
        // max_digits10 保证 double 往返解析时不丢失精度。
        output_ << std::setprecision(std::numeric_limits<double>::max_digits10)
                << value;
    }

    void value(const std::string& value) {
        beforeValue();
        writeEscaped(value);
    }

    /** 防止 const char* 在重载决议中被优先转换成 bool。 */
    void value(const char* value) {
        if (value == nullptr) {
            throw JsonWriteError("a JSON string pointer must not be null");
        }
        beforeValue();
        writeEscaped(value);
    }

    void finish() {
        if (!stack_.empty()) {
            throw JsonWriteError("internal error: unclosed JSON container");
        }
        if (pretty_) output_ << '\n';
        if (!output_) throw JsonWriteError("failed while writing JSON output");
    }

private:
    enum class Kind { Object, Array };
    struct Frame {
        explicit Frame(Kind frame_kind) : kind(frame_kind) {}
        Kind kind;
        bool first{true};
        bool awaiting_value{false};
    };

    std::ostream& output_;
    bool pretty_{true};
    bool root_written_{false};
    std::vector<Frame> stack_;

    void requireFrame(Kind kind, const char* operation) const {
        if (stack_.empty() || stack_.back().kind != kind) {
            throw JsonWriteError(std::string("internal JSON state error at ") +
                                 operation);
        }
        if (stack_.back().awaiting_value && kind == Kind::Object) {
            throw JsonWriteError("a JSON object key has no value");
        }
    }

    void beforeValue() {
        if (stack_.empty()) {
            if (root_written_) {
                throw JsonWriteError("only one JSON root value may be written");
            }
            root_written_ = true;
            return;
        }

        Frame& frame = stack_.back();
        if (frame.kind == Kind::Object) {
            if (!frame.awaiting_value) {
                throw JsonWriteError("a JSON object value requires a key");
            }
            frame.awaiting_value = false;
            return;
        }

        if (!frame.first) output_ << ',';
        newlineAndIndent();
        frame.first = false;
    }

    void newlineAndIndent() {
        if (!pretty_) return;
        output_ << '\n';
        for (std::size_t index = 0; index < stack_.size(); ++index) {
            output_ << "  ";
        }
    }

    void writeEscaped(const std::string& value) {
        output_ << '"';
        static constexpr char hex[] = "0123456789abcdef";
        for (unsigned char character : value) {
            switch (character) {
                case '"': output_ << "\\\""; break;
                case '\\': output_ << "\\\\"; break;
                case '\b': output_ << "\\b"; break;
                case '\f': output_ << "\\f"; break;
                case '\n': output_ << "\\n"; break;
                case '\r': output_ << "\\r"; break;
                case '\t': output_ << "\\t"; break;
                default:
                    if (character < 0x20U) {
                        output_ << "\\u00" << hex[(character >> 4U) & 0x0FU]
                                << hex[character & 0x0FU];
                    } else {
                        // 输入字符串按 UTF-8 保存；非 ASCII 字节原样写出。
                        output_ << static_cast<char>(character);
                    }
            }
        }
        output_ << '"';
    }
};

template <typename T>
void writeOptional(Emitter& json, const std::optional<T>& value) {
    if (value) {
        json.value(*value);
    } else {
        json.nullValue();
    }
}

const char* sourceTypeName(SourceType value) {
    switch (value) {
        case SourceType::OfficialRegistry: return "official_registry";
        case SourceType::Academic: return "academic";
        case SourceType::OpenWeb: return "open_web";
        case SourceType::SocialMedia: return "social_media";
        case SourceType::NewsMedia: return "news_media";
        case SourceType::VideoOrGitHub: return "video_or_github";
        case SourceType::MarketData: return "market_data";
    }
    throw JsonWriteError("unsupported source type");
}

const char* nliLabelName(NliLabel value) {
    switch (value) {
        case NliLabel::Support: return "support";
        case NliLabel::Refute: return "refute";
        case NliLabel::Neutral: return "neutral";
    }
    throw JsonWriteError("unsupported NLI label");
}

const char* claimStatusName(ClaimStatus value) {
    switch (value) {
        case ClaimStatus::Determinate: return "determinate";
        case ClaimStatus::InsufficientInformation:
            return "insufficient_information";
    }
    throw JsonWriteError("unsupported claim status");
}

const char* claimTypeName(ClaimType value) {
    switch (value) {
        case ClaimType::Numeric: return "数值型";
        case ClaimType::Temporal: return "时间型";
        case ClaimType::Identity: return "身份型";
        case ClaimType::Location: return "地点型";
        case ClaimType::Existence: return "存在型";
        case ClaimType::Causal: return "因果型";
    }
    throw JsonWriteError("unsupported claim type");
}

const char* relationTypeName(RelationType value) {
    switch (value) {
        case RelationType::Temporal: return "temporal";
        case RelationType::Conditional: return "conditional";
        case RelationType::Contrast: return "contrast";
        case RelationType::Parallel: return "coordinate";
        case RelationType::Causal: return "causal";
    }
    throw JsonWriteError("unsupported relation type");
}

const char* aggregationTypeName(AggregationType value) {
    switch (value) {
        case AggregationType::And: return "and";
        case AggregationType::Or: return "or";
        case AggregationType::Not: return "not";
        case AggregationType::Implication: return "implication";
        case AggregationType::Causal: return "causal";
    }
    throw JsonWriteError("unsupported aggregation type");
}

const char* dimensionName(RelationDimension value) {
    switch (value) {
        case RelationDimension::Time: return "S_time";
        case RelationDimension::Mechanism: return "S_mech";
        case RelationDimension::Counterfactual: return "S_counter";
        case RelationDimension::Confounder: return "S_confound";
        case RelationDimension::DoseResponse: return "S_dose";
        case RelationDimension::Contrast: return "S_contrast";
        case RelationDimension::Coordinate: return "S_coordinate";
    }
    throw JsonWriteError("unsupported relation dimension");
}

const char* juryVerdictName(JuryVerdict value) {
    switch (value) {
        case JuryVerdict::Support: return "support";
        case JuryVerdict::Refute: return "refute";
        case JuryVerdict::Uncertain: return "uncertain";
    }
    throw JsonWriteError("unsupported jury verdict");
}

const char* jurorRoleName(JurorRole value) {
    switch (value) {
        case JurorRole::StrictLegal: return "strict_legal";
        case JurorRole::OpenWebPragmatist: return "open_web_pragmatist";
        case JurorRole::CausalSkeptic: return "causal_skeptic";
        case JurorRole::AntiManipulation: return "anti_manipulation";
    }
    throw JsonWriteError("unsupported juror role");
}

const char* credibilityLabelName(CredibilityLabel value) {
    switch (value) {
        case CredibilityLabel::ConfirmedTrue: return "confirmed_true";
        case CredibilityLabel::MostlyTrueWithFlaws: return "mostly_true";
        case CredibilityLabel::InsufficientInformation:
            return "insufficient_information";
        case CredibilityLabel::ConfirmedFalse: return "confirmed_false";
    }
    throw JsonWriteError("unsupported credibility label");
}

const char* rejectionReasonName(RejectionReason value) {
    switch (value) {
        case RejectionReason::EmptyPredictionSet:
            return "empty_prediction_set";
        case RejectionReason::MultipleCandidateLabels:
            return "multiple_candidate_labels";
    }
    throw JsonWriteError("unsupported rejection reason");
}

void writeStringArray(Emitter& json, const std::vector<std::string>& values) {
    json.beginArray();
    for (const auto& value : values) json.value(value);
    json.endArray();
}

void writeStringSet(Emitter& json,
                    const std::unordered_set<std::string>& values) {
    // unordered_set 的遍历顺序不稳定；排序后便于版本对比和测试。
    std::vector<std::string> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    writeStringArray(json, sorted);
}

void writeSevenElements(Emitter& json, const SevenElements& value) {
    json.beginObject();
    json.key("主体"); writeOptional(json, value.subject);
    json.key("行为"); writeOptional(json, value.action);
    json.key("客体"); writeOptional(json, value.object);
    json.key("时间"); writeOptional(json, value.time);
    json.key("地点"); writeOptional(json, value.location);
    json.key("数量"); writeOptional(json, value.quantity);
    json.key("结果"); writeOptional(json, value.result);
    json.endObject();
}

void writeEvidenceInput(Emitter& json, const Evidence& evidence) {
    json.beginObject();
    json.key("evidence_id"); json.value(evidence.id);
    json.key("original_source_id"); json.value(evidence.original_source_id);
    json.key("source_type");
    if (evidence.source_type) json.value(sourceTypeName(*evidence.source_type));
    else json.nullValue();
    json.key("keywords"); writeStringSet(json, evidence.keywords);
    // 旧协议没有 keyword_matches。为空时省略而不是写出空数组，保证包含
    // 非空 keywords 的旧输入经过 JsonWriter 后仍可被 JsonReader 再次读取。
    if (!evidence.keyword_matches.empty()) {
        json.key("keyword_matches");
        json.beginArray();
        for (const auto& match : evidence.keyword_matches) {
            json.beginObject();
            json.key("evidence_keyword"); json.value(match.evidence_keyword);
            json.key("claim_keyword"); json.value(match.claim_keyword);
            json.key("similarity"); json.value(match.similarity);
            json.endObject();
        }
        json.endArray();
    }
    json.key("event_day"); writeOptional(json, evidence.event_day);
    json.key("nli_label"); json.value(nliLabelName(evidence.nli_label));
    json.key("nli_confidence"); json.value(evidence.nli_confidence);
    json.endObject();
}

void writeClaimInput(Emitter& json, const AtomicClaim& claim) {
    json.beginObject();
    json.key("claim_id"); json.value(claim.id);
    json.key("claim_text"); json.value(claim.text);
    json.key("seven_elements"); writeSevenElements(json, claim.seven_elements);
    json.key("check_question"); json.value(claim.check_question);
    json.key("claim_type"); json.value(claimTypeName(claim.claim_type));
    json.key("entities"); writeStringArray(json, claim.entities);
    json.key("canon_refs"); writeStringArray(json, claim.canon_refs);
    json.key("keywords"); writeStringSet(json, claim.keywords);
    json.key("event_day"); writeOptional(json, claim.event_day);
    json.key("time_sensitive"); json.value(claim.time_sensitive);
    json.key("evidence");
    json.beginArray();
    for (const auto& evidence : claim.evidence) writeEvidenceInput(json, evidence);
    json.endArray();
    json.endObject();
}

void writeRequiredDimensions(
    Emitter& json, const std::vector<RelationDimension>& dimensions) {
    json.beginArray();
    for (RelationDimension dimension : dimensions) {
        json.value(dimensionName(dimension));
    }
    json.endArray();
}

void writeRelationDimensions(Emitter& json,
                             const RelationDimensionScores& scores) {
    json.beginObject();
    if (scores.temporal_order) {
        json.key("S_time"); json.value(*scores.temporal_order);
    }
    if (scores.mechanism) {
        json.key("S_mech"); json.value(*scores.mechanism);
    }
    if (scores.counterfactual_support) {
        json.key("S_counter"); json.value(*scores.counterfactual_support);
    }
    if (scores.contrast) {
        json.key("S_contrast"); json.value(*scores.contrast);
    }
    if (scores.coordinate) {
        json.key("S_coordinate"); json.value(*scores.coordinate);
    }
    json.endObject();
}

void writeCausalDimensions(Emitter& json, const CausalFactorScores& scores) {
    json.beginObject();
    if (scores.temporal_order) {
        json.key("S_time"); json.value(*scores.temporal_order);
    }
    if (scores.mechanism) {
        json.key("S_mech"); json.value(*scores.mechanism);
    }
    if (scores.counterfactual_support) {
        json.key("S_counter"); json.value(*scores.counterfactual_support);
    }
    if (scores.confounder_control) {
        json.key("S_confound"); json.value(*scores.confounder_control);
    }
    if (scores.dose_response) {
        json.key("S_dose"); json.value(*scores.dose_response);
    }
    json.endObject();
}

const std::string& claimIdAt(const QualifierRequest& request,
                             std::size_t index,
                             const std::string& path) {
    if (index >= request.claims.size()) {
        throw JsonWriteError(path + ": claim index is out of range");
    }
    return request.claims[index].id;
}

void writeRelationInput(Emitter& json,
                        const RelationInput& relation,
                        const QualifierRequest& request) {
    json.beginObject();
    json.key("edge_id"); json.value(relation.edge_id);
    json.key("claim_i");
    json.value(claimIdAt(request, relation.left_claim, "relation.claim_i"));
    json.key("claim_j");
    json.value(claimIdAt(request, relation.right_claim, "relation.claim_j"));
    json.key("rel"); json.value(relationTypeName(relation.type));
    json.key("required_dimensions");
    writeRequiredDimensions(json, relation.required_dimensions);
    json.key("dimension_scores");
    writeRelationDimensions(json, relation.dimension_scores);
    json.key("nli_score"); json.value(relation.nli_score);
    json.endObject();
}

void writeAggregationInput(Emitter& json,
                           const AggregationInput& aggregation,
                           const QualifierRequest& request) {
    json.beginObject();
    json.key("edge_id"); json.value(aggregation.edge_id);
    json.key("claim_i");
    json.value(claimIdAt(request, aggregation.left_claim,
                         "aggregation.claim_i"));
    json.key("claim_j");
    json.value(claimIdAt(request, aggregation.right_claim,
                         "aggregation.claim_j"));
    json.key("rel"); json.value(aggregationTypeName(aggregation.type));
    json.key("required_dimensions");
    writeRequiredDimensions(json, aggregation.required_dimensions);
    json.key("dimension_scores");
    writeCausalDimensions(json, aggregation.causal_factors);
    json.key("nli_score"); writeOptional(json, aggregation.nli_score);
    json.endObject();
}

void writeJuryInput(Emitter& json, const ClaimJuryOpinions& group) {
    json.beginObject();
    json.key("claim_id"); json.value(group.claim_id);
    json.key("opinions");
    json.beginArray();
    for (const auto& opinion : group.opinions) {
        json.beginObject();
        const bool has_any_audit = opinion.agent_id || opinion.agent_role ||
                                   opinion.model_id || opinion.prompt_version;
        const bool has_all_audit = opinion.agent_id && opinion.agent_role &&
                                   opinion.model_id && opinion.prompt_version;
        if (has_any_audit && !has_all_audit) {
            throw JsonWriteError(
                "jury opinion audit fields must be all present or all absent");
        }
        if (has_all_audit) {
            json.key("agent_id"); json.value(*opinion.agent_id);
            json.key("agent_role");
            json.value(jurorRoleName(*opinion.agent_role));
            json.key("model_id"); json.value(*opinion.model_id);
            json.key("prompt_version"); json.value(*opinion.prompt_version);
        }
        json.key("verdict"); json.value(juryVerdictName(opinion.verdict));
        json.key("hybrid_retrieval_score");
        json.value(opinion.hybrid_retrieval_score);
        json.key("confidence"); json.value(opinion.confidence);
        json.key("completed_without_error");
        json.value(opinion.completed_without_error);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeOriginalRequest(Emitter& json, const QualifierRequest& request) {
    json.key("schema_version"); json.value(request.schema_version);
    json.key("request_id"); json.value(request.request_id);
    json.key("event_id"); json.value(request.event_id);
    json.key("original_text"); json.value(request.original_text);
    json.key("source"); json.value(request.source);
    json.key("publish_date"); json.value(request.publish_date);
    json.key("event_nature"); json.value(request.event_nature);
    json.key("domain"); json.value(request.domain);
    json.key("event_time"); json.value(request.event_time);
    json.key("seven_elements"); writeSevenElements(json, request.seven_elements);

    json.key("atomic_claims");
    json.beginArray();
    for (const auto& claim : request.claims) writeClaimInput(json, claim);
    json.endArray();

    json.key("logic_edges");
    json.beginArray();
    for (const auto& relation : request.relations) {
        writeRelationInput(json, relation, request);
    }
    for (const auto& aggregation : request.aggregations) {
        writeAggregationInput(json, aggregation, request);
    }
    json.endArray();

    json.key("jury_opinions");
    json.beginArray();
    for (const auto& group : request.jury_opinions) writeJuryInput(json, group);
    json.endArray();
}

void requireSize(std::size_t actual,
                 std::size_t expected,
                 const std::string& field) {
    if (actual != expected) {
        throw JsonWriteError(field + " must contain " +
                             std::to_string(expected) + " entries, but contains " +
                             std::to_string(actual));
    }
}

void validateResponse(const QualifierResponse& response) {
    const std::size_t claim_count = response.request.claims.size();
    const auto& result = response.result;
    if (response.algorithm_version.empty()) {
        throw JsonWriteError("algorithm_version must not be empty");
    }
    requireSize(result.claim_evaluations.size(), claim_count,
                "claim_evaluations");
    requireSize(result.relation_evaluations.size(),
                response.request.relations.size(), "relation_evaluations");
    requireSize(result.aggregation_evaluations.size(),
                response.request.aggregations.size(), "aggregation_evaluations");
    requireSize(result.claim_logic_evaluations.size(), claim_count,
                "claim_logic_evaluations");
    requireSize(result.jury_evaluations.size(), claim_count,
                "jury_evaluations");
    requireSize(result.multi_dimensional.claims.size(), claim_count,
                "multi_dimensional.claims");
    requireSize(result.probability_evaluations.size(), claim_count,
                "probability_evaluations");
    requireSize(result.rejection_decisions.size(), claim_count,
                "rejection_decisions");

    for (std::size_t index = 0; index < claim_count; ++index) {
        const std::string& expected_id = response.request.claims[index].id;
        if (result.claim_evaluations[index].claim_id != expected_id) {
            throw JsonWriteError("claim_evaluations[" + std::to_string(index) +
                                 "] has a mismatched claim_id");
        }
        if (result.multi_dimensional.claims[index].claim_id != expected_id) {
            throw JsonWriteError("multi_dimensional.claims[" +
                                 std::to_string(index) +
                                 "] has a mismatched claim_id");
        }
        if (result.claim_logic_evaluations[index].claim_index != index) {
            throw JsonWriteError("claim_logic_evaluations must be ordered by "
                                 "claim_index");
        }
        if (result.rejection_decisions[index] &&
            !result.probability_evaluations[index]) {
            throw JsonWriteError("a rejection decision requires a layer-6 "
                                 "probability result");
        }
    }
    if (result.conformal_threshold) {
        const auto& threshold = *result.conformal_threshold;
        if (threshold.family_count == 0 || threshold.rank == 0) {
            throw JsonWriteError("conformal_threshold must come from a "
                                 "non-empty calibration set");
        }
        if (threshold.is_infinite) {
            if (!std::isinf(threshold.value) || threshold.value < 0.0) {
                throw JsonWriteError("an infinite conformal threshold must "
                                     "have value +infinity");
            }
        } else if (!std::isfinite(threshold.value)) {
            throw JsonWriteError("a finite conformal threshold must contain "
                                 "a finite value");
        }
    }
}

void writeEvidenceAssessment(Emitter& json,
                             const EvidenceAssessment& assessment) {
    json.beginObject();
    json.key("evidence_id"); json.value(assessment.evidence_id);
    json.key("cluster_id"); json.value(assessment.cluster_id);
    json.key("base_reliability");
    writeOptional(json, assessment.base_reliability);
    json.key("relevance"); writeOptional(json, assessment.relevance);
    json.key("timeliness"); writeOptional(json, assessment.timeliness);
    json.key("reliability"); writeOptional(json, assessment.reliability);
    json.key("independence_discount");
    writeOptional(json, assessment.independence_discount);
    json.key("nli_label"); json.value(nliLabelName(assessment.nli_label));
    json.key("nli_confidence"); json.value(assessment.nli_confidence);
    json.endObject();
}

void writeLayer1(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_1_evidence_authenticity");
    json.beginObject();
    json.key("claims");
    json.beginArray();
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        json.beginObject();
        json.key("claim_id"); json.value(request.claims[index].id);
        json.key("evidence");
        json.beginArray();
        for (const auto& assessment : result.claim_evaluations[index].evidence) {
            writeEvidenceAssessment(json, assessment);
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeLayer2(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_2_evidence_sufficiency");
    json.beginObject();
    json.key("claims");
    json.beginArray();
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        const auto& evaluation = result.claim_evaluations[index];
        json.beginObject();
        json.key("claim_id"); json.value(request.claims[index].id);
        json.key("evidence_clusters");
        json.beginArray();
        for (const auto& cluster : evaluation.evidence_clusters) {
            json.beginObject();
            json.key("cluster_id"); json.value(cluster.cluster_id);
            json.key("support"); json.value(cluster.support);
            json.key("refute"); json.value(cluster.refute);
            json.key("neutral"); json.value(cluster.neutral);
            json.endObject();
        }
        json.endArray();
        json.key("support_strength");
        writeOptional(json, evaluation.support_strength);
        json.key("refute_strength");
        writeOptional(json, evaluation.refute_strength);
        json.key("direction"); writeOptional(json, evaluation.direction);
        json.key("content_score"); writeOptional(json, evaluation.content_score);
        json.key("sufficiency_score");
        writeOptional(json, evaluation.sufficiency_score);
        json.key("status"); json.value(claimStatusName(evaluation.status));
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeLogicEdgeResult(Emitter& json,
                          const LogicEvaluation& evaluation,
                          const QualifierRequest& request) {
    json.beginObject();
    std::visit(
        [&](const auto& input) {
            using Input = std::decay_t<decltype(input)>;
            json.key("edge_id"); json.value(input.edge_id);
            json.key("claim_i");
            json.value(claimIdAt(request, input.left_claim,
                                 "logic_result.claim_i"));
            json.key("claim_j");
            json.value(claimIdAt(request, input.right_claim,
                                 "logic_result.claim_j"));
            json.key("relation_type");
            if constexpr (std::is_same_v<Input, RelationInput>) {
                json.value(relationTypeName(input.type));
            } else {
                json.value(aggregationTypeName(input.type));
            }
        },
        evaluation.input);
    json.key("logic_score"); writeOptional(json, evaluation.score);
    json.key("logic_available"); json.value(evaluation.logic_available);
    json.endObject();
}

void writeLayer3(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_3_logic_relationship");
    json.beginObject();
    json.key("edges");
    json.beginArray();
    for (const auto& evaluation : result.relation_evaluations) {
        writeLogicEdgeResult(json, evaluation, request);
    }
    for (const auto& evaluation : result.aggregation_evaluations) {
        writeLogicEdgeResult(json, evaluation, request);
    }
    json.endArray();
    json.key("claims");
    json.beginArray();
    for (const auto& evaluation : result.claim_logic_evaluations) {
        json.beginObject();
        json.key("claim_id");
        json.value(claimIdAt(request, evaluation.claim_index,
                             "claim_logic_evaluation.claim_index"));
        json.key("logic_score"); writeOptional(json, evaluation.score);
        json.key("logic_available"); json.value(evaluation.logic_available);
        json.key("contributing_edge_count");
        json.value(evaluation.contributing_edge_count);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeLayer4(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_4_jury_evaluation");
    json.beginObject();
    json.key("claims");
    json.beginArray();
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        const auto& evaluation = result.jury_evaluations[index];
        json.beginObject();
        json.key("claim_id"); json.value(request.claims[index].id);
        json.key("has_valid_votes"); json.value(evaluation.has_valid_votes);
        json.key("jury_missing"); json.value(evaluation.jury_missing);
        json.key("vote_distribution");
        if (evaluation.vote_distribution) {
            json.beginObject();
            json.key("support"); json.value((*evaluation.vote_distribution)[0]);
            json.key("refute"); json.value((*evaluation.vote_distribution)[1]);
            json.key("uncertain"); json.value((*evaluation.vote_distribution)[2]);
            json.endObject();
        } else {
            json.nullValue();
        }
        json.key("direction"); writeOptional(json, evaluation.direction);
        json.key("consensus_score");
        writeOptional(json, evaluation.consensus_score);
        json.key("jury_score"); writeOptional(json, evaluation.jury_score);
        json.key("uncertain_vote_share");
        writeOptional(json, evaluation.uncertain_vote_share);
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeDimensions(Emitter& json, const DimensionScores& dimensions) {
    json.beginObject();
    json.key("content"); writeOptional(json, dimensions.content);
    json.key("logic"); writeOptional(json, dimensions.logic);
    json.key("sufficiency"); writeOptional(json, dimensions.sufficiency);
    json.key("source"); writeOptional(json, dimensions.source);
    json.key("independence"); writeOptional(json, dimensions.independence);
    json.key("jury"); writeOptional(json, dimensions.jury);
    json.key("consensus"); writeOptional(json, dimensions.consensus);
    json.key("uncertainty"); writeOptional(json, dimensions.uncertainty);
    json.endObject();
}

void writeLayer5(Emitter& json, const QualifierResult& result) {
    json.key("layer_5_multi_dimensional_aggregation");
    json.beginObject();
    json.key("claims");
    json.beginArray();
    for (const auto& evaluation : result.multi_dimensional.claims) {
        json.beginObject();
        json.key("claim_id"); json.value(evaluation.claim_id);
        json.key("dimensions"); writeDimensions(json, evaluation.dimensions);
        json.key("logic_available"); json.value(evaluation.logic_available);
        json.key("jury_missing"); json.value(evaluation.jury_missing);
        json.key("content_direction");
        writeOptional(json, evaluation.content_direction);
        json.key("content_logic_direction");
        writeOptional(json, evaluation.content_logic_direction);
        json.key("evidence_quality");
        writeOptional(json, evaluation.evidence_quality);
        json.key("evidence_direction");
        writeOptional(json, evaluation.evidence_direction);
        json.key("jury_direction");
        writeOptional(json, evaluation.jury_direction);
        json.key("final_direction");
        writeOptional(json, evaluation.final_direction);
        json.key("mc_score"); writeOptional(json, evaluation.mc_score);
        json.endObject();
    }
    json.endArray();
    json.key("event_summary");
    json.beginObject();
    json.key("dimensions");
    writeDimensions(json, result.multi_dimensional.dimensions);
    json.key("mc_score"); writeOptional(json, result.multi_dimensional.mc_score);
    json.key("has_insufficient_claims");
    json.value(result.multi_dimensional.has_insufficient_claims);
    json.endObject();
    json.endObject();
}

void writeProbabilities(Emitter& json,
                        const FourClassProbabilities& probabilities) {
    json.beginObject();
    json.key("confirmed_true");
    writeOptional(json, probabilities.confirmed_true);
    json.key("mostly_true"); writeOptional(json, probabilities.mostly_true);
    json.key("insufficient_information");
    writeOptional(json, probabilities.insufficient_information);
    json.key("confirmed_false");
    writeOptional(json, probabilities.confirmed_false);
    json.endObject();
}

void writeNullableProbabilityObject(
    Emitter& json,
    const std::optional<ProbabilityEvaluation>& evaluation,
    bool calibrated) {
    if (!evaluation) {
        json.nullValue();
        return;
    }
    writeProbabilities(json, calibrated ? evaluation->probabilities
                                        : evaluation->uncalibrated_probabilities);
}

void writeLayer6(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_6_probability_calibration");
    json.beginObject();
    json.key("claims");
    json.beginArray();
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        const auto& evaluation = result.probability_evaluations[index];
        json.beginObject();
        json.key("claim_id"); json.value(request.claims[index].id);
        json.key("mc_direction");
        if (evaluation) writeOptional(json, evaluation->mc_direction);
        else json.nullValue();
        json.key("positive_mass");
        if (evaluation) writeOptional(json, evaluation->positive_mass);
        else json.nullValue();
        json.key("negative_mass");
        if (evaluation) writeOptional(json, evaluation->negative_mass);
        else json.nullValue();
        json.key("uncertainty_mass");
        if (evaluation) writeOptional(json, evaluation->uncertainty_mass);
        else json.nullValue();
        json.key("truth_completeness");
        if (evaluation) writeOptional(json, evaluation->truth_completeness);
        else json.nullValue();
        json.key("uncalibrated_probabilities");
        writeNullableProbabilityObject(json, evaluation, false);
        json.key("calibrated_probabilities");
        writeNullableProbabilityObject(json, evaluation, true);
        json.key("predicted_label");
        if (evaluation && evaluation->predicted_label) {
            json.value(credibilityLabelName(*evaluation->predicted_label));
        } else {
            json.nullValue();
        }
        json.endObject();
    }
    json.endArray();
    json.endObject();
}

void writeThreshold(Emitter& json,
                    const std::optional<ConformalThreshold>& threshold) {
    if (!threshold) {
        json.nullValue();
        return;
    }
    json.beginObject();
    json.key("value");
    // JSON 没有 Infinity；is_infinite=true 时用 null 明确表示不可写为数值。
    if (threshold->is_infinite) json.nullValue();
    else json.value(threshold->value);
    json.key("is_infinite"); json.value(threshold->is_infinite);
    json.key("family_count"); json.value(threshold->family_count);
    json.key("rank"); json.value(threshold->rank);
    json.endObject();
}

void writeDecision(Emitter& json,
                   const std::string& claim_id,
                   const std::optional<RejectionDecision>& decision) {
    json.beginObject();
    json.key("claim_id"); json.value(claim_id);
    json.key("prediction_set");
    if (decision) {
        json.beginArray();
        for (CredibilityLabel label : decision->prediction_set) {
            json.value(credibilityLabelName(label));
        }
        json.endArray();
    } else {
        json.nullValue();
    }
    json.key("accepted_label");
    if (decision && decision->accepted_label) {
        json.value(credibilityLabelName(*decision->accepted_label));
    } else {
        json.nullValue();
    }
    json.key("should_reject");
    if (decision) json.value(decision->should_reject);
    else json.nullValue();
    json.key("reasons");
    if (decision) {
        json.beginArray();
        for (RejectionReason reason : decision->reasons) {
            json.value(rejectionReasonName(reason));
        }
        json.endArray();
    } else {
        json.nullValue();
    }
    json.key("suggested_message");
    if (decision) json.value(decision->suggested_message);
    else json.nullValue();
    json.endObject();
}

void writeLayer7(Emitter& json,
                 const QualifierRequest& request,
                 const QualifierResult& result) {
    json.key("layer_7_conformal_rejection");
    json.beginObject();
    json.key("threshold"); writeThreshold(json, result.conformal_threshold);
    json.key("claims");
    json.beginArray();
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        writeDecision(json, request.claims[index].id,
                      result.rejection_decisions[index]);
    }
    json.endArray();
    json.endObject();
}

std::optional<double> acceptedProbability(
    const ProbabilityEvaluation& evaluation,
    CredibilityLabel label) {
    switch (label) {
        case CredibilityLabel::ConfirmedTrue:
            return evaluation.probabilities.confirmed_true;
        case CredibilityLabel::MostlyTrueWithFlaws:
            return evaluation.probabilities.mostly_true;
        case CredibilityLabel::InsufficientInformation:
            return evaluation.probabilities.insufficient_information;
        case CredibilityLabel::ConfirmedFalse:
            return evaluation.probabilities.confirmed_false;
    }
    return std::nullopt;
}

void writeAnswerAgentInput(Emitter& json,
                           const QualifierRequest& request,
                           const QualifierResult& result) {
    json.key("answer_agent_input");
    json.beginObject();
    json.key("claim_decisions");
    json.beginArray();
    bool has_rejected_claims = false;
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        const auto& decision = result.rejection_decisions[index];
        const auto& probability = result.probability_evaluations[index];
        const bool should_answer =
            decision && !decision->should_reject && decision->accepted_label;
        if (!should_answer) has_rejected_claims = true;

        json.beginObject();
        json.key("claim_id"); json.value(request.claims[index].id);
        json.key("should_answer"); json.value(should_answer);
        json.key("label");
        if (should_answer) {
            json.value(credibilityLabelName(*decision->accepted_label));
        } else {
            json.nullValue();
        }
        json.key("probability");
        if (should_answer && probability) {
            writeOptional(json, acceptedProbability(
                                    *probability, *decision->accepted_label));
        } else {
            json.nullValue();
        }
        json.key("suggested_message");
        if (decision) json.value(decision->suggested_message);
        else json.nullValue();
        json.endObject();
    }
    json.endArray();
    json.key("event_has_rejected_claims"); json.value(has_rejected_claims);
    json.endObject();
}

void writeQualificationResult(Emitter& json,
                              const QualifierResponse& response) {
    json.key("qualification_result");
    json.beginObject();
    json.key("algorithm_version"); json.value(response.algorithm_version);
    json.key("input_schema_version");
    json.value(response.request.schema_version);
    json.key("config_version");
    if (response.config_version.empty()) json.nullValue();
    else json.value(response.config_version);
    json.key("calibration_status");
    if (response.calibration_status.empty()) json.nullValue();
    else json.value(response.calibration_status);
    json.key("calculated_at");
    if (response.calculated_at.empty()) json.nullValue();
    else json.value(response.calculated_at);

    writeLayer1(json, response.request, response.result);
    writeLayer2(json, response.request, response.result);
    writeLayer3(json, response.request, response.result);
    writeLayer4(json, response.request, response.result);
    writeLayer5(json, response.result);
    writeLayer6(json, response.request, response.result);
    writeLayer7(json, response.request, response.result);
    writeAnswerAgentInput(json, response.request, response.result);
    json.endObject();
}

void writeDocument(std::ostream& output,
                   const QualifierResponse& response,
                   bool pretty) {
    validateResponse(response);
    Emitter json(output, pretty);
    json.beginObject();
    writeOriginalRequest(json, response.request);
    writeQualificationResult(json, response);
    json.endObject();
    json.finish();
}

}  // namespace

JsonWriteError::JsonWriteError(const std::string& message)
    : std::runtime_error(message) {}

void JsonWriter::write(std::ostream& output,
                       const QualifierResponse& response,
                       bool pretty) {
    writeDocument(output, response, pretty);
}

std::string JsonWriter::writeString(const QualifierResponse& response,
                                    bool pretty) {
    std::ostringstream output;
    writeDocument(output, response, pretty);
    return output.str();
}

void JsonWriter::writeFile(const std::filesystem::path& path,
                           const QualifierResponse& response,
                           bool pretty) {
    // 先在内存中完成全部校验和序列化，避免业务数据错误留下半截 JSON 文件。
    const std::string document = writeString(response, pretty);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw JsonWriteError("cannot open JSON output file: " + path.string());
    }
    output.write(document.data(), static_cast<std::streamsize>(document.size()));
    if (!output) {
        throw JsonWriteError("failed while writing JSON output file: " +
                             path.string());
    }
}

void JsonWriter::writeJsonLine(std::ostream& output,
                               const QualifierResponse& response) {
    writeDocument(output, response, false);
    output << '\n';
    if (!output) throw JsonWriteError("failed while writing JSONL output");
}

}  // namespace chongming::qualifier
