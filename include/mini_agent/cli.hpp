#pragma once
/// @file
/// @brief Terminal front end — event rendering, REPL, slash commands (Stage 7).
//
// 【Stage 7】终端界面 —— 事件渲染 + REPL + 斜杠命令 + 权限确认。
//
// 这一层**不含任何 agent 逻辑**，纯粹是把 loop 吐出的事件画到屏幕上。
// 做对了的标志：换成 Web 前端只要换掉这一个文件。
//
// 渲染用 std::visit 分派 AgentEvent —— 加一种事件时这里会编译报错，
// 这正是 variant 相对于"事件基类 + dynamic_cast"的价值。
//
// ── 事件到屏幕 ──────────────────────────────────────────────────────────────
//
//   Agent ──▶ on_event(AgentEvent)  ──▶ Renderer::operator()
//                                            │
//        std::visit 分派五种：                │
//          TextEvent        文字，可能是一小块（流式）
//          ThinkingEvent    暗色，可以整个关掉
//          ToolCallEvent    ⚙ name({args})
//          ToolResultEvent  ✓ 0.3s   /   ✗ 出错了
//          StopEvent        end_turn / max_steps / interrupt
//
//   ⚠️ Renderer **有状态**，因为流式的文字是一小块一小块来的：
//
//        in_text_ = false    ⚙ glob(...) 直接从行首打
//        in_text_ = true     ⚙ 之前要先换行，否则工具调用会插进半句话中间
//
//      这就是它是个类而不是个函数的全部理由。EventSink 按值拿它，
//      所以那两个 bool 得是可拷贝的普通成员。
//
// ── 三个入口 ────────────────────────────────────────────────────────────────
//
//   cli_main(argc, argv)
//        │  手写的参数解析，三十行。C++ 没有 argparse，而这个项目只该有
//        │  两个依赖 —— 为了解析几个 flag 引第三个不划算
//        │
//        ├─ 有任务参数 ──▶ app.agent().run(任务)   跑一次就退出
//        └─ 没有       ──▶ repl(app)
//                             │
//                        ┌────┴─────────────────────────┐
//                        │  读一行                       │
//                        │    以 / 开头 ──▶ handle_command() ── true ──> 退出
//                        │    否则       ──▶ agent.run(line)          │
//                        └───────────────────────────────────────────┘
//
//   handle_command 认这些：
//     /help /tools /mode /memory /skills /bg /compact /usage /session /clear /quit
//   认不出的**打出列表**，不是报错 —— 用户打错一个字母，给他看有哪些就够了。
//
// ── ask_user：沙箱和终端之间的那根线 ────────────────────────────────────────
//
//   Sandbox ──▶ asker_(tool, subject, reason) ──▶ ask_user()
//                                                    │
//                                          [y] 允许一次
//                                          [a] 本会话都允许 → remember_allow()
//                                          其他 → 拒绝
//
//   ⚠️ 除 y / a 之外一律当拒绝。打错一个键往"不"的方向错，代价小得多。
//   ⚠️ 沙箱**不认识终端** —— 它只拿到一个 AskFn。web 前端会传一个推消息的，
//      测试传一个立即返回的，CI 里传空的（那时 Ask 落到 Deny）。
//
#include <string>
#include <vector>

#include "mini_agent/parser.hpp"
#include "mini_agent/sandbox.hpp"

namespace mini {

class App;

/// @brief Renders agent events to the terminal.
///
/// @note Stateful on purpose: it has to remember whether it is mid-way through
///       a streamed line. Without that, a tool call prints itself into the
///       middle of a half-written sentence.
class Renderer {
  public:
    /// @brief Build a renderer.
    /// @param show_thinking Print the model's reasoning summary.
    explicit Renderer(bool show_thinking = true);

    /// @brief Render one event.
    /// @param event Any of the five agent events.
    /// @note A function object rather than a function so the streaming state
    ///       lives somewhere. EventSink takes it by value, hence the copyable
    ///       members.
    void operator()(const AgentEvent& event);

  private:
    void newline_if_needed();

    bool show_thinking_;
    bool in_text_ = false;
    bool in_thinking_ = false;
};

/// @brief Ask the person at the terminal whether a call may proceed.
///
/// @param tool    Which tool wants to run.
/// @param subject What it wants to act on — the path, the command.
/// @param reason  Why it is being asked, from Decision::reason.
/// @return What they chose: deny, allow once, or allow for the session.
///
/// @note This is the AskFn the sandbox calls. The sandbox knows nothing about
///       terminals; a web front end would supply a different one, and a test
///       supplies a lambda.
/// @note Anything other than y or a is a refusal. Erring toward "no" on a
///       mistyped key is the cheaper mistake.
Confirm ask_user(std::string_view tool, std::string_view subject, std::string_view reason);

/// @brief Handle one slash command.
///
/// @param app  The application, for the state a command inspects or changes.
/// @param line The whole line the user typed, starting with '/'.
/// @return true when the session should end.
///
/// @note /help /tools /mode /memory /skills /bg /compact /usage /session
///       /clear /quit. An unknown command prints the list rather than failing.
bool handle_command(App& app, std::string_view line);

/// @brief The interactive loop: read a line, run a turn, repeat.
/// @param app The application.
/// @return Process exit code.
int repl(App& app);

/// @brief Parse the command line, then run one task or enter the REPL.
///
/// @param argc From main.
/// @param argv From main.
/// @return Process exit code.
///
/// @note The parsing is hand-written, about thirty lines. C++ has no argparse,
///       and this project is allowed exactly two dependencies — a third for
///       flag parsing is not worth it.
int cli_main(int argc, char** argv);

}  // namespace mini
