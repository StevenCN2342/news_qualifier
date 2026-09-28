#include "json_reader.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <variant>

namespace chongming::qualifier {
namespace {

/*
 * 解析分为两层：
 *
 * 1. Parser 只处理 JSON 语法，将文本转换成下面的 JsonValue 树；
 * 2. parseRequest 及其辅助函数负责协议和业务校验，再转换成评分器类型。
 *
 * 分层的目的，是让“JSON 写错了”和“字段虽然是合法 JSON、但不符合评分
 * 协议”产生不同、可定位的错误。这里没有使用全局状态，因此多个
 * news_qualifier 子进程或同一进程内的多个独立调用之间不会共享解析状态。
 */

/**
 * JSON 的最小内部表示（AST）。
 *
 * JSON 只有六种值：null、boolean、number、string、array、object。数字暂以
 * double 保存，后续 asInteger/asProbability 会根据业务字段继续收紧约束。
 * Object 采用 std::map，主要为了拥有稳定的键存储；协议逻辑不依赖键顺序。
 */
struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    using Storage =
        std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Storage value;
};

/**
 * 不依赖第三方库的递归下降 JSON 解析器。
 *
 * Parser 对输入字符串仅持有引用，因此 Parser 的生命周期不能超过 text。
 * position_ 是字节偏移；line_/column_ 用于向调用方报告易读的语法位置。
 */
class Parser {
public:
    Parser(const std::string& text, std::string source_name)
        : text_(text), source_name_(std::move(source_name)) {}

    JsonValue parseDocument() {
        // 一个完整文档只能有一个根值。根值前后允许 JSON 空白字符，但不允许
        // 拼接第二个 JSON；JSONL 由 readNextJsonLine 在更外层逐行切分。
        skipWhitespace();
        JsonValue result = parseValue();
        skipWhitespace();
        if (!atEnd()) {
            fail("unexpected content after the JSON value");
        }
        return result;
    }

private:
    // text_ 不拥有内存，source_name_ 则自行保存，确保异常构造时仍可使用。
    const std::string& text_;
    std::string source_name_;
    std::size_t position_{0};
    std::size_t line_{1};
    std::size_t column_{1};

    bool atEnd() const noexcept { return position_ >= text_.size(); }

    /** 查看当前字节但不移动游标；EOF 在需要字符的语境下属于语法错误。 */
    char peek() const {
        if (atEnd()) {
            fail("unexpected end of input");
        }
        return text_[position_];
    }

    /** 消费一个字节，并同步维护从 1 开始的行列号。 */
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

    /** 所有语法错误都带上“来源:行:列”，便于定位文件或 JSONL 记录。 */
    [[noreturn]] void fail(const std::string& message) const {
        throw JsonReadError(source_name_ + ":" + std::to_string(line_) + ":" +
                            std::to_string(column_) + ": " + message);
    }

    /** JSON 标准只认可空格、制表、回车和换行四种空白。 */
    void skipWhitespace() {
        while (!atEnd()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            take();
        }
    }

    /** 根据首字节把解析分派到 JSON 六种值类型。 */
    JsonValue parseValue() {
        if (atEnd()) {
            fail("expected a JSON value");
        }
        switch (peek()) {
            case '{':
                return JsonValue{parseObject()};
            case '[':
                return JsonValue{parseArray()};
            case '"':
                return JsonValue{parseString()};
            case 't':
                parseLiteral("true");
                return JsonValue{true};
            case 'f':
                parseLiteral("false");
                return JsonValue{false};
            case 'n':
                parseLiteral("null");
                return JsonValue{nullptr};
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                    return JsonValue{parseNumber()};
                }
                fail("expected an object, array, string, number, boolean, or null");
        }
    }

