#pragma once
/// @file
/// @brief Sub-agents: single-use, context-isolated workers that return a conclusion (Stage 6).
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

/// @brief How deep sub-agents may nest.
///
/// @warning 2 means a sub-agent cannot spawn another. Without a ceiling, a
///          model that decides delegation is the answer keeps delegating, and
///          each level multiplies the cost of the one above it.
inline constexpr int kMaxAgentDepth = 2;   // 子 agent 不能再派子 agent

/// @brief A sub-agent profile: which tools, which prompt, which model.
///
/// @note These are the knobs that make a sub-agent worth having. Narrower
///       tools mean less that can go wrong; a cheaper model means delegation
///       is not more expensive than doing the work inline.
struct AgentType {
    std::string name;               ///< How the coordinator refers to it.

    /// @brief What it is for, written for the **coordinating model** to read.
    /// @note This is the only thing telling the coordinator which jobs to hand
    ///       over. Vague wording here shows up as work delegated to the wrong
    ///       profile, or not delegated at all.
    std::string description;

    /// @brief Tool allow-list; the sub-agent gets a ToolRegistry::subset of it.
    /// @note This is why tools are held by shared_ptr — the parent registry and
    ///       the subset point at the same instances.
    std::vector<std::string> tools;

    std::string system;             ///< Replaces the identity section.
    bool use_main_model = false;    ///< false uses cfg.subagent_model, which is cheaper.
    int max_steps = 20;             ///< Lower than the main agent's; the job is narrower.
};

/// @brief The available profiles.
///
/// @return The built-in list, in a stable order.
///
/// @note Four to start with: explorer (read-only investigation, reports
///       `file:line`), coder (one change, verified by itself), reviewer
///       (read-only, reports without editing), general (everything, for tasks
///       that do not fit).
const std::vector<AgentType>& agent_types();

/// @brief Look a profile up by name.
/// @param name What the coordinator asked for.
/// @return nullptr when there is no such profile.
/// @note nullptr rather than an exception, the same reasoning as
///       ToolRegistry::get: the name came from the model, so a typo is routine
///       and it can correct itself once told what exists.
const AgentType* find_agent_type(std::string_view name);

/// @brief The profile list, formatted for the coordinating model.
/// @return One entry per profile: name and description.
/// @note Goes into the system prompt, so it must stay byte-stable — which is
///       why agent_types() has a fixed order.
std::string render_agent_types();

/// @brief Run a sub-agent to completion and return its conclusion.
///
/// @param cfg         Configuration; the sub-agent model comes from here.
/// @param llm         Model client, shared with the parent.
/// @param sandbox     Permission gate, shared with the parent.
/// @param parent_ctx  Copied, then narrowed — see the warning.
/// @param agent_type  Which profile.
/// @param task_prompt What the sub-agent should do.
/// @return Its final text. Only this crosses back; the sub-agent's own
///         history is discarded.
///
/// @warning parent_ctx is **copied and then narrowed**, never used as-is:
///          a fresh session, a registry subset, spawn cleared so it cannot
///          nest, and depth + 1. Sharing the parent's session would let a
///          sub-agent's exploratory turns land in the conversation the parent
///          is about to send.
///
/// @note memory, skills and background stay shared. Those are the resources a
///       sub-agent should reach — what it must not reach is the parent's
///       conversation.
/// @note Returning only the conclusion is the point of the whole mechanism:
///       twenty turns of investigation cost the parent one paragraph of
///       context instead of twenty turns of it.
std::string spawn_subagent(const Config& cfg, LlmClient& llm, Sandbox& sandbox,
                           const ToolContext& parent_ctx, std::string_view agent_type,
                           std::string_view task_prompt);

}  // namespace mini
