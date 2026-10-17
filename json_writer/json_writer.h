#pragma once

#include "../include/probability_adjuster.h"
#include "../include/rejection_trigger.h"
#include "../json_reader/json_reader.h"

#include <filesystem>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace chongming::qualifier {

/**
 * 七层算法的一次完整计算结果。
 *
 * 所有逐声明数组均按 QualifierRequest::claims 的顺序排列。第六、七层尚未
 * 产生结果时，对应位置使用 nullopt，而不是用 0 或 0.5 表示“未计算”。
 * 第三层将普通关系和因果聚合分开保存，以便与 JsonReader 的两个数组一一
 * 对齐；写出时会统一放入 layer_3_logic_relationship.edges。
 */
struct QualifierResult {
    std::vector<ClaimEvaluation> claim_evaluations;
    std::vector<LogicEvaluation> relation_evaluations;
    std::vector<LogicEvaluation> aggregation_evaluations;
    std::vector<ClaimLogicEvaluation> claim_logic_evaluations;
    std::vector<JuryEvaluation> jury_evaluations;
    MultiDimensionalEvaluation multi_dimensional;
    std::vector<std::optional<ProbabilityEvaluation>> probability_evaluations;
    std::optional<ConformalThreshold> conformal_threshold;
    std::vector<std::optional<RejectionDecision>> rejection_decisions;
};

/** 原始请求、版本元数据和七层结果共同组成最终响应。 */
struct QualifierResponse {
    QualifierRequest request;
    QualifierResult result;
    std::string algorithm_version{"0"};
    std::string config_version;
    std::string calibration_status;
    // 建议使用带时区的 ISO 8601 字符串；写出器只负责原样保存。
    std::string calculated_at;
};

/** 输出结构不完整、索引越界或包含非有限数字时抛出的异常。 */
class JsonWriteError : public std::runtime_error {
public:
    explicit JsonWriteError(const std::string& message);
};

/**
 * 将 QualifierResponse 序列化成 UTF-8 JSON。
 *
 * Writer 不执行七层算法，也不会修改输入对象。pretty=true 生成便于审核的
 * 缩进格式；JSONL 应使用 writeJsonLine，以保证每个响应只占一行。
 */
class JsonWriter {
public:
    static void write(
        std::ostream& output,
        const QualifierResponse& response,
        bool pretty = true);

    static std::string writeString(
        const QualifierResponse& response,
        bool pretty = true);

    static void writeFile(
        const std::filesystem::path& path,
        const QualifierResponse& response,
        bool pretty = true);

    static void writeJsonLine(
        std::ostream& output,
        const QualifierResponse& response);
};

}  // namespace chongming::qualifier
