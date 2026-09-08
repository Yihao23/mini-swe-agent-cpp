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

#include <fnmatch.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <system_error>

#include "mini_agent/config.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/process.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/scheduler.hpp"
#include "mini_agent/skills.hpp"
#include "mini_agent/subagent.hpp"



namespace mini {

namespace fs = std::filesystem;

namespace {

// ── 取参数 ──────────────────────────────────────────────────────────────────
//
// ⚠️ 不要用 args.value(key, default)。key 存在但类型不对时它**抛异常**，不是
//    退回默认值 —— {"path": 42} 会抛 json::type_error。schema 是给模型的提示，
//    不是保证；模型给错类型是家常便饭。executor 虽然兜得住异常，但模型收到的
//    会是一句没头没尾的 what()，看不出该改哪个参数。

/// 取一个字符串参数；缺失或类型不对都返回 nullopt。
std::optional<std::string> str_arg(const Json& args, const char* key) {
    if (!args.is_object()) return std::nullopt;
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/// 取一个整数参数；缺失或类型不对都退回 fallback。
int int_arg(const Json& args, const char* key, int fallback) {
    if (!args.is_object()) return fallback;
    const auto it = args.find(key);
    return (it != args.end() && it->is_number_integer()) ? it->get<int>() : fallback;
}

/// @brief Reads a file with line numbers, or lists a directory.
///
/// @note Line numbers are part of the output, not decoration: edit works by
///       matching a unique string, and the model needs somewhere to anchor
///       when it explains which part it is changing.
/// @note Stamps the file's mtime onto the session. edit and write both read
///       that back — this is the only thing that makes "you have not read this
///       file" and "it changed after you read it" answerable.
/// @note A directory path lists its entries rather than failing. The model
///       often cannot tell which it has; failing would cost a round trip to
///       learn something the call already knew.
// ── glob / grep 共用 ───────────────────────────────────────────────────────

/// 遍历时永远跳过的目录名。
///
/// ⚠️ 不跳的话结果会被淹没：`build/` 下面几千个 .o 和 CMake 的中间文件，
///    `.git/` 下面成千上万个 object。模型要找的那三个源文件会排在几百行之后，
///    而且中间那些内容会把整轮上下文撑爆。
const char* const kSkipDirs[] = {
    ".git", ".hg", ".svn", "build", "node_modules", "__pycache__",
    ".venv", "venv", "target", "dist", ".mini-agent", ".cache",
};

bool is_skipped_dir(const std::string& name) {
    return std::ranges::find(kSkipDirs, name) != std::ranges::end(kSkipDirs);
}

/// glob 模式匹配。不加 FNM_PATHNAME，所以 `*` 会跨 `/`。
///
/// @note 副作用是 `*.cpp` 和 `**/*.cpp` 在这里等价 —— 都能匹配 src/a/b.cpp。
///       对 agent 来说这是想要的：模型写 `*.cpp` 时几乎总是指"所有 cpp 文件"，
///       而不是"仅顶层的"。和 Sandbox::Rule::matches 的取舍一致。
///
/// ⚠️ `**/` 必须能匹配**零个**目录，所以匹配失败时要把它整个删掉再试一次。
///    fnmatch 眼里 `**` 只是两个 `*` 连写、等于一个 `*`，于是 `src/**/*.cpp`
///    被拆成 "src/" + `*` + "/" + "*.cpp" —— 那个斜杠是字面量，必须存在。
///    结果 src/tools/builtin.cpp 匹配得上，src/app.cpp 匹配不上。
///    bash 的 globstar、ripgrep、Claude Code 都把 `**/` 当成「零个或多个目录」，
///    模型也是照这个预期写的。这个 bug 是 agent 自己跑起来之后发现的：
///    它拿 src/**/*.cpp 只捞回一个文件，于是自己换了两个模式重试。
bool glob_match(const std::string& pattern, const std::string& rel) {
    if (::fnmatch(pattern.c_str(), rel.c_str(), 0) == 0) return true;
    // 每次去掉一个 `**/` 再试；有多个就逐个递归，一定会收敛。
    const auto at = pattern.find("**/");
    if (at == std::string::npos) return false;
    return glob_match(std::string(pattern).erase(at, 3), rel);
}

/// 从 root 往下走，对每个**文件**调 fn(相对路径, 绝对路径)。
///
/// @note 用 recursive_directory_iterator 的 disable_recursion_pending() 来剪枝，
///       而不是进去之后再过滤 —— 后者仍然会把 .git 下面几万个条目全遍历一遍。
template <class F>
void walk(const fs::path& root, const fs::path& rel_base, F&& fn) {
    std::error_code ec;
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& p = it->path();
        if (it->is_directory(ec)) {
            if (is_skipped_dir(p.filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;   // 跳过符号链接、fifo、设备
        fn(p.lexically_relative(rel_base).generic_string(), p);
    }
}

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
        const auto path = str_arg(args, "path");
        if (!path || path->empty()) return ToolResult::error("缺少 path 参数（必须是字符串）");
        const auto& rel = *path;

        // 路径边界由 sandbox 统一判定 —— 模型给的 path 是不可信输入，
        // `..`、符号链接、绝对路径都要挡在这里。
        const auto [p, decision] = ctx.sandbox->resolve_path(rel);
        if (!decision.allowed()) return ToolResult::error(decision.reason + ": " + rel);

        std::error_code ec;
        if (!fs::exists(p, ec)) return ToolResult::error("文件不存在: " + rel);

        if (fs::is_directory(p, ec)) return list_directory(p, rel);

        std::ifstream in(p);
        if (!in) return ToolResult::error("打不开: " + rel);

        const int offset = std::max(1, int_arg(args, "offset", 1));   // 从 1 开始，和编辑器一致
        const int limit = std::max(1, int_arg(args, "limit", 2000));

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

/// @brief Creates a file, or replaces one whole.
///
/// @note Overwriting a file the model has not read is refused, the same
///       discipline edit follows. write replaces everything, so doing it
///       sight-unseen deletes work the model never knew was there. A file that
///       does not exist yet has nothing to lose and needs no prior read.
/// @note A file this tool just wrote counts as read from then on — its content
///       is what we put there, so nothing about it is unknown.
class WriteTool final : public Tool {
  public:
    std::string_view name() const override { return "write"; }

    std::string_view description() const override {
        return "把 content 写进 path，文件不存在就新建、已存在则**整个覆盖**。"
               "只改一小段时用 edit，别用 write 重写整个文件。"
               "覆盖已有文件前必须先 read。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"path", {{"type", "string"}, {"description", "文件路径，相对于工作目录"}}},
              {"content", {{"type", "string"}, {"description", "文件的完整内容"}}}}},
            {"required", Json::array({"path", "content"})},
        };
    }

    bool read_only() const override { return false; }
    bool requires_permission() const override { return true; }

    /// ⚠️ 这是本文件里最关键的一个 override。默认实现取「按 key 字母序的第一个
    ///    字符串参数」，而 content < path —— 沙箱会拿**要写入的正文**去匹配规则，
    ///    于是 `Write(src/**)` 这类规则永远命中不了。整层权限静默失效，
    ///    没有任何报错。删掉这个函数，代码照样编译、照样跑。
    std::string subject(const Json& args) const override {
        return str_arg(args, "path").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto path = str_arg(args, "path");
        const auto body = str_arg(args, "content");
        if (!path || path->empty()) return ToolResult::error("缺少 path 参数（必须是字符串）");
        if (!body) return ToolResult::error("缺少 content 参数（必须是字符串）");
        const auto& rel = *path;

        const auto [p, decision] = ctx.sandbox->resolve_path(rel);
        if (!decision.allowed()) return ToolResult::error(decision.reason + ": " + rel);

        std::error_code ec;
        const bool existed = fs::exists(p, ec);
        if (existed && fs::is_directory(p, ec))
            return ToolResult::error(rel + " 是一个目录");

        // ⚠️ 覆盖已有文件前必须先 read —— write 是整个替换，没读过就写等于
        //    凭想象删掉别人的代码。新文件没有这个问题。
        if (existed) {
            if (!ctx.session) return ToolResult::error("没有会话上下文，无法确认是否已 read");
            if (!ctx.session->read_files().contains(p.string()))
                return ToolResult::error(rel + " 已存在，必须先用 read 读过才能覆盖");
        }

        // 父目录可能还不存在。不建的话模型得先跑一条 bash mkdir，白费一轮。
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
            if (ec) return ToolResult::error("无法创建目录 " + p.parent_path().string());
        }

        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) return ToolResult::error("无法写入: " + rel);
        out << *body;
        out.close();
        if (!out) return ToolResult::error("写入 " + rel + " 时出错");