    /**
     * 解析 object，并拒绝重复键。
     *
     * 重复键在部分 JSON 实现中会“后值覆盖前值”，但评分输入不应有这种歧义，
     * 因此这里将其视为错误。
     */
    JsonValue::Object parseObject() {
        take();  // {
        skipWhitespace();
        JsonValue::Object result;
        if (!atEnd() && peek() == '}') {
            take();
            return result;
        }
        while (true) {
            if (atEnd() || peek() != '"') {
                fail("expected a quoted object key");
            }
            std::string key = parseString();
            skipWhitespace();
            if (take() != ':') {
                fail("expected ':' after object key");
            }
            skipWhitespace();
            JsonValue value = parseValue();
            if (!result.emplace(std::move(key), std::move(value)).second) {
                fail("duplicate object key");
            }
            skipWhitespace();
            const char separator = take();
            if (separator == '}') {
                return result;
            }
            if (separator != ',') {
                fail("expected ',' or '}' in object");
            }
            skipWhitespace();
        }
    }

    /** 解析 array；尾随逗号不属于标准 JSON，因此不会被接受。 */
    JsonValue::Array parseArray() {
        take();  // [
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
            if (separator == ']') {
                return result;
            }
            if (separator != ',') {
                fail("expected ',' or ']' in array");
            }
            skipWhitespace();
        }
    }

    /** 将合法 Unicode 码点编码成 1～4 字节 UTF-8。 */
    static void appendUtf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    /** 将一个十六进制字符转换为数值；-1 表示非法字符。 */
    static int hexValue(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }

    /** 读取 JSON `\uXXXX` 中的四位 UTF-16 code unit。 */
    std::uint32_t parseHexCodeUnit() {
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            if (atEnd()) fail("incomplete Unicode escape");
            const int digit = hexValue(take());
            if (digit < 0) fail("invalid hexadecimal digit in Unicode escape");
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    /**
     * 解析 JSON 字符串和所有标准转义。
     *
     * BMP 以外字符由 UTF-16 高低代理项组成；这里先合并为 Unicode 码点，
     * 再转换为 UTF-8。未转义的 UTF-8 字节会原样保留。
     */
    std::string parseString() {
        take();  // opening quote
        std::string result;
        while (!atEnd()) {
            const unsigned char value = static_cast<unsigned char>(take());
            if (value == '"') return result;
            if (value < 0x20) fail("unescaped control character in string");
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
                    std::uint32_t codepoint = parseHexCodeUnit();
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (atEnd() || take() != '\\' || atEnd() || take() != 'u') {
                            fail("high surrogate must be followed by a low surrogate");
                        }
                        const std::uint32_t low = parseHexCodeUnit();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("invalid low surrogate");
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10U) +
                                    (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        fail("unexpected low surrogate");
                    }
                    appendUtf8(result, codepoint);
                    break;
                }
                default:
                    fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    /** 精确消费 true、false 或 null；不接受前缀或拼写变体。 */
    void parseLiteral(const char* literal) {
        for (const char* cursor = literal; *cursor != '\0'; ++cursor) {
            if (atEnd() || take() != *cursor) {
                fail(std::string("invalid literal; expected ") + literal);
            }
        }
    }

    /**
     * 按 JSON 数字文法解析符号、整数、小数和指数。
     *
     * 这里主动拒绝 01、1.、1e 等非标准写法；std::stod 只负责最终数值转换，
     * 不能替代前面的文法检查。NaN 和 Infinity 也不属于 JSON。
     */
    double parseNumber() {
        const std::size_t start = position_;
        if (peek() == '-') take();
        if (atEnd()) fail("incomplete number");
        if (peek() == '0') {
            take();
            if (!atEnd() && peek() >= '0' && peek() <= '9') {
                fail("leading zero is not allowed in number");
            }
        } else {
            if (peek() < '1' || peek() > '9') fail("invalid number");
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        if (!atEnd() && peek() == '.') {
            take();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("fraction requires at least one digit");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
            take();
            if (!atEnd() && (peek() == '+' || peek() == '-')) take();
            if (atEnd() || peek() < '0' || peek() > '9') {
                fail("exponent requires at least one digit");
            }
            while (!atEnd() && peek() >= '0' && peek() <= '9') take();
        }
        const std::string token = text_.substr(start, position_ - start);
        try {
            std::size_t consumed = 0;
            const double value = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(value)) {
                fail("number is outside the supported finite range");
            }
            return value;
        } catch (const std::exception&) {
            fail("invalid number");
        }
    }
};

