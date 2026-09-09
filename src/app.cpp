// 【Stage 7】装配层。⚠️ 成员声明顺序 = 构造顺序，agent_ 必须最后声明。
#include "mini_agent/app.hpp"

#include <optional>

#include "mini_agent/mcp.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/skills.hpp"
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

      // ⚠️ 必须在 ctx 之前声明 —— ctx 里存的是指向它们的裸指针。
      //    关掉的时候用 nullopt 而不是"建一个空的"：ToolContext 里那些字段是
      //    指针就是为了表达"这一层可能不存在"，建个空对象反而让工具以为有。
      std::optional<Memory> memory;
      std::optional<SkillRegistry> skills;

      // ⚠️ 也必须在 ctx 之前 —— 但更要紧的是它在 **agent 之后析构**。
      //    成员逆序析构：agent → ctx → session → registry → skills → memory →
      //    background。也就是说 agent 已经不可能再碰后台任务了，这时才去杀进程。
      //    反过来（background 声明在 agent 之后）就是：进程全被杀掉了，agent
      //    才开始析构 —— 中间那一段它会对着一堆已经死掉的 pid 工作。
      std::optional<BackgroundManager> background;

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

      // ④ Stage 5：两个渐进式披露的来源。只在开关打开时建 ——
      //    Memory 的构造会建目录，关掉的时候不该在别人的工作区里留下空文件夹。
      if (cfg.enable_memory) memory.emplace(cfg.memory_dir());
      if (cfg.enable_skills) skills.emplace(cfg.skills_dirs());

      // Stage 6：后台任务。没有开关 —— bash 的 run_in_background 参数总是在的，
      // 而"能不能用"由 ctx.background 是否为空决定（子 agent 里就是空）。
      background.emplace();

      for (auto& t : builtin_tools(cfg))            // ⑤ 注册工具
          registry.add(std::move(t));

ctx.cfg      = &cfg;                         // ⑥ 接线
        ctx.sandbox  = &sandbox;
        ctx.session  = &session;
        ctx.registry = &registry;
        ctx.memory   = memory ? &*memory : nullptr;
        ctx.skills   = skills ? &*skills : nullptr;
        ctx.background = &*background;

        // ⑦ Stage 6：派子 agent 的入口。
        //
        // ⚠️ 捕获 this（这里是 Impl*）。Impl 由 App 的 unique_ptr 持有，而 App
        //    是**不可移动**的（成员互指），所以这个地址在整个 App 生命周期内
        //    都有效。App 删掉移动构造正是为了这个 —— 可移动的话这就是悬垂捕获，
        //    而且没有任何东西会说话。
        //
        // ⚠️ lambda 里读的是成员 ctx，不是它的拷贝。spawn_subagent 要拿 ctx
        //    当模板复制一份再收窄，而此刻（接线进行中）ctx.spawn 自己还是空的 ——
        //    拷贝的话子 agent 拿到的就是那个半成品快照。读成员则永远是最新的。
        if (cfg.enable_subagents)
            ctx.spawn = [this](std::string_view type, std::string_view prompt) {
                return spawn_subagent(cfg, *llm, sandbox, ctx, type, prompt);
            };
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
  Memory* App::memory()             { return impl_->memory ? &*impl_->memory : nullptr; }
  SkillRegistry* App::skills()      { return impl_->skills ? &*impl_->skills : nullptr; }
  BackgroundManager* App::background() { return &*impl_->background; }
  const Config& App::cfg() const    { return impl_->cfg; }
  const std::vector<std::string>& App::warnings() const { return impl_->warnings; }


}  // namespace mini
