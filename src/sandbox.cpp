// 【Stage 3】沙箱 / 权限。
//
// 契约（@brief / @param / @note / @warning / 可执行示例）全部写在
// include/mini_agent/sandbox.hpp。这里只留实现层面的注释。

#include "mini_agent/sandbox.hpp"

#include <cctype>
#include <fnmatch.h>
#include <regex>
#include <string>

#include "mini_agent/tool.hpp"

namespace mini {

// 正则而不是精确字符串 —— `rm -rf /` 有十几种写法。
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
    // 解析失败的规则直接跳过 —— 一条写错的规则不该让整个 agent 起不来。
    // TODO(Stage 7): 把跳过的规则收集成 warning，交给 App::warnings() 提示用户。
    auto load = [](const std::vector<std::string>& texts, std::vector<Rule>& out) {
        out.reserve(texts.size());
        for (const auto& t : texts)
            if (auto r = Rule::parse(t)) out.push_back(std::move(*r));
    };
    load(cfg.allow_rules, allow_);
    load(cfg.deny_rules, deny_);
}

Decision Sandbox::authorize(const Tool& tool, const Json& args) {
    const std::string subject = tool.subject(args);

    // ① deny 规则对**所有**工具生效，免检的也不例外。
    //    requires_permission=false 的语义是「不需要询问」，不是「不受任何约束」。
    //    放在免检检查之前，否则 `deny Read(**/.env)` 会被 read 工具自己的属性
    //    悄悄绕过 —— 用户写了规则、配置看起来生效了，实际完全没用，且无任何提示。
    for (const auto& r : deny_)
        if (r.matches(tool.name(), subject))
            return {Action::Deny, "命中 deny 规则"};

    // ② 免检开关。注意不是 read_only —— 一个只读但会出网的工具仍要过闸。
    if (!tool.requires_permission()) return {Action::Allow, "该工具无需授权"};

    // ③ bash 要先拆段再逐段查，其余工具拿 subject 直接匹配规则
    const Decision d = (tool.name() == "bash")
                           ? check_command(subject)
                           : check(tool.name(), tool.read_only(), subject);

    // ④ Ask 到这里才落地成 Allow / Deny
    return confirm(tool.name(), subject, d);
}