/*
 * 以下是协议字段读取工具。path 使用类似 $.claims[0].evidence[1] 的 JSON Path，
 * source 表示文件名、流名或 JSONL 行号。二者组合后，调用方无需查看解析器内部
 * 状态也能定位业务错误。
 */

/** 抛出字段级错误；与 Parser::fail 的语法错误格式保持一致。 */
[[noreturn]] void fieldError(const std::string& source,
                             const std::string& path,
                             const std::string& message) {
    throw JsonReadError(source + ": " + path + ": " + message);
}

/** 要求值为 object；返回指针指向 JsonValue 树内部，不转移所有权。 */
const JsonValue::Object* asObject(const JsonValue& value,
                                 const std::string& source,
                                 const std::string& path) {
    const auto* result = std::get_if<JsonValue::Object>(&value.value);
    if (!result) fieldError(source, path, "expected an object");
    return result;
}

/** 要求值为 array；返回指针指向 JsonValue 树内部，不转移所有权。 */
const JsonValue::Array* asArray(const JsonValue& value,
                               const std::string& source,
                               const std::string& path) {
    const auto* result = std::get_if<JsonValue::Array>(&value.value);
    if (!result) fieldError(source, path, "expected an array");
    return result;
}

/** 读取必填键；缺少字段时将字段名追加到错误路径。 */
const JsonValue& required(const JsonValue::Object& object,
                          const std::string& key,
                          const std::string& source,
                          const std::string& path) {
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        fieldError(source, path + "." + key, "required field is missing");
    }
    return iterator->second;
}

