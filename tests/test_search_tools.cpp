// glob 和 grep。
//
// 两个工具都在遍历文件树，所以除了"能不能找到"，更要紧的是"会不会被淹没"：
// 不跳过 build/ 和 .git/，模型要的那三个文件会排在几百行之后；不跳过二进制，
// 一个 .o 就能吐出几千行乱码把整轮上下文撑爆。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <unistd.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    ToolContext ctx;
    ToolPtr glob = make_glob_tool();
    ToolPtr grep = make_grep_tool();

    Fixture() {
        root = fs::temp_directory_path() /
               ("search_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        root = fs::canonical(root);
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    /// 建一个文件（父目录自动建）。body 里的内容原样写入。
    void seed(const std::string& rel, const std::string& body = "x\n") const {
        const auto p = cfg.workdir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }
    /// 把某个文件的 mtime 往前推 n 秒，用来做排序测试。
    void age(const std::string& rel, int seconds) const {
        const auto p = cfg.workdir / rel;
        fs::last_write_time(p, fs::last_write_time(p) - std::chrono::seconds(seconds));
    }

    ToolResult do_glob(Json a) { return glob->run(a, ctx); }
    ToolResult do_grep(Json a) { return grep->run(a, ctx); }
};

bool has(const ToolResult& r, std::string_view s) {
    return r.content.find(s) != std::string::npos;
}
/// content 里 a 出现在 b 前面？
bool before(const ToolResult& r, std::string_view a, std::string_view b) {
    const auto ia = r.content.find(a), ib = r.content.find(b);
    return ia != std::string::npos && ib != std::string::npos && ia < ib;
}

}  // namespace

// ── 两个工具都要声明对 ──────────────────────────────────────────────────────
TEST(both_declare_themselves_correctly) {
    Fixture f;
    for (const auto& t : {f.glob, f.grep}) {
        CHECK_MSG(t->read_only(), "只读 → executor 可以并发跑");
        CHECK_MSG(!t->requires_permission(), "和 read 一致：边界由 sandbox 的路径检查管");
        CHECK(t->input_schema().at("required").at(0) == "pattern");
    }
    CHECK(f.glob->name() == "glob");
    CHECK(f.grep->name() == "grep");
}

TEST(subject_is_the_search_root_not_the_pattern) {
    Fixture f;
    // 权限规则约束的是"能看哪个目录"，不是"能搜什么词"。
    CHECK(f.glob->subject(Json{{"pattern", "**/*.cpp"}, {"path", "src"}}) == "src");
    CHECK(f.grep->subject(Json{{"pattern", "TODO"}, {"path", "src"}}) == "src");
    CHECK_MSG(f.glob->subject(Json{{"pattern", "*.cpp"}}) == ".",
              "没给 path 就是工作目录本身，不能是空串 —— 空串在沙箱里是「路径为空」");
}

// ── glob ────────────────────────────────────────────────────────────────────
TEST(glob_finds_files_by_name) {
    Fixture f;
    f.seed("src/a.cpp");
    f.seed("src/b.hpp");
    f.seed("README.md");

    const auto r = f.do_glob({{"pattern", "*.cpp"}});
    CHECK(has(r, "src/a.cpp"));
    CHECK(!has(r, "b.hpp"));
    CHECK(!has(r, "README.md"));
}

TEST(glob_star_crosses_directories) {
    Fixture f;
    f.seed("a/b/c/deep.cpp");
    // fnmatch 不加 FNM_PATHNAME，所以 * 跨 / —— 模型写 *.cpp 时想要的是
    // 「所有 cpp」，不是「仅顶层的」。和 Sandbox::Rule::matches 的取舍一致。
    CHECK(has(f.do_glob({{"pattern", "*.cpp"}}), "a/b/c/deep.cpp"));
    CHECK(has(f.do_glob({{"pattern", "**/*.cpp"}}), "a/b/c/deep.cpp"));
    CHECK(has(f.do_glob({{"pattern", "a/**/deep.cpp"}}), "a/b/c/deep.cpp"));
}

