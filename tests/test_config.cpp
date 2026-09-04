//
// 配置加载 —— 锁住优先级链和一对互逆函数。
//
//     cmake --build build && ./build/test_config
//
// 这一层的错误全都不响：往返对不上只是「存出去的模式读不回来」，
// 优先级写反只是「环境变量不生效」，配置文件解析抛异常才会被发现——
// 而那时 agent 已经启动失败了。
//
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "microtest.hpp"
#include "mini_agent/config.hpp"

using namespace mini;
namespace fs = std::filesystem;

// --- 夹具 -------------------------------------------------------------------
namespace {

/// 每次一个全新的空 workdir。带 pid 是为了 ctest 并行时互不干扰。
fs::path make_workdir() {
    static int n = 0;
    auto dir = fs::temp_directory_path() /
               ("mini-agent-config-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_config(const fs::path& workdir, std::string_view json) {
    fs::create_directories(workdir / ".mini-agent");
    std::ofstream(workdir / ".mini-agent" / "config.json") << json;
}

/// setenv/unsetenv 是进程全局的，用 RAII 保证测试之间不串味。
struct ScopedEnv {
    std::string key;
    ScopedEnv(const char* k, const char* v) : key(k) { ::setenv(k, v, 1); }
    ~ScopedEnv() { ::unsetenv(key.c_str()); }
};

}  // namespace

// ===========================================================================
// to_string / permission_mode_from_string —— 一对互逆函数
// ===========================================================================

TEST(permission_mode_survives_a_round_trip) {
    // ⚠️ 这两个函数必须逐字对齐。写成 "read_only" 而读的是 "read-only" 时，
    //    编译通过、运行不崩，只是存进 config.json 的权限模式下次读不回来，
    //    静默退回默认的 Ask —— 一个把沙箱悄悄放松的 bug。
    for (auto m : {PermissionMode::ReadOnly, PermissionMode::Ask, PermissionMode::Auto,
                   PermissionMode::Yolo}) {
        const auto text = to_string(m);
        const auto back = permission_mode_from_string(text);
        CHECK_MSG(back.has_value(), "to_string 产出的字符串必须能被 from_string 解析");
        CHECK_MSG(*back == m, "round-trip 之后必须是同一个模式");
    }
}

TEST(permission_mode_uses_the_cli_spelling) {
    // 用户在命令行敲的是 --mode read-only，配置文件里写的也该是这个词。
    CHECK(to_string(PermissionMode::ReadOnly) == "read-only");
    CHECK(to_string(PermissionMode::Yolo) == "yolo");
}

TEST(unknown_permission_mode_returns_nullopt) {
    // 用户会拼错。调用方要能给一条可读的报错，而不是接住一个异常。
    CHECK(!permission_mode_from_string("readonly").has_value());
    CHECK(!permission_mode_from_string("").has_value());
    CHECK(!permission_mode_from_string("YOLO").has_value());   // 大小写敏感
}

// ===========================================================================
// normalize —— 派生路径的前提
// ===========================================================================

TEST(normalize_makes_workdir_absolute_and_fills_state_dir) {
    Config cfg;
    cfg.workdir = ".";
    cfg.state_dir.clear();
    cfg.normalize();

    CHECK_MSG(cfg.workdir.is_absolute(), "sandbox 的越界判断依赖绝对路径");
    CHECK_MSG(!cfg.state_dir.empty(), "state_dir 为空时要填 <workdir>/.mini-agent");
    CHECK(cfg.state_dir == cfg.workdir / ".mini-agent");
}

TEST(normalize_keeps_an_explicit_state_dir) {
    auto wd = make_workdir();
    Config cfg;
    cfg.workdir = wd;
    cfg.state_dir = wd / "custom";
    cfg.normalize();
    CHECK(cfg.state_dir == wd / "custom");
}

TEST(normalize_is_idempotent) {
    // load_config 会调用两次（读文件前定位 state_dir，读文件后可能被覆盖）。
    Config cfg;
    cfg.workdir = ".";
    cfg.normalize();
    const auto once = cfg.state_dir;
    cfg.normalize();
    CHECK_MSG(cfg.state_dir == once, "第二次 normalize 不该再往路径上叠一层");
}

TEST(derived_paths_hang_off_state_dir) {
    Config cfg;
    cfg.workdir = ".";
    cfg.normalize();

    CHECK(cfg.sessions_dir() == cfg.state_dir / "sessions");
    CHECK(cfg.memory_dir() == cfg.state_dir / "memory");
    CHECK(cfg.mcp_config() == cfg.state_dir / "mcp.json");
    CHECK_MSG(cfg.skills_dirs().size() >= 2, "技能目录靠前的优先，至少项目私有 + 项目内置");
    CHECK_MSG(cfg.skills_dirs().front() == cfg.state_dir / "skills", "第一个是项目私有的");
}

// ===========================================================================
// load_config —— 默认值 → 配置文件 → 环境变量
// ===========================================================================

TEST(load_falls_back_to_defaults_without_a_config_file) {
    Config cfg = load_config(make_workdir());
    CHECK(cfg.model == kMainModel);
    CHECK(cfg.max_steps == 40);
    CHECK(cfg.permission_mode == PermissionMode::Ask);
    CHECK_MSG(cfg.workdir.is_absolute(), "load 结束时必须已经 normalize 过");
}

TEST(config_file_overrides_defaults) {
    auto wd = make_workdir();
    write_config(wd, R"JSON({"model":"from-file","max_steps":7,"permission_mode":"yolo"})JSON");

    Config cfg = load_config(wd);
    CHECK(cfg.model == "from-file");
    CHECK(cfg.max_steps == 7);
    CHECK(cfg.permission_mode == PermissionMode::Yolo);
}

TEST(config_file_leaves_unmentioned_fields_alone) {
    // j.value(key, cfg.xxx) 的写法保证「文件里没写的字段保留默认值」。
    auto wd = make_workdir();
    write_config(wd, R"JSON({"model":"only-model"})JSON");

    Config cfg = load_config(wd);
    CHECK(cfg.model == "only-model");
    CHECK_MSG(cfg.max_steps == 40, "文件没提到的字段不该被清零");
    CHECK_MSG(cfg.subagent_model == kSubModel, "同上");
}

TEST(config_file_can_carry_sandbox_rules) {
    auto wd = make_workdir();
    write_config(wd, R"JSON({"deny_rules":["Bash(rm *)","Write(/etc/**)"],"allow_rules":["Read(**)"]})JSON");

    Config cfg = load_config(wd);
    CHECK(cfg.deny_rules.size() == 2);
    CHECK(cfg.deny_rules.at(0) == "Bash(rm *)");
    CHECK(cfg.allow_rules.size() == 1);
}

TEST(environment_wins_over_the_config_file) {
    // 优先级：命令行 > 环境变量 > config.json > 默认值。
    // 写反了的话，用户设的 MINI_AGENT_MODE 会被文件里的值盖掉 —— 一个
    // 「我明明关掉了权限却还是被放行」的安全问题。
    auto wd = make_workdir();
    write_config(wd, R"JSON({"model":"from-file","permission_mode":"yolo"})JSON");

    ScopedEnv e1{"MINI_AGENT_MODEL", "from-env"};
    ScopedEnv e2{"MINI_AGENT_MODE", "read-only"};

    Config cfg = load_config(wd);
    CHECK_MSG(cfg.model == "from-env", "环境变量必须盖过配置文件");
    CHECK_MSG(cfg.permission_mode == PermissionMode::ReadOnly, "权限模式同理");
}

TEST(environment_only_touches_what_it_sets) {
    auto wd = make_workdir();
    write_config(wd, R"JSON({"model":"from-file","max_steps":7})JSON");
    ScopedEnv e{"MINI_AGENT_MODEL", "from-env"};

    Config cfg = load_config(wd);
    CHECK(cfg.model == "from-env");
    CHECK_MSG(cfg.max_steps == 7, "环境变量没设的字段要保留文件里的值，不是整份覆盖");
}

TEST(load_survives_a_malformed_config_file) {
    // 用户手写配置会写错。启动失败比用默认值继续跑糟糕得多。
    auto wd = make_workdir();
    write_config(wd, "{ this is not json");

    Config cfg = load_config(wd);      // 不抛就算过
    CHECK_MSG(cfg.model == kMainModel, "解析失败时退回默认值");
}

TEST(load_survives_a_non_object_config_file) {
    auto wd = make_workdir();
    write_config(wd, "[1, 2, 3]");     // 合法 JSON，但不是对象

    Config cfg = load_config(wd);
    CHECK(cfg.model == kMainModel);
}

// ===========================================================================
// ensure_dirs
// ===========================================================================

TEST(ensure_dirs_creates_what_the_agent_writes_to) {
    Config cfg = load_config(make_workdir());
    cfg.ensure_dirs();

    CHECK(fs::exists(cfg.state_dir));
    CHECK(fs::exists(cfg.sessions_dir()));
    CHECK(fs::exists(cfg.memory_dir()));
    CHECK_MSG(fs::exists(cfg.skills_dirs().front()), "项目私有的技能目录该建出来");
}

TEST(ensure_dirs_is_idempotent) {
    // App 每次启动都会调。目录已存在不是错误。
    Config cfg = load_config(make_workdir());
    cfg.ensure_dirs();
    cfg.ensure_dirs();
    CHECK(fs::exists(cfg.sessions_dir()));
}

TEST(ensure_dirs_does_not_create_files_as_directories) {
    // mcp_config() 指向一个文件。把它当目录建出来，之后读它会拿到 EISDIR。
    Config cfg = load_config(make_workdir());
    cfg.ensure_dirs();
    CHECK_MSG(!fs::exists(cfg.mcp_config()), "mcp.json 是文件，不该被 ensure_dirs 建成目录");
}

TEST(ensure_dirs_leaves_user_owned_skill_dirs_alone) {
    // skills_dirs() 里后两个是 <workdir>/skills 和 ~/.mini-agent/skills，
    // 属于用户。替他建目录会在别人的项目里留下空文件夹。
    auto wd = make_workdir();
    Config cfg = load_config(wd);
    cfg.ensure_dirs();
    CHECK_MSG(!fs::exists(wd / "skills"), "不该在用户的 workdir 里凭空建 skills/");
}

int main() { return mt::run_all(); }
