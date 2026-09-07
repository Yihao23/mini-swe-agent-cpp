// 【Stage 5】Skill 插件。
//
// 和 memory 共用 frontmatter 解析（那部分在 test_memory.cpp 里测）。这里测的是
// skill 独有的两件事：一个 skill 是一个**文件夹**，以及多个搜索目录之间的优先级。

#include "microtest.hpp"

#include "mini_agent/skills.hpp"

#include <unistd.h>
#include <filesystem>
#include <format>
#include <fstream>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    fs::path project;   ///< 靠前，项目私有
    fs::path global;    ///< 靠后，用户全局

    Fixture() {
        root = fs::temp_directory_path() /
               ("skills_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        project = root / "project";
        global = root / "global";
        fs::create_directories(project);
        fs::create_directories(global);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    /// 在 dir 下造一个 skill 文件夹。
    void seed(const fs::path& dir, const std::string& folder, const std::string& text) const {
        fs::create_directories(dir / folder);
        std::ofstream(dir / folder / "SKILL.md", std::ios::binary) << text;
    }
    /// 往某个 skill 文件夹里放一个附属文件。
    void seed_file(const fs::path& dir, const std::string& folder,
                   const std::string& name, const std::string& text = "x") const {
        fs::create_directories((dir / folder / name).parent_path());
        std::ofstream(dir / folder / name, std::ios::binary) << text;
    }
    SkillRegistry registry() const { return SkillRegistry({project, global}); }
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

std::string skill_md(std::string_view name, std::string_view desc, std::string_view body = "正文") {
    return std::format("---\nname: {}\ndescription: {}\n---\n\n{}\n", name, desc, body);
}

}  // namespace

// ── 扫描 ────────────────────────────────────────────────────────────────────
TEST(a_skill_is_a_folder_containing_SKILL_md) {
    Fixture f;
    f.seed(f.project, "pdf", skill_md("pdf-processing", "处理 PDF 时用"));
    const auto r = f.registry();

    CHECK(r.size() == 1);
    CHECK_MSG(r.get("pdf-processing") != nullptr, "按 frontmatter 里的 name 查，不是文件夹名");
    CHECK(r.get("pdf-processing")->description == "处理 PDF 时用");
}

TEST(a_bare_markdown_file_is_not_a_skill) {
    Fixture f;
    std::ofstream(f.project / "loose.md") << skill_md("loose", "d");
    CHECK_MSG(f.registry().size() == 0,
              "skill 必须是文件夹 —— 散落的 .md 是 memory 的形态，不是 skill 的");
}

TEST(a_folder_without_SKILL_md_is_skipped) {
    Fixture f;
    fs::create_directories(f.project / "empty-folder");
    f.seed(f.project, "real", skill_md("real", "d"));
    CHECK(f.registry().size() == 1);
}

TEST(a_skill_without_a_description_is_discarded) {
    Fixture f;
    f.seed(f.project, "nodesc", "---\nname: nodesc\n---\n正文\n");
    f.seed(f.project, "good", skill_md("good", "有描述"));
    // description 是索引里唯一能让模型判断相关性的东西。空的等于永远不会被加载。
    CHECK(f.registry().size() == 1);
    CHECK(f.registry().get("good") != nullptr);
}

TEST(a_missing_name_falls_back_to_the_folder_name) {
    Fixture f;
    f.seed(f.project, "my-folder", "---\ndescription: 有描述没名字\n---\n正文\n");
    // ⚠️ 兜底用**文件夹名**，不是文件名 —— 每个 SKILL.md 都叫这个名字，
    //    用文件名的话所有匿名 skill 会撞成一个。
    CHECK_MSG(f.registry().get("my-folder") != nullptr, "应该用文件夹名兜底");
    CHECK(f.registry().get("SKILL") == nullptr);
}

TEST(a_missing_directory_is_not_an_error) {
    Fixture f;
    SkillRegistry r({f.root / "does-not-exist", f.project});
    f.seed(f.project, "x", skill_md("x", "d"));
    r.reload();
    CHECK_MSG(r.size() == 1, "配置里列了一个不存在的目录，不该让整个 registry 起不来");
}

// ── 目录优先级 ──────────────────────────────────────────────────────────────
TEST(an_earlier_directory_shadows_a_later_one) {
    Fixture f;
    f.seed(f.global, "review", skill_md("code-review", "全局版本", "全局正文"));
    f.seed(f.project, "review", skill_md("code-review", "项目版本", "项目正文"));

    const auto r = f.registry();   // {project, global}
    CHECK_MSG(r.size() == 1, "同名只留一个");
    CHECK_MSG(r.get("code-review")->description == "项目版本",
              "⚠️ 靠前的目录赢 —— 项目私有的 skill 要能盖住用户全局装的同名 skill。"
              "反过来的话仓库里的约定会被别人机器上的配置覆盖");
}

TEST(skills_from_different_directories_all_show_up) {
    Fixture f;
    f.seed(f.project, "a", skill_md("proj-only", "d"));
    f.seed(f.global, "b", skill_md("glob-only", "d"));
    const auto r = f.registry();
    CHECK(r.size() == 2);
    CHECK(r.get("proj-only") != nullptr);
    CHECK(r.get("glob-only") != nullptr);
}

// ── 索引 ────────────────────────────────────────────────────────────────────
TEST(the_index_is_byte_stable) {
    Fixture f;
    for (const char* n : {"zebra", "alpha", "middle"})
        f.seed(f.project, n, skill_md(n, "描述"));

    // index_text() 进 system prompt，缓存逐字节匹配前缀。map 有序所以稳定；
    // 换成 unordered_map 每次启动的字节都可能不同 —— 缓存永远命不中。
    const auto first = f.registry().index_text();
    for (int i = 0; i < 3; ++i) CHECK(f.registry().index_text() == first);
    CHECK_MSG(first.find("alpha") < first.find("zebra"), "按名字排序");
}

TEST(the_index_carries_the_description_not_the_body) {
    Fixture f;
    f.seed(f.project, "x", skill_md("x", "什么时候该用它", "正文非常长非常长"));
    const auto idx = f.registry().index_text();
    CHECK(has(idx, "什么时候该用它"));
    CHECK_MSG(!has(idx, "正文非常长非常长"),
              "正文按需加载 —— 十个 skill 全文塞进 system，每轮固定开销就是几千 token，"
              "而其中九个跟当前任务无关");
}

TEST(an_empty_registry_contributes_nothing) {
    Fixture f;
    CHECK_MSG(f.registry().index_text().empty(),
              "没有 skill 就不该往 system 里加一行标题 —— 那也是字节");
}

// ── render ──────────────────────────────────────────────────────────────────
TEST(render_lists_the_sibling_files) {
    Fixture f;
    f.seed(f.project, "pdf", skill_md("pdf", "处理 PDF", "运行 convert.py"));
    f.seed_file(f.project, "pdf", "convert.py", "print()");
    f.seed_file(f.project, "pdf", "template.tex");

    const auto r = f.registry().get("pdf")->render();
    CHECK(has(r, "运行 convert.py"));
    // ⚠️ skill 是个文件夹。正文里常写"运行 convert.py"却不写它在哪 ——
    //    列出来一行，省掉模型一轮 glob，也让它知道 template.tex 存在。
    CHECK_MSG(has(r, "pdf/convert.py"), "同目录的附属文件要列出来");
    CHECK_MSG(has(r, "pdf/template.tex"), "全部列出来，不只是正文提到的那个");
    CHECK_MSG(!has(r, "SKILL.md"), "SKILL.md 自己不用列 —— 正文就是它");
}

TEST(render_omits_the_listing_when_there_is_nothing_else) {
    Fixture f;
    f.seed(f.project, "solo", skill_md("solo", "d", "只有正文"));
    const auto r = f.registry().get("solo")->render();
    CHECK(has(r, "只有正文"));
    CHECK_MSG(!has(r, "同目录下还有"), "没有附属文件就别加一个空标题");
}

TEST(render_carries_name_and_description) {
    Fixture f;
    f.seed(f.project, "x", skill_md("the-name", "the-desc", "the-body"));
    const auto r = f.registry().get("the-name")->render();
    CHECK(has(r, "the-name"));
    CHECK(has(r, "the-desc"));
    CHECK(has(r, "the-body"));
}

TEST(sibling_listing_is_stable) {
    Fixture f;
    f.seed(f.project, "s", skill_md("s", "d"));
    for (const char* n : {"z.py", "a.py", "m.py"}) f.seed_file(f.project, "s", n);
    const auto first = f.registry().get("s")->render();
    for (int i = 0; i < 3; ++i) CHECK(f.registry().get("s")->render() == first);
    CHECK(first.find("a.py") < first.find("z.py"));
}

// ── 查找 ────────────────────────────────────────────────────────────────────
TEST(get_returns_null_rather_than_throwing) {
    Fixture f;
    f.seed(f.project, "x", skill_md("x", "d"));
    const auto r = f.registry();
    CHECK(r.get("nope") == nullptr);
    CHECK(r.get("") == nullptr);
}

TEST(reload_picks_up_a_new_skill) {
    Fixture f;
    SkillRegistry r({f.project, f.global});
    CHECK(r.size() == 0);
    f.seed(f.project, "late", skill_md("late", "d"));
    r.reload();
    CHECK_MSG(r.size() == 1, "跑着的时候加一个 skill 应该不用重启 —— "
                             "那是它们值得手写的前提");
}

int main() { return mt::run_all(); }