TEST(globstar_matches_zero_directories) {
    Fixture f;
    f.seed("src/flat.cpp");           // src/ 正下方，中间没有目录
    f.seed("src/tools/nested.cpp");   // 中间有一层

    // ⚠️ 这是 agent 自己跑起来之后发现的 bug。fnmatch 眼里 `**` 只是两个 `*`
    //    连写，于是 src/**/*.cpp 被拆成 "src/" + * + "/" + "*.cpp" —— 那个
    //    斜杠是字面量，必须存在。结果只捞回嵌套的那个，漏掉 src/ 正下方的 20 个。
    const auto r = f.do_glob({{"pattern", "src/**/*.cpp"}});
    CHECK_MSG(has(r, "src/flat.cpp"),
              "`**/` 必须能匹配零个目录 —— bash globstar、ripgrep 都是这个语义，"
              "模型也是照这个预期写的");
    CHECK(has(r, "src/tools/nested.cpp"));

    // 上一条用嵌套文件是测不出来的：中间有目录时，修复前后都匹配。
    CHECK_MSG(has(f.do_glob({{"pattern", "**/flat.cpp"}}), "src/flat.cpp"),
              "开头的 **/ 同理，要能跨过 src/ 这一层");
}

TEST(glob_sorts_newest_first) {
    Fixture f;
    // ⚠️ 名字要让两种排序**打架**。用 old.cpp / new.cpp 是没用的：
    //    n < o，字母序也把 new 排前面，两种实现答案一致 —— 断言证明不了任何事。
    //    （变异测试抓到过这一版：把排序换成按名字，测试照样全绿。）
    f.seed("a_newest.cpp");
    f.seed("z_oldest.cpp");
    f.age("a_newest.cpp", 3600);        // 字母序在前，但是最旧的

    const auto r = f.do_glob({{"pattern", "*.cpp"}});
    CHECK_MSG(before(r, "z_oldest.cpp", "a_newest.cpp"),
              "按修改时间倒序 —— 模型问「有哪些 cpp」时要的几乎总是最近动过的那几个");
    CHECK_MSG(!before(r, "a_newest.cpp", "z_oldest.cpp"),
              "字母序会给出相反的答案，这一条就是用来区分它俩的");
}

TEST(glob_skips_build_and_vcs_directories) {
    Fixture f;
    f.seed("src/real.cpp");
    f.seed("build/generated.cpp");
    f.seed(".git/hooks/sample.cpp");
    f.seed("node_modules/pkg/index.cpp");

    const auto r = f.do_glob({{"pattern", "*.cpp"}});
    CHECK(has(r, "src/real.cpp"));
    CHECK_MSG(!has(r, "build/"), "build/ 下面几千个中间文件会把真正的结果淹掉");
    CHECK_MSG(!has(r, ".git/"), ".git/ 下面成千上万个 object 同理");
    CHECK(!has(r, "node_modules/"));
}

TEST(glob_says_so_when_nothing_matches) {
    Fixture f;
    f.seed("a.cpp");
    const auto r = f.do_glob({{"pattern", "*.rs"}});
    CHECK_MSG(!r.is_error, "没找到不是错误 —— 那是一个有效答案");
    CHECK_MSG(!r.content.empty(), "要明说没找到，不能返回空串让模型自己猜");
}

TEST(glob_can_start_from_a_subdirectory) {
    Fixture f;
    f.seed("src/in.cpp");
    f.seed("docs/out.cpp");
    const auto r = f.do_glob({{"pattern", "*.cpp"}, {"path", "src"}});
    CHECK(has(r, "src/in.cpp"));
    CHECK(!has(r, "docs/out.cpp"));
}

// ── grep ────────────────────────────────────────────────────────────────────
TEST(grep_finds_content_with_line_numbers) {
    Fixture f;
    f.seed("a.py", "import os\nx = compute()\ny = 2\n");

    const auto r = f.do_grep({{"pattern", "compute"}});
    CHECK(has(r, "a.py:2:"));
    CHECK_MSG(has(r, "x = compute()"), "要带上那一行的内容，否则模型还得再 read 一次");
    CHECK(r.metadata.at("matches") == 1);
}