        // 这次改动是我们自己做的：记下 mtime，下一次 edit 才不会当成「被人改过」。
        // 顺带也让刚新建的文件算作「读过」——内容就是我们刚写的，没有未知。
        if (ctx.session) {
            const auto stamp = fs::last_write_time(p, ec);
            if (!ec) ctx.session->read_files()[p.string()] = stamp;
        }

        const auto lines = 1 + std::ranges::count(*body, '\n');
        return ToolResult{
            .content = std::format("已{} {}（{} 字节，{} 行）", existed ? "覆盖" : "创建", rel,
                                   body->size(), body->empty() ? 0 : lines),
            .metadata = {{"created", !existed}, {"bytes", body->size()}},
        };
    }
};

/// @brief Replaces one span inside a file.
///
/// Three guards, and each exists because the failure it prevents is silent:
///   1. the file must have been read — otherwise the model is editing from
///      imagination;
///   2. it must not have changed since — otherwise this overwrites whatever
///      the other writer did;
///   3. old_string must be unique — otherwise only the first of several
///      matches changes and the model believes it changed them all.
///
/// @note write has none of guards 2 and 3 and does not need them: replacing
///       the file whole is its stated job, and there is no "which occurrence"
///       to get wrong.
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
        return str_arg(args, "path").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto path = str_arg(args, "path");
        const auto old_arg = str_arg(args, "old_string");
        const auto new_arg = str_arg(args, "new_string");
        if (!path || path->empty()) return ToolResult::error("缺少 path 参数（必须是字符串）");
        if (!old_arg) return ToolResult::error("缺少 old_string 参数（必须是字符串）");
        const auto& rel = *path;
        const auto& old_s = *old_arg;
        const std::string new_s = new_arg.value_or(std::string{});   // 允许空 = 删除这段
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

