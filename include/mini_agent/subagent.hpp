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
// ── 派一个子 agent 时，什么换掉、什么共享 ───────────────────────────────────
//
//   父 agent 的 ToolContext                   子 agent 的 ToolContext
//   ──────────────────────                   ────────────────────────
//     cfg         ─────────共享────────────>   cfg          配置，只读
//     sandbox     ─────────共享────────────>   sandbox      同一道闸，子 agent
//                                                            的每次调用照样过
//     memory      ─────────共享────────────>   memory      ┐ 这些是**资源**，
//     skills      ─────────共享────────────>   skills      │ 子 agent 该够得着
//     background  ─────────共享────────────>   background  ┘
//
//     session     ───────✂ 换掉 ───────────>   全新的 Session（且不 bind）
//     registry    ───────✂ 换掉 ───────────>   registry.subset(type->tools)
//     spawn       ───────✂ 清空 ───────────>   {}          不能再嵌套
//     todos       ───────✂ 清空 ───────────>   []          计划不继承
//     depth       ───────  +1  ───────────>   depth + 1
//
//   分界线是「资源」和「这一轮的工作区」。session 和 registry 属于后者 ——
//   共享它们，子 agent 二十轮探索会全部落进父 agent 下一轮要发出去的历史里，
//   而派它的意义正好是避免这件事。
//
// ── 一次调用走完的路 ────────────────────────────────────────────────────────
//
//   模型: task(agent_type="explorer", prompt="X 在哪？")
//        │
//        ▼
//   TaskTool::run()  ── ctx.spawn 为空 ──> error（这里已经是子 agent 了）
//        │ 非空
//        ▼
//   ctx.spawn(type, prompt)          ← App 在接线时装的 lambda
//        │
//        ▼
//   spawn_subagent()
//        │
//        ├─ find_agent_type(type) ── 找不到 ──> 返回错误 + 列出可用的
//        ├─ depth + 1 >= kMaxAgentDepth ──> 返回「已达嵌套上限」
//        │
//        ├─ registry->subset(type->tools)      收窄
//        ├─ Session sub_session;               全新，不 bind → 不落盘
//        ├─ ToolContext sub_ctx = parent_ctx;  复制，再按上表改五处
//        ├─ AgentOptions{model, max_steps, identity = type->system}
//        │
//        ▼
//   Agent sub(cfg, llm, narrowed, sandbox, sub_session, sub_ctx, {}, opts)
//        │                        ═══════  ════════════
//        │                        ⚠️ registry 和 session 是**直接传给 Agent** 的。
//        │                           sub_ctx 里那两个字段只给**工具**看。
//        │                           两处都要改对：漏了前者，循环用错工具表；
//        │                           漏了后者，工具把 read_files 记到父那边去。
//        │
//        │  事件不往上传（第七个参数是 {}）—— 父 agent 的终端不该被
//        │  子 agent 的工具调用刷屏
//        ▼
//   sub.run(prompt)   跑到 end_turn 或者用完 max_steps
//        │
//        ▼
//   只有 conclusion 跨回来                    ← 二十轮调查 = 一段文字
//        │
//        ▼
//   ToolResult{content = conclusion}
//
// ── 三道防线挡「无限嵌套」──────────────────────────────────────────────────
//
//   ① 工具白名单     四种类型的 tools 里都没有 task/task_graph
//                     → 子 agent 根本拿不到那个工具        ← 当前实际生效的那道
//   ② sub_ctx.spawn  清空
//                     → 就算拿到了工具，TaskTool 也会拒绝
//   ③ depth 检查     depth + 1 >= kMaxAgentDepth 直接返回
//                     → 就算前两道都破了，也只能再深一层
//
//   ⚠️ ① 和 ② 现在互为冗余，所以单行变异测不出 ②（改任何一处，另一处都兜住）。
//      留着 ② 是因为 ① 是**数据**：哪天有人给 general 加上 task，
//      白名单那道就没了，而那时 ② 是唯一还站着的。
//
// ── 一条这一层保证不了的 ────────────────────────────────────────────────────
//
//   任务描述必须**自包含**。子 agent 看不到父的历史，所以 prompt 里得写清
//   路径、约束、什么算完成。这是隔离的直接代价，代码检查不了 ——
//   只能在 TaskTool 的 description 里对模型讲清楚。
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
