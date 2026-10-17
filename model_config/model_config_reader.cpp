#include "model_config_reader.h"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace chongming::qualifier {
namespace {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage =
        std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage value;
};

/** 配置文件专用的无依赖递归下降 JSON 解析器。 */
class Parser {
public:
    Parser(const std::string& text, std::string source)
        : text_(text), source_(std::move(source)) {}

    JsonValue parseDocument() {
        skipWhitespace();
        JsonValue result = parseValue();
        skipWhitespace();
        if (!atEnd()) fail("unexpected content after the JSON value");
        return result;
    }

private:
    const std::string& text_;
    std::string source_;
    std::size_t position_{0};
    std::size_t line_{1};
    std::size_t column_{1};

    bool atEnd() const noexcept { return position_ >= text_.size(); }

    [[noreturn]] void fail(const std::string& message) const {
        throw ModelConfigError(source_ + ":" + std::to_string(line_) + ":" +
                               std::to_string(column_) + ": " + message);
    }

    char peek() const {
        if (atEnd()) fail("unexpected end of input");
        return text_[position_];
    }

    char take() {
        const char value = peek();
        ++position_;
        if (value == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
        return value;
    }

    void skipWhitespace() {
        while (!atEnd()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            take();
        }
    }

    JsonValue parseValue() {
        if (atEnd()) fail("expected a JSON value");
        switch (peek()) {
            case '{': return JsonValue{parseObject()};
            case '[': return JsonValue{parseArray()};
            case '"': return JsonValue{parseString()};
            case 't': parseLiteral("true"); return JsonValue{true};
            case 'f': parseLiteral("false"); return JsonValue{false};
            case 'n': parseLiteral("null"); return JsonValue{nullptr};
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    return JsonValue{parseNumber()};
                }
                fail("expected an object, array, string, number, boolean, or null");
        }
    }

    JsonValue::Object parseObject() {
        take();
        skipWhitespace();
        JsonValue::Object result;
        if (!atEnd() && peek() == '}') {
            take();
            return result;
        }
        while (true) {
            if (atEnd() || peek() != '"') fail("expected an object key");
            std::string key = parseString();
            skipWhitespace();
            if (take() != ':') fail("expected ':' after object key");
            skipWhitespace();
            JsonValue value = parseValue();
            if (!result.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate object key");
            }
            skipWhitespace();
            const char separator = take();
            if (separator == '}') return result;
            if (separator != ',') fail("expected ',' or '}' in object");
            skipWhitespace();
        }
    }

    JsonValue::Array parseArray() {
        take();
        skipWhitespace();
        JsonValue::Array result;
        if (!atEnd() && peek() == ']') {
            take();
            return result;
        }
        while (true) {
            result.push_back(parseValue());
            skipWhitespace();
            const char separator = take();
            if (separator == ']') return result;
            if (separator != ',') fail("expected ',' or ']' in array");
            skipWhitespace();
        }
    }

    static int hexDigit(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    std::uint32_t parseCodeUnit() {
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            if (atEnd()) fail("incomplete Unicode escape");
            const int digit = hexDigit(take());
            if (digit < 0) fail("invalid Unicode escape");
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    static void appendUtf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(
                static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    std::string parseString() {
        take();
        std::string result;
        while (!atEnd()) {
            const unsigned char value = static_cast<unsigned char>(take());
            if (value == '"') return result;
            if (value < 0x20U) fail("unescaped control character in string");
            if (value != '\\') {
                result.push_back(static_cast<char>(value));
                continue;
            }
            if (atEnd()) fail("incomplete string escape");
            switch (take()) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = parseCodeUnit();
                    if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                        if (atEnd() || take() != '\\' || atEnd() ||
                            take() != 'u') {
                            fail("high surrogate requires a low surrogate");
                        }
                        const std::uint32_t low = parseCodeUnit();
                        if (low < 0xDC00U || low > 0xDFFFU) {
                            fail("invalid low surrogate");
                        }
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) +
                                    (low - 0xDC00U);
                    } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                        fail("unexpected low surrogate");
                    }
                    appendUtf8(result, codepoint);
                    break;
                }
                default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    void parseLiteral(const char* literal) {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (atEnd() || take() != *cursor) {
                fail(std::string("invalid literal; expected ") + literal);
            }
        }
    }