/// @brief Finds files by name, newest first.
///
/// @note Sorted by modification time descending, not alphabetically. A model
///       asking "which .cpp files are there" almost always wants the ones
///       someone touched recently; alphabetical order puts app.cpp first for
///       no reason anyone cares about.
/// @note Never opens a file — it only reads directory entries, which is why it
///       can answer in milliseconds where grep has to read everything.
class GlobTool final : public Tool {
  public:
    std::string_view name() const override { return "glob"; }

    std::string_view description() const override {
        return "按文件名模式查找文件，结果按修改时间从新到旧排序。"
               "模式如 `**/*.cpp`、`test_*.py`、`src/**/*.hpp`。"
               "找文件内容用 grep，读文件用 read。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"}, {"description", "glob 模式，如 **/*.cpp"}}},
              {"path", {{"type", "string"}, {"description", "从哪个子目录开始找，默认工作目录"}}}}},
            {"required", Json::array({"pattern"})},
        };
    }

    bool read_only() const override { return true; }
    bool requires_permission() const override { return false; }   // 边界仍由 sandbox 管

    /// 审查对象是搜索**起点**，不是模式 —— 权限规则约束的是"能看哪个目录"。
    std::string subject(const Json& args) const override {
        return str_arg(args, "path").value_or(std::string{"."});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto pattern = str_arg(args, "pattern");
        if (!pattern || pattern->empty())
            return ToolResult::error("缺少 pattern 参数（必须是字符串）");

        const auto [root, decision] = ctx.sandbox->resolve_path(
            str_arg(args, "path").value_or(std::string{"."}));
        if (!decision.allowed()) return ToolResult::error(decision.reason);

        std::error_code ec;
        if (!fs::is_directory(root, ec)) return ToolResult::error("不是目录: " + root.string());

        std::vector<std::pair<fs::file_time_type, std::string>> hits;
        walk(root, ctx.cfg->workdir, [&](const std::string& rel, const fs::path& abs) {
            if (!glob_match(*pattern, rel) && !glob_match(*pattern, fs::path(rel).filename().string()))
                return;
            std::error_code e;
            hits.emplace_back(fs::last_write_time(abs, e), rel);
        });

        if (hits.empty()) return ToolResult{.content = "没有匹配 " + *pattern + " 的文件"};

        std::ranges::sort(hits, std::ranges::greater{}, &decltype(hits)::value_type::first);
        const bool truncated = hits.size() > kMaxResults;
        if (truncated) hits.resize(kMaxResults);

        std::string out;
        for (const auto& [_, rel] : hits) out += rel + "\n";
        if (truncated)
            out += std::format("... (只显示最近修改的 {} 个，用更精确的模式缩小范围)\n",
                               kMaxResults);
        return ToolResult{.content = std::move(out),
                          .metadata = {{"count", hits.size()}, {"truncated", truncated}}};
    }

  private:
    static constexpr std::size_t kMaxResults = 200;
};

