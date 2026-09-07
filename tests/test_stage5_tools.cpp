// 【Stage 5】memory 和 skill 两个工具，以及它们在 App 里的接线。
//
// 工具本身很薄，真正要盯的是三件事：
//   * 关掉开关时不能崩，也不能在别人的工作区里留下空目录
//   * memory 的 subject() 必须是被操作的那条记忆，不是 action
//   * write 必须要求 description —— 没有它这条记忆写了等于没写

#include "microtest.hpp"

#include "mini_agent/app.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/skills.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <unistd.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    std::unique_ptr<Memory> mem;
    std::unique_ptr<SkillRegistry> skills;
    ToolContext ctx;
    ToolPtr memory_tool = make_memory_tool();
    ToolPtr skill_tool = make_skill_tool();

    Fixture() {
        root = fs::temp_directory_path() /
               ("s5_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        cfg.ensure_dirs();
        sandbox = std::make_unique<Sandbox>(cfg);
        mem = std::make_unique<Memory>(cfg.memory_dir());
        skills = std::make_unique<SkillRegistry>(cfg.skills_dirs());
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
        ctx.memory = mem.get();
        ctx.skills = skills.get();
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    ToolResult do_mem(Json a) { return memory_tool->run(a, ctx); }
    ToolResult do_skill(Json a) { return skill_tool->run(a, ctx); }

    /// 在第一个 skills 目录下造一个 skill。
    void seed_skill(const std::string& folder, const std::string& name,
                    const std::string& desc, const std::string& body = "正文") const {
        const auto dir = cfg.skills_dirs().front() / folder;
        fs::create_directories(dir);
        std::ofstream(dir / "SKILL.md", std::ios::binary)
            << std::format("---\nname: {}\ndescription: {}\n---\n\n{}\n", name, desc, body);
    }
};

bool has(const ToolResult& r, std::string_view s) {
    return r.content.find(s) != std::string::npos;
}

}  // namespace

// ── 声明 ────────────────────────────────────────────────────────────────────
TEST(the_two_tools_declare_themselves_correctly) {
    Fixture f;
    CHECK(f.skill_tool->name() == "skill");
    CHECK_MSG(f.skill_tool->read_only(), "skill 只读文件");
    CHECK_MSG(!f.skill_tool->requires_permission(), "和 read 一致：只开配置里指定的目录");

    CHECK(f.memory_tool->name() == "memory");
    CHECK_MSG(!f.memory_tool->read_only(), "write/delete 会改文件");
    CHECK_MSG(f.memory_tool->requires_permission(), "会改文件就要过闸");
}

TEST(memory_subject_is_the_item_not_the_action) {
    Fixture f;
    // ⚠️ 字母序是 action < body < description < name < query < type，
    //    默认实现会把 "write" 这个动作交给沙箱审查。规则要约束的是被操作的
    //    那条记忆，不是动作名。
    const Json args{{"action", "write"}, {"body", "b"},
                    {"description", "d"}, {"name", "user-prefs"}};
    CHECK_MSG(f.memory_tool->subject(args) == "user-prefs",
              "必须 override subject() —— 否则沙箱审查的是 action");
    CHECK_MSG(f.memory_tool->subject(args) != "write", "默认实现返回的就是这个错误答案");
    // search 没有 name，退回 action 而不是空串（空串在沙箱里是「路径为空」）
    CHECK(f.memory_tool->subject(Json{{"action", "search"}, {"query", "q"}}) == "search");
}

// ── memory 工具 ─────────────────────────────────────────────────────────────
TEST(memory_write_then_load) {
    Fixture f;
    CHECK(!f.do_mem({{"action", "write"}, {"name", "style"},
                     {"description", "提交信息的格式要求"},
                     {"body", "Conventional Commits"}, {"type", "feedback"}}).is_error);

    const auto loaded = f.do_mem({{"action", "load"}, {"name", "style"}});
    CHECK(!loaded.is_error);
    CHECK(has(loaded, "Conventional Commits"));
    CHECK_MSG(has(loaded, "feedback"), "render 要带上类型");
}

TEST(memory_write_demands_a_description) {
    Fixture f;
    // ⚠️ 没有 description 的记忆会进索引、占位置，而模型看到一条空描述永远
    //    不会去加载它 —— 写了等于没写，还白占上下文。
    const auto r = f.do_mem({{"action", "write"}, {"name", "x"}, {"body", "b"}});
    CHECK(r.is_error);
    CHECK_MSG(has(r, "什么时候该用它"), "报错要说清 description 该写什么");
    CHECK_MSG(f.mem->items().empty(), "拒绝了就不能真的写出去");
}

TEST(memory_write_demands_a_body) {
    Fixture f;
    CHECK(f.do_mem({{"action", "write"}, {"name", "x"}, {"description", "d"}}).is_error);
}

TEST(memory_search_returns_the_rendered_hits) {
    Fixture f;
    f.do_mem({{"action", "write"}, {"name", "commit-style"},
              {"description", "提交信息格式"}, {"body", "正文"}});
    const auto r = f.do_mem({{"action", "search"}, {"query", "提交信息"}});
    CHECK(!r.is_error);
    CHECK(has(r, "commit-style"));
    CHECK(r.metadata.at("hits") == 1);
}

