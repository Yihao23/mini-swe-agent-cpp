// 【Stage 1】输出解析。

#include "mini_agent/parser.hpp"

#include "mini_agent/llm.hpp"

namespace mini {

bool ParsedResponse::wants_tools() const {
    if (tool_calls.empty()) return false;
    if (stop_reason == "end_turn"){
            return false;
        }
    return true;
}

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
