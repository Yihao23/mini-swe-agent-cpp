// 【Stage 7】权限规则的匹配：工具名也是 glob，而记住的批准必须是字面量。
//
// 两件事，一件是新能力，一件是修漏洞：
//
//   * 工具名走 glob。`Mcp__github__*` 以前什么都匹配不到 —— 工具名是逐字符比较
//     的，而配置看起来完全正常。MCP 工具名加 `mcp__<server>__` 前缀，本来就是
//     为了让规则能按 server 写。
//   * 规则两边都是 glob，所以 remember_allow() 记下的东西必须先转义。以前
//     subject 原样存进去：用户批准 `rm build/*.o`，那个 `*` 就成了通配符。
//
// 每条用例的输入都刻意让「对的实现」和「坏的实现」给出不同答案。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/sandbox.hpp"

#include <unistd.h>
#include <filesystem>
#include <string>

using namespace mini;
namespace fs = std::filesystem;

namespace {

bool rule_matches(std::string_view rule, std::string_view tool, std::string_view subject) {
    const auto r = Rule::parse(rule);
    return r && r->matches(tool, subject);
}

/// Ask 模式的沙箱，外加一个数问了几次的假用户（每次都答「总是允许」）。
struct Asking {
    Config cfg;
    int asked = 0;
    Sandbox sb;

    static Config make_cfg() {
        Config c;
        c.workdir = fs::temp_directory_path();
        c.permission_mode = PermissionMode::Ask;
        c.normalize();
        return c;
    }
    Asking()
        : cfg(make_cfg()), sb(cfg, [this](auto, auto, auto) { ++asked; return Confirm::Always; }) {}

    /// 走一遍 check → confirm，返回这次之后累计问了几次。
    int ask(std::string_view tool, std::string_view subject) {
        (void)sb.confirm(tool, subject, sb.check(tool, false, subject));
        return asked;
    }
};

}  // namespace

// ── 工具名是 glob ───────────────────────────────────────────────────────────

TEST(a_server_prefix_rule_covers_that_servers_tools) {
    // ⚠️ 这条以前是 false。逐字符比较下 `Mcp__github__*` 的长度就对不上。
    CHECK(rule_matches("Mcp__github__*", "mcp__github__search", "q"));
    CHECK(rule_matches("Mcp__github__*", "mcp__github__create_pr", "q"));
}

TEST(a_server_prefix_rule_does_not_reach_another_server) {
    CHECK(!rule_matches("Mcp__github__*", "mcp__slack__post", "q"));
    // 前缀要带上结尾的双下划线，否则 github 会盖住 githubenterprise
    CHECK(!rule_matches("Mcp__github__*", "mcp__githubenterprise__search", "q"));
}

TEST(a_name_without_wildcards_still_matches_only_itself) {
    // ⚠️ 防改过头。换成前缀匹配或子串匹配的实现，这两条会变成 true ——
    //    deny Bash 就连带禁掉了 bash_output 和 kill_task 之外的一切 bash_*。
    CHECK(rule_matches("Bash", "bash", "ls"));
    CHECK(!rule_matches("Bash", "bash_output", "bg_1"));
    CHECK(!rule_matches("task", "task_graph", "x"));
}

TEST(a_bare_star_covers_every_tool) {
    CHECK(rule_matches("*", "read", "a.txt"));
    CHECK(rule_matches("*", "mcp__github__search", "q"));
}

TEST(tool_names_ignore_case_but_subjects_do_not) {
    CHECK(rule_matches("MCP__GitHub__*", "mcp__github__search", "q"));
    CHECK(rule_matches("Write(src/**)", "write", "src/a.py"));
    // Linux 上路径和命令都区分大小写，subject 这一侧不能跟着放宽。
    CHECK(!rule_matches("Write(SRC/**)", "write", "src/a.py"));
}

// ── 记住的批准是字面量 ──────────────────────────────────────────────────────

TEST(always_allowing_a_command_with_a_star_does_not_allow_other_paths) {
    Asking a;
    CHECK(a.ask("Bash", "rm build/*.o") == 1);
    // ⚠️ 这条是那个漏洞。以前 `rm build/*.o` 被原样存成 glob，
    //    下面这条匹配得上，于是不再询问、直接放行。
    CHECK_MSG(a.ask("Bash", "rm build/../../home/secret.o") == 2,
              "批准过的命令里的 * 不能变成通配符");
}

TEST(always_allowing_still_remembers_the_exact_subject) {
    Asking a;
    CHECK(a.ask("Bash", "rm build/*.o") == 1);
    // 反面对照：转义不能把"记住"本身弄坏。原样的同一条命令不该再问。
    CHECK_MSG(a.ask("Bash", "rm build/*.o") == 1, "同一条命令批准一次就够了");
}

TEST(other_glob_characters_in_a_subject_are_literal_too) {
    Asking a;
    CHECK(a.ask("Bash", "ls file?.txt") == 1);
    CHECK(a.ask("Bash", "ls fileX.txt") == 2);
    CHECK(a.ask("Bash", "cat [abc].log") == 3);
    CHECK(a.ask("Bash", "cat a.log") == 4);
    CHECK(a.ask("Bash", "echo a\\\\b") == 5);
    CHECK_MSG(a.ask("Bash", "echo a\\\\b") == 5, "反斜杠本身也要能被原样记住");
}

TEST(always_allowing_a_tool_whose_name_has_a_star_does_not_allow_its_neighbours) {
    Asking a;
    // MCP 工具名来自第三方 server，里面带什么都有可能。
    CHECK(a.ask("mcp__weird__a*", "x") == 1);
    CHECK_MSG(a.ask("mcp__weird__abc", "x") == 2, "工具名里的 * 同样不能变成通配符");
    CHECK(a.ask("mcp__weird__a*", "x") == 2);
}

int main() { return mt::run_all(); }