/// @brief Finds files by content.
///
/// @warning Binary files are skipped, detected by a NUL byte in the first 8 KB.
///          Without that a single `.o` under build/ can emit thousands of lines
///          of mojibake and swallow the whole turn's context — and the model
///          cannot do anything with the result either way.
///
/// @note Output is `path:line: text`, the shape every developer tool uses. The
///       line number is what lets the model go straight to read with an offset
///       instead of pulling the whole file.
/// @note Long lines are clipped. One minified .js line is a megabyte.
class GrepTool final : public Tool {
  public:
    std::string_view name() const override { return "grep"; }

    std::string_view description() const override {
        return "在文件内容里搜正则，返回 路径:行号: 内容。"
               "可用 glob 参数限定文件（如 `**/*.cpp`）。"
               "按文件名找文件用 glob，读整个文件用 read。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"pattern", {{"type", "string"}, {"description", "ECMAScript 正则"}}},
              {"path", {{"type", "string"}, {"description", "从哪个子目录开始搜，默认工作目录"}}},
              {"glob", {{"type", "string"}, {"description", "只搜匹配这个模式的文件"}}},
              {"ignore_case", {{"type", "boolean"}, {"description", "忽略大小写"}}}}},
            {"required", Json::array({"pattern"})},
        };
    }

    bool read_only() const override { return true; }
    bool requires_permission() const override { return false; }

    std::string subject(const Json& args) const override {
        return str_arg(args, "path").value_or(std::string{"."});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto pattern = str_arg(args, "pattern");
        if (!pattern || pattern->empty())
            return ToolResult::error("缺少 pattern 参数（必须是字符串）");

        // ⚠️ 正则是模型给的，写坏了会抛。抛出去模型只能看到一句 what()，
        //    这里翻译成它能改的话。
        auto flags = std::regex::ECMAScript | std::regex::optimize;
        if (args.is_object() && args.contains("ignore_case") && args["ignore_case"] == true)
            flags |= std::regex::icase;
        std::regex re;
        try {
            re.assign(*pattern, flags);
        } catch (const std::regex_error& e) {
            return ToolResult::error("正则有语法错误: " + *pattern + " —— " + e.what());
        }

        const auto [root, decision] = ctx.sandbox->resolve_path(
            str_arg(args, "path").value_or(std::string{"."}));
        if (!decision.allowed()) return ToolResult::error(decision.reason);

        std::error_code ec;
        if (!fs::is_directory(root, ec)) return ToolResult::error("不是目录: " + root.string());

        const auto file_glob = str_arg(args, "glob");
        std::string out;
        std::size_t matches = 0, files = 0;
        bool truncated = false;

        walk(root, ctx.cfg->workdir, [&](const std::string& rel, const fs::path& abs) {
            if (truncated) return;
            if (file_glob && !glob_match(*file_glob, rel) &&
                !glob_match(*file_glob, fs::path(rel).filename().string()))
                return;

            std::error_code e;
            if (fs::file_size(abs, e) > kMaxFileBytes || e) return;   // 太大，正则会很慢

            std::ifstream in(abs, std::ios::binary);
            if (!in) return;
            if (looks_binary(in)) return;
            in.clear();
            in.seekg(0);

            std::string line;
            int lineno = 0;
            bool counted = false;
            while (std::getline(in, line)) {
                ++lineno;
                if (!std::regex_search(line, re)) continue;
                if (++matches > kMaxMatches) { truncated = true; return; }
                if (!counted) { ++files; counted = true; }
                if (line.size() > kMaxLineChars)
                    line = line.substr(0, kMaxLineChars) + " …(行过长已截断)";
                out += std::format("{}:{}: {}\n", rel, lineno, line);
            }
        });

        if (out.empty()) return ToolResult{.content = "没有匹配 " + *pattern + " 的内容"};
        if (truncated)
            out += std::format("... (超过 {} 处匹配，用更精确的正则或 glob 缩小范围)\n",
                               kMaxMatches);
        return ToolResult{.content = std::move(out),
                          .metadata = {{"matches", matches}, {"files", files},
                                       {"truncated", truncated}}};
    }

  private:
    static constexpr std::size_t kMaxMatches = 200;
    static constexpr std::uintmax_t kMaxFileBytes = 2u << 20;   // 2 MiB
    static constexpr std::size_t kMaxLineChars = 400;
    static constexpr std::streamsize kSniffBytes = 8192;

    /// NUL 字节 = 二进制。文本文件里不会有，可执行文件、.o、图片里到处都是。
    static bool looks_binary(std::istream& in) {
        char buf[kSniffBytes];
        in.read(buf, kSniffBytes);
        const auto got = in.gcount();
        return std::memchr(buf, '\0', static_cast<std::size_t>(got)) != nullptr;
    }
};

