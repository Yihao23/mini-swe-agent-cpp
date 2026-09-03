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
    // Stage 4 之前可以先返回一个写死的字符串
    todo("Stage 1/4: Agent::build_system_blocks");
}

void Agent::inject_turn_context() {
    todo("Stage 4/6: Agent::inject_turn_context —— 后台通知、todo 变化");
}

std::string Agent::run(std::string_view) {
    // 十行核心 + 步数上限 + 事件 + 中断（Stage 1）
    // 循环开头判断是否压缩、注入动态上下文（Stage 4）
    todo("Stage 1: Agent::run");
}

std::string Agent::handle_interrupt(const std::vector<ToolCallEvent>&) {
    // ⚠️ 每个未完成的 tool_use 都要补一个 error 结果，否则下一轮 400
    todo("Stage 1: Agent::handle_interrupt");
}

}  // namespace mini
