// 【Stage 3】沙箱 / 权限。

#include "mini_agent/sandbox.hpp"

#include <cctype>
#include <fnmatch.h>
#include <regex>
#include <string>

#include "mini_agent/tool.hpp"

namespace mini {

// 永远拒绝，不问、不看规则、不看模式。这一层挡的是「一旦执行就无法挽回」的操作。
const std::vector<DangerPattern> kDangerous = {
    {R"(\brm\s+(-[a-zA-Z]*\s+)*-?[rf]{1,2}\s+/\s*$)", "rm -rf /"},
    {R"(\brm\s+-[a-zA-Z]*r[a-zA-Z]*f|\brm\s+-[a-zA-Z]*f[a-zA-Z]*r)", "递归强制删除"},
    {R"(\bsudo\b)", "sudo 提权"},
    {R"(\bmkfs(\.\w+)?\b)", "格式化文件系统"},
    {R"(\bdd\s+.*\bof=/dev/)", "dd 写裸设备"},
    {R"(:\(\)\s*\{.*\|.*&.*\}\s*;)", "fork bomb"},
    {R"(\bchmod\s+(-[a-zA-Z]+\s+)*777\b)", "chmod 777"},
    {R"((curl|wget)\b[^|]*\|\s*(ba)?sh)", "下载后直接执行"},
    {R"(>\s*/dev/(sd|nvme|hd)\w*)", "写裸设备"},
    {R"(\bgit\s+push\b.*(--force|-f)\b)", "强制推送"},
};

std::optional<Rule> Rule::parse(std::string_view text) {
    // 三种形式：Bash / Write(src/**) / Bash(git status:*)
    auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    if (text.empty()) return std::nullopt;

    const auto lp = text.find('(');
    if (lp == std::string_view::npos) return Rule{std::string(text), std::nullopt};

    // 括号不配对 → 解析失败。不抛异常：用户配置文件写错要给可读报错。
    if (text.back() != ')') return std::nullopt;
    std::string tool(text.substr(0, lp));
    if (tool.empty()) return std::nullopt;

    std::string pat(text.substr(lp + 1, text.size() - lp - 2));
    // 约定：结尾 :* 是前缀匹配，git status:* → glob "git status*"
    if (pat.ends_with(":*")) pat = pat.substr(0, pat.size() - 2) + "*";
    return Rule{std::move(tool), std::move(pat)};
}

bool Rule::matches(std::string_view tool_name, std::string_view subject) const {
    // 配置里写 "Bash"，Tool::name() 返回 "bash" —— 工具名大小写不敏感
    if (tool.size() != tool_name.size()) return false;
    for (std::size_t i = 0; i < tool.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tool[i])) !=
            std::tolower(static_cast<unsigned char>(tool_name[i])))
            return false;

    if (!pattern) return true;   // 没有 pattern = 整个工具都匹配
    // 不加 FNM_PATHNAME —— Write(src/**) 要能匹配 src/a/b.py，* 必须能跨 /
    return ::fnmatch(pattern->c_str(), std::string(subject).c_str(), 0) == 0;
}

Sandbox::Sandbox(const Config& cfg, AskFn asker)
    : cfg_(cfg), mode_(cfg.permission_mode), asker_(std::move(asker)) {
    // TODO(Stage 3): 把 cfg.allow_rules / cfg.deny_rules 解析成 Rule 列表。
    // 现在规则表为空 —— 只有 mode_ 这条全局基线在起作用，authorize() 还是 todo。
}

Decision Sandbox::authorize(const Tool&, const Json&) {
    todo("Stage 3: Sandbox::authorize —— executor 唯一的入口");
}

std::pair<fs::path, Decision> Sandbox::resolve_path(std::string_view) const {
    // ⚠️ weakly_canonical（文件可能不存在），按路径分量比前缀，别按字符串比
    todo("Stage 3: Sandbox::resolve_path");
}

std::vector<std::string> Sandbox::split_command(std::string_view cmd) {
    // ⚠️ 不拆的话，`ls && rm -rf /` 整条去匹配 allow 规则 Bash(ls*) —— 前缀命中，
    //    整条放行。这一条不做，前面所有权限设计都是装饰。
    std::vector<std::string> segs;
    std::string cur;
    char quote = 0;   // 引号内的 ; | & 是数据，不是分隔符

    auto flush = [&segs, &cur] {
        const auto b = cur.find_first_not_of(" \t");
        if (b != std::string::npos) cur = cur.substr(b, cur.find_last_not_of(" \t") - b + 1);
        else cur.clear();
        if (!cur.empty()) segs.push_back(cur);
        cur.clear();
    };

    for (std::size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') { quote = c; cur += c; continue; }

        // ⚠️ 两字符的先判：&& 若按单个 & 处理会拆出一个空段
        if (i + 1 < cmd.size() && c == '&' && cmd[i + 1] == '&') { flush(); ++i; continue; }
        if (i + 1 < cmd.size() && c == '|' && cmd[i + 1] == '|') { flush(); ++i; continue; }
        if (c == ';' || c == '|' || c == '&') { flush(); continue; }
        cur += c;
    }
    flush();
    return segs;
}

Decision Sandbox::check_command(std::string_view) const {
    todo("Stage 3: Sandbox::check_command");
}

Decision Sandbox::check(std::string_view, bool, std::string_view) const {
    // ⚠️ read_only 不等于免授权（有测试专门锁这个语义）
    todo("Stage 3: Sandbox::check");
}

Decision Sandbox::confirm(std::string_view, std::string_view, Decision) {
    todo("Stage 3: Sandbox::confirm —— 没有 asker 时怎么办？");
}

void Sandbox::remember_allow(std::string_view, std::string_view) {
    todo("Stage 3: Sandbox::remember_allow");
}

}  // namespace mini