/// @brief Loads one skill's manual in full.
///
/// @note Read-only and exempt from the gate, like read: it opens files the
///       operator put there on purpose, inside directories the config names.
class SkillTool final : public Tool {
  public:
    std::string_view name() const override { return "skill"; }

    std::string_view description() const override {
        return "按名字加载一份操作手册的完整内容。"
               "system prompt 里只有索引（名字 + 什么时候用），正文要用这个工具取。"
               "任务和某个 skill 的描述对上时就加载它，别凭印象操作。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}, {"description", "索引里列出的 skill 名字"}}}}},
            {"required", Json::array({"name"})},
        };
    }

    bool read_only() const override { return true; }
    bool requires_permission() const override { return false; }

    std::string subject(const Json& args) const override {
        return str_arg(args, "name").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto want = str_arg(args, "name");
        if (!want || want->empty()) return ToolResult::error("缺少 name 参数（必须是字符串）");
        if (!ctx.skills) return ToolResult::error("skills 未启用");

        const Skill* s = ctx.skills->get(*want);
        if (!s) {
            // ⚠️ 列出有哪些 —— 模型是照着索引写的名字，写错时它自己能纠正，
            //    光说"没有这个 skill"它只能再猜一次。
            std::string names;
            for (const Skill* k : ctx.skills->all()) names += (names.empty() ? "" : ", ") + k->name;
            return ToolResult::error("没有名为 " + *want + " 的 skill。可用: " +
                                     (names.empty() ? "（一个都没有）" : names));
        }
        return ToolResult{.content = s->render(),
                          .metadata = {{"skill", s->name}, {"path", s->path.string()}}};
    }
};

/// @brief Search, load, write and delete long-term memories.
///
/// @note One tool with an `action` rather than four tools. Four would take four
///       slots in every request's tool list — bytes in the cached prefix — for
///       operations the model uses rarely and never confuses.
/// @warning `write` and `delete` change files, so this tool is **not**
///          read_only and does need authorisation, unlike skill.
class MemoryTool final : public Tool {
  public:
    std::string_view name() const override { return "memory"; }

