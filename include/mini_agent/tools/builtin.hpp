#pragma once
//
// 【Stage 2/5/6】内置工具的工厂函数。
//
// ── 注意这里没有一个具体的 class 声明 ───────────────────────────────────────
//
// ReadTool / BashTool 这些类型全部关在各自的 .cpp 里，外面只看得见 `ToolPtr`。
// 好处：
//   * 改一个工具的私有成员，不会引起全项目重编译
//   * 注册表只依赖 Tool 接口，编译期依赖是一条线而不是一张网
//   * 逼你只通过接口使用工具 —— 想在别处 dynamic_cast 回 BashTool？做不到，这是对的
//
// 这是 C++ 里"接口/实现分离"最省事的一种做法，比 pimpl 轻，比暴露类干净。
//
#include <vector>

#include "mini_agent/tool.hpp"

namespace mini {

struct Config;

// --- Stage 2 ---
ToolPtr make_read_tool();
ToolPtr make_write_tool();
ToolPtr make_edit_tool();
ToolPtr make_glob_tool();
ToolPtr make_grep_tool();
ToolPtr make_bash_tool();

// --- Stage 4：agent 自己维护的计划清单，每轮通过 reminder 回灌 ---
ToolPtr make_todo_tool();

// --- Stage 5：渐进式披露的两个入口 ---
ToolPtr make_skill_tool();
ToolPtr make_memory_tool();

// --- Stage 6 ---
ToolPtr make_task_tool();          // 派一个子 agent
ToolPtr make_task_graph_tool();    // 派一张带依赖的任务图
ToolPtr make_bash_output_tool();   // 读后台任务输出
ToolPtr make_kill_task_tool();

/// 按 cfg 的开关拼出内置工具表。
/// 顺序无所谓 —— ToolRegistry::schemas() 会按名字排序（缓存前缀要稳定）。
std::vector<ToolPtr> builtin_tools(const Config& cfg);

}  // namespace mini
