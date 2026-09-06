// 【Stage 1】输出解析。

#include "mini_agent/parser.hpp"

#include "mini_agent/llm.hpp"

namespace mini {

/// @brief Should the loop execute tools and iterate again?
///
/// @return true only when tool calls are present and the turn was not declared
///         finished.
///
/// @note The `end_turn` guard covers a combination the API does not actually
///       emit. Running those tools would append results that nothing reads —
///       the loop returns on this same response either way.
bool ParsedResponse::wants_tools() const {
    if (tool_calls.empty()) return false;
    if (stop_reason == "end_turn"){
            return false;
        }
    return true;
}

/// @brief Split one response into everything downstream needs, in a single pass.
///
/// @param llmr The client's response.
/// @return Flattened text and thinking for the screen, tool calls for the
///         executor, and the assistant message for the history.
///
/// @warning `message` takes `llmr.content` **as is**. Thinking signatures are in
///          those blocks and the API validates them on the next turn — an
///          earlier version rebuilt the message from JSON and produced an empty
///          one, which meant every tool_result in the history referenced a
///          tool_use that was no longer there.
///
/// @code
/// LlmResponse r;
/// r.content = {ThinkingBlock{"hmm","sig_x"}, TextBlock{"Let me look"},
///              ToolUseBlock{"toolu_1","read",{{"path","a.py"}}}};
/// r.stop_reason = "tool_use";
///
/// auto p = parse(r);
/// // p.text                 == "Let me look"
/// // p.thinking             == "hmm"
/// // p.tool_calls.size()    == 1
/// // p.message.content.size() == 3   ← all three, signature intact
/// @endcode
ParsedResponse parse(const LlmResponse& llmr) {
    // 一趟遍历同时产出：文本、思考、工具调用、写回历史的 Message
    
    std::string text;
    std::string think;
    std::vector<ToolCallEvent> tooluse;
    Message m{Role::Assistant, llmr.content};
    for(const auto&b : llmr.content){
        if(const auto* t = std::get_if<TextBlock>(&b)) text+=t->text;
        else if (const auto* t = std::get_if<ThinkingBlock>(&b)) think+=t->thinking;
        else if (const auto* t = std::get_if<ToolUseBlock>(&b)) tooluse.push_back(ToolCallEvent{t->id,t->name,t->input});
    }
   return ParsedResponse{std::move(text), std::move(think), std::move(tooluse),
                        llmr.stop_reason, std::move(m)};

}

/// @brief Pack a batch of tool results into one user message.
///
/// @param tr The results, in call order.
/// @return One Role::User message with a tool_result block per entry.
///
/// @warning All results go in a single message. Spreading them across several
///          teaches the model that parallel tool calls do not come back
///          together, and it starts serialising them — a slowdown nothing
///          reports.
///
/// @note Empty output is replaced with a placeholder so the model can tell
///       "succeeded, no output" from "nothing ran". `write` legitimately
///       produces nothing on success.
/// @note is_error is copied through unconditionally; to_json is what omits it
///       when false.
///
/// @code
/// tool_result_message({{.id="t1", .name="read",  .output="x = 1"},
///                      {.id="t2", .name="bash",  .output="boom", .is_error=true}});
/// // to_json →
/// // {"role":"user","content":[
/// //    {"type":"tool_result","tool_use_id":"t1","content":"x = 1"},
/// //    {"type":"tool_result","tool_use_id":"t2","content":"boom","is_error":true}]}
/// @endcode
Message tool_result_message(const std::vector<ToolResultEvent>& tr) {
    // ⚠️ 所有结果必须打进**一条** user Message
    // ⚠️ is_error 只在为真时才带这个字段；输出为空时给个占位串
    Message m{Role::User,{}};      
    m.content.reserve(tr.size());

    for(const auto& b : tr){
        ToolResultBlock tb;

        tb.tool_use_id = b.id;
        tb.content = b.output.empty()?"(no output)" : b.output;
        tb.is_error = b.is_error;
        m.content.push_back(std::move(tb));
    }
    return m;
}

}  // namespace mini
