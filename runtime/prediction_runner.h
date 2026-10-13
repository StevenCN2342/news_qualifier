#pragma once

#include <filesystem>
#include <optional>
#include <ostream>

namespace chongming::qualifier {

struct PredictionOptions {
    std::filesystem::path model_path;
    std::filesystem::path input_path;
    std::optional<std::filesystem::path> output_path;
    bool write_stdout{false};
    bool pretty_json{true};
};

/** predict 模式的文件适配与七层流水线调度器。 */
class PredictionRunner {
public:
    static void run(const PredictionOptions& options, std::ostream& stdout_stream);
};

}  // namespace chongming::qualifier
