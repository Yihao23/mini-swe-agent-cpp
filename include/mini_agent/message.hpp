#pragma once
//
// 【Stage 0】对话历史的数据模型。
//
// ── C++ 的第一个设计决定：variant 还是虚函数？──────────────────────────────
//
// content block 的类型是一个**封闭集合** —— 由 API 定义，你不会去扩展它。
// 封闭集合用 std::variant：
//     * 加一种块时，所有 std::visit 都会编译报错，逼你处理（穷尽性检查）
//     * 值语义，没有堆分配，拷贝/落盘都简单
//
// 而 Tool 是**开放集合** —— 用户随时会加自己的工具。开放集合才用虚函数（见 tool.hpp）。
//
// 这条判据在整个项目里反复出现：封闭用 variant，开放用虚函数。
//
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mini_agent/json.hpp"

namespace mini {

/// @brief Who a message came from, as the API models it.
///
/// @note `User` covers everything fed *to* the model, which includes tool
///       results — those carry Role::User even though no person typed them.
///       Use has_tool_result() when the distinction matters (safe_split does).
enum class Role { User, Assistant };

/// @brief Plain text, from either side of the conversation.
struct TextBlock {
    std::string text;
};

/// @brief The model's reasoning, with the signature that authenticates it.
///
/// @warning `signature` must be echoed back byte for byte. The API validates
///          it on the next turn and rejects the request if it was altered —
///          which is why compaction and streaming both take care to preserve it.
struct ThinkingBlock {
    std::string thinking;
    std::string signature;  // ⚠️ 必须原样带回，API 会校验；改了就报错
};

/// @brief Reasoning the API withheld, kept as an opaque blob.
///
/// @note Nothing here is readable, but it still has to be carried through the
///       history unchanged — dropping it breaks the chain the next turn checks.
struct RedactedThinkingBlock {
    std::string data;
};

/// @brief The model asking for a tool to be run.
///
/// @note `id` is what pairs this call with its result. Every tool_use needs a
///       matching tool_result in the following user message or the next request
///       is rejected — see Agent::handle_interrupt and Session::safe_split,
///       both of which exist to keep that pairing intact.
/// @note `input` is Json because tool schemas are arbitrary. This is a boundary,
///       which is where json.hpp says Json is allowed to appear.
struct ToolUseBlock {
    std::string id;    // toolu_...，回传 tool_result 时要对上
    std::string name;
    Json input;        // 工具参数是任意 schema，这里 Json 是合理的（边界）
};

/// @brief What a tool produced, on its way back to the model.
///
/// @note `is_error` is serialised only when true — see to_json. A failed tool
///       is reported as a result the model can read and work around, never as
///       an exception that ends the run.
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

/// @brief One piece of a message. A closed set defined by the API.
///
/// A variant rather than a class hierarchy: the alternatives are fixed by
/// Anthropic, and every std::visit over this type stops compiling the moment a
/// sixth one is added — which is exactly the reminder you want. A base class
/// with dynamic_cast would silently fall through to a default branch instead.
using ContentBlock =
    std::variant<TextBlock, ThinkingBlock, RedactedThinkingBlock, ToolUseBlock, ToolResultBlock>;

/// @brief One turn of the conversation: a role plus its content blocks.
///
/// @note An assistant turn commonly holds several blocks — thinking, then text,
///       then one or more tool_use. All of them belong to the same message.
///
/// @code
/// Message m{Role::Assistant, {TextBlock{"Let me look"},
///                             ToolUseBlock{"toolu_1", "read", {{"path","a.py"}}}}};
/// to_json(m);
/// // {"role":"assistant","content":[{"type":"text","text":"Let me look"},
/// //                                {"type":"tool_use","id":"toolu_1",...}]}
/// @endcode
struct Message {
    Role role{};
    std::vector<ContentBlock> content;
};

// --- 序列化（在 src/message.cpp 实现）---------------------------------------

/// @brief Serialise one block into its wire form.
/// @return An object whose `type` field names the alternative.
/// @code
/// to_json(ContentBlock{TextBlock{"hi"}});   // {"type":"text","text":"hi"}
/// @endcode
Json to_json(const ContentBlock& block);

/// @brief Serialise one message: `{"role": ..., "content": [...]}`.
Json to_json(const Message& msg);

/// @brief Serialise a whole history into the `messages` array of a request.
Json to_json(const std::vector<Message>& msgs);

/// @brief Parse one block back from its wire form.
///
/// @return nullopt for a `type` this build does not know.
///
/// @note Discarding beats guessing. The API will add block types, and
///       substituting an empty TextBlock would put words in the model's mouth
///       that it never said. Callers skip the nullopts.
///
/// 反向。未知 type 返回 nullopt —— 丢弃而不是猜结构：API 会加新块类型，
/// 伪造一个空 TextBlock 等于伪造历史。调用方负责跳过 nullopt。
std::optional<ContentBlock> block_from_json(const Json& j);

/// @brief Parse one message; unknown blocks inside it are dropped.
Message message_from_json(const Json& j);

/// @brief Parse a history array, e.g. when resuming a session from disk.
std::vector<Message> messages_from_json(const Json& j);

// --- 小工具 -----------------------------------------------------------------
/// @brief Concatenate every TextBlock in a message.
///
/// @return Empty when the message holds no text — routine, not exceptional: an
///         assistant turn that only calls tools has none, and neither does a
///         tool_result message.
///
/// @note Thinking is deliberately excluded. It is not part of the answer.
///
/// 把一条消息里所有 TextBlock 拼起来。
std::string text_of(const Message& msg);

/// @brief Does this message carry any tool_result block?
///
/// @note Separates a real user turn from a tool result, since both are
///       Role::User. Session::safe_split relies on this: cutting the history at
///       a tool_result orphans its tool_use and the next request fails.
///
/// 这条消息里有没有 tool_result 块（Stage 4 找压缩切分点要用）。
bool has_tool_result(const Message& msg);

/// @brief The wire spelling of a role: `"user"` or `"assistant"`.
std::string_view to_string(Role role);
}  // namespace mini
