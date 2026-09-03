// 【Stage 6】多 agent。
#include "mini_agent/subagent.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/json.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/sandbox.hpp"

namespace mini {

const std::vector<AgentType>& agent_types() {
    todo("Stage 6: agent_types —— explorer / coder / reviewer / general");
}

const AgentType* find_agent_type(std::string_view) { todo("Stage 6: find_agent_type"); }
std::string render_agent_types() { todo("Stage 6: render_agent_types —— 写给协调者模型看"); }

std::string spawn_subagent(const Config&, LlmClient&, Sandbox&, const ToolContext&,
                           std::string_view, std::string_view) {
    // 复制 parent_ctx，但：新 Session、subset 工具、spawn 置空、depth+1
    todo("Stage 6: spawn_subagent");
}

}  // namespace mini
