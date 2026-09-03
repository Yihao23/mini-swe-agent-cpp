// 【Stage 2/5/6】内置工具。
//
// 具体的类定义就写在这个文件里（或拆成 tools/file.cpp、tools/bash.cpp…都行），
// 外面只看得见工厂函数返回的 ToolPtr。
//
// 一个工具的样子：
//
//     namespace {
//     class ReadTool final : public Tool {
//       public:
//         std::string_view name() const override { return "read"; }
//         std::string_view description() const override { return "..."; }
//         Json input_schema() const override { return {...}; }
//         bool read_only() const override { return true; }
//         bool requires_permission() const override { return false; }
//         ToolResult run(const Json& args, ToolContext& ctx) override { ... }
//     };
//     }  // namespace
//     ToolPtr make_read_tool() { return std::make_shared<ReadTool>(); }

#include "mini_agent/tools/builtin.hpp"

#include "mini_agent/config.hpp"

namespace mini {

ToolPtr make_read_tool() { todo("Stage 2: read —— 带行号、offset/limit、目录要列出来"); }
ToolPtr make_write_tool() { todo("Stage 2: write"); }
ToolPtr make_edit_tool() { todo("Stage 2: edit —— 陈旧检查 + old_string 唯一性"); }
ToolPtr make_glob_tool() { todo("Stage 2: glob —— 按修改时间倒序"); }
ToolPtr make_grep_tool() { todo("Stage 2: grep —— std::regex 够用，注意跳过二进制/大文件"); }
ToolPtr make_bash_tool() { todo("Stage 2: bash —— 调 run_shell，权限已由 executor 过闸"); }
ToolPtr make_todo_tool() { todo("Stage 4: todo —— 覆盖式提交，每轮由 reminder 回灌"); }
ToolPtr make_skill_tool() { todo("Stage 5: skill —— 按名字加载完整手册"); }
ToolPtr make_memory_tool() { todo("Stage 5: memory —— search/load/write/delete"); }
ToolPtr make_task_tool() { todo("Stage 6: task —— 派一个子 agent"); }
ToolPtr make_task_graph_tool() { todo("Stage 6: task_graph —— 派一张带依赖的图"); }
ToolPtr make_bash_output_tool() { todo("Stage 6: bash_output"); }
ToolPtr make_kill_task_tool() { todo("Stage 6: kill_task"); }

std::vector<ToolPtr> builtin_tools(const Config&) { todo("Stage 2: builtin_tools —— 按开关拼表"); }

}  // namespace mini