/** 读取可选键；不存在返回 nullptr，存在时仍需由调用方校验其类型。 */
const JsonValue* optional(const JsonValue::Object& object,
                          const std::string& key) {
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

/** 读取字符串；业务 ID 和正文可通过 allow_empty=false 禁止空字符串。 */
std::string asString(const JsonValue& value,
                     const std::string& source,
                     const std::string& path,
                     bool allow_empty = true) {
    const auto* result = std::get_if<std::string>(&value.value);
    if (!result) fieldError(source, path, "expected a string");
    if (!allow_empty && result->empty()) fieldError(source, path, "must not be empty");
    return *result;
}

/** 读取一般有限数字；JSON 语法层已经排除了 NaN 和 Infinity。 */
double asNumber(const JsonValue& value,
                const std::string& source,
                const std::string& path) {
    const auto* result = std::get_if<double>(&value.value);
    if (!result) fieldError(source, path, "expected a number");
    return *result;
}

/** 读取算法概率或分数，并执行 README 统一要求的 [0,1] 边界检查。 */
double asProbability(const JsonValue& value,
                     const std::string& source,
                     const std::string& path) {
    const double result = asNumber(value, source, path);
    if (result < 0.0 || result > 1.0) {
        fieldError(source, path, "must be within [0, 1]");
    }
    return result;
}

/**
 * 读取不含小数部分、且能放入 int64_t 的整数。
 * schemaVersion 和 eventDay 共用此规则；eventDay 的纪元由协议双方约定。
 */
std::int64_t asInteger(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    const double number = asNumber(value, source, path);
    if (std::trunc(number) != number ||
        number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        fieldError(source, path, "expected a 64-bit integer");
    }
    return static_cast<std::int64_t>(number);
}

/** 严格读取 JSON boolean；数字 0/1 不会被隐式当成 false/true。 */
bool asBoolean(const JsonValue& value,
               const std::string& source,
               const std::string& path) {
    const auto* result = std::get_if<bool>(&value.value);
    if (!result) fieldError(source, path, "expected a boolean");
    return *result;
}

/**
 * 字符串到 enum 的统一转换器。
 * 枚举匹配区分大小写，避免拼写错误静默落入某个默认类别。
 */
template <typename Enum>
Enum parseEnum(const JsonValue& value,
               const std::map<std::string, Enum>& names,
               const std::string& source,
               const std::string& path) {
    const std::string name = asString(value, source, path, false);
    const auto iterator = names.find(name);
    if (iterator == names.end()) fieldError(source, path, "unsupported enum value '" + name + "'");
    return iterator->second;
}

/** 来源类型的线协议名称与 C++ enum 的唯一映射表。 */
SourceType parseSourceType(const JsonValue& value,
                           const std::string& source,
                           const std::string& path) {
    static const std::map<std::string, SourceType> names{
        {"OfficialRegistry", SourceType::OfficialRegistry},
        {"Academic", SourceType::Academic},
        {"OpenWeb", SourceType::OpenWeb},
        {"SocialMedia", SourceType::SocialMedia},
        {"NewsMedia", SourceType::NewsMedia},
        {"VideoOrGitHub", SourceType::VideoOrGitHub},
        {"MarketData", SourceType::MarketData},
    };
    return parseEnum(value, names, source, path);
}

/** NLI 支持、反驳和中立三分类的映射表。 */
NliLabel parseNliLabel(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    static const std::map<std::string, NliLabel> names{
        {"Support", NliLabel::Support},
        {"Refute", NliLabel::Refute},
        {"Neutral", NliLabel::Neutral},
    };
    return parseEnum(value, names, source, path);
}

/** README 第三步全部关系类型的映射表。 */
RelationType parseRelationType(const JsonValue& value,
                               const std::string& source,
                               const std::string& path) {
    static const std::map<std::string, RelationType> names{
        {"Temporal", RelationType::Temporal},
        {"Conditional", RelationType::Conditional},
        {"Contrast", RelationType::Contrast},
        {"Parallel", RelationType::Parallel},
        {"And", RelationType::And},
        {"Or", RelationType::Or},
        {"Not", RelationType::Not},
        {"Implication", RelationType::Implication},
        {"Causal", RelationType::Causal},
    };
    return parseEnum(value, names, source, path);
}

/** 陪审团三分类裁决的映射表。 */
JuryVerdict parseJuryVerdict(const JsonValue& value,
                             const std::string& source,
                             const std::string& path) {
    static const std::map<std::string, JuryVerdict> names{
        {"Support", JuryVerdict::Support},
        {"Refute", JuryVerdict::Refute},
        {"Uncertain", JuryVerdict::Uncertain},
    };
    return parseEnum(value, names, source, path);
}

/**
 * 将关键词数组转为集合。
 * 重复关键词由 unordered_set 自动去重，空关键词本身不允许出现。
 */
std::unordered_set<std::string> parseKeywords(const JsonValue& value,
                                               const std::string& source,
                                               const std::string& path) {
    std::unordered_set<std::string> result;
    const auto& array = *asArray(value, source, path);
    for (std::size_t index = 0; index < array.size(); ++index) {
        result.insert(asString(array[index], source,
                               path + "[" + std::to_string(index) + "]", false));
    }
    return result;
}

/**
 * 将一条 evidence JSON 转换为 Evidence。
 *
 * 必填：id、keywords、nliLabel、nliConfidence。
 * 可选：originalSourceId、sourceType、eventDay。sourceType 缺失时保留
 * nullopt，后续可靠性计算会按未知基线处理，而不会冒充任一真实来源类型。
 */
Evidence parseEvidence(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    const auto& object = *asObject(value, source, path);
    Evidence result;
    result.id = asString(required(object, "id", source, path), source,
                         path + ".id", false);
    if (const JsonValue* item = optional(object, "originalSourceId")) {
        result.original_source_id =
            asString(*item, source, path + ".originalSourceId");
    }
    if (const JsonValue* item = optional(object, "sourceType")) {
        result.source_type = parseSourceType(*item, source, path + ".sourceType");
    }
    result.keywords = parseKeywords(required(object, "keywords", source, path),
                                    source, path + ".keywords");
    if (const JsonValue* item = optional(object, "eventDay")) {
        result.event_day = asInteger(*item, source, path + ".eventDay");
    }
    result.nli_label = parseNliLabel(required(object, "nliLabel", source, path),
                                     source, path + ".nliLabel");
    result.nli_confidence = asProbability(
        required(object, "nliConfidence", source, path), source,
        path + ".nliConfidence");
    return result;
}

/**
 * 将声明及其内嵌证据转换为 AtomicClaim。
 *
 * evidence 必须是数组，但允许为空；这代表“已经解析了该声明，但当前没有
 * 候选证据”。同一声明内部的 evidence id 必须唯一，避免审计记录歧义。
 */
AtomicClaim parseClaim(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    const auto& object = *asObject(value, source, path);
    AtomicClaim result;
    result.id = asString(required(object, "id", source, path), source,
                         path + ".id", false);
    result.text = asString(required(object, "text", source, path), source,
                           path + ".text", false);
    result.keywords = parseKeywords(required(object, "keywords", source, path),
                                    source, path + ".keywords");
    if (const JsonValue* item = optional(object, "eventDay")) {
        result.event_day = asInteger(*item, source, path + ".eventDay");
    }
    if (const JsonValue* item = optional(object, "timeSensitive")) {
        result.time_sensitive = asBoolean(*item, source, path + ".timeSensitive");
    }
    const auto& evidence = *asArray(required(object, "evidence", source, path),
                                    source, path + ".evidence");
    std::unordered_map<std::string, bool> evidence_ids;
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        const std::string item_path = path + ".evidence[" + std::to_string(index) + "]";
        Evidence parsed = parseEvidence(evidence[index], source, item_path);
        if (!evidence_ids.emplace(parsed.id, true).second) {
            fieldError(source, item_path + ".id", "duplicate evidence id within claim");
        }
        result.evidence.push_back(std::move(parsed));
    }
    return result;
}