    std::string_view description() const override {
        return "长期记忆：search / load / write / delete。"
               "记用户的偏好、项目的约定、已经查明又容易忘的事实；"
               "别记代码里已有的东西（结构、历史、CLAUDE.md）。"
               "一条记一件事，description 写「什么时候该用它」而不是「它是什么」。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"action", {{"type", "string"},
                          {"enum", Json::array({"search", "load", "write", "delete"})},
                          {"description", "要做什么"}}},
              {"query", {{"type", "string"}, {"description", "search 用：关键词"}}},
              {"name", {{"type", "string"}, {"description", "load/write/delete 用：记忆名"}}},
              {"description", {{"type", "string"}, {"description", "write 用：什么时候该用它"}}},
              {"body", {{"type", "string"}, {"description", "write 用：正文"}}},
              {"type", {{"type", "string"},
                        {"enum", Json::array({"user", "feedback", "project", "reference"})},
                        {"description", "write 用：记忆类型"}}}}},
            {"required", Json::array({"action"})},
        };
    }

    bool read_only() const override { return false; }            // write/delete 会改文件
    bool requires_permission() const override { return true; }

    /// ⚠️ 必须 override：字母序是 action < body < description < name < query < type，
    ///    默认实现会把 action 交给沙箱。审查对象应该是被操作的那条记忆。
    std::string subject(const Json& args) const override {
        if (const auto n = str_arg(args, "name")) return *n;
        return str_arg(args, "action").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto action = str_arg(args, "action");
        if (!action) return ToolResult::error("缺少 action 参数（search/load/write/delete）");
        if (!ctx.memory) return ToolResult::error("memory 未启用");
        Memory& mem = *ctx.memory;

        if (*action == "search") {
            const auto q = str_arg(args, "query");
            if (!q || q->empty()) return ToolResult::error("search 需要 query");
            const auto hits = mem.search(*q, 5);
            if (hits.empty()) return ToolResult{.content = "没有匹配 " + *q + " 的记忆"};
            std::string out;
            for (const auto& h : hits) out += h.render() + "\n\n";
            return ToolResult{.content = std::move(out), .metadata = {{"hits", hits.size()}}};
        }

        // ⚠️ 先校验 action，再要 name。反过来的话一个拼错的 action 收到的是
        //    "frobnicate 需要 name" —— 模型会照着补一个 name 再试一次，
        //    而真正错的是动作名，它从这句话里看不出来。
        if (*action != "load" && *action != "write" && *action != "delete")
            return ToolResult::error("未知 action: " + *action + "（search/load/write/delete）");

        const auto name = str_arg(args, "name");
        if (!name || name->empty()) return ToolResult::error(*action + " 需要 name");

        if (*action == "load") {
            const auto item = mem.get(*name);
            if (!item) return ToolResult::error("没有名为 " + *name + " 的记忆");
            return ToolResult{.content = item->render()};
        }
        if (*action == "delete") {
            // 删除和写入一样重要：一条后来发现是错的记忆，不删掉会一直被召回。
            if (!mem.remove(*name)) return ToolResult::error("没有名为 " + *name + " 的记忆");
            return ToolResult{.content = "已删除 " + *name};
        }
        if (*action == "write") {
            const auto desc = str_arg(args, "description");
            const auto body = str_arg(args, "body");
            // ⚠️ description 必填。没有它这条记忆会进索引、占位置，而模型看到
            //    一条空描述永远不会去加载它 —— 写了等于没写，还占着上下文。
            if (!desc || desc->empty())
                return ToolResult::error("write 需要 description（写「什么时候该用它」）");
            if (!body || body->empty()) return ToolResult::error("write 需要 body");
            const auto p = mem.write(*name, *desc, *body, memory_type_of(args));
            return ToolResult{.content = "已记住 " + *name, .metadata = {{"path", p.string()}}};
        }
        return ToolResult::error("未知 action: " + *action);   // 上面已挡住，这里是兜底
    }

  private:
    static MemoryType memory_type_of(const Json& args) {
        const auto t = str_arg(args, "type").value_or(std::string{"reference"});
        if (t == "user") return MemoryType::User;
        if (t == "feedback") return MemoryType::Feedback;
        if (t == "project") return MemoryType::Project;
        return MemoryType::Reference;
    }
};

/// @brief Hands one job to a sub-agent and returns only its conclusion.
///
/// @note The saving is context, not time. Twenty turns of investigation come
///       back as one paragraph instead of twenty turns of transcript.
/// @warning Not read_only: a `coder` sub-agent edits files. The permission
///          gate applies to the spawn itself; the sub-agent's own calls go
///          through the same Sandbox again.
class TaskTool final : public Tool {
  public:
    std::string_view name() const override { return "task"; }

    std::string_view description() const override {
        return "把一件边界清楚的活交给子 agent，只拿回它的结论。"
               "它有自己的上下文，二十轮调查在你这里只花一段文字。"
               "适合：要读很多文件才能回答的问题、能独立完成并自验的改动、评审。"
               "不适合：需要来回商量的、边界不清的 —— 那种自己做。";
    }

    Json input_schema() const override {
        Json types = Json::array();
        for (const auto& t : agent_types()) types.push_back(t.name);
        return Json{
            {"type", "object"},
            {"properties",
             {{"agent_type", {{"type", "string"}, {"enum", types},
                              {"description", "哪一种子 agent"}}},
              {"prompt", {{"type", "string"},
                          {"description", "任务描述。子 agent 看不到你的上下文，"
                                          "要自包含：说清目标、边界、什么算完成"}}}}},
            {"required", Json::array({"agent_type", "prompt"})},
        };
    }

