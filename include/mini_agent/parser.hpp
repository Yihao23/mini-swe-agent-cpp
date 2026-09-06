#pragma once
//
// 【Stage 1】输出解析 —— 把模型响应归一化成两样东西：
//   1. 事件流：给 UI / 日志用的扁平结构
//   2. 写回历史的 assistant Message
//
// 事件也是**封闭集合**，所以又是 variant（见 message.hpp 的判据）。
// UI 层用 std::visit 分派，加一种事件时所有 visitor 都会编译报错 —— 这正是你要的。
//
#include <functional>
#include <string>
#include <variant>
#include <vector>

#include "mini_agent/message.hpp"

namespace mini {

/// @brief Visible text on its way to the screen.
///
/// @note Streaming emits one of these per chunk, so a single sentence may
///       arrive as a dozen events. Non-streaming emits one for the whole turn.
///       Renderer tracks whether it is mid-line because of this.
struct TextEvent {
    std::string text;
};
/// @brief A fragment of the model's reasoning, shown dimmed.
/// @note Suppressed entirely when Config::show_thinking is false.
struct ThinkingEvent {
    std::string text;
};
/// @brief A tool the model asked for. Doubles as the executor's input type.
///
/// @note Nearly identical to ToolUseBlock, and deliberately separate.
///       ToolUseBlock is wire format and lives in message.hpp; this is an
///       internal event that is never serialised. Merging them would make
///       message.hpp — the bottom layer — depend on parser.hpp above it.
struct ToolCallEvent {
    std::string id;
    std::string name;
    Json input;
};
/// @brief What a tool produced. Doubles as the executor's output type.
///
/// @note `name` and `duration_sec` are for the display only — tool_result_message
///       drops them when building the block that goes back to the API.
struct ToolResultEvent {
    std::string id;
    std::string name;
    std::string output;
    bool is_error = false;
    double duration_sec = 0.0;
};
/// @brief Why the turn ended.
///
/// @note Only `end_turn` means the model finished on its own. The rest are
///       worth surfacing: `max_steps` in particular means the task may be
///       incomplete, and showing it as success would mislead.
struct StopEvent {
    std::string reason;   // end_turn | max_tokens | refusal | max_steps | interrupt | compacted
    std::string detail;
};

/// @brief Everything the UI can be told about, as a closed set.
///
/// A variant so that adding a sixth event breaks every std::visit at compile
/// time. The alternative — an event base class plus dynamic_cast — would let a
/// new event slip silently into whatever default branch exists.
using AgentEvent = std::variant<TextEvent, ThinkingEvent, ToolCallEvent, ToolResultEvent, StopEvent>;

/// @brief Where events go. Empty is valid and means nobody is watching.
///
/// @warning May be empty, so every call site needs `if (on_event_)` first —
///          invoking an empty std::function throws bad_function_call.
using EventSink = std::function<void(const AgentEvent&)>;

/// @brief One model response, split into what each consumer needs.
///
/// The same response is wanted three ways: flattened text for the screen, tool
/// calls for the executor, and the untouched blocks for the history.
///
/// @code
/// LlmResponse r;
/// r.content = {TextBlock{"Let me look"},
///              ToolUseBlock{"toolu_1","read",{{"path","a.py"}}}};
/// r.stop_reason = "tool_use";
///
/// auto p = parse(r);
/// // p.text        == "Let me look"
/// // p.tool_calls  == { ToolCallEvent{"toolu_1","read",{"path":"a.py"}} }
/// // p.message     == Message{Assistant, both blocks, verbatim}
/// // p.wants_tools() == true
/// @endcode
struct ParsedResponse {
    std::string text;                      // 拼接后的可见文本
    std::string thinking;                  // 拼接后的思考摘要
    std::vector<ToolCallEvent> tool_calls;
    std::string stop_reason;
    Message message;                       // 写回历史的 assistant 轮

    /// @brief Should the loop run tools and go round again?
    ///
    /// @return true only when there are tool calls *and* the model did not
    ///         declare the turn finished.
    ///
    /// @note stop_reason alone is not enough, and neither is an empty check.
    ///       A response carrying tool_use blocks alongside `end_turn` is a
    ///       combination the API does not produce; running those tools would
    ///       append results nothing will ever read, since the loop stops anyway.
    ///
    /// 循环要不要继续。想想：只看 stop_reason 够吗？
    bool wants_tools() const;
};

struct LlmResponse;   // llm.hpp

/// @brief Split one response into text, thinking, tool calls and a history entry.
///
/// @param response What the client returned.
/// @return All four products of a single pass over `response.content`.
///
/// @warning `message` must hold the blocks **verbatim**, not a reconstruction.
///          Thinking signatures live in there and the API validates them next
///          turn; rebuilding from the flattened text would drop them.
///
/// 遍历 response.content，一趟同时产出文本、思考、工具调用和历史消息。
ParsedResponse parse(const LlmResponse& response);

/// @brief Pack a batch of tool results into **one** user message.
///
/// @param results One entry per tool that ran, in the order they were called.
/// @return A single Role::User message holding one tool_result block each.
///
/// @warning Splitting these across messages teaches the model to stop calling
///          tools in parallel. Nothing reports it — runs just get slower.
/// @warning Every tool_use needs its result here. A missing one makes the next
///          request fail outright.
///
/// @note Empty output becomes a placeholder string: the model cannot otherwise
///       tell "ran fine, produced nothing" from "nothing happened".
/// @note `is_error` is set from the event; to_json omits it when false.
///
/// @code
/// tool_result_message({{.id="toolu_1", .name="read",  .output="x = 1"},
///                      {.id="toolu_2", .name="write", .output="", .is_error=false}});
/// // Message{User, { ToolResultBlock{"toolu_1", "x = 1",       false},
/// //                 ToolResultBlock{"toolu_2", "(no output)", false} }}
/// @endcode
///
/// 把一批工具结果打成**一条** user Message。
Message tool_result_message(const std::vector<ToolResultEvent>& results);

}  // namespace mini
