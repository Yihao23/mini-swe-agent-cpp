// 【Stage 7】装配层。⚠️ 成员声明顺序 = 构造顺序，agent_ 必须最后声明。
#include "mini_agent/app.hpp"

#include "mini_agent/mcp.hpp"
#include "mini_agent/subagent.hpp"
#include "mini_agent/tools/builtin.hpp"

namespace mini {

struct App::Impl {
    // 顺序敏感！被指向的对象要在前面：
    //   cfg, llm, sandbox, background, memory, skills, mcp, registry, session, ctx, agent
};

App::App(Config, std::unique_ptr<LlmClient>, AskFn, EventSink, std::optional<Session>)
    : impl_(nullptr) {
    todo("Stage 7: App 构造 —— 把十几个模块接起来；ctx.spawn 用 lambda 捕获依赖");
}

App::~App() = default;

Agent& App::agent() { todo("Stage 7: App::agent"); }
Session& App::session() { todo("Stage 7: App::session"); }
ToolRegistry& App::registry() { todo("Stage 7: App::registry"); }
Sandbox& App::sandbox() { todo("Stage 7: App::sandbox"); }
LlmClient& App::llm() { todo("Stage 7: App::llm"); }
Memory* App::memory() { todo("Stage 7: App::memory"); }
SkillRegistry* App::skills() { todo("Stage 7: App::skills"); }
BackgroundManager* App::background() { todo("Stage 7: App::background"); }
const Config& App::cfg() const { todo("Stage 7: App::cfg"); }
const std::vector<std::string>& App::warnings() const { todo("Stage 7: App::warnings"); }

}  // namespace mini
