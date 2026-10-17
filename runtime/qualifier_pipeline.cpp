#include "qualifier_pipeline.h"

#include <unordered_map>
#include <utility>

namespace chongming::qualifier {

QualifierPipelineError::QualifierPipelineError(const std::string& message)
    : std::runtime_error(message) {}

QualifierPipeline::QualifierPipeline(const ModelConfig& config)
    : atomic_content_qualifier_(config.atomic_content,
                                config.evidence_authenticity),
      logic_relationship_qualifier_(config.logic_relationship),
      multi_dimensional_qualifier_(config.multi_dimensional),
      probability_adjuster_(config.probability_adjustment),
      rejection_trigger_(config.rejection_trigger),
      conformal_threshold_(config.conformal_threshold) {}

QualifierResult QualifierPipeline::run(const QualifierRequest& request) const {
    if (request.schema_version != 3) {
        throw QualifierPipelineError(
            "QualifierPipeline only accepts input schema version 3");
    }
    if (request.claims.empty()) {
        throw QualifierPipelineError("request contains no atomic claims");
    }

    QualifierResult result;

    // 第一、二层由 AtomicContentQualifier 统一组织：它先评估每条证据，随后
    // 形成来源簇、内容方向和充分性结果。
    result.claim_evaluations.reserve(request.claims.size());
    for (const auto& claim : request.claims) {
        result.claim_evaluations.push_back(
            atomic_content_qualifier_.evaluate(claim));
    }

    // 第三层分别保存普通语义关系和因果聚合结果；汇总时再放入同一个边数组。
    result.relation_evaluations.reserve(request.relations.size());
    result.aggregation_evaluations.reserve(request.aggregations.size());
    std::vector<LogicEvaluation> all_logic_evaluations;
    all_logic_evaluations.reserve(request.relations.size() +
                                  request.aggregations.size());
    for (const auto& relation : request.relations) {
        LogicEvaluation evaluation = logic_relationship_qualifier_.evaluate(
            relation, result.claim_evaluations);
        result.relation_evaluations.push_back(evaluation);
        all_logic_evaluations.push_back(std::move(evaluation));
    }
    for (const auto& aggregation : request.aggregations) {
        LogicEvaluation evaluation = logic_relationship_qualifier_.evaluate(
            aggregation, result.claim_evaluations);
        result.aggregation_evaluations.push_back(evaluation);
        all_logic_evaluations.push_back(std::move(evaluation));
    }
    result.claim_logic_evaluations =
        logic_relationship_qualifier_.aggregateByClaim(
            request.claims.size(), all_logic_evaluations);

    // jury_opinions 使用 claim_id 关联，不能假设 JSON 数组顺序与 claims 相同。
    std::unordered_map<std::string, const std::vector<JurorOpinion>*>
        jury_by_claim;
    for (const auto& group : request.jury_opinions) {
        if (!jury_by_claim.emplace(group.claim_id, &group.opinions).second) {
            throw QualifierPipelineError(
                "duplicate jury-opinion group for claim '" + group.claim_id +
                "'");
        }
    }
    result.jury_evaluations.reserve(request.claims.size());
    for (const auto& claim : request.claims) {
        const auto iterator = jury_by_claim.find(claim.id);
        if (iterator == jury_by_claim.end()) {
            throw QualifierPipelineError(
                "missing jury-opinion group for claim '" + claim.id + "'");
        }
        result.jury_evaluations.push_back(
            jury_.deliberate(*iterator->second));
    }

    // 第五层消费前四层已经按声明对齐的结果。
    result.multi_dimensional = multi_dimensional_qualifier_.evaluate(
        result.claim_evaluations, result.claim_logic_evaluations,
        result.jury_evaluations);

    if (result.multi_dimensional.claims.size() != request.claims.size()) {
        throw QualifierPipelineError(
            "layer 5 did not return one result per atomic claim");
    }

    // 第六层允许因前置必要量未定义而返回 nullopt；第七层对此保持未计算，
    // 不会用伪造的零概率代替。
    result.probability_evaluations.reserve(request.claims.size());
    result.rejection_decisions.reserve(request.claims.size());
    for (std::size_t index = 0; index < request.claims.size(); ++index) {
        std::optional<ProbabilityEvaluation> probability =
            probability_adjuster_.evaluate(
                result.multi_dimensional.claims[index]);
        if (probability) {
            result.rejection_decisions.push_back(rejection_trigger_.decide(
                probability->probabilities, conformal_threshold_));
        } else {
            result.rejection_decisions.push_back(std::nullopt);
        }
        result.probability_evaluations.push_back(std::move(probability));
    }
    result.conformal_threshold = conformal_threshold_;
    return result;
}

}  // namespace chongming::qualifier