    double parseNumber() {
        const std::size_t start = position_;
        if (peek() == '-') take();
        if (atEnd()) fail("incomplete number");
        if (peek() == '0') {
            take();
            if (!atEnd() && peek() >= '0' && peek() <= '9') {
                fail("leading zero is not allowed");
            }
        } else {
            if (peek() < '1' || peek() > '9') fail("invalid number");
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        if (!atEnd() && peek() == '.') {
            take();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("fraction requires a digit");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            take();
            if (!atEnd() && (peek() == '+' || peek() == '-')) take();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("exponent requires a digit");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        const std::string token = text_.substr(start, position_ - start);
        try {
            std::size_t consumed = 0;
            const double result = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(result)) {
                fail("number is outside the finite range");
            }
            return result;
        } catch (const std::exception&) {
            fail("invalid number");
        }
    }
};

[[noreturn]] void fieldError(const std::string& source,
                             std::string_view path,
                             const std::string& message) {
    throw ModelConfigError(source + ": " + std::string(path) + ": " + message);
}

const JsonValue::Object& asObject(const JsonValue& value,
                                  const std::string& source,
                                  std::string_view path) {
    const auto* object = std::get_if<JsonValue::Object>(&value.value);
    if (!object) fieldError(source, path, "expected an object");
    return *object;
}

const JsonValue& required(const JsonValue::Object& object,
                          std::string_view key,
                          const std::string& source,
                          std::string_view path) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        fieldError(source, std::string(path) + "." + std::string(key),
                   "required field is missing");
    }
    return iterator->second;
}

std::string asString(const JsonValue& value,
                     const std::string& source,
                     std::string_view path) {
    const auto* result = std::get_if<std::string>(&value.value);
    if (!result) fieldError(source, path, "expected a string");
    if (result->empty()) fieldError(source, path, "must not be empty");
    return *result;
}

double asNumber(const JsonValue& value,
                const std::string& source,
                std::string_view path) {
    const auto* result = std::get_if<double>(&value.value);
    if (!result) fieldError(source, path, "expected a number");
    return *result;
}

double positiveNumber(const JsonValue& value,
                      const std::string& source,
                      std::string_view path) {
    const double result = asNumber(value, source, path);
    if (result <= 0.0) fieldError(source, path, "must be greater than zero");
    return result;
}

double nonNegativeNumber(const JsonValue& value,
                         const std::string& source,
                         std::string_view path) {
    const double result = asNumber(value, source, path);
    if (result < 0.0) fieldError(source, path, "must not be negative");
    return result;
}

double probability(const JsonValue& value,
                   const std::string& source,
                   std::string_view path) {
    const double result = asNumber(value, source, path);
    if (result < 0.0 || result > 1.0) {
        fieldError(source, path, "must be within [0, 1]");
    }
    return result;
}

bool asBoolean(const JsonValue& value,
               const std::string& source,
               std::string_view path) {
    const auto* result = std::get_if<bool>(&value.value);
    if (!result) fieldError(source, path, "expected a boolean");
    return *result;
}

std::size_t asSize(const JsonValue& value,
                   const std::string& source,
                   std::string_view path) {
    const double number = asNumber(value, source, path);
    if (number < 0.0 || std::trunc(number) != number ||
        number > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        fieldError(source, path, "expected a non-negative integer");
    }
    return static_cast<std::size_t>(number);
}

std::int64_t asInt64(const JsonValue& value,
                     const std::string& source,
                     std::string_view path) {
    const double number = asNumber(value, source, path);
    if (std::trunc(number) != number ||
        number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        fieldError(source, path, "expected a 64-bit integer");
    }
    return static_cast<std::int64_t>(number);
}

