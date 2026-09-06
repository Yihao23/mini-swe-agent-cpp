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
#include "mini_agent/process.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"



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

        // 路径边界由 sandbox 统一判定 —— 模型给的 path 是不可信输入，
        // `..`、符号链接、绝对路径都要挡在这里。
        const auto [p, decision] = ctx.sandbox->resolve_path(rel);
        if (!decision.allowed()) return ToolResult::error(decision.reason + ": " + rel);

        std::error_code ec;
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

        // 记下这次读取的 mtime。edit 靠它判断「读过没有」和「读完之后有没有被人改过」。
        if (ctx.session) {
            std::error_code mt;
            const auto stamp = fs::last_write_time(p, mt);
            if (!mt) ctx.session->read_files()[p.string()] = stamp;
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

class EditTool final : public Tool {
  public:
    std::string_view name() const override { return "edit"; }

    std::string_view description() const override {
        return "把文件里的 old_string 替换成 new_string。必须先用 read 读过这个文件。"
               "old_string 必须在文件中唯一出现 —— 不唯一时多带几行上下文。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "要修改的文件"}}},
              {"old_string", {{"type", "string"}, {"description", "被替换的原文，必须唯一"}}},
              {"new_string", {{"type", "string"}, {"description", "替换成什么"}}}}},
            {"required", Json::array({"path", "old_string", "new_string"})},
        };
    }

    bool read_only() const override { return false; }
    bool requires_permission() const override { return true; }

    /// ⚠️ 必须 override。默认实现取「第一个字符串参数」，而 nlohmann 按 key 字母序
    ///    遍历：new_string < old_string < path，默认会把**要写入的内容**当成审查对象，
    ///    于是 Write(src/**) 这类规则永远匹配不上 —— 沙箱静默失效。
    std::string subject(const Json& args) const override {
        return args.value("path", std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto rel = args.value("path", std::string{});
        const auto old_s = args.value("old_string", std::string{});
        const auto new_s = args.value("new_string", std::string{});
        if (rel.empty()) return ToolResult::error("缺少 path 参数");
        if (old_s.empty()) return ToolResult::error("old_string 不能为空");

        const auto [p, decision] = ctx.sandbox->resolve_path(rel);
        if (!decision.allowed()) return ToolResult::error(decision.reason + ": " + rel);

        std::error_code ec;
        if (!fs::exists(p, ec)) return ToolResult::error("文件不存在: " + rel);

        // ① 必须先 read —— 否则模型是在凭想象改文件
        if (!ctx.session) return ToolResult::error("没有会话上下文，无法确认是否已 read");
        auto& seen = ctx.session->read_files();
        const auto it = seen.find(p.string());
        if (it == seen.end())
            return ToolResult::error("必须先用 read 读过 " + rel + " 才能 edit");

        // ② 陈旧检查：read 之后文件被外部改过，这次 edit 会覆盖掉别人的改动
        const auto now = fs::last_write_time(p, ec);
        if (!ec && now != it->second)
            return ToolResult::error(rel + " 在你 read 之后被修改过，请重新 read 再 edit");

        std::ifstream in(p, std::ios::binary);
        if (!in) return ToolResult::error("打不开: " + rel);
        std::string content((std::istreambuf_iterator<char>(in)), {});
        in.close();

        // ③ 唯一性：出现多次时只替换第一个，模型会以为全改了
        const auto first = content.find(old_s);
        if (first == std::string::npos)
            return ToolResult::error("在 " + rel + " 中找不到 old_string");
        if (content.find(old_s, first + old_s.size()) != std::string::npos)
            return ToolResult::error("old_string 在 " + rel +
                                     " 中出现多次，请多带几行上下文使其唯一");

        content.replace(first, old_s.size(), new_s);

        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) return ToolResult::error("无法写入: " + rel);
        out << content;
        out.close();

        // ④ 更新时间戳 —— 这次改动是我们自己做的，不该在下一次 edit 时被当成"被人改过"
        const auto stamp = fs::last_write_time(p, ec);
        if (!ec) it->second = stamp;

        const auto line = 1 + std::count(content.begin(), content.begin() + first, '\n');
        return ToolResult{.content = std::format("已修改 {}（第 {} 行附近）", rel, line)};
    }
};

/// @brief Runs one shell command through run_shell.
///
/// @note Makes no permission decision of its own. By the time run() is called
///       the executor has already put the command through Sandbox::authorize,
///       which splits it on `&& || ; |` and checks every segment against
///       kDangerous and the rules. A tool that re-checked would be a second
///       place to keep the policy correct.
/// @note read_only() is false, so the executor will not run it concurrently
///       with anything: a command can write files, and two of them racing on
///       the same tree is not something the model can reason about.
class BashTool final : public Tool {
  public:
    std::string_view name() const override { return "bash"; }

