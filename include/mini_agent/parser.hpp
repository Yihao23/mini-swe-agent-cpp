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

struct TextEvent {
    std::string text;
};
struct ThinkingEvent {
    std::string text;
};
struct ToolCallEvent {
    std::string id;
    std::string name;
    Json input;
};
struct ToolResultEvent {
    std::string id;
    std::string name;
    std::string output;
    bool is_error = false;
    double duration_sec = 0.0;
};
struct StopEvent {
    std::string reason;   // end_turn | max_tokens | refusal | max_steps | interrupt | compacted
    std::string detail;
};

using AgentEvent = std::variant<TextEvent, ThinkingEvent, ToolCallEvent, ToolResultEvent, StopEvent>;
using EventSink = std::function<void(const AgentEvent&)>;

struct ParsedResponse {
    std::string text;                      // 拼接后的可见文本
    std::string thinking;                  // 拼接后的思考摘要
    std::vector<ToolCallEvent> tool_calls;
    std::string stop_reason;
    Message message;                       // 写回历史的 assistant 轮

    /// 循环要不要继续。想想：只看 stop_reason 够吗？
    bool wants_tools() const;
};

struct LlmResponse;   // llm.hpp

/// 遍历 response.content，一趟同时产出文本、思考、工具调用和历史消息。
/// TODO(Stage 1)
ParsedResponse parse(const LlmResponse& response);

/// 把一批工具结果打成**一条** user Message。
///
/// ⚠️ 拆成多条会让模型逐渐学会不再并行调工具。
/// ⚠️ 每个 tool_use 必须有配对结果，少一个下一轮直接 400。
/// TODO(Stage 1)
Message tool_result_message(const std::vector<ToolResultEvent>& results);

}  // namespace mini
