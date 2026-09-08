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
// ── 一轮长什么样 ────────────────────────────────────────────────────────────
//
//   run("把 a.py 的 bug 修了")
//        │
//        │  session.add_user_text()
//        ▼
//   ┌─ for (step < max_steps) ──────────────────────────────────────────┐
//   │                                                                    │
//   │  ① g_interrupt?  ──是──> handle_interrupt({})  ──> return         │
//   │        │否                  ↑ 安全点 A：还没发请求，历史是完整的     │
//   │        ▼                                                           │
//   │  ② 超过 compact_at_tokens?  ──是──> session.compact()             │
//   │        │                              ↑ 必须在发请求**之前**：     │
//   │        │                                超了才补救，这一轮已经废了 │
//   │        ▼                                                           │
//   │  ③ inject_turn_context()   后台通知 / todo → <system-reminder>    │
//   │        │                   ↑ 追加成独立的一条 user 消息，          │
//   │        │                     只动末尾，不碰缓存前缀                │
//   │        ▼                                                           │
//   │  ④ llm.complete(req)  ────失败──> return "请求失败: ..."          │
//   │        │成功              ↑ 失败是值不是异常，429/529 可重试       │
//   │        ▼                                                           │
//   │  ⑤ parse(resp)  →  text / thinking / tool_calls / message         │
//   │        │           一趟遍历同时产出四样                            │
//   │        ▼                                                           │
//   │  ⑥ session.append(parsed.message)   ⚠️ 原样存，签名藏在块里        │
//   │        │                                                           │
//   │        ▼                                                           │
//   │  ⑦ 发事件：ThinkingEvent / TextEvent                              │
//   │        │    ⚠️ TextEvent 只在**非流式**时发 ——                     │
//   │        │      流式的话 on_text 回调已经吐过了，再发就重复           │
//   │        ▼                                                           │
//   │  ⑧ parsed.wants_tools()? ──否──> StopEvent + return parsed.text  │
//   │        │是                                                         │
//   │        ▼                                                           │
//   │  ⑨ g_interrupt?  ──是──> handle_interrupt(tool_calls) ──> return  │
//   │        │否                  ↑ 安全点 B：模型要了工具但还没跑，      │
//   │        ▼                      每个 tool_use 都得补一条 error 结果  │
//   │  ⑩ executor.run_batch(tool_calls)                                 │
//   │        │      授权 → 并发/串行 → 超时 → 截断 → 事件               │
//   │        ▼                                                           │
//   │  ⑪ session.append(tool_result_message(results))                   │
//   │        │      ⚠️ 一批结果打成**一条** user 消息                    │
//   │        └──────────────────────────> 回到 ①                        │
//   └────────────────────────────────────────────────────────────────────┘
//                    │ 用完 max_steps
//                    ▼
//              StopEvent{"max_steps"} + 返回最后一段文本
//
// ── 请求是怎么拼的 ──────────────────────────────────────────────────────────
//
//   LlmRequest {                      渲染顺序 = 缓存前缀的顺序
//     tools    ── registry_.schemas()      按名字排序，最稳定 ─┐ 循环外算一次
//     system   ── build_system_blocks()    静态，断点打在末块 ─┘
//     messages ── &session_.messages()     每轮追加，最易变
//   }
//
//   ⚠️ tools 和 system **在循环外**算，而且 LlmRequest 存的是指针 ——
//      指向的对象必须活过整个循环。放进循环里的话它们指向临时量，
//      而且每轮重算会白白重复工作。
//
// ── 三个不变量 ──────────────────────────────────────────────────────────────
//
//   ┌── I1  每个 tool_use 都有配对的 tool_result ────────────────────────┐
//   │ 违反 → 下一轮请求 400。而历史已经落盘，--continue 回来照样 400，    │
//   │        整个会话永久报废。                                          │
//   │ 维护者：⑪ 正常路径；handle_interrupt() 中断路径（补 error 结果）；  │
//   │        Session::safe_split() 压缩时只切在真正的用户输入上          │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I2  system + tools 的字节在整个会话里不变 ───────────────────────┐
//   │ 违反 → 缓存每轮作废，账单十倍，而功能完全正常，什么都不会失败。      │
//   │ 维护者：build_system() 不许放动态内容；schemas() 按名字排序；       │
//   │        动态内容一律走 ③ 的 <system-reminder>                       │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I3  ctx_.session 和 session_ 是同一个对象 ───────────────────────┐
//   │ 违反 → read 记下的文件时间戳进了 A，edit 去 B 查 —— 「改之前要先读」 │
//   │        这条规矩静默失效，而历史看起来完全正常。                     │
//   │ 维护者：构造函数的调用方（App）。这里只能在文档里写明               │
//   └────────────────────────────────────────────────────────────────────┘
//
// 两个中断安全点（① 和 ⑨）不是随便挑的：它们是历史处于**一致状态**的两个时刻。
// 放在 ⑩ 中间的话，一半工具跑完了、结果还没写进历史，那才是真正难收拾的。
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
