#pragma once
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

class App {
  public:
    /// llm 传空则内部建 AnthropicClient；测试传 FakeLlm。
    /// asker 为空 = 非交互模式。
    App(Config cfg, std::unique_ptr<LlmClient> llm = nullptr, AskFn asker = {},
        EventSink on_event = {}, std::optional<Session> session = std::nullopt);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;              // ← 见上面"成员互指"的说明
    App& operator=(App&&) = delete;

    Agent& agent();
    Session& session();
    ToolRegistry& registry();
    Sandbox& sandbox();
    LlmClient& llm();
    Memory* memory();
    SkillRegistry* skills();
    BackgroundManager* background();
    const Config& cfg() const;
    const std::vector<std::string>& warnings() const;   // MCP 起不来之类的非致命问题

  private:
    struct Impl;                      // 成员顺序敏感，全关在 .cpp 里更省心
    std::unique_ptr<Impl> impl_;
};

}  // namespace mini