/**
 * 把协议中的稳定 claim id 解析成算法内部数组下标。
 * 关系和陪审团输入只能引用本请求 claims 中已声明的 id。
 */
std::size_t claimIndex(const std::unordered_map<std::string, std::size_t>& indices,
                       const std::string& claim_id,
                       const std::string& source,
                       const std::string& path) {
    const auto iterator = indices.find(claim_id);
    if (iterator == indices.end()) fieldError(source, path, "unknown claim id '" + claim_id + "'");
    return iterator->second;
}

/**
 * 将关系 JSON 转换为 RelationInput。
 *
 * 外部协议使用 leftClaimId/rightClaimId，防止数组重新排序后关系指向错误；
 * 转换完成后保存为当前 claims 数组下标。NOT 是一元关系，因此不要求
 * rightClaimId。
 *
 * 字段约束：
 * - Temporal/Conditional/Contrast/Parallel 在解析成功时必须有 nliScore；
 * - Causal 在解析成功时必须有 causalProbability 和
 *   counterfactualProbability；
 * - And/Or/Not/Implication 直接使用声明分数聚合，不强制额外概率；
 * - parsingSucceeded=false 时后续逻辑层会走多数投票，因此不强制模型分数。
 */
RelationInput parseRelation(
    const JsonValue& value,
    const std::unordered_map<std::string, std::size_t>& indices,
    const std::string& source,
    const std::string& path) {
    const auto& object = *asObject(value, source, path);
    RelationInput result;
    const std::string left_id = asString(
        required(object, "leftClaimId", source, path), source,
        path + ".leftClaimId", false);
    result.left_claim = claimIndex(indices, left_id, source, path + ".leftClaimId");
    result.type = parseRelationType(required(object, "type", source, path),
                                    source, path + ".type");
    if (result.type != RelationType::Not) {
        const std::string right_id = asString(
            required(object, "rightClaimId", source, path), source,
            path + ".rightClaimId", false);
        result.right_claim = claimIndex(indices, right_id, source,
                                        path + ".rightClaimId");
    }
    if (const JsonValue* item = optional(object, "parsingSucceeded")) {
        result.parsing_succeeded =
            asBoolean(*item, source, path + ".parsingSucceeded");
    }
    if (const JsonValue* item = optional(object, "nliScore")) {
        result.nli_score = asProbability(*item, source, path + ".nliScore");
    }
    if (const JsonValue* item = optional(object, "causalProbability")) {
        result.causal_probability =
            asProbability(*item, source, path + ".causalProbability");
    }
    if (const JsonValue* item = optional(object, "counterfactualProbability")) {
        result.counterfactual_probability =
            asProbability(*item, source, path + ".counterfactualProbability");
    }
    if (result.parsing_succeeded && result.type == RelationType::Causal &&
        (!optional(object, "causalProbability") ||
         !optional(object, "counterfactualProbability"))) {
        fieldError(source, path,
                   "a parsed Causal relation requires causalProbability and "
                   "counterfactualProbability");
    }
    const bool uses_nli = result.type == RelationType::Temporal ||
                          result.type == RelationType::Conditional ||
                          result.type == RelationType::Contrast ||
                          result.type == RelationType::Parallel;
    if (result.parsing_succeeded && uses_nli && !optional(object, "nliScore")) {
        fieldError(source, path, "this parsed relation type requires nliScore");
    }
    return result;
}

