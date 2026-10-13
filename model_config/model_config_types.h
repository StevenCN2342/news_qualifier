#pragma once

#include "atomic_content_qualifier.h"
#include "logic_relationship_qualifier.h"
#include "multi_dimensional_qualifier.h"
#include "probability_adjuster.h"
#include "rejection_trigger.h"

#include <cstdint>
#include <string>

namespace chongming::qualifier {

/**
 * 一份可完整复现生产推理结果的冻结模型配置。
 *
 * 宏定义只负责各 Config 的默认构造值；ModelConfigReader 会用 JSON 中的值
 * 覆盖它们，再由 QualifierPipeline 通过构造函数注入各层算法。
 */
struct ModelConfig {
    std::string model_version;
    std::string algorithm_version;
    std::int64_t input_schema_version{0};
    std::string calibration_status;

    AtomicContentConfig atomic_content;
    EvidenceAuthenticityConfig evidence_authenticity;
    LogicRelationshipConfig logic_relationship;
    MultiDimensionalConfig multi_dimensional;
    ProbabilityAdjusterConfig probability_adjustment;
    RejectionTriggerConfig rejection_trigger;
    ConformalThreshold conformal_threshold;
};

}  // namespace chongming::qualifier