    bool read_only() const override { return false; }
    bool requires_permission() const override { return true; }

    /// 审查对象是子 agent 类型 —— 规则要能写 `deny Task(coder)`。
    /// 默认实现会取 agent_type（字母序在 prompt 之前），碰巧对，仍然写明。
    std::string subject(const Json& args) const override {
        return str_arg(args, "agent_type").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto type = str_arg(args, "agent_type");
        const auto prompt = str_arg(args, "prompt");
        if (!type || type->empty()) return ToolResult::error("缺少 agent_type 参数");
        if (!prompt || prompt->empty()) return ToolResult::error("缺少 prompt 参数");
        // ⚠️ spawn 为空 = 这里已经是子 agent 了。spawn_subagent 会清空它，
        //    深度检查是第二道保险 —— 两道都留着，因为绕过任何一道的代价
        //    是无限递归地烧钱。
        if (!ctx.spawn) return ToolResult::error("当前上下文不允许派子 agent");

        const std::string out = ctx.spawn(*type, *prompt);
        return ToolResult{.content = out.empty() ? "(子 agent 没有产出结论)" : out,
                          .metadata = {{"agent_type", *type}}};
    }
};

/// @brief Runs a graph of sub-agent jobs with dependencies, several at a time.
///
/// @note Runs on Scheduler, which knows nothing about LLMs — which is what
///       lets its topological ordering, concurrency and cycle detection be
///       tested in half a second without spending a token.
class TaskGraphTool final : public Tool {
  public:
    std::string_view name() const override { return "task_graph"; }

    std::string_view description() const override {
        return "一次派一张带依赖的子 agent 任务图：无依赖的并发跑，"
               "有依赖的等上游完成并拿到上游结论。"
               "适合能拆成几块、块之间只有少量依赖的大活。"
               "只有两三个任务、或者依赖是一条链的话，用 task 一个个派更简单。";
    }