/**
 * 转换一名陪审员的意见。两个分数均按 [0,1] 校验；推理是否无错误完成使用
 * 独立 boolean，不能由置信度是否为零推断。
 */
JurorOpinion parseOpinion(const JsonValue& value,
                          const std::string& source,
                          const std::string& path) {
    const auto& object = *asObject(value, source, path);
    JurorOpinion result;
    result.verdict = parseJuryVerdict(required(object, "verdict", source, path),
                                      source, path + ".verdict");
    result.hybrid_retrieval_score = asProbability(
        required(object, "hybridRetrievalScore", source, path), source,
        path + ".hybridRetrievalScore");
    result.confidence = asProbability(
        required(object, "confidence", source, path), source,
        path + ".confidence");
    result.completed_without_error = asBoolean(
        required(object, "completedWithoutError", source, path), source,
        path + ".completedWithoutError");
    return result;
}

/**
 * 读取“一条声明 → 多名陪审员意见”的分组。
 * claimId 只负责外部关联，不写入 JurorOpinion，避免每条意见重复存储相同 ID。
 */
ClaimJuryOpinions parseClaimOpinions(
    const JsonValue& value,
    const std::unordered_map<std::string, std::size_t>& indices,
    const std::string& source,
    const std::string& path) {
    const auto& object = *asObject(value, source, path);
    ClaimJuryOpinions result;
    result.claim_id = asString(required(object, "claimId", source, path), source,
                               path + ".claimId", false);
    claimIndex(indices, result.claim_id, source, path + ".claimId");
    const auto& opinions = *asArray(required(object, "opinions", source, path),
                                    source, path + ".opinions");
    for (std::size_t index = 0; index < opinions.size(); ++index) {
        result.opinions.push_back(parseOpinion(
            opinions[index], source,
            path + ".opinions[" + std::to_string(index) + "]"));
    }
    return result;
}

/**
 * 将完整根对象转换为 QualifierRequest。
 *
 * 转换顺序必须是 claims → relations → juryOpinions：后两者都需要第一步创建
 * 的 claim id 索引。relations 和 juryOpinions 缺失时等价于空集合，但
 * schemaVersion、requestId、claims 是协议级必填字段。
 */
