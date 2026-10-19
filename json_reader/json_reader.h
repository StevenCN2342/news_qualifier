#pragma once

#include "qualifier_types.h"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace chongming::qualifier {

/** 一条声明对应的陪审团原始意见；claim_id 用于避免依赖数组顺序。 */
struct ClaimJuryOpinions {
    std::string claim_id;
    std::vector<JurorOpinion> opinions;
};

/**
 * news_qualifier 子进程的一次完整输入。
 *
 * JSON 协议使用稳定的 claim id；JsonReader 会在读取逻辑边时把 id 转换为
 * 算法所需的 claims 数组下标。声明和逻辑边数量由材料决定，不在解析层设置
 * 经验性上下限。当前只接受预处理 agent 输出的 schema_version=3。
 */
struct QualifierRequest {
    std::int64_t schema_version{0};
    std::string request_id;
    std::string event_id;
    std::string original_text;
    std::string source;
    std::string publish_date;
    std::string event_nature;
    std::string domain;
    std::string event_time;
    SevenElements seven_elements;
    std::vector<AtomicClaim> claims;
    std::vector<RelationInput> relations;
    std::vector<AggregationInput> aggregations;
    std::vector<ClaimJuryOpinions> jury_opinions;
};

/** JSON 语法、字段类型或业务约束不合法时抛出的异常。 */
class JsonReadError : public std::runtime_error {
public:
    explicit JsonReadError(const std::string& message);
};

/**
 * 将 JSON/JSONL 输入转换为评分算法的数据类型。
 *
 * 本类只负责读取和校验，不运行评分算法，也不生成输出 JSON。所有接口都是
 * 无状态的，可由 ChongMing 启动的多个独立子进程并行调用。
 */
class JsonReader {
public:
    /** 读取一个完整 JSON 文档；流中除空白外不能包含第二个值。 */
    static QualifierRequest read(
        std::istream& input,
        const std::string& source_name = "<stream>");

    /** 从 UTF-8 文件读取一个完整 JSON 文档。 */
    static QualifierRequest readFile(const std::filesystem::path& path);

    /** 从字符串读取一个完整 JSON 文档，便于进程协议和单元测试复用。 */
    static QualifierRequest readString(
        const std::string& json,
        const std::string& source_name = "<string>");

    /**
     * 从 JSONL 流读取下一条非空记录。
     *
     * line_number 由调用方初始化为 0，本函数每读取一行便递增；到达 EOF
     * 返回 nullopt。每行必须是独立、完整的 JSON 对象，不支持跨行记录。
     */
    static std::optional<QualifierRequest> readNextJsonLine(
        std::istream& input,
        std::size_t& line_number,
        const std::string& source_name = "<jsonl>");
};

}  // namespace chongming::qualifier