    Json input_schema() const override {
        return Json{
            {"type", "object"},
            {"properties",
             {{"tasks",
               {{"type", "array"},
                {"description", "任务列表"},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"id", {{"type", "string"}, {"description", "图内唯一"}}},
                    {"prompt", {{"type", "string"}, {"description", "自包含的任务描述"}}},
                    {"deps", {{"type", "array"}, {"items", {{"type", "string"}}},
                              {"description", "必须先完成的任务 id"}}},
                    {"agent_type", {{"type", "string"}, {"description", "默认 general"}}},
                    {"priority", {{"type", "integer"}, {"description", "越大越先跑"}}}}},
                  {"required", Json::array({"id", "prompt"})}}}}}}},
            {"required", Json::array({"tasks"})},
        };
    }

    bool read_only() const override { return false; }
    bool requires_permission() const override { return true; }

    std::string subject(const Json& args) const override {
        // 图里可能混着好几种 agent_type，用任务数当审查对象没有意义 ——
        // 交出工具名，规则只能整个允许或整个拒绝这个工具。
        (void)args;
        return "task_graph";
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        if (!args.is_object() || !args.contains("tasks") || !args["tasks"].is_array())
            return ToolResult::error("缺少 tasks 参数（必须是数组）");
        if (!ctx.spawn) return ToolResult::error("当前上下文不允许派子 agent");

        Scheduler sched(ctx.cfg->max_parallel_tools);
        for (const auto& t : args["tasks"]) {
            if (!t.is_object()) return ToolResult::error("tasks 里每一项都必须是对象");
            const auto id = t.value("id", std::string{});
            const auto prompt = t.value("prompt", std::string{});
            if (id.empty() || prompt.empty())
                return ToolResult::error("每个任务都要有非空的 id 和 prompt");

            std::vector<std::string> deps;
            if (t.contains("deps") && t["deps"].is_array())
                for (const auto& d : t["deps"])
                    if (d.is_string()) deps.push_back(d.get<std::string>());

            try {
                sched.add(id, prompt, std::move(deps), t.value("priority", 0),
                          t.value("agent_type", std::string{"general"}));
            } catch (const std::invalid_argument& e) {
                return ToolResult::error(e.what());   // 重复 id
            }
        }

        // ⚠️ 必须先 validate。成环的图 run() 会安安静静地什么都不做就返回 ——
        //    调用方看到"成功"，而所有任务都停在 pending。错误信息里带着环的
        //    路径，模型才知道该断哪条边。
        if (const auto err = sched.validate())
            return ToolResult::error("任务图有问题: " + *err);

        // ⚠️ runner 在**工作线程**里跑，而 ctx.spawn 会被并发调用。
        //    这里只捕获 ctx 的 spawn 拷贝，不碰任何共享可变状态。
        auto spawn = ctx.spawn;
        sched.run([&spawn](const Task& t, const std::map<std::string, std::string>& upstream) {
            std::string prompt = t.prompt;
            if (!upstream.empty()) {
                prompt += "\n\n上游任务的结论：\n";
                for (const auto& [id, result] : upstream)
                    prompt += "\n[" + id + "]\n" + result + "\n";
            }
            return spawn(t.agent_type, prompt);
        });

        std::size_t done = 0, failed = 0, blocked = 0;
        for (const auto& [_, t] : sched.tasks()) {
            done += t.status == TaskStatus::Done;
            failed += t.status == TaskStatus::Failed;
            blocked += t.status == TaskStatus::Blocked;
        }
        return ToolResult{
            .content = sched.render(),
            // 有任务失败就是失败，但输出照给 —— 成功的那几个的结论正是
            // 模型下一步要读的东西。
            .is_error = failed > 0 || blocked > 0,
            .metadata = {{"done", done}, {"failed", failed}, {"blocked", blocked}},
        };
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
        return str_arg(args, "command").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto command = str_arg(args, "command");
        if (!command) return ToolResult::error("缺少 command 参数（必须是字符串）");
        const auto& cmd = *command;
        if (cmd.empty()) return ToolResult::error("command 不能为空");

        const int cfg_timeout = ctx.cfg->tool_timeout_sec;
        const int want = int_arg(args, "timeout_sec", cfg_timeout);
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
ToolPtr make_write_tool() { return std::make_shared<WriteTool>(); }
ToolPtr make_edit_tool() { return std::make_shared<EditTool>(); }
ToolPtr make_glob_tool() { return std::make_shared<GlobTool>(); }
ToolPtr make_grep_tool() { return std::make_shared<GrepTool>(); }
ToolPtr make_bash_tool() { return std::make_shared<BashTool>(); }
ToolPtr make_todo_tool() { todo("Stage 4: todo —— 覆盖式提交，每轮由 reminder 回灌"); }
ToolPtr make_skill_tool() { return std::make_shared<SkillTool>(); }
ToolPtr make_memory_tool() { return std::make_shared<MemoryTool>(); }
ToolPtr make_task_tool() { return std::make_shared<TaskTool>(); }
ToolPtr make_task_graph_tool() { return std::make_shared<TaskGraphTool>(); }
ToolPtr make_bash_output_tool() { todo("Stage 6: bash_output"); }
ToolPtr make_kill_task_tool() { todo("Stage 6: kill_task"); }

std::vector<ToolPtr> builtin_tools(const Config& cfg) {
    // 顺序无所谓 —— ToolRegistry::schemas() 会按名字排序（缓存前缀要稳定）。
    std::vector<ToolPtr> v;
    v.push_back(make_read_tool());
    v.push_back(make_write_tool());
    v.push_back(make_edit_tool());
    v.push_back(make_glob_tool());
    v.push_back(make_grep_tool());
    v.push_back(make_bash_tool());

    if (cfg.enable_memory) v.push_back(make_memory_tool());
    if (cfg.enable_skills) v.push_back(make_skill_tool());

    if (cfg.enable_subagents) {
        v.push_back(make_task_tool());
        v.push_back(make_task_graph_tool());
    }

    // TODO(Stage 4/6): 下面这些工厂现在还是 todo()，一调就抛。开关默认为 true，
    //   所以要等对应 Stage 写完再打开，否则默认配置下 agent 直接起不来。
    // v.push_back(make_bash_output_tool());   // Stage 6：BackgroundManager 还没写
    // v.push_back(make_kill_task_tool());     // 同上
    return v;
}

}  // namespace mini
