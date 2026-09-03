// 【Stage 3】沙箱 / 权限。

#include "mini_agent/sandbox.hpp"

#include "mini_agent/tool.hpp"

namespace mini {

const std::vector<DangerPattern> kDangerous = {
    // TODO(Stage 3): rm -rf / sudo / mkfs / dd if= / fork bomb / chmod 777 /
    //                curl|sh / wget|sh / 写裸设备 / git push --force
};

std::optional<Rule> Rule::parse(std::string_view) {
    todo("Stage 3: Rule::parse —— `Bash(git status:*)` / `Write(src/**)` / `Bash`");
}

bool Rule::matches(std::string_view, std::string_view) const {
    todo("Stage 3: Rule::matches —— glob 匹配，可以用 <fnmatch.h>");
}

Sandbox::Sandbox(const Config& cfg, AskFn asker)
    : cfg_(cfg), mode_(cfg.permission_mode), asker_(std::move(asker)) {
    todo("Stage 3: Sandbox 构造 —— 解析 allow/deny 规则");
}

Decision Sandbox::authorize(const Tool&, const Json&) {
    todo("Stage 3: Sandbox::authorize —— executor 唯一的入口");
}

std::pair<fs::path, Decision> Sandbox::resolve_path(std::string_view) const {
    // ⚠️ weakly_canonical（文件可能不存在），按路径分量比前缀，别按字符串比
    todo("Stage 3: Sandbox::resolve_path");
}

std::vector<std::string> Sandbox::split_command(std::string_view) {
    todo("Stage 3: Sandbox::split_command —— 按 && || ; | 拆段，这条不做前面全白搭");
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