TEST(memory_search_says_so_when_nothing_matches) {
    Fixture f;
    f.do_mem({{"action", "write"}, {"name", "x"}, {"description", "d"}, {"body", "b"}});
    const auto r = f.do_mem({{"action", "search"}, {"query", "完全无关"}});
    CHECK_MSG(!r.is_error, "没找到不是错误 —— 那是一个有效答案");
    CHECK(!r.content.empty());
}

TEST(memory_delete_removes_it) {
    Fixture f;
    f.do_mem({{"action", "write"}, {"name", "gone"}, {"description", "d"}, {"body", "b"}});
    CHECK(!f.do_mem({{"action", "delete"}, {"name", "gone"}}).is_error);
    CHECK(f.do_mem({{"action", "load"}, {"name", "gone"}}).is_error);
    CHECK_MSG(f.do_mem({{"action", "delete"}, {"name", "gone"}}).is_error,
              "删一条不存在的要报错，不是静默成功");
}

TEST(memory_rejects_a_bad_action_and_names_the_valid_ones) {
    Fixture f;
    const auto r = f.do_mem({{"action", "frobnicate"}});
    CHECK(r.is_error);
    CHECK_MSG(has(r, "search"), "报错要列出可用的 action，模型才能自纠");
}

TEST(memory_wrong_argument_types_give_a_clean_error) {
    Fixture f;
    CHECK(f.do_mem({{"action", 42}}).is_error);
    CHECK(f.do_mem(Json::object()).is_error);
    CHECK(f.do_mem(Json::array()).is_error);
    CHECK(f.do_mem({{"action", "load"}, {"name", 42}}).is_error);
}

// ── skill 工具 ──────────────────────────────────────────────────────────────
TEST(skill_loads_the_full_manual) {
    Fixture f;
    f.seed_skill("pdf", "pdf-processing", "处理 PDF 时用", "先跑 convert.py");
    f.skills->reload();

    const auto r = f.do_skill({{"name", "pdf-processing"}});
    CHECK(!r.is_error);
    CHECK_MSG(has(r, "先跑 convert.py"), "正文要全文返回 —— 索引里只有描述");
    CHECK(r.metadata.at("skill") == "pdf-processing");
}

TEST(skill_lists_the_alternatives_when_the_name_is_wrong) {
    Fixture f;
    f.seed_skill("a", "code-review", "评审时用");
    f.skills->reload();

    const auto r = f.do_skill({{"name", "typo-name"}});
    CHECK(r.is_error);
    // ⚠️ 模型是照着索引写的名字。列出有哪些它能自己纠正，
    //    光说"没有这个 skill"它只能再猜一次。
    CHECK_MSG(has(r, "code-review"), "报错要列出可用的 skill");
}

TEST(skill_wrong_argument_types_give_a_clean_error) {
    Fixture f;
    CHECK(f.do_skill({{"name", 42}}).is_error);
    CHECK(f.do_skill(Json::object()).is_error);
}

// ── 开关关掉时 ──────────────────────────────────────────────────────────────
TEST(the_tools_report_rather_than_crash_when_disabled) {
    Fixture f;
    f.ctx.memory = nullptr;
    f.ctx.skills = nullptr;
    // ToolContext 里这些字段是指针，就是为了表达「这一层可能不存在」。
    CHECK(f.do_mem({{"action", "search"}, {"query", "x"}}).is_error);
    CHECK(f.do_skill({{"name", "x"}}).is_error);
}

// ── App 接线 ────────────────────────────────────────────────────────────────
TEST(app_registers_the_stage5_tools_and_wires_the_context) {
    Fixture f;
    Config cfg = f.cfg;
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));

    CHECK_MSG(app.registry().get("memory") != nullptr, "memory 工具要注册进去");
    CHECK_MSG(app.registry().get("skill") != nullptr, "skill 工具要注册进去");
    CHECK_MSG(app.memory() != nullptr, "App::memory() 不该再返回 nullptr");
    CHECK_MSG(app.skills() != nullptr, "App::skills() 不该再返回 nullptr");
}

TEST(app_leaves_no_memory_directory_when_the_switch_is_off) {
    // ⚠️ 不能复用 Fixture 的 cfg —— 它构造时已经 ensure_dirs() 过，目录早就在了，
    //    那样这条断言测的是脚手架而不是被测代码。用一份全新的、没建过目录的。
    Fixture f;
    Config cfg;
    cfg.workdir = f.root / "fresh";
    fs::create_directories(cfg.workdir);
    cfg.permission_mode = PermissionMode::Yolo;
    cfg.enable_memory = false;
    cfg.enable_skills = false;
    cfg.normalize();
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));

    CHECK(app.memory() == nullptr);
    CHECK(app.skills() == nullptr);
    CHECK_MSG(app.registry().get("memory") == nullptr, "关掉了就不该注册那个工具");
    CHECK_MSG(!fs::exists(cfg.memory_dir()),
              "关掉的时候不该在别人的工作区里留下一个空目录");
}

int main() { return mt::run_all(); }