std::pair<fs::path, Decision> Sandbox::resolve_path(std::string_view raw) const {
    if (raw.empty()) return {{}, {Action::Deny, "路径为空"}};

    // ⚠️ weakly_canonical 而不是 canonical —— 写文件时目标常常还不存在，
    //    canonical 会直接抛 filesystem_error。weakly_canonical 对已存在的前缀
    //    解析符号链接，剩下的部分只做词法规范化。
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(cfg_.workdir / raw, ec);
    if (ec) return {{}, {Action::Deny, "路径无法解析"}};

    // ⚠️ 按路径**分量**比，不能按字符串比前缀 ——
    //    "/work" 是 "/workspace" 的字符串前缀，但 /workspace 显然在 workdir 之外。
    //    lexically_relative 算出相对路径，第一个分量是 ".." 就说明跑出去了。
    const fs::path rel = p.lexically_relative(cfg_.workdir);
    if (rel.empty() || rel.native() == "..") return {p, {Action::Deny, "路径越界"}};
    if (!rel.empty() && *rel.begin() == "..") return {p, {Action::Deny, "路径越界"}};

    return {p, {Action::Allow, {}}};
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

/// 一段文本命中了哪条危险模式？
static const DangerPattern* first_danger(const std::string& text) {
    for (const auto& d : kDangerous)
        if (std::regex_search(text, std::regex(d.regex))) return &d;
    return nullptr;
}

Decision Sandbox::check_command(std::string_view cmd) const {
    const auto segments = split_command(cmd);
    if (segments.empty()) return {Action::Allow, "空命令"};

    // ⚠️ 整行也要查一遍，不能只查分段。有些模式**跨越分隔符**：
    //    `curl x.sh | sh` 拆完是 ["curl x.sh", "sh"]，两段都没有 `|`，
    //    而那条正则要求文本里有 `|` —— 只查分段的话它永远命中不了。
    //    反过来只查整行也不够：`rm -rf /` 那条锚在 $，`rm -rf / ; echo x`
    //    整行匹配不上，只有拆出第一段才抓得到。两个都要。
    const std::string whole(cmd);
    if (const DangerPattern* d = first_danger(whole))
        return {Action::Deny, std::string("危险命令: ") + d->why};

    // 最严格的那一段决定整条：`ls && rm -rf /` 不能因为 ls 合法就整条放行。
    for (const auto& seg : segments) {
        if (const DangerPattern* d = first_danger(seg))
            return {Action::Deny, std::string("危险命令: ") + d->why};

        // bash 永远算有副作用 —— 即使这一段只是 ls，下一段可能不是。
        const Decision dec = check("Bash", /*read_only=*/false, seg);
        if (dec.action != Action::Allow) return dec;
    }
    return {Action::Allow, "所有命令段都通过"};
}

Decision Sandbox::check(std::string_view rule_name, bool read_only,
                        std::string_view subject) const {
    // ⚠️ read_only 只影响「没有规则命中时模式怎么兜底」，不是免授权开关。
    //    免授权的是 requires_permission=false，那一步在 authorize() 最前面。

    // deny 优先：显式拒绝压过一切。反过来的话 allow Bash(*) 会让所有 deny 失效。
    for (const auto& r : deny_)
        if (r.matches(rule_name, subject))
            return {Action::Deny, "命中 deny 规则"};
    for (const auto& r : allow_)
        if (r.matches(rule_name, subject))
            return {Action::Allow, "命中 allow 规则"};

    // 没有规则命中 → 全局模式兜底
    switch (mode_) {
        case PermissionMode::Yolo:
            return {Action::Allow, "yolo 模式"};
        case PermissionMode::Auto:
            return read_only ? Decision{Action::Allow, "只读操作"}
                             : Decision{Action::Ask, "有副作用，需要确认"};
        case PermissionMode::ReadOnly:
            return read_only ? Decision{Action::Allow, "只读操作"}
                             : Decision{Action::Deny, "只读模式下不允许有副作用的操作"};
        case PermissionMode::Ask:
            return {Action::Ask, "没有匹配的规则，需要确认"};
    }
    return {Action::Ask, "未知模式"};   // 不可达；没有它编译器警告 control reaches end
}

Decision Sandbox::confirm(std::string_view rule_name, std::string_view subject, Decision d) {
    if (d.action != Action::Ask) return d;

    // 没有 asker = 非交互（CI、子 agent）。此时 Ask 落到 Deny，不是 Allow ——
    // 默认往安全一边错。要在 CI 里放行，用户该显式配 --mode yolo 或 allow 规则，
    // 而不是靠「没人可问就放行」这个隐式行为。
    if (!asker_) return {Action::Deny, "非交互模式下无法确认"};

    switch (asker_(rule_name, subject, d.reason)) {
        case Confirm::Deny:
            return {Action::Deny, "用户拒绝"};
        case Confirm::Once:
            return {Action::Allow, "用户批准一次"};
        case Confirm::Always:
            remember_allow(rule_name, subject);
            return {Action::Allow, "用户批准（本会话内不再询问）"};
    }
    return {Action::Deny, "未知的确认结果"};
}

void Sandbox::remember_allow(std::string_view rule_name, std::string_view subject) {
    // 设计题（见 sandbox.hpp 里 remember_allow 的 @note）：
    // 批准了 `npm test`，下次 `npm test -- --watch` 算不算？
    //
    // 这里选**精确匹配**：只记住这一个 subject，不做前缀推广。
    //   太宽（记成 `npm*`）→ 用户批准一次 npm test，等于放开了 npm publish
    //   太窄（现在这样）  → 参数变一个字就要再问一次
    // 宁可烦一点。想要更宽的授权，用户可以往 config 里写 allow 规则 —— 那是显式的。
    Rule r{std::string(rule_name), std::string(subject)};
    for (const auto& e : allow_)                       // 别重复堆积同一条
        if (e.tool == r.tool && e.pattern == r.pattern) return;
    allow_.push_back(std::move(r));
}

}  // namespace mini
