// 【Stage 7】装配层。⚠️ 成员声明顺序 = 构造顺序，agent_ 必须最后声明。
#include "mini_agent/app.hpp"

#include "mini_agent/mcp.hpp"
#include "mini_agent/subagent.hpp"
#include "mini_agent/tools/builtin.hpp"

namespace mini {

struct App::Impl {
    // 顺序敏感！被指向的对象要在前面：
    //   cfg, llm, sandbox, background, memory, skills, mcp, registry, session, ctx, agent
      // ⚠️  声明顺序 = 构造顺序，析构逆序。被读取/被指向的必须在前面。
      Config cfg;
      std::unique_ptr<LlmClient> llm;
      EventSink on_event;
      std::vector<std::string> warnings;
      Sandbox sandbox;                  // 构造时读 cfg.permission_mode
      ToolRegistry registry;
      Session session;
      ToolContext ctx;                  // 指向上面几个
      std::unique_ptr<Agent> agent;     // 最后 —— 依赖全部，且要最先析构

            Impl(Config c, std::unique_ptr<LlmClient> l, AskFn asker, EventSink ev,
           std::optional<Session> s)
          : cfg(std::move(c)),
            llm(std::move(l)),
            on_event(std::move(ev)),
            sandbox(cfg, std::move(asker)),
            session(s ? std::move(*s) : Session{}) {
      cfg.ensure_dirs();                          // ① 建目录
      if (!llm)                                    // ② 没传就建真的客户端
          llm = std::make_unique<AnthropicClient>(cfg);
      session.bind(cfg.sessions_dir());            // ③ 会话落盘位置
      registry.add(make_read_tool());              // ④ 注册工具
      registry.add(make_edit_tool());
      // registry.add(make_write_tool());   // TODO(Stage 2)

ctx.cfg      = &cfg;                         // ⑤ 接线
        ctx.sandbox  = &sandbox;
        ctx.session  = &session;
        ctx.registry = &registry;
        agent = std::make_unique<Agent>(cfg, *llm, registry, sandbox, session, ctx, on_event);
        
}

};

App::App(Config cfg, std::unique_ptr<LlmClient> llm, AskFn asker, EventSink ev, std::optional<Session> s)
    : impl_(std::make_unique<Impl>(std::move(cfg), std::move(llm), 
    std::move(asker), std::move(ev), std::move(s))) {}

App::~App() = default;

  Agent& App::agent()               { return *impl_->agent; }
  Session& App::session()           { return impl_->session; }
  ToolRegistry& App::registry()     { return impl_->registry; }
  Sandbox& App::sandbox()           { return impl_->sandbox; }
  LlmClient& App::llm()             { return *impl_->llm; }
  Memory* App::memory()             { return nullptr; }   // Stage 5
  SkillRegistry* App::skills()      { return nullptr; }   // Stage 5
  BackgroundManager* App::background() { return nullptr; }// Stage 6
  const Config& App::cfg() const    { return impl_->cfg; }
  const std::vector<std::string>& App::warnings() const { return impl_->warnings; }


}  // namespace mini
