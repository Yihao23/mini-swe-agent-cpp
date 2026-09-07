// 【Stage 1】Agent 主循环。

#include "mini_agent/loop.hpp"

#include <format>

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
    // ⚠️ 这段必须逐字节稳定 —— 它渲染在请求最前面，变一个字节 prompt 缓存整个
    //    作废。所有动态内容走 inject_turn_context()。
    // Stage 5 的 skills / memory 还没接上，先传 nullptr（build_system 会跳过）。
    return build_system(cfg_, ctx_.skills, ctx_.memory, opts_.system_extra, opts_.identity);
}

void Agent::inject_turn_context() {
    const std::string ctx = turn_context(ctx_.background, ctx_.todos);
    if (ctx.empty()) return;

    // ⚠️ 追加成一条独立的 user 消息，不是塞进已有消息里。
    //    塞进去会改动已经落盘的历史，而且下一轮 turn_context 变了之后，
    //    那条消息的字节也跟着变 —— 缓存从那一条起全部作废。
    //    独立成一条则只有末尾新增，前缀不动。
    session_.append(Message{Role::User, {TextBlock{reminder(ctx)}}});
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

          // ⚠️ 压缩要在**发请求之前**判断，不能等 API 返回 400 再补救。
          //    超了才压的话这一轮已经废了，而历史已经落盘。
          //    compact() 自己负责找安全切分点，找不到就返回 false，不是错误。
          if (cfg_.compact_at_tokens > 0 &&
              session_.estimated_tokens() > cfg_.compact_at_tokens) {
              const int before = session_.estimated_tokens();
              if (session_.compact(llm_) && on_event_)
                  on_event_(TextEvent{std::format("[已压缩上下文：约 {} → {} token]\n",
                                                  before, session_.estimated_tokens())});
          }
          inject_turn_context();

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
