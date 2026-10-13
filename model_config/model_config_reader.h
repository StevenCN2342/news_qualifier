#pragma once

#include "model_config_types.h"

#include <filesystem>
#include <istream>
#include <stdexcept>
#include <string>

namespace chongming::qualifier {

/** 配置 JSON 的语法、字段或参数约束不合法时抛出的异常。 */
class ModelConfigError : public std::runtime_error {
public:
    explicit ModelConfigError(const std::string& message);
};

/** 读取并严格校验生产推理使用的冻结模型配置。 */
class ModelConfigReader {
public:
    static ModelConfig read(
        std::istream& input,
        const std::string& source_name = "<model-config-stream>");

    static ModelConfig readFile(const std::filesystem::path& path);

    static ModelConfig readString(
        const std::string& json,
        const std::string& source_name = "<model-config-string>");
};

}  // namespace chongming::qualifier
