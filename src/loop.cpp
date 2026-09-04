// 【Stage 1】Agent 主循环。

#include "mini_agent/loop.hpp"

#include "mini_agent/parser.hpp"
#include "mini_agent/prompt.hpp"
#include "mini_agent/sandbox.hpp"

namespace mini {

volatile std::sig_atomic_t g_interrupt = 0;

Agent::Agent(const Config& cfg, LlmClient& llm, ToolRegistry& registry, Sandbox& sandbox,
             Session& session, ToolContext& ctx, EventSink on_event, AgentOptions opts)
    : cfg_(cfg), llm_(llm), registry_(registry), sandbox_(sandbox), session_(session), ctx_(ctx),
      on_event_(std::move(on_event)), opts_(std::move(opts)), executor_(registry, ctx, on_event_) {}

std::vector<SystemBlock> Agent::build_system_blocks() const {
    // ⚠️ 这段文本必须逐字节稳定。它渲染在请求最前面，变一个字节 prompt 缓存
    //    就整个作废 —— 所以绝不能塞时间戳、随机 id、每轮都变的内容。
    //    TODO(Stage 4): 换成 prompt.cpp 里组装的多块版本
    SystemBlock b;
    b.text =
        "You are a software engineering agent working in " + cfg_.workdir.string() + ".\n"
        "Use the provided tools to inspect and modify the codebase.\n"
        "Read before you edit. Prefer small, verifiable steps. Be concise.";
    if (!opts_.identity.empty()) b.text = opts_.identity + "\n\n" + b.text;
    if (!opts_.system_extra.empty()) b.text += "\n\n" + opts_.system_extra;
    b.cache_breakpoint = true;   // 打在最后一块上，一次缓存 tools + system
    return {std::move(b)};
}

void Agent::inject_turn_context() {
    todo("Stage 4/6: Agent::inject_turn_context —— 后台通知、todo 变化");
}

std::string Agent::run(std::string_view user_input) {
    // 十行核心 + 步数上限 + 事件 + 中断（Stage 1）
    // 循环开头判断是否压缩、注入动态上下文（Stage 4)
      interrupted_ = false;
      if (!user_input.empty()) session_.add_user_text(std::string(user_input));
      const int max_steps = opts_.max_steps > 0 ? opts_.max_steps : cfg_.max_steps;
      // ⚠️  system 和 messages 在 LlmRequest 里是指针，必须指向活过整个循环的对象。
      //    放在循环外，别让它们指向临时量。
      const auto system = build_system_blocks();
      const Json tools = registry_.schemas();
      std::string last_text;

      for (int step = 0; step < max_steps; ++step) {
          if (g_interrupt) { interrupted_ = true; return handle_interrupt({}); }

          LlmRequest req;
          req.system   = &system;
          req.messages = &session_.messages();
          req.tools    = tools;
          req.model    = opts_.model;                 // 空 = 用 cfg.model，子 agent 会覆盖
          if (cfg_.stream && on_event_)
              req.on_text = [this](std::string_view t) { on_event_(TextEvent{std::string(t)});
  };

            auto resp = llm_.complete(req);

          if (!resp) {                                 // 失败是值，不是异常
              const auto& e = resp.error();
              if (on_event_) on_event_(StopEvent{"error", e.message});
              return "请求失败: " + e.message;
          }
          auto parsed = parse(*resp);
          session_.append(parsed.message);
          last_text = parsed.text;

          if (on_event_) {
              if (!parsed.thinking.empty()) on_event_(ThinkingEvent{parsed.thinking});
              // 流式时 on_text 已经吐过了，只有非流式才补发（loop.hpp:57）
              if (!cfg_.stream && !parsed.text.empty()) on_event_(TextEvent{parsed.text});
          }
          if (!parsed.wants_tools()) {
              if (on_event_) on_event_(StopEvent{parsed.stop_reason, {}});
              return parsed.text;
          }
          if (g_interrupt) { interrupted_ = true; return handle_interrupt(parsed.tool_calls);
  }
          auto results = executor_.run_batch(parsed.tool_calls);
          session_.append(tool_result_message(results));
      }
      if (on_event_) on_event_(StopEvent{"max_steps", "达到步数上限 " +
  std::to_string(max_steps)});
      return last_text.empty() ? "达到步数上限，未能完成" : last_text;


}

std::string Agent::handle_interrupt(const std::vector<ToolCallEvent>& pending) {
    // ⚠️ 每个未完成的 tool_use 都要补一个 error 结果，否则下一轮 400。
    //    历史已经落盘，缺一条配对结果就是「这个会话永久用不了」——
    //    --continue 回来照样 400。
    if (!pending.empty()) {
        std::vector<ToolResultEvent> aborted;
        aborted.reserve(pending.size());
        for (const auto& c : pending)
            aborted.push_back({.id = c.id, .name = c.name, .output = "用户中断", .is_error = true});
        session_.append(tool_result_message(aborted));
    }
    if (on_event_) on_event_(StopEvent{"interrupt", {}});
    return "已中断";
}

}  // namespace mini
