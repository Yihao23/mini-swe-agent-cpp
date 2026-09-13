// 【Stage 7】写错的权限规则：跳过，但不能静默跳过。
//
// 以前 Sandbox 的构造函数把解析失败的规则直接丢掉。agent 照常启动，配置文件里
// 那一行明明还在 —— 从外面看，一条悄悄消失的规则和一条正常工作的规则长得
// 一模一样。最危险的是 deny：用户以为某个操作被禁止了，实际没有。
//
// 这里盯三件事：
//   * I6：配置里每条规则，要么生效，要么留下一条引用它的警告
//   * deny 的警告要把后果说出来，不能和 allow 共用一种措辞
//   * 警告要真的走到 App::warnings() —— cli.cpp 打印的是那里

#include "microtest.hpp"

#include "mini_agent/app.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/sandbox.hpp"

#include <unistd.h>
#include <filesystem>
#include <memory>
#include <string>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

/// 一份指向独立临时目录的 Config。
struct Fixture {
    fs::path root;
    Config cfg;

    Fixture() {
        root = fs::temp_directory_path() /
               ("rulewarn_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

/// 有没有哪条警告引用了这条规则原文。
bool mentioned(const std::vector<std::string>& ws, std::string_view rule) {
    for (const auto& w : ws)
        if (has(w, "\"" + std::string(rule) + "\"")) return true;
    return false;
}

}  // namespace

// ── 没写错就没有警告 ────────────────────────────────────────────────────────

TEST(well_formed_rules_produce_no_warnings) {
    Fixture f;
    f.cfg.allow_rules = {"Bash", "Write(src/**)", "Bash(git status:*)"};
    f.cfg.deny_rules = {"Read(**/.env)"};
    const Sandbox sb(f.cfg);
    CHECK_MSG(sb.warnings().empty(), "三种合法写法都不该报警 —— 误报会让人习惯性忽略警告");
}

// ── 写错的要留下警告 ────────────────────────────────────────────────────────

TEST(every_skipped_rule_leaves_a_warning_that_quotes_it) {
    Fixture f;
    // 三种解析失败：括号没闭合、括号前没有工具名、只有空白
    f.cfg.allow_rules = {"Bash(git log", "(rm)"};
    f.cfg.deny_rules = {"   "};
    const Sandbox sb(f.cfg);

    // ⚠️ I6：每一条都要有着落。只数个数不够 —— 三条警告引用的可能是同一条规则。
    CHECK(sb.warnings().size() == 3);
    CHECK_MSG(mentioned(sb.warnings(), "Bash(git log"), "警告里要带原文，不然用户不知道改哪一行");
    CHECK(mentioned(sb.warnings(), "(rm)"));
    CHECK(mentioned(sb.warnings(), "   "));
}

TEST(a_skipped_deny_rule_says_the_operation_is_not_forbidden) {
    Fixture f;
    f.cfg.deny_rules = {"Bash(npm publish:*"};
    const Sandbox sb(f.cfg);
    CHECK(sb.warnings().size() == 1);
    // ⚠️ 跳过一条 allow 最坏是多问一句；跳过一条 deny，用户以为被禁的操作照样能跑。
    //    措辞必须把这个后果说出来，不能和 allow 用同一句话。
    CHECK_MSG(has(sb.warnings()[0], "没有被禁止"), "deny 的警告要说清后果");
    CHECK_MSG(has(sb.warnings()[0], "deny"), "要说清是哪一类规则");
}

TEST(a_skipped_allow_rule_is_worded_differently) {
    Fixture f;
    f.cfg.allow_rules = {"Bash(npm test:*"};
    const Sandbox sb(f.cfg);
    CHECK(sb.warnings().size() == 1);
    CHECK_MSG(!has(sb.warnings()[0], "没有被禁止"), "allow 被跳过不是安全问题，别吓人");
    CHECK(has(sb.warnings()[0], "allow"));
}

TEST(the_warning_shows_how_to_write_it) {
    Fixture f;
    f.cfg.allow_rules = {"Bash(git log"};
    const Sandbox sb(f.cfg);
    // 光说"写错了"不够，照着改的例子要在同一条消息里。
    CHECK(has(sb.warnings()[0], "Bash(git status:*)"));
}

// ── 跳过的只是那一条 ────────────────────────────────────────────────────────

TEST(a_good_rule_beside_a_bad_one_still_works) {
    Fixture f;
    f.cfg.deny_rules = {"Bash(rm -rf", "Bash(npm publish:*)"};
    const Sandbox sb(f.cfg);
    CHECK(sb.warnings().size() == 1);
    // yolo 模式：没有规则命中就放行。所以这里拒绝只可能是那条合法的 deny 起了作用。
    CHECK_MSG(sb.check("Bash", false, "npm publish --tag latest").action == Action::Deny,
              "坏规则只跳过它自己，同一列表里的好规则照常生效");
}

TEST(a_skipped_deny_rule_really_does_not_apply) {
    Fixture f;
    f.cfg.deny_rules = {"Bash(npm publish:*"};   // 少了右括号
    const Sandbox sb(f.cfg);
    // 这条证明警告说的是实话：yolo 模式下它确实没拦住。
    CHECK(sb.check("Bash", false, "npm publish").action == Action::Allow);
}

// ── 走到用户眼前 ────────────────────────────────────────────────────────────

TEST(the_app_forwards_rule_warnings) {
    Fixture f;
    f.cfg.deny_rules = {"Read(**/.env"};
    Config cfg = f.cfg;
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));

    // ⚠️ cli.cpp 打印的是 App::warnings()，不是 Sandbox::warnings()。
    //    沙箱记下了但 App 没转交，用户照样什么都看不到。
    CHECK_MSG(mentioned(app.warnings(), "Read(**/.env"), "警告要走到 App::warnings()");
}

TEST(a_clean_config_starts_without_warnings) {
    Fixture f;
    Config cfg = f.cfg;
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));
    CHECK_MSG(app.warnings().empty(), "默认配置下不该有任何警告");
}

int main() { return mt::run_all(); }
