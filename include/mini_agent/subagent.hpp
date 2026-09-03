#pragma once
//
// 【Stage 6】多 agent —— 子 agent 是"一次性的、上下文隔离的、只返回结论的"工人。
//
// ── 新手直觉是错的 ──────────────────────────────────────────────────────────
//
// 直觉：子 agent = 并行 = 快。
// **真正的价值：上下文隔离。** 子 agent 读了两万行代码，主 agent 只收到 300 字结论 ——
// 主循环的上下文没被中间过程撑爆，缓存前缀也保住了。并行只是附赠。
//
// 代价你必须接受：**子 agent 看不到主 agent 的历史**，所以派活时任务描述必须自包含
// （路径、约束、要什么格式的结论）。这个限制不是缺陷，是隔离的定义。
//
// 实现上就三件事：
//   1. 全新的 Session（这就是隔离）
//   2. 收窄的工具集（explorer 只给只读工具 → 天然改不坏东西）
//   3. 可能更便宜的模型（"读得多、判断少"的活不需要最强模型）
//
#include <string>
#include <vector>

#include "mini_agent/tool.hpp"

namespace mini {

struct Config;
class LlmClient;
class Sandbox;

inline constexpr int kMaxAgentDepth = 2;   // 子 agent 不能再派子 agent

struct AgentType {
    std::string name;
    std::string description;              // 写给**协调者模型**看的：什么活该交给它
    std::vector<std::string> tools;       // 工具白名单
    std::string system;                   // 覆盖身份段
    bool use_main_model = false;          // false = 用 cfg.subagent_model
    int max_steps = 20;
};

/// 建议先定这四种。每种给什么工具、什么 prompt、什么模型，是你的设计题。
///   explorer  只读调查，返回结论 + `文件:行号`
///   coder     独立完成一处改动并自验
///   reviewer  只读评审，只报告不改
///   general   工具齐全，边界不清的任务
/// TODO(Stage 6)
const std::vector<AgentType>& agent_types();
const AgentType* find_agent_type(std::string_view name);
std::string render_agent_types();

/// 跑一个子 agent，返回它的最终文本。
///
/// 注意 parent_ctx 的用法：**复制**一份（共享 memory/skills/background），但
///   * session 换成全新的
///   * registry 换成 subset
///   * spawn 置空（禁止再嵌套）
///   * depth + 1
/// TODO(Stage 6)
std::string spawn_subagent(const Config& cfg, LlmClient& llm, Sandbox& sandbox,
                           const ToolContext& parent_ctx, std::string_view agent_type,
                           std::string_view task_prompt);

}  // namespace mini
