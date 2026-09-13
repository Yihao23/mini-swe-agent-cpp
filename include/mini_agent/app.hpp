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
//   * agent 必须是最后一个声明的成员
// 顺序写错了，构造 Agent 时拿到的是还没初始化的对象 —— 这类 bug 只在 release
// 构建下偶尔炸，非常难查。写完回来数一遍顺序。
//
// 顺序还管着析构：agent 最先析构，background 在它之后 —— 循环彻底不再碰后台
// 任务了，才轮到去杀那些子进程。反过来就是「进程全死了，agent 才开始收尾」。
//
// ── 声明顺序（= 构造顺序，析构逆序）─────────────────────────────────────────
//
//   记号见 executor.hpp 的图例：──▶ 同步 / ══▶ fork-join / ──▷ 异步
//
//     cfg          ─┐  谁都指着它
//     llm           │
//     on_event      │
//     warnings      │
//     sandbox       │  构造时读 cfg.permission_mode
//     memory        │  optional —— 关掉就不建，连目录都不建
//     skills        │
//     background    │  ⚠️ 也在 ctx 之前，但真正要紧的是它在 agent **之后**析构
//     mcp           │  ⚠️ 要在 registry **之前** —— 远程工具持有 server 的
//                   │     shared_ptr，工具必须先死，server 才能死
//     registry      │
//     session       │
//     ctx          ─┘  ⚠️ 里面全是指向上面那些的**裸指针**
//     agent         ←  最后。它依赖全部，也因此最先析构
//
//   ⚠️ 顺序错了没有任何诊断。把 agent 挪到 ctx 前面，它拿到的就是一个还没
//      接好线的 ToolContext —— 编译过、跑起来，然后工具拿到一堆 nullptr。
//
//   析构是逆序，所以这份清单也是一份"谁先死"的清单，倒着读：
//      agent 先死 → … → background 后死。循环彻底不再碰后台任务了，才轮到
//      去杀那些子进程。把 background 挪到 agent 后面就反过来了 —— 进程全被
//      杀掉，agent 才开始收尾，中间那一段它对着一堆已经死掉的 pid 工作。
//
// ── 接线：谁指向谁 ──────────────────────────────────────────────────────────
//
//                     ┌────────── cfg（唯一一份）──────────┐
//                     ▼                                    ▼
//        sandbox ──引用──> cfg        Agent ──引用──> cfg / llm / registry
//           ▲                                        / sandbox / session / ctx
//           │                                               │
//           └───────────── ctx.sandbox ────────────────────┘
//                         ctx.cfg / ctx.session / ctx.registry
//                         ctx.memory / ctx.skills     ← 可能是 nullptr
//                         ctx.spawn                   ← lambda，捕获 this
//
//   ctx 里全是**非拥有裸指针**，App 是它们唯一的所有者。
//
// ── 那两句 delete 到底买到了什么 ────────────────────────────────────────────
//
//   先说清楚**不是**什么：那些裸指针的有效性不靠它们，靠的是 pimpl。
//
//        App（栈上）                    Impl（堆上，地址永不变）
//        ┌────────────┐                ┌──────────────────────┐
//        │ unique_ptr │───────────────▶│ cfg / sandbox / …    │
//        │   impl_    │                │ ctx.spawn = [Impl*]──┼──┐
//        └────────────┘                │                      │◀─┘
//                                      └──────────────────────┘
//
//   移动 App 搬走的只是那个 unique_ptr —— Impl 一个字节都没动。ctx 的指针、
//   lambda 捕获的 this（写在 Impl 的构造函数里，所以是 Impl*）全都照样有效。
//
//   真正的理由是这两条：
//
//        App(const App&) = delete;
//            其实 unique_ptr 本来就不可拷贝，编译器自己会拦。写出来是给读的人
//            看的 —— 别费劲去"修"它，两份 App 各自管一堆子进程和会话文件，
//            那不是拷贝能表达的东西。
//
//        App(App&&) = delete;
//            ⚠️ 这一条**编译器本来不会说话**。unique_ptr 可移动，所以移动构造
//            默认就有。移动之后源 App 的 impl_ 是 nullptr，而每个访问器都是
//            `return *impl_->agent;` —— 直接解引用空指针，没有任何诊断。
//            删掉它，用被移走的壳就变成编译错误。
//
//   ⚠️ 还有第二个作用：哪天有人把 pimpl 去掉、把成员摊回 App 里，上面那条
//      "指针天然有效"就不成立了 —— 那时移动会让 ctx 指向搬空的旧对象。
//      规则先立在这儿，省得那一天再想起来。
//
// ── 构造做的八件事 ──────────────────────────────────────────────────────────
//
//   ① cfg.ensure_dirs()               建目录（看 enable_* 开关）
//   ② llm 为空 → AnthropicClient       测试传 FakeLlm 进来就跳过
//   ③ session.bind(sessions_dir())     定落盘位置，此刻还没写文件；
//                                      恢复的会话已有路径，不重新 bind
//   ④ memory / skills                  开关打开才建
//        background                    没有开关 —— 总是建
//   ⑤ builtin_tools(cfg) → registry    工具表也按开关拼
//   ⑥ load_mcp_servers() → registry    外部 server 的工具进同一张表；
//                                      起不来的记进 warnings，不抛
//   ⑦ ctx 的十个字段接线
//   ⑧ ctx.spawn = lambda               Stage 6；捕获 this
//        ▼
//   agent = make_unique<Agent>(...)    最后一步，此刻一切就绪
//
// ── 为什么用 pimpl ──────────────────────────────────────────────────────────
//
//   成员顺序是**语义相关**的，而顺序写在类定义里。摊在头文件里的话，每个
//   include 它的人都看得见、也都可能"顺手整理一下"—— 而整理的代价是一个
//   不报错的 bug。关进 .cpp，顺序就只有一个地方能改。
//
//   附带好处：头文件不必 include sandbox / session / registry 的定义。
//
// ── warnings：非致命问题的出口 ──────────────────────────────────────────────
//
//   一条解析不了的权限规则、一个起不来的 MCP server —— 都收进 warnings()，
//   不抛。为了 config 里的一行写错就不让 agent 启动，代价比收益大得多。
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
/// @warning Non-movable, and deliberately — though not for the reason it looks
///          like. Impl lives on the heap and never moves, so the raw pointers
///          inside ToolContext would survive a move of the App just fine. What
///          would not survive is the moved-from App: its impl_ is null and
///          every accessor dereferences it. Deleting the move turns that into
///          a compile error instead of a null dereference with no diagnostic.
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
    /// @return Never null — always built, since bash's run_in_background is
    ///         always offered. What narrows it is the ToolContext a sub-agent
    ///         gets, not this.
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
