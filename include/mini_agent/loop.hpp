#pragma once
/// @file
/// @brief The agent loop — the heart of the project, and one of its shortest files (Stage 1).
//
// 【Stage 1】Agent 主循环 —— 整个项目的心脏，也应该是最短的文件之一。
//
// 核心就这十行（写完回来对照；如果你的 run() 比这复杂很多，说明有东西漏进来了）：
//
//     for (;;) {
//         auto resp   = llm.complete(req);
//         auto parsed = parse(*resp);
//         session.append(parsed.message);
//         if (!parsed.wants_tools()) return parsed.text;
//         auto results = executor.run_batch(parsed.tool_calls);
//         session.append(tool_result_message(results));
//     }
//
// 围绕它的一切都是流程控制：
//   步数上限     防止无限循环烧钱                       (Stage 1)
//   事件回调     UI 拿到流式文本 / 工具调用 / 结果       (Stage 1)
//   中断处理     Ctrl-C 变成历史里的一条记录，不是崩溃   (Stage 1)
//   上下文压缩   超过阈值把老历史换成纪要                (Stage 4)
//   动态注入     每轮开始前塞后台通知、todo              (Stage 4/6)
//
// 子 agent 复用的就是这个类，只是换一套 tools 和一个更便宜的 model (Stage 6)。
//
#include <csignal>
#include <string>

#include "mini_agent/executor.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

class Sandbox;

/// @brief Per-agent overrides, so one Config can serve several agents.
///
/// @note Every field's empty/zero value means "use the Config". A sub-agent
///       differs from the main one in three or four ways, and listing only
///       those is clearer than copying a Config and editing it.
struct AgentOptions {
    std::string name = "main";   ///< Shown in events and logs.
    std::string model;           ///< Empty means cfg.model. Sub-agents run a cheaper one.
    int max_steps = 0;           ///< 0 means cfg.max_steps.
    std::string system_extra;    ///< Appended to the system prompt as its own block.

    /// @brief Replaces the identity section rather than adding to it.
    /// @note A sub-agent is a different persona, not the main one plus a note.
    std::string identity;
};

/// @brief The agent loop: ask the model, run what it asks for, repeat.
///
/// The whole cycle is a dozen lines. Everything around it — the step ceiling,
/// compaction, interrupt handling — exists because of a way the plain loop
/// fails.
///
/// @note Holds references to everything. They must all outlive the run; App is
///       what arranges that.
class Agent {
  public:
    /// @brief Wire an agent to the pieces it drives.
    ///
    /// @param cfg      ⚠️ Held by reference; must outlive the agent.
    /// @param llm      The model client. FakeLlm in tests.
    /// @param registry The tools available to this agent.
    /// @param sandbox  The permission gate the executor consults.
    /// @param session  The conversation history.
    /// @param ctx      What tools see at run time.
    /// @param on_event Progress notifications; empty is fine.
    /// @param opts     Per-agent overrides.
    ///
    /// @warning `ctx.session` must point at the **same** Session passed here.
    ///          Two instances means the history the tools see is not the one
    ///          the loop sends, and read's file record never reaches edit's
    ///          staleness check — which then silently stops working while the
    ///          transcript looks perfectly normal.
    Agent(const Config& cfg, LlmClient& llm, ToolRegistry& registry, Sandbox& sandbox,
          Session& session, ToolContext& ctx, EventSink on_event = {}, AgentOptions opts = {});

    /// @brief Run until the model stops asking for tools.
    ///
    /// @param user_input The task. Empty continues an existing conversation.
    /// @return The model's final text, or a message saying why it stopped.
    ///
    /// @warning A TextEvent is emitted **only when not streaming**. The stream
    ///          callback already delivered the text, and emitting it again
    ///          prints the whole response twice.
    ///
    /// @note The step ceiling is what stops a model that keeps calling tools
    ///       from looping until the budget is gone.
    /// @note The interrupt flag is checked at two points: before sending, and
    ///       after parsing but before running the tools. Both are places where
    ///       the history is in a consistent state.
    /// @note Compaction happens before the request, never after a refusal —
    ///       by then the turn is already lost and the history already saved.
    std::string run(std::string_view user_input = {});

    /// @brief Did the last run end on Ctrl-C?
    /// @return true when it was interrupted rather than finished.
    bool interrupted() const { return interrupted_; }

  private:
    std::vector<SystemBlock> build_system_blocks() const;
    void inject_turn_context();

    /// Ctrl-C：把"被打断"写进历史，而不是让进程崩掉。
    ///
    /// ⚠️ 关键：要给每个**未完成的 tool_use** 补一个 error 类型的 tool_result，
    /// 否则下一轮请求会因为 tool_use 没有配对结果被 API 拒绝。
    ///
    /// C++ 里怎么接 Ctrl-C：signal handler 里只能改 volatile sig_atomic_t 标志位
    /// （不能 new、不能加锁、不能打日志），循环在安全点检查它。
    std::string handle_interrupt(const std::vector<ToolCallEvent>& pending);

    const Config& cfg_;
    LlmClient& llm_;
    ToolRegistry& registry_;
    Sandbox& sandbox_;
    Session& session_;
    ToolContext& ctx_;
    EventSink on_event_;
    AgentOptions opts_;
    Executor executor_;
    bool interrupted_ = false;
};

/// 全局中断标志。main() 里 std::signal(SIGINT, ...) 设置它，循环检查它。
extern volatile std::sig_atomic_t g_interrupt;

}  // namespace mini
