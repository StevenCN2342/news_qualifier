#include "prediction_runner.h"

#include "json_reader.h"
#include "json_writer.h"
#include "model_config_reader.h"
#include "qualifier_pipeline.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace chongming::qualifier {
namespace {

std::string currentUtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    const std::tm* utc_pointer = std::gmtime(&value);
    if (utc_pointer == nullptr) {
        throw std::runtime_error("failed to obtain the current UTC time");
    }
    const std::tm utc = *utc_pointer;
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void validateOptions(const PredictionOptions& options) {
    if (options.model_path.empty()) {
        throw std::invalid_argument("predict requires --model");
    }
    if (options.input_path.empty()) {
        throw std::invalid_argument("predict requires --input");
    }
    if (options.write_stdout == options.output_path.has_value()) {
        throw std::invalid_argument(
            "predict requires exactly one of --output and --stdout");
    }
}

}  // namespace

void PredictionRunner::run(const PredictionOptions& options,
                           std::ostream& stdout_stream) {
    validateOptions(options);
    const ModelConfig model = ModelConfigReader::readFile(options.model_path);
    QualifierRequest request = JsonReader::readFile(options.input_path);
    if (request.schema_version != model.input_schema_version) {
        throw std::invalid_argument(
            "input schema version does not match the frozen model config");
    }

    const QualifierPipeline pipeline(model);
    QualifierResult result = pipeline.run(request);

    QualifierResponse response;
    response.request = std::move(request);
    response.result = std::move(result);
    response.algorithm_version = model.algorithm_version;
    response.config_version = model.model_version;
    response.calibration_status = model.calibration_status;
    response.calculated_at = currentUtcTimestamp();

    if (options.write_stdout) {
        JsonWriter::write(stdout_stream, response, options.pretty_json);
    } else {
        JsonWriter::writeFile(*options.output_path, response,
                              options.pretty_json);
    }
}

}  // namespace chongming::qualifier