    std::string_view description() const override {
        // ⚠️ 唯一告诉模型「何时该用」的地方。这里点名 read/edit/glob 是有意的 ——
        //    不写的话模型会用 `cat`、`sed -i`、`find`，绕开所有做了边界检查的工具。
        return "在工作目录里执行一条 shell 命令，返回合并后的 stdout+stderr 和退出码。"
               "用来跑测试、构建、git 等。"
               "读文件用 read、改文件用 edit、找文件用 glob —— 不要用 cat/sed/find 代替。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}, {"description", "要执行的 shell 命令"}}},
              {"timeout_sec",
               {{"type", "integer"}, {"description", "最多等多少秒，不填用配置里的默认值"}}}}},
            {"required", Json::array({"command"})},
        };
    }

    bool read_only() const override { return false; }            // 可能写文件 → 不并发
    bool requires_permission() const override { return true; }   // 必须过闸

    /// ⚠️ 默认实现取「按 key 字母序的第一个字符串参数」，这里只有 command 一个字符串，
    ///    结果碰巧是对的。仍然显式写出来：沙箱拿这个字符串去拆段、匹配 kDangerous，
    ///    哪天多加一个字符串参数（比如 description），默认实现会静默交出错误的东西。
    std::string subject(const Json& args) const override {
        return args.value("command", std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        // ⚠️ 不能用 args.value("command", "")：key 存在但类型不对时它**抛异常**，
        //    不是返回默认值。schema 只是给模型的提示，不是保证 —— 模型完全可能
        //    发 {"command": 42}。executor 虽然会兜住异常，但报错文字会变成一句
        //    没头没尾的 what()，模型看不懂该怎么改。
        if (!args.is_object() || !args.contains("command") || !args["command"].is_string())
            return ToolResult::error("缺少 command 参数（必须是字符串）");
        const auto cmd = args["command"].get<std::string>();
        if (cmd.empty()) return ToolResult::error("command 不能为空");

        const int cfg_timeout = ctx.cfg->tool_timeout_sec;
        const int want = args.contains("timeout_sec") && args["timeout_sec"].is_number_integer()
                             ? args["timeout_sec"].get<int>()
                             : cfg_timeout;
        // 模型给的超时只能往下调，不能超过配置上限 —— 否则它可以自己解除限制。
        const auto timeout = std::chrono::seconds{std::clamp(want, 1, cfg_timeout)};

        const auto r = run_shell(cmd, ctx.cfg->workdir, timeout, ctx.cfg->max_output_chars);

        // ProcessResult 有四种失败方式，这里塌成 ToolResult 的一个 bool + 一段文字。
        // 塌之前要把区别写进文字里，否则模型分不清「命令失败了」和「命令没跑起来」。
        if (r.spawn_failed)
            return ToolResult::error("无法启动命令: " + r.output);

        std::string body = r.output.empty() ? "(无输出)" : r.output;
        Json meta{{"exit_code", r.exit_code}, {"duration_ms", r.duration.count()}};

        if (r.timed_out)
            return ToolResult{.content = body, .is_error = true, .metadata = std::move(meta)};

        // 退出码非零是**失败**，但输出照给 —— 编译错误、测试失败都走这条路，
        // 那段输出正是模型下一步要读的东西。
        if (r.exit_code != 0)
            body += std::format("\n[退出码 {}]", r.exit_code);

        return ToolResult{
            .content = std::move(body),
            .is_error = r.exit_code != 0,
            .metadata = std::move(meta),
        };
    }
};

}  // namespace

ToolPtr make_read_tool() { return std::make_shared<ReadTool>(); }
ToolPtr make_write_tool() { todo("Stage 2: write"); }
ToolPtr make_edit_tool() { return std::make_shared<EditTool>(); }
ToolPtr make_glob_tool() { todo("Stage 2: glob —— 按修改时间倒序"); }
ToolPtr make_grep_tool() { todo("Stage 2: grep —— std::regex 够用，注意跳过二进制/大文件"); }
ToolPtr make_bash_tool() { return std::make_shared<BashTool>(); }
ToolPtr make_todo_tool() { todo("Stage 4: todo —— 覆盖式提交，每轮由 reminder 回灌"); }
ToolPtr make_skill_tool() { todo("Stage 5: skill —— 按名字加载完整手册"); }
ToolPtr make_memory_tool() { todo("Stage 5: memory —— search/load/write/delete"); }
ToolPtr make_task_tool() { todo("Stage 6: task —— 派一个子 agent"); }
ToolPtr make_task_graph_tool() { todo("Stage 6: task_graph —— 派一张带依赖的图"); }
ToolPtr make_bash_output_tool() { todo("Stage 6: bash_output"); }
ToolPtr make_kill_task_tool() { todo("Stage 6: kill_task"); }

std::vector<ToolPtr> builtin_tools(const Config&) { todo("Stage 2: builtin_tools —— 按开关拼表"); }

}  // namespace mini
