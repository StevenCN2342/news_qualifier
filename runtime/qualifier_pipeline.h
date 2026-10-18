#pragma once

#include "json_writer.h"
#include "jury.h"
#include "model_config_types.h"

#include <stdexcept>
#include <string>

namespace chongming::qualifier {

/** 七层之间的数据对齐或请求结构不满足运行条件时抛出的异常。 */
class QualifierPipelineError : public std::runtime_error {
public:
    explicit QualifierPipelineError(const std::string& message);
};

/**
 * 使用一份冻结配置执行完整七层前向计算。
 *
 * 本类不读取文件、不写 JSON，也不保存请求状态；同一个实例可顺序处理多条
 * 请求。命令行 predict 和项目外的批量调度脚本均通过本类执行相同计算流程。
 */
class QualifierPipeline {
public:
    explicit QualifierPipeline(const ModelConfig& config);

    QualifierResult run(const QualifierRequest& request) const;

private:
    AtomicContentQualifier atomic_content_qualifier_;
    LogicRelationshipQualifier logic_relationship_qualifier_;
    Jury jury_;
    MultiDimensionalQualifier multi_dimensional_qualifier_;
    ProbabilityAdjuster probability_adjuster_;
    RejectionTrigger rejection_trigger_;
    ConformalThreshold conformal_threshold_;
};

}  // namespace chongming::qualifier