TEST(grep_takes_a_regex_not_a_literal) {
    Fixture f;
    f.seed("a.cpp", "int foo();\nint foobar();\nint bar();\n");
    CHECK(f.do_grep({{"pattern", "^int foo\\(\\)"}}).metadata.at("matches") == 1);
    CHECK(f.do_grep({{"pattern", "foo"}}).metadata.at("matches") == 2);
}

TEST(grep_reports_a_bad_regex_instead_of_throwing) {
    Fixture f;
    f.seed("a.cpp");
    // 正则是模型给的。抛出去它只能看到一句 what()，改不了。
    const auto r = f.do_grep({{"pattern", "[unclosed"}});
    CHECK(r.is_error);
    CHECK_MSG(has(r, "[unclosed"), "报错里要带上是哪个正则");
}

TEST(grep_can_filter_files_by_glob) {
    Fixture f;
    f.seed("a.cpp", "TODO here\n");
    f.seed("b.md", "TODO here\n");

    const auto r = f.do_grep({{"pattern", "TODO"}, {"glob", "*.cpp"}});
    CHECK(has(r, "a.cpp"));
    CHECK(!has(r, "b.md"));
}

TEST(grep_skips_binary_files) {
    Fixture f;
    f.seed("real.txt", "needle\n");
    f.seed("blob.o", std::string("needle\0\0\0garbage", 16));

    const auto r = f.do_grep({{"pattern", "needle"}});
    CHECK(has(r, "real.txt"));
    CHECK_MSG(!has(r, "blob.o"),
              "一个 .o 就能吐出几千行乱码，把整轮上下文撑爆，而且模型也用不上");
}

TEST(grep_skips_build_and_vcs_directories) {
    Fixture f;
    f.seed("src/real.cpp", "needle\n");
    f.seed("build/gen.cpp", "needle\n");
    f.seed(".git/COMMIT_EDITMSG", "needle\n");

    const auto r = f.do_grep({{"pattern", "needle"}});
    CHECK(has(r, "src/real.cpp"));
    CHECK(!has(r, "build/"));
    CHECK(!has(r, ".git/"));
}

TEST(grep_clips_very_long_lines) {
    Fixture f;
    f.seed("min.js", "needle" + std::string(100000, 'x') + "\n");
    const auto r = f.do_grep({{"pattern", "needle"}});
    CHECK(has(r, "min.js"));
    CHECK_MSG(r.content.size() < 2000, "一行 minified js 就是一兆");
}

TEST(grep_honours_ignore_case) {
    Fixture f;
    f.seed("a.txt", "Needle\n");
    CHECK(f.do_grep({{"pattern", "needle"}}).metadata.value("matches", 0) == 0);
    CHECK(f.do_grep({{"pattern", "needle"}, {"ignore_case", true}})
              .metadata.at("matches") == 1);
}

TEST(grep_says_so_when_nothing_matches) {
    Fixture f;
    f.seed("a.cpp", "hello\n");
    const auto r = f.do_grep({{"pattern", "zzz"}});
    CHECK(!r.is_error);
    CHECK(!r.content.empty());
}

// ── 沙箱接线 ────────────────────────────────────────────────────────────────
TEST(search_outside_the_workdir_is_refused) {
    Fixture f;
    std::ofstream(f.root / "secret.txt") << "needle\n";   // workdir 的兄弟目录
    CHECK(f.do_glob({{"pattern", "*.txt"}, {"path", ".."}}).is_error);
    CHECK(f.do_grep({{"pattern", "needle"}, {"path", ".."}}).is_error);
    CHECK(f.do_grep({{"pattern", "needle"}, {"path", "/etc"}}).is_error);
}

TEST(wrong_argument_types_give_a_clean_error) {
    Fixture f;
    f.seed("a.cpp");
    CHECK(f.do_glob({{"pattern", 42}}).is_error);
    CHECK(f.do_glob(Json::object()).is_error);
    CHECK(f.do_grep({{"pattern", 42}}).is_error);
    CHECK(f.do_grep(Json::array()).is_error);
    // path 类型不对时退回默认值，不抛
    CHECK(!f.do_glob({{"pattern", "*.cpp"}, {"path", 42}}).is_error);
}

int main() { return mt::run_all(); }
