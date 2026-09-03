// 【Stage 0/1】对话历史的序列化。
//
// 提示：std::visit + overloaded 惯用法能让 to_json 写得很干净：
//
//     template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
//     std::visit(overloaded{
//         [](const TextBlock& b) { return Json{{"type","text"},{"text",b.text}}; },
//         [](const ToolUseBlock& b) { ... },
//         ...
//     }, block);
//
// 加一种块类型时，漏了分支会编译报错 —— 这是 variant 相对于继承的主要好处。

#include "mini_agent/message.hpp"

#include <algorithm>
#include <utility>

namespace {

// 只在本文件可见（匿名 namespace = 内部链接）。模板不能在函数体内声明，
// 所以它必须待在这里，不能塞进 to_json 里面。
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

namespace mini {

Json to_json(const ContentBlock& block) {
    // 每个 lambda 都显式写 -> Json：不写的话各自推导返回类型，std::visit
    // 要求所有分支类型一致，否则会炸出一屏看不懂的模板报错。
    return std::visit(
        overloaded{
            [](const TextBlock& b) -> Json {
                return {{"type", "text"}, {"text", b.text}};
            },
            [](const ThinkingBlock& b) -> Json {
                // signature 必须原样带回，API 会校验
                return {{"type", "thinking"},
                        {"thinking", b.thinking},
                        {"signature", b.signature}};
            },
            [](const RedactedThinkingBlock& b) -> Json {
                return {{"type", "redacted_thinking"}, {"data", b.data}};
            },
            [](const ToolUseBlock& b) -> Json {
                return {{"type", "tool_use"},
                        {"id", b.id},
                        {"name", b.name},
                        {"input", b.input}};
            },
            [](const ToolResultBlock& b) -> Json {
                Json j{{"type", "tool_result"},
                       {"tool_use_id", b.tool_use_id},
                       {"content", b.content}};
                if (b.is_error) j["is_error"] = true;  // 省略即 false
                return j;
            },
        },
        block);
}

Json to_json(const Message& msg) {
    Json blocks = Json::array();
    for (const auto& b : msg.content) blocks.push_back(to_json(b));
    return {{"role", to_string(msg.role)}, {"content", blocks}};
}

Json to_json(const std::vector<Message>& msgs) {
    Json out = Json::array();
    for (const auto& m : msgs) out.push_back(to_json(m));
    return out;
}

std::optional<ContentBlock> block_from_json(const Json& j) {
    // at() 用于必需字段（缺了就抛）；value() 用于可选字段（缺了给默认值）。
    const auto type = j.value("type", std::string{});

    if (type == "text") {
        return TextBlock{.text = j.value("text", std::string{})};
    }
    if (type == "thinking") {
        return ThinkingBlock{.thinking = j.at("thinking").get<std::string>(),
                             .signature = j.at("signature").get<std::string>()};
    }
    if (type == "redacted_thinking") {
        return RedactedThinkingBlock{.data = j.at("data").get<std::string>()};
    }
    if (type == "tool_use") {
        return ToolUseBlock{.id = j.at("id").get<std::string>(),
                            .name = j.at("name").get<std::string>(),
                            .input = j.value("input", Json::object())};
    }
    if (type == "tool_result") {
        return ToolResultBlock{.tool_use_id = j.at("tool_use_id").get<std::string>(),
                               .content = j.value("content", std::string{}),
                               .is_error = j.value("is_error", false)};
    }

    // 未知 type —— 丢弃。API 以后会加新块类型；造一个空 TextBlock 顶替
    // 等于把没发生过的话写进历史。
    return std::nullopt;
}

Message message_from_json(const Json& j) {
    Message msg;
    msg.role = j.value("role", std::string{}) == "user" ? Role::User : Role::Assistant;
    if (auto it = j.find("content"); it != j.end() && it->is_array()) {
        for (const auto& b : *it) {
            if (auto block = block_from_json(b)) msg.content.push_back(std::move(*block));
        }
    }
    return msg;
}

std::vector<Message> messages_from_json(const Json& j) {
    std::vector<Message> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& m : j) out.push_back(message_from_json(m));
    return out;
}

std::string text_of(const Message& msg) {
    std::string out;
    for (const auto& b : msg.content) {
        if (const auto* t = std::get_if<TextBlock>(&b)) out += t->text;
    }
    return out;
}

bool has_tool_result(const Message& msg) {
    return std::ranges::any_of(msg.content, [](const ContentBlock& b) {
        return std::holds_alternative<ToolResultBlock>(b);
    });
}

std::string_view to_string(Role role) {
    // 字符串字面量有静态存储期，返回 string_view 指向它是安全的。
    return role == Role::User ? "user" : "assistant";
}

}  // namespace mini
