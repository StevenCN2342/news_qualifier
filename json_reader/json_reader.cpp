#include "json_reader.h"

#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>
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

/** 返回可选字段指针；缺少时返回 nullptr。 */
const JsonValue* optionalField(const JsonValue::Object& object,
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
 * schema_version 和 event_day 共用此规则；event_day 的纪元由协议双方约定。
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

/** 来源类型的标准蛇形名称与 C++ enum 的唯一映射表。 */
SourceType parseSourceType(const JsonValue& value,
                           const std::string& source,
                           const std::string& path) {
    static const std::map<std::string, SourceType> names{
        {"official_registry", SourceType::OfficialRegistry},
        {"academic", SourceType::Academic},
        {"open_web", SourceType::OpenWeb},
        {"social_media", SourceType::SocialMedia},
        {"news_media", SourceType::NewsMedia},
        {"video_or_github", SourceType::VideoOrGitHub},
        {"market_data", SourceType::MarketData},
    };
    return parseEnum(value, names, source, path);
}

/** NLI 支持、反驳和中立三分类的映射表。 */
NliLabel parseNliLabel(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    static const std::map<std::string, NliLabel> names{
        {"support", NliLabel::Support},
        {"refute", NliLabel::Refute},
        {"neutral", NliLabel::Neutral},
    };
    return parseEnum(value, names, source, path);
}

/** 原子声明拆分规则规定的声明类型；保留规则原文中的中文标准词。 */
ClaimType parseClaimType(const JsonValue& value,
                         const std::string& source,
                         const std::string& path) {
    static const std::map<std::string, ClaimType> names{
        {"数值型", ClaimType::Numeric}, {"时间型", ClaimType::Temporal},
        {"身份型", ClaimType::Identity}, {"地点型", ClaimType::Location},
        {"存在型", ClaimType::Existence}, {"因果型", ClaimType::Causal},
    };
    return parseEnum(value, names, source, path);
}

/** 陪审团三分类裁决的映射表。 */
JuryVerdict parseJuryVerdict(const JsonValue& value,
                             const std::string& source,
                             const std::string& path) {
    static const std::map<std::string, JuryVerdict> names{
        {"support", JuryVerdict::Support},
        {"refute", JuryVerdict::Refute},
        {"uncertain", JuryVerdict::Uncertain},
    };
    return parseEnum(value, names, source, path);
}

/** 第一版 sandbox 的四种陪审员角色。 */
JurorRole parseJurorRole(const JsonValue& value,
                         const std::string& source,
                         const std::string& path) {
    static const std::map<std::string, JurorRole> names{
        {"strict_legal", JurorRole::StrictLegal},
        {"open_web_pragmatist", JurorRole::OpenWebPragmatist},
        {"causal_skeptic", JurorRole::CausalSkeptic},
        {"anti_manipulation", JurorRole::AntiManipulation},
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

/** 读取字符串数组；保留输入顺序并拒绝空字符串。 */
std::vector<std::string> parseStringArray(const JsonValue& value,
                                          const std::string& source,
                                          const std::string& path) {
    std::vector<std::string> result;
    const auto& array = *asArray(value, source, path);
    for (std::size_t index = 0; index < array.size(); ++index) {
        result.push_back(asString(array[index], source,
                                  path + "[" + std::to_string(index) + "]", false));
    }
    return result;
}

/** 读取规则允许为 null 的字符串；键本身仍必须存在。 */
std::optional<std::string> parseNullableString(const JsonValue& value,
                                               const std::string& source,
                                               const std::string& path) {
    if (std::holds_alternative<std::nullptr_t>(value.value)) return std::nullopt;
    return asString(value, source, path, false);
}

/** 七个中文键必须全部出现；没有对应语义时值为 JSON null。 */
SevenElements parseSevenElements(const JsonValue& value,
                                 const std::string& source,
                                 const std::string& path) {
    const auto& object = *asObject(value, source, path);
    SevenElements result;
    result.subject = parseNullableString(required(object, "主体", source, path),
                                         source, path + ".主体");
    result.action = parseNullableString(required(object, "行为", source, path),
                                        source, path + ".行为");
    result.object = parseNullableString(required(object, "客体", source, path),
                                        source, path + ".客体");
    result.time = parseNullableString(required(object, "时间", source, path),
                                      source, path + ".时间");
    result.location = parseNullableString(required(object, "地点", source, path),
                                          source, path + ".地点");
    result.quantity = parseNullableString(required(object, "数量", source, path),
                                          source, path + ".数量");
    result.result = parseNullableString(required(object, "结果", source, path),
                                        source, path + ".结果");
    return result;
}

/** 将预处理 agent 标准化后的一条 evidence 转换为 Evidence。 */
Evidence parseEvidence(const JsonValue& value,
                       const std::string& source,
                       const std::string& path) {
    const auto& object = *asObject(value, source, path);
    Evidence result;
    result.id = asString(required(object, "evidence_id", source, path), source,
                         path + ".evidence_id", false);
    result.original_source_id = asString(
        required(object, "original_source_id", source, path), source,
        path + ".original_source_id");
    const JsonValue source_type = required(object, "source_type", source, path);
    if (!std::holds_alternative<std::nullptr_t>(source_type.value)) {
        result.source_type = parseSourceType(source_type, source,
                                             path + ".source_type");
    }
    result.keywords = parseKeywords(required(object, "keywords", source, path),
                                    source, path + ".keywords");
    const JsonValue event_day = required(object, "event_day", source, path);
    if (!std::holds_alternative<std::nullptr_t>(event_day.value)) {
        result.event_day = asInteger(event_day, source, path + ".event_day");
    }
    result.nli_label = parseNliLabel(required(object, "nli_label", source, path),
                                     source, path + ".nli_label");
    result.nli_confidence = asProbability(
        required(object, "nli_confidence", source, path), source,
        path + ".nli_confidence");
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
    result.id = asString(required(object, "claim_id", source, path), source,
                         path + ".claim_id", false);
    result.text = asString(required(object, "claim_text", source, path), source,
                           path + ".claim_text", false);
    result.seven_elements = parseSevenElements(
        required(object, "seven_elements", source, path), source,
        path + ".seven_elements");
    result.check_question = asString(
        required(object, "check_question", source, path), source,
        path + ".check_question", false);
    result.claim_type = parseClaimType(required(object, "claim_type", source, path),
                                       source, path + ".claim_type");
    if (result.claim_type == ClaimType::Causal) {
        fieldError(source, path + ".claim_type",
                   "a causal statement must be split into two claims and a "
                   "causal logic edge");
    }
    result.entities = parseStringArray(required(object, "entities", source, path),
                                       source, path + ".entities");
    result.canon_refs = parseStringArray(required(object, "canon_refs", source, path),
                                         source, path + ".canon_refs");
    result.keywords = parseKeywords(required(object, "keywords", source, path),
                                    source, path + ".keywords");
    const JsonValue event_day = required(object, "event_day", source, path);
    if (!std::holds_alternative<std::nullptr_t>(event_day.value)) {
        result.event_day = asInteger(event_day, source, path + ".event_day");
    }
    result.time_sensitive = asBoolean(
        required(object, "time_sensitive", source, path), source,
        path + ".time_sensitive");
    const auto& evidence = *asArray(required(object, "evidence", source, path),
                                    source, path + ".evidence");
    std::unordered_map<std::string, bool> evidence_ids;
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        const std::string item_path = path + ".evidence[" + std::to_string(index) + "]";
        Evidence parsed = parseEvidence(evidence[index], source, item_path);
        if (!evidence_ids.emplace(parsed.id, true).second) {
            fieldError(source, item_path + ".evidence_id",
                       "duplicate evidence id within claim");
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

/** 把 S_* 标准名转换为算法枚举；不在解析器内猜测同义词。 */
RelationDimension parseDimension(const JsonValue& value,
                                 const std::string& source,
                                 const std::string& path) {
    static const std::map<std::string, RelationDimension> names{
        {"S_time", RelationDimension::Time},
        {"S_mech", RelationDimension::Mechanism},
        {"S_counter", RelationDimension::Counterfactual},
        {"S_confound", RelationDimension::Confounder},
        {"S_dose", RelationDimension::DoseResponse},
        {"S_contrast", RelationDimension::Contrast},
        {"S_coordinate", RelationDimension::Coordinate},
    };
    return parseEnum(value, names, source, path);
}

std::vector<RelationDimension> parseDimensions(const JsonValue& value,
                                               const std::string& source,
                                               const std::string& path) {
    std::vector<RelationDimension> result;
    std::set<RelationDimension> unique;
    const auto& array = *asArray(value, source, path);
    for (std::size_t index = 0; index < array.size(); ++index) {
        RelationDimension item = parseDimension(
            array[index], source, path + "[" + std::to_string(index) + "]");
        if (!unique.insert(item).second) {
            fieldError(source, path, "required_dimensions contains a duplicate");
        }
        result.push_back(item);
    }
    return result;
}

void requireDimensions(const std::vector<RelationDimension>& actual,
                       std::initializer_list<RelationDimension> expected,
                       const std::string& source,
                       const std::string& path) {
    if (std::set<RelationDimension>(actual.begin(), actual.end()) !=
        std::set<RelationDimension>(expected.begin(), expected.end())) {
        fieldError(source, path,
                   "dimensions do not exactly match those required by rel");
    }
}

double dimensionScore(const JsonValue::Object& scores,
                      const std::string& key,
                      const std::string& source,
                      const std::string& path) {
    return asProbability(required(scores, key, source, path), source,
                         path + "." + key);
}

/** dimension_scores 只允许出现当前关系实际需要的键。 */
void requireScoreKeys(const JsonValue::Object& scores,
                      std::initializer_list<std::string> expected,
                      const std::string& source,
                      const std::string& path) {
    std::set<std::string> actual;
    for (const auto& item : scores) actual.insert(item.first);
    if (actual != std::set<std::string>(expected.begin(), expected.end())) {
        fieldError(source, path,
                   "score keys do not exactly match required_dimensions");
    }
}

/**
 * 将一条标准 logic_edges 记录路由到语义关系或因果聚合。
 * required_dimensions 与 rel 必须严格匹配，避免维度遗漏后仍进入算法。
 */
void parseLogicEdge(const JsonValue& value,
                    const std::unordered_map<std::string, std::size_t>& indices,
                    const std::string& source,
                    const std::string& path,
                    QualifierRequest& request) {
    const auto& object = *asObject(value, source, path);
    const std::string edge_id = asString(
        required(object, "edge_id", source, path), source, path + ".edge_id", false);
    const std::string left_id = asString(
        required(object, "claim_i", source, path), source, path + ".claim_i", false);
    const std::string right_id = asString(
        required(object, "claim_j", source, path), source, path + ".claim_j", false);
    const std::size_t left = claimIndex(indices, left_id, source, path + ".claim_i");
    const std::size_t right = claimIndex(indices, right_id, source, path + ".claim_j");
    if (left == right) fieldError(source, path, "a logic edge must connect two claims");

    const std::string rel = asString(required(object, "rel", source, path),
                                     source, path + ".rel", false);
    const auto dimensions = parseDimensions(
        required(object, "required_dimensions", source, path), source,
        path + ".required_dimensions");
    const auto& scores = *asObject(
        required(object, "dimension_scores", source, path), source,
        path + ".dimension_scores");
    const double nli_score = asProbability(
        required(object, "nli_score", source, path), source, path + ".nli_score");

    if (rel == "causal") {
        requireDimensions(dimensions,
                          {RelationDimension::Time, RelationDimension::Mechanism,
                           RelationDimension::Counterfactual,
                           RelationDimension::Confounder,
                           RelationDimension::DoseResponse},
                          source, path + ".required_dimensions");
        requireScoreKeys(scores,
                         {"S_time", "S_mech", "S_counter", "S_confound",
                          "S_dose"},
                         source, path + ".dimension_scores");
        AggregationInput result;
        result.edge_id = edge_id;
        result.left_claim = left;
        result.right_claim = right;
        result.type = AggregationType::Causal;
        result.required_dimensions = dimensions;
        result.nli_score = nli_score;
        result.causal_factors.temporal_order = dimensionScore(
            scores, "S_time", source, path + ".dimension_scores");
        result.causal_factors.mechanism = dimensionScore(
            scores, "S_mech", source, path + ".dimension_scores");
        result.causal_factors.counterfactual_support = dimensionScore(
            scores, "S_counter", source, path + ".dimension_scores");
        result.causal_factors.confounder_control = dimensionScore(
            scores, "S_confound", source, path + ".dimension_scores");
        result.causal_factors.dose_response = dimensionScore(
            scores, "S_dose", source, path + ".dimension_scores");
        request.aggregations.push_back(std::move(result));
        return;
    }

    RelationInput result;
    result.edge_id = edge_id;
    result.left_claim = left;
    result.right_claim = right;
    result.required_dimensions = dimensions;
    result.nli_score = nli_score;
    if (rel == "temporal") {
        result.type = RelationType::Temporal;
        requireDimensions(dimensions, {RelationDimension::Time}, source,
                          path + ".required_dimensions");
        requireScoreKeys(scores, {"S_time"}, source,
                         path + ".dimension_scores");
        result.dimension_scores.temporal_order = dimensionScore(
            scores, "S_time", source, path + ".dimension_scores");
    } else if (rel == "conditional") {
        result.type = RelationType::Conditional;
        requireDimensions(dimensions,
                          {RelationDimension::Mechanism,
                           RelationDimension::Counterfactual},
                          source, path + ".required_dimensions");
        requireScoreKeys(scores, {"S_mech", "S_counter"}, source,
                         path + ".dimension_scores");
        result.dimension_scores.mechanism = dimensionScore(
            scores, "S_mech", source, path + ".dimension_scores");
        result.dimension_scores.counterfactual_support = dimensionScore(
            scores, "S_counter", source, path + ".dimension_scores");
    } else if (rel == "contrast") {
        result.type = RelationType::Contrast;
        requireDimensions(dimensions, {RelationDimension::Contrast}, source,
                          path + ".required_dimensions");
        requireScoreKeys(scores, {"S_contrast"}, source,
                         path + ".dimension_scores");
        result.dimension_scores.contrast = dimensionScore(
            scores, "S_contrast", source, path + ".dimension_scores");
    } else if (rel == "coordinate") {
        result.type = RelationType::Parallel;
        requireDimensions(dimensions, {RelationDimension::Coordinate}, source,
                          path + ".required_dimensions");
        requireScoreKeys(scores, {"S_coordinate"}, source,
                         path + ".dimension_scores");
        result.dimension_scores.coordinate = dimensionScore(
            scores, "S_coordinate", source, path + ".dimension_scores");
    } else {
        fieldError(source, path + ".rel", "unsupported enum value '" + rel + "'");
    }
    request.relations.push_back(std::move(result));
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
    const JsonValue* agent_id = optionalField(object, "agent_id");
    const JsonValue* agent_role = optionalField(object, "agent_role");
    const JsonValue* model_id = optionalField(object, "model_id");
    const JsonValue* prompt_version = optionalField(object, "prompt_version");
    const int audit_field_count = (agent_id ? 1 : 0) + (agent_role ? 1 : 0) +
                                  (model_id ? 1 : 0) +
                                  (prompt_version ? 1 : 0);
    if (audit_field_count != 0 && audit_field_count != 4) {
        fieldError(source, path,
                   "agent_id, agent_role, model_id and prompt_version must "
                   "either all be present or all be absent");
    }
    if (audit_field_count == 4) {
        result.agent_id = asString(*agent_id, source, path + ".agent_id", false);
        result.agent_role = parseJurorRole(
            *agent_role, source, path + ".agent_role");
        result.model_id = asString(*model_id, source, path + ".model_id", false);
        result.prompt_version = asString(
            *prompt_version, source, path + ".prompt_version", false);
    }
    result.verdict = parseJuryVerdict(required(object, "verdict", source, path),
                                      source, path + ".verdict");
    result.hybrid_retrieval_score = asProbability(
        required(object, "hybrid_retrieval_score", source, path), source,
        path + ".hybrid_retrieval_score");
    result.confidence = asProbability(
        required(object, "confidence", source, path), source,
        path + ".confidence");
    result.completed_without_error = asBoolean(
        required(object, "completed_without_error", source, path), source,
        path + ".completed_without_error");
    return result;
}

/**
 * 读取“一条声明 → 多名陪审员意见”的分组。
 * claim_id 只负责外部关联，不写入 JurorOpinion，避免每条意见重复存储相同 ID。
 */
ClaimJuryOpinions parseClaimOpinions(
    const JsonValue& value,
    const std::unordered_map<std::string, std::size_t>& indices,
    const std::string& source,
    const std::string& path) {
    const auto& object = *asObject(value, source, path);
    ClaimJuryOpinions result;
    result.claim_id = asString(required(object, "claim_id", source, path), source,
                               path + ".claim_id", false);
    claimIndex(indices, result.claim_id, source, path + ".claim_id");
    const auto& opinions = *asArray(required(object, "opinions", source, path),
                                    source, path + ".opinions");
    std::set<std::string> agent_ids;
    std::set<JurorRole> agent_roles;
    for (std::size_t index = 0; index < opinions.size(); ++index) {
        JurorOpinion opinion = parseOpinion(
            opinions[index], source,
            path + ".opinions[" + std::to_string(index) + "]");
        if (opinion.agent_id && !agent_ids.insert(*opinion.agent_id).second) {
            fieldError(source,
                       path + ".opinions[" + std::to_string(index) +
                           "].agent_id",
                       "duplicate agent_id within claim");
        }
        if (opinion.agent_role &&
            !agent_roles.insert(*opinion.agent_role).second) {
            fieldError(source,
                       path + ".opinions[" + std::to_string(index) +
                           "].agent_role",
                       "duplicate agent_role within claim");
        }
        result.opinions.push_back(std::move(opinion));
    }
    return result;
}

/**
 * 将完整根对象转换为 QualifierRequest。
 *
 * 转换顺序必须从 atomic_claims 开始：logic_edges 和 jury_opinions 都需要
 * 第一阶段创建的 claim id 索引。schema_version=3 表示输入已经过预处理
 * agent 的同义词标准化与评分字段补齐。
 */
QualifierRequest parseRequest(const JsonValue& root, const std::string& source) {
    const auto& object = *asObject(root, source, "$");
    QualifierRequest result;
    result.schema_version = asInteger(
        required(object, "schema_version", source, "$"), source,
        "$.schema_version");
    if (result.schema_version != 3) {
        fieldError(source, "$.schema_version",
                   "only schema_version 3 is supported");
    }
    result.request_id = asString(required(object, "request_id", source, "$"), source,
                                 "$.request_id", false);
    result.event_id = asString(required(object, "event_id", source, "$"), source,
                               "$.event_id", false);
    result.original_text = asString(required(object, "original_text", source, "$"),
                                    source, "$.original_text", false);
    result.source = asString(required(object, "source", source, "$"), source,
                             "$.source", false);
    result.publish_date = asString(required(object, "publish_date", source, "$"),
                                   source, "$.publish_date", false);
    result.event_nature = asString(required(object, "event_nature", source, "$"),
                                   source, "$.event_nature", false);
    result.domain = asString(required(object, "domain", source, "$"), source,
                             "$.domain", false);
    result.event_time = asString(required(object, "event_time", source, "$"), source,
                                 "$.event_time", false);
    result.seven_elements = parseSevenElements(
        required(object, "seven_elements", source, "$"), source,
        "$.seven_elements");

    const auto& claims = *asArray(
        required(object, "atomic_claims", source, "$"), source,
        "$.atomic_claims");
    if (claims.size() < 2 || claims.size() > 5) {
        fieldError(source, "$.atomic_claims",
                   "the splitting rule requires 2 to 5 claims");
    }
    // indices 同时完成声明 ID 唯一性检查，并供关系/陪审团分组解析使用。
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < claims.size(); ++index) {
        const std::string path = "$.atomic_claims[" + std::to_string(index) + "]";
        AtomicClaim claim = parseClaim(claims[index], source, path);
        if (!indices.emplace(claim.id, index).second) {
            fieldError(source, path + ".claim_id", "duplicate claim id");
        }
        result.claims.push_back(std::move(claim));
    }

    const auto& edges = *asArray(
        required(object, "logic_edges", source, "$"), source, "$.logic_edges");
    if (edges.empty() || edges.size() > 4) {
        fieldError(source, "$.logic_edges",
                   "the splitting rule requires 1 to 4 logic edges");
    }
    std::set<std::string> edge_ids;
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const std::string path = "$.logic_edges[" + std::to_string(index) + "]";
        const auto& edge_object = *asObject(edges[index], source, path);
        const std::string edge_id = asString(
            required(edge_object, "edge_id", source, path), source,
            path + ".edge_id", false);
        if (!edge_ids.insert(edge_id).second) {
            fieldError(source, path + ".edge_id", "duplicate edge id");
        }
        parseLogicEdge(edges[index], indices, source, path, result);
    }

    const auto& groups = *asArray(
        required(object, "jury_opinions", source, "$"), source,
        "$.jury_opinions");
    // 每条声明最多出现一个分组；没有意见时仍可提交空数组，供第四层识别缺失。
    std::unordered_map<std::string, bool> grouped_claims;
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const std::string path = "$.jury_opinions[" + std::to_string(index) + "]";
        ClaimJuryOpinions group =
            parseClaimOpinions(groups[index], indices, source, path);
        if (!grouped_claims.emplace(group.claim_id, true).second) {
            fieldError(source, path + ".claim_id",
                       "duplicate jury-opinion group for claim");
        }
        result.jury_opinions.push_back(std::move(group));
    }
    if (grouped_claims.size() != result.claims.size()) {
        fieldError(source, "$.jury_opinions",
                   "every atomic claim requires one jury group; use an empty "
                   "opinions array when no jury output is available");
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
    // 先完成纯语法解析，再做 schema_version 和各业务字段校验。
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
