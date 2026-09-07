#pragma once
/// @file
/// @brief Assembly — wires a dozen modules into a runnable agent (Stage 7).
//
// 【Stage 7】装配层 —— 把十几个模块接成一个能跑的 agent。
//
// 刻意集中在一个类里：想知道"谁依赖谁"，读这一个构造函数就够了。
//
// ── C++ 特有的难点：成员声明顺序 = 构造顺序 ─────────────────────────────────
//
// ToolContext 里全是指向其他成员的指针，Agent 又持有 ToolContext 的引用。
// 成员**按声明顺序**构造、按逆序析构，所以：
//   * 被指向的东西（sandbox / session / memory / …）必须声明在**前面**
//   * agent_ 必须是最后一个声明的成员
// 顺序写错了，构造 Agent 时拿到的是还没初始化的对象 —— 这类 bug 只在 release
// 构建下偶尔炸，非常难查。写完回来数一遍顺序。
//
// 另一个坑：App 里存了大量互指的成员，所以 **App 必须不可拷贝、不可移动**
// （移动会让 ToolContext 里的指针指向旧对象）。显式 delete 掉。
//
#include <memory>
#include <string>
#include <vector>

#include "mini_agent/background.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/skills.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

class McpClient;

/// @brief Assembly: builds every layer and hands out references to them.
///
/// Nothing here has logic of its own. Its job is construction order and
/// lifetime — Sandbox borrows the Config, ToolContext borrows the Sandbox and
/// the Session, Agent borrows all of them. Get the order wrong and a member
/// binds to something not built yet.
///
/// @warning Non-movable, and deliberately. The members point at each other, so
///          moving the App would leave those pointers aimed at the old
///          addresses. Deleting the move is what makes that a compile error
///          rather than a use-after-move nobody notices.
///
/// @note pimpl, so member declaration order — which is construction order —
///       stays in the .cpp where it can be reasoned about, instead of in a
///       header that every layer includes.
class App {
  public:
    /// @brief Build everything.
    ///
    /// @param cfg      Taken by value; the App owns the copy every layer borrows.
    /// @param llm      Null builds an AnthropicClient. Tests pass a FakeLlm.
    /// @param asker    How to ask a human. Empty means non-interactive, and
    ///                 the sandbox then resolves Ask to Deny.
    /// @param on_event Progress notifications; empty is fine.
    /// @param session  Resume from this one; nullopt starts fresh.
    ///
    /// @note A non-fatal problem — an MCP server that will not start, a
    ///       permission rule that will not parse — is collected into
    ///       warnings() rather than thrown. Failing to start over one bad line
    ///       in a config file is worse than starting without it.
    App(Config cfg, std::unique_ptr<LlmClient> llm = nullptr, AskFn asker = {},
        EventSink on_event = {}, std::optional<Session> session = std::nullopt);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;              // ← 见上面"成员互指"的说明
    App& operator=(App&&) = delete;

    /// @brief The agent loop. @return A reference owned by this App.
    Agent& agent();
    /// @brief The conversation history. @return A reference owned by this App.
    Session& session();
    /// @brief The tools. @return A reference owned by this App.
    ToolRegistry& registry();
    /// @brief The permission gate. @return A reference owned by this App.
    Sandbox& sandbox();
    /// @brief The model client. @return A reference owned by this App.
    LlmClient& llm();

    /// @brief Long-term memory (Stage 5).
    /// @return nullptr when disabled or not yet implemented — hence a pointer.
    Memory* memory();
    /// @brief Skills (Stage 5).
    /// @return nullptr when disabled or not yet implemented.
    SkillRegistry* skills();
    /// @brief Background tasks (Stage 6).
    /// @return nullptr when not yet implemented.
    BackgroundManager* background();

    /// @brief The configuration every layer was built from.
    /// @return A const reference; it does not change after construction.
    const Config& cfg() const;

    /// @brief Non-fatal problems noticed while starting up.
    /// @return Messages for the user: an MCP server that would not start, a
    ///         permission rule that would not parse. Empty on a clean start.
    /// @note These are collected rather than thrown so one bad line in a config
    ///       file cannot stop the agent from running.
    const std::vector<std::string>& warnings() const;

  private:
    struct Impl;                      // 成员顺序敏感，全关在 .cpp 里更省心
    std::unique_ptr<Impl> impl_;
};

}  // namespace mini
