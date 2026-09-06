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

/// @brief Fuse several lambdas into one object with an overloaded call operator.
///
/// std::visit wants a single callable that accepts every alternative. Inheriting
/// from each closure type collects their operator()s, and the using-declaration
/// is what makes them a single overload set — without it, name lookup finds the
/// same name in several base classes and reports an ambiguity instead of
/// resolving by argument type.
///
/// @note Lives in an anonymous namespace: internal linkage, and a note to the
///       reader that it is a local detail. The standard library has no
///       equivalent (P0051 was never adopted), so every file that wants it
///       declares its own.
/// @note It cannot move inside to_json — templates may not be declared at
///       block scope.
///
/// @code
/// std::visit(overloaded{
///     [](const TextBlock& b)    -> Json { return {{"type","text"}, ...}; },
///     [](const ToolUseBlock& b) -> Json { return {{"type","tool_use"}, ...}; },
///     // omit one alternative and this stops compiling
/// }, block);
/// @endcode
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

}  // namespace

namespace mini {

/// @brief Serialise one content block into its wire form.
///
/// @return An object tagged with `type`, shaped as the Messages API expects.
///
/// @note Each lambda spells out `-> Json`. Left to deduce, the branches would
///       settle on different types and std::visit — which needs one common
///       return type — fails with a wall of template diagnostics.
/// @note `is_error` is emitted only when true. Absent means false on the wire,
///       and sending `"is_error": false` is not what the API expects.
///
/// @code
/// to_json(ContentBlock{TextBlock{"hi"}});
/// // {"type":"text","text":"hi"}
///
/// to_json(ContentBlock{ToolResultBlock{"toolu_1", "output", false}});
/// // {"type":"tool_result","tool_use_id":"toolu_1","content":"output"}
///
/// to_json(ContentBlock{ToolResultBlock{"toolu_1", "boom", true}});
/// // {"type":"tool_result","tool_use_id":"toolu_1","content":"boom","is_error":true}
/// @endcode
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

/// @brief Serialise one message.
/// @return `{"role": "user"|"assistant", "content": [...]}`
Json to_json(const Message& msg) {
    Json blocks = Json::array();
    for (const auto& b : msg.content) blocks.push_back(to_json(b));
    return {{"role", to_string(msg.role)}, {"content", blocks}};
}

/// @brief Serialise a whole history into the request's `messages` array.
///
/// @note This is the part of the request that grows every turn, which is why it
///       sits after system and tools: the cache prefix matches from the front,
///       so the growing section has to come last.
Json to_json(const std::vector<Message>& msgs) {
    Json out = Json::array();
    for (const auto& m : msgs) out.push_back(to_json(m));
    return out;
}

/// @brief Parse one block back from the wire.
///
/// @param j A content block object; anything without a known `type` is refused.
/// @return nullopt for an unrecognised type.
///
/// @note Returning nullopt rather than substituting an empty TextBlock: the API
///       will grow new block types, and inventing content would write words into
///       the history that the model never produced. Callers skip the nullopts.
///
/// @code
/// block_from_json(Json{{"type","text"},{"text","hi"}});      // TextBlock{"hi"}
/// block_from_json(Json{{"type","something_new"}});           // nullopt
/// @endcode
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

/// @brief Parse one message; blocks this build does not recognise are dropped.
///
/// @note An unknown `role` falls back to Assistant. A message is either one or
///       the other, and mislabelling a user turn as assistant is the more
///       confusing failure.
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

/// @brief Parse a history array — used when resuming a session from disk.
///
/// @note Non-array input yields an empty history rather than throwing. A
///       corrupted session file should degrade to a fresh start, not stop the
///       agent from launching.
std::vector<Message> messages_from_json(const Json& j) {
    std::vector<Message> out;
    if (!j.is_array()) return out;
    out.reserve(j.size());
    for (const auto& m : j) out.push_back(message_from_json(m));
    return out;
}

/// @brief Concatenate every TextBlock in a message.
///
/// @return Empty whenever the message holds no text — which is routine: an
///         assistant turn that only calls tools has none, and neither does a
///         tool_result message.
///
/// @note Thinking blocks are skipped on purpose. Reasoning is not the answer,
///       and folding it into the visible text would show the user both.
/// @note Several TextBlocks are joined without a separator. The API sends at
///       most one per turn in practice, so this has not mattered yet.
///
/// @code
/// text_of({Role::Assistant, {ThinkingBlock{"hmm","sig"}, TextBlock{"Hello"}}});
/// // "Hello"   — the thinking is not included
/// text_of({Role::Assistant, {ToolUseBlock{"t1","read",{}}}});
/// // ""        — no text at all
/// @endcode
std::string text_of(const Message& msg) {
    std::string out;
    for (const auto& b : msg.content) {
        if (const auto* t = std::get_if<TextBlock>(&b)) out += t->text;
    }
    return out;
}

/// @brief Does this message carry a tool_result block?
///
/// @note Tells a genuine user turn apart from a tool result, both of which are
///       Role::User. Session::safe_split cuts the history only at the former:
///       splitting on a tool_result leaves it referencing a tool_use that has
///       gone into the summary, and the next request is rejected.
///
/// @code
/// has_tool_result({Role::User, {TextBlock{"look at a.py"}}});        // false
/// has_tool_result({Role::User, {ToolResultBlock{"t1","out",false}}}); // true
/// @endcode
bool has_tool_result(const Message& msg) {
    return std::ranges::any_of(msg.content, [](const ContentBlock& b) {
        return std::holds_alternative<ToolResultBlock>(b);
    });
}

/// @brief The wire spelling of a role.
/// @return `"user"` or `"assistant"` — exactly what the API expects.
std::string_view to_string(Role role) {
    // 字符串字面量有静态存储期，返回 string_view 指向它是安全的。
    return role == Role::User ? "user" : "assistant";
}

}  // namespace mini