void requireWeightSum(std::initializer_list<double> weights,
                      const std::string& source,
                      std::string_view path) {
    double sum = 0.0;
    for (double weight : weights) sum += weight;
    if (std::abs(sum - 1.0) > 1e-9) {
        fieldError(source, path, "weights must sum to one");
    }
}

SourceType parseSourceType(const std::string& value,
                           const std::string& source,
                           std::string_view path) {
    if (value == "official_registry") return SourceType::OfficialRegistry;
    if (value == "academic") return SourceType::Academic;
    if (value == "open_web") return SourceType::OpenWeb;
    if (value == "social_media") return SourceType::SocialMedia;
    if (value == "news_media") return SourceType::NewsMedia;
    if (value == "video_or_github") return SourceType::VideoOrGitHub;
    if (value == "market_data") return SourceType::MarketData;
    fieldError(source, path, "unsupported source type '" + value + "'");
}

ModelConfig parseConfig(const JsonValue& root, const std::string& source) {
    const auto& object = asObject(root, source, "$");
    ModelConfig result;
    result.model_version = asString(
        required(object, "model_version", source, "$"), source,
        "$.model_version");
    result.algorithm_version = asString(
        required(object, "algorithm_version", source, "$"), source,
        "$.algorithm_version");
    result.input_schema_version = asInt64(
        required(object, "input_schema_version", source, "$"), source,
        "$.input_schema_version");
    result.calibration_status = asString(
        required(object, "calibration_status", source, "$"), source,
        "$.calibration_status");

    if (result.algorithm_version != "0") {
        fieldError(source, "$.algorithm_version",
                   "only algorithm version 0 is supported");
    }
    if (result.input_schema_version != 0) {
        fieldError(source, "$.input_schema_version",
                   "only input schema version 0 is supported");
    }

    const auto& optimized = asObject(
        required(object, "optimized_parameters", source, "$"), source,
        "$.optimized_parameters");
    result.atomic_content.sigmoid_alpha = positiveNumber(
        required(optimized, "sigmoid_alpha", source,
                 "$.optimized_parameters"),
        source, "$.optimized_parameters.sigmoid_alpha");
    result.evidence_authenticity.sufficiency_gamma = positiveNumber(
        required(optimized, "sufficiency_gamma", source,
                 "$.optimized_parameters"),
        source, "$.optimized_parameters.sufficiency_gamma");

    const auto& causal = asObject(
        required(optimized, "causal_weights", source,
                 "$.optimized_parameters"),
        source, "$.optimized_parameters.causal_weights");
    result.logic_relationship.causal_time_weight = probability(
        required(causal, "time", source,
                 "$.optimized_parameters.causal_weights"),
        source, "$.optimized_parameters.causal_weights.time");
    result.logic_relationship.causal_mechanism_weight = probability(
        required(causal, "mechanism", source,
                 "$.optimized_parameters.causal_weights"),
        source, "$.optimized_parameters.causal_weights.mechanism");
    result.logic_relationship.causal_counterfactual_weight = probability(
        required(causal, "counterfactual", source,
                 "$.optimized_parameters.causal_weights"),
        source, "$.optimized_parameters.causal_weights.counterfactual");
    result.logic_relationship.causal_confounder_weight = probability(
        required(causal, "confounder", source,
                 "$.optimized_parameters.causal_weights"),
        source, "$.optimized_parameters.causal_weights.confounder");
    result.logic_relationship.causal_dose_response_weight = probability(
        required(causal, "dose_response", source,
                 "$.optimized_parameters.causal_weights"),
        source, "$.optimized_parameters.causal_weights.dose_response");
    requireWeightSum(
        {result.logic_relationship.causal_time_weight,
         result.logic_relationship.causal_mechanism_weight,
         result.logic_relationship.causal_counterfactual_weight,
         result.logic_relationship.causal_confounder_weight,
         result.logic_relationship.causal_dose_response_weight},
        source, "$.optimized_parameters.causal_weights");

    const auto& aggregation = asObject(
        required(optimized, "aggregation_weights", source,
                 "$.optimized_parameters"),
        source, "$.optimized_parameters.aggregation_weights");
    result.multi_dimensional.content_weight = probability(
        required(aggregation, "content", source,
                 "$.optimized_parameters.aggregation_weights"),
        source, "$.optimized_parameters.aggregation_weights.content");
    result.multi_dimensional.logic_weight = probability(
        required(aggregation, "logic", source,
                 "$.optimized_parameters.aggregation_weights"),
        source, "$.optimized_parameters.aggregation_weights.logic");
    result.multi_dimensional.sufficiency_weight = probability(
        required(aggregation, "sufficiency", source,
                 "$.optimized_parameters.aggregation_weights"),
        source, "$.optimized_parameters.aggregation_weights.sufficiency");
    result.multi_dimensional.jury_weight = probability(
        required(aggregation, "jury", source,
                 "$.optimized_parameters.aggregation_weights"),
        source, "$.optimized_parameters.aggregation_weights.jury");
    result.multi_dimensional.source_weight = probability(
        required(aggregation, "source", source,
                 "$.optimized_parameters.aggregation_weights"),
        source, "$.optimized_parameters.aggregation_weights.source");
    requireWeightSum(
        {result.multi_dimensional.content_weight,
         result.multi_dimensional.logic_weight,
         result.multi_dimensional.sufficiency_weight,
         result.multi_dimensional.jury_weight,
         result.multi_dimensional.source_weight},
        source, "$.optimized_parameters.aggregation_weights");
    if (result.multi_dimensional.logic_weight > 0.8) {
        fieldError(source, "$.optimized_parameters.aggregation_weights.logic",
                   "must not exceed 0.8");
    }

    const auto& fitted = asObject(
        required(object, "fitted_calibration", source, "$"), source,
        "$.fitted_calibration");
    result.probability_adjustment.temperature = positiveNumber(
        required(fitted, "temperature", source, "$.fitted_calibration"),
        source, "$.fitted_calibration.temperature");
    if (result.probability_adjustment.temperature < 0.1 ||
        result.probability_adjustment.temperature > 5.0) {
        fieldError(source, "$.fitted_calibration.temperature",
                   "must be within [0.1, 5.0]");
    }

    const auto& fixed = asObject(
        required(object, "fixed_parameters", source, "$"), source,
        "$.fixed_parameters");
    result.evidence_authenticity.reference_cluster_count = positiveNumber(
        required(fixed, "reference_cluster_count", source,
                 "$.fixed_parameters"),
        source, "$.fixed_parameters.reference_cluster_count");
    result.evidence_authenticity.time_decay_per_day = nonNegativeNumber(
        required(fixed, "time_decay_per_day", source, "$.fixed_parameters"),
        source, "$.fixed_parameters.time_decay_per_day");
    const double epsilon = positiveNumber(
        required(fixed, "epsilon", source, "$.fixed_parameters"), source,
        "$.fixed_parameters.epsilon");
    if (epsilon >= 0.25) {
        fieldError(source, "$.fixed_parameters.epsilon",
                   "must be less than 0.25");
    }
    result.atomic_content.epsilon = epsilon;
    result.probability_adjustment.epsilon = epsilon;
    result.rejection_trigger.error_budget = probability(
        required(fixed, "conformal_error_budget", source,
                 "$.fixed_parameters"),
        source, "$.fixed_parameters.conformal_error_budget");
    if (result.rejection_trigger.error_budget <= 0.0 ||
        result.rejection_trigger.error_budget >= 1.0) {
        fieldError(source, "$.fixed_parameters.conformal_error_budget",
                   "must be within (0, 1)");
    }

    const auto& baselines = asObject(
        required(fixed, "source_baselines", source, "$.fixed_parameters"),
        source, "$.fixed_parameters.source_baselines");
    static const std::set<std::string> expected_sources{
        "official_registry", "academic", "open_web", "social_media",
        "news_media", "video_or_github", "market_data"};
    std::set<std::string> actual_sources;
    result.evidence_authenticity.source_baselines.clear();
    for (const auto& item : baselines) {
        actual_sources.insert(item.first);
        const SourceType type = parseSourceType(
            item.first, source,
            "$.fixed_parameters.source_baselines." + item.first);
        result.evidence_authenticity.source_baselines[type] = probability(
            item.second, source,
            "$.fixed_parameters.source_baselines." + item.first);
    }
    if (actual_sources != expected_sources) {
        fieldError(source, "$.fixed_parameters.source_baselines",
                   "must contain exactly the seven supported source types");
    }

    const auto& conformal = asObject(
        required(object, "conformal_calibration", source, "$"), source,
        "$.conformal_calibration");
    result.conformal_threshold.is_infinite = asBoolean(
        required(conformal, "is_infinite", source,
                 "$.conformal_calibration"),
        source, "$.conformal_calibration.is_infinite");
    result.conformal_threshold.family_count = asSize(
        required(conformal, "family_count", source,
                 "$.conformal_calibration"),
        source, "$.conformal_calibration.family_count");
    result.conformal_threshold.rank = asSize(
        required(conformal, "rank", source, "$.conformal_calibration"),
        source, "$.conformal_calibration.rank");
    if (result.conformal_threshold.family_count == 0 ||
        result.conformal_threshold.rank == 0) {
        fieldError(source, "$.conformal_calibration",
                   "family_count and rank must be greater than zero");
    }

    const JsonValue& threshold_value =
        required(conformal, "threshold", source, "$.conformal_calibration");
    if (result.conformal_threshold.is_infinite) {
        if (!std::holds_alternative<std::nullptr_t>(threshold_value.value)) {
            fieldError(source, "$.conformal_calibration.threshold",
                       "must be null when is_infinite is true");
        }
        result.conformal_threshold.value =
            std::numeric_limits<double>::infinity();
        if (result.conformal_threshold.rank <=
            result.conformal_threshold.family_count) {
            fieldError(source, "$.conformal_calibration.rank",
                       "must exceed family_count for an infinite threshold");
        }
    } else {
        result.conformal_threshold.value = probability(
            threshold_value, source, "$.conformal_calibration.threshold");
        if (result.conformal_threshold.rank >
            result.conformal_threshold.family_count) {
            fieldError(source, "$.conformal_calibration.rank",
                       "must not exceed family_count for a finite threshold");
        }
    }

    const double raw_rank =
        static_cast<double>(result.conformal_threshold.family_count + 1) *
        (1.0 - result.rejection_trigger.error_budget);
    const std::size_t expected_rank =
        static_cast<std::size_t>(std::ceil(raw_rank - 1e-12));
    if (result.conformal_threshold.rank != expected_rank) {
        fieldError(source, "$.conformal_calibration.rank",
                   "does not match family_count and conformal_error_budget");
    }
    return result;
}

}  // namespace

ModelConfigError::ModelConfigError(const std::string& message)
    : std::runtime_error(message) {}

ModelConfig ModelConfigReader::read(std::istream& input,
                                    const std::string& source_name) {
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw ModelConfigError(source_name + ": failed while reading input");
    }
    return readString(buffer.str(), source_name);
}

ModelConfig ModelConfigReader::readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ModelConfigError("cannot open model config: " + path.string());
    }
    return read(input, path.string());
}

ModelConfig ModelConfigReader::readString(const std::string& json,
                                          const std::string& source_name) {
    Parser parser(json, source_name);
    return parseConfig(parser.parseDocument(), source_name);
}

}  // namespace chongming::qualifier