QualifierRequest parseRequest(const JsonValue& root, const std::string& source) {
    const auto& object = *asObject(root, source, "$");
    QualifierRequest result;
    result.schema_version = asInteger(
        required(object, "schemaVersion", source, "$"), source,
        "$.schemaVersion");
    if (result.schema_version != 1) {
        fieldError(source, "$.schemaVersion", "only schemaVersion 1 is supported");
    }
    result.request_id = asString(required(object, "requestId", source, "$"), source,
                                 "$.requestId", false);

    const auto& claims = *asArray(required(object, "claims", source, "$"), source,
                                  "$.claims");
    // indices 同时完成声明 ID 唯一性检查，并供关系/陪审团分组解析使用。
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < claims.size(); ++index) {
        const std::string path = "$.claims[" + std::to_string(index) + "]";
        AtomicClaim claim = parseClaim(claims[index], source, path);
        if (!indices.emplace(claim.id, index).second) {
            fieldError(source, path + ".id", "duplicate claim id");
        }
        result.claims.push_back(std::move(claim));
    }

    if (const JsonValue* relations_value = optional(object, "relations")) {
        const auto& relations = *asArray(*relations_value, source, "$.relations");
        for (std::size_t index = 0; index < relations.size(); ++index) {
            result.relations.push_back(parseRelation(
                relations[index], indices, source,
                "$.relations[" + std::to_string(index) + "]"));
        }
    }

    if (const JsonValue* opinions_value = optional(object, "juryOpinions")) {
        const auto& groups = *asArray(*opinions_value, source, "$.juryOpinions");
        // 每条声明最多出现一个分组；一个分组内部可以有任意数量的意见。
        std::unordered_map<std::string, bool> grouped_claims;
        for (std::size_t index = 0; index < groups.size(); ++index) {
            const std::string path = "$.juryOpinions[" + std::to_string(index) + "]";
            ClaimJuryOpinions group =
                parseClaimOpinions(groups[index], indices, source, path);
            if (!grouped_claims.emplace(group.claim_id, true).second) {
                fieldError(source, path + ".claimId",
                           "duplicate jury-opinion group for claim");
            }
            result.jury_opinions.push_back(std::move(group));
        }
    }
    return result;
}

/** JSONL 允许空行；只有四种 JSON 空白字符组成的行才视为空行。 */
bool isBlank(const std::string& value) {
    for (char character : value) {
        if (character != ' ' && character != '\t' && character != '\r' &&
            character != '\n') {
            return false;
        }
    }
    return true;
}

}  // namespace

JsonReadError::JsonReadError(const std::string& message)
    : std::runtime_error(message) {}

QualifierRequest JsonReader::read(std::istream& input,
                                  const std::string& source_name) {
    // 完整 JSON 文档允许跨行，因而先读取到字符串，再交给同一解析入口。
    // 该接口适合单请求文件或一次性子进程 stdin；持续任务流应使用 JSONL 接口。
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        throw JsonReadError(source_name + ": failed while reading input");
    }
    return readString(buffer.str(), source_name);
}

QualifierRequest JsonReader::readFile(const std::filesystem::path& path) {
    // binary 模式阻止 Windows 文本模式改写换行；JSON 解析器本身同时接受 CR/LF。
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw JsonReadError("cannot open JSON file: " + path.string());
    }
    return read(input, path.string());
}

QualifierRequest JsonReader::readString(const std::string& json,
                                        const std::string& source_name) {
    // 先完成纯语法解析，再做 schemaVersion 和各业务字段校验。
    Parser parser(json, source_name);
    return parseRequest(parser.parseDocument(), source_name);
}

std::optional<QualifierRequest> JsonReader::readNextJsonLine(
    std::istream& input,
    std::size_t& line_number,
    const std::string& source_name) {
    // 一次只保留一行，适合父进程持续向子进程写入请求，不会随历史任务数增长内存。
    // line_number 属于协议诊断状态，由调用方保存，从而允许多次调用延续行号。
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        if (isBlank(line)) continue;
        // 每一非空行必须独立成文；Parser 会拒绝同一行拼接多个 JSON 根值。
        return readString(line, source_name + ":line " +
                                    std::to_string(line_number));
    }
    if (input.bad()) {
        throw JsonReadError(source_name + ": failed while reading JSONL input");
    }
    return std::nullopt;
}

}  // namespace chongming::qualifier
