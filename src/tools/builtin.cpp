// 【Stage 2/5/6】内置工具。
//
// 具体的类定义就写在这个文件里（或拆成 tools/file.cpp、tools/bash.cpp…都行），
// 外面只看得见工厂函数返回的 ToolPtr。
//
// 一个工具的样子：
//
//   
//     ToolPtr make_read_tool() { return std::make_shared<ReadTool>(); }

#include "mini_agent/tools/builtin.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <system_error>

#include "mini_agent/config.hpp"



namespace mini {

namespace fs = std::filesystem;

namespace {

class ReadTool final : public Tool {
  public:
    std::string_view name() const override { return "read"; }

    std::string_view description() const override {
        // ⚠️ 这是唯一告诉模型"何时该用"的地方（tool.hpp:69）。
        //    写得含糊，模型就会在该用 grep 的时候用 read。
        return "读取文件内容，带行号。可用 offset/limit 只读一段。"
               "path 是目录时列出条目。读之前不要猜文件内容。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "文件或目录路径，相对于工作目录"}}},
              {"offset", {{"type", "integer"}, {"description", "起始行号，从 1 开始"}}},
              {"limit", {{"type", "integer"}, {"description", "最多读多少行，默认 2000"}}}}},
            {"required", Json::array({"path"})},
        };
    }

    bool read_only() const override { return true; }             // executor 敢并发
    bool requires_permission() const override { return false; }  // 越界防护在 sandbox，不在这

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto rel = args.value("path", std::string{});
        if (rel.empty()) return ToolResult::error("缺少 path 参数");

        // TODO(Stage 3): 换成 ctx.sandbox->resolve_path(rel) —— 它负责挡 ../../etc/passwd
        std::error_code ec;
        const fs::path p = fs::weakly_canonical(ctx.cfg->workdir / rel, ec);
        if (ec) return ToolResult::error("路径无法解析: " + rel);
        if (!fs::exists(p, ec)) return ToolResult::error("文件不存在: " + rel);

        if (fs::is_directory(p, ec)) return list_directory(p, rel);

        std::ifstream in(p);
        if (!in) return ToolResult::error("打不开: " + rel);

        const int offset = std::max(1, args.value("offset", 1));   // 从 1 开始，和编辑器一致
        const int limit = std::max(1, args.value("limit", 2000));

        std::string out;
        std::string line;
        int lineno = 0, emitted = 0;
        bool truncated = false;
        while (std::getline(in, line)) {
            if (++lineno < offset) continue;
            if (emitted >= limit) { truncated = true; break; }
            ++emitted;
            out += std::format("{:6}\t{}\n", lineno, line);
        }

        if (lineno == 0) return ToolResult{.content = "(空文件)"};
        if (emitted == 0)
            return ToolResult{.content = std::format("(文件共 {} 行，offset {} 已越过末尾)",
                                                     lineno, offset)};
        // 明确告诉模型还有更多 —— 否则它会把这段当成全部内容下结论
        if (truncated)
            out += std::format("... (还有更多行，用 offset={} 继续读)\n", offset + emitted);
        return ToolResult{.content = std::move(out)};
    }

  private:
    /// 模型经常拿不准某个路径是文件还是目录。直接列出来，省一轮往返。
    static ToolResult list_directory(const fs::path& p, const std::string& rel) {
        std::error_code ec;
        std::vector<std::string> entries;
        for (const auto& e : fs::directory_iterator(p, ec)) {
            auto name = e.path().filename().string();
            entries.push_back(e.is_directory(ec) ? name + "/" : name);
        }
        if (ec) return ToolResult::error("无法列出目录: " + rel);

        std::ranges::sort(entries);   // 顺序稳定 —— 文件系统的遍历顺序不保证
        std::string out = rel + " 是一个目录，包含 " + std::to_string(entries.size()) + " 项:\n";
        for (const auto& e : entries) out += "  " + e + "\n";
        return ToolResult{.content = std::move(out)};
    }
};

}  // namespace

ToolPtr make_read_tool() { return std::make_shared<ReadTool>(); }
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
