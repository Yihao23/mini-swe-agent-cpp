// 【Stage 5】长期记忆，以及和 skills 共用的 frontmatter 解析。
//
// 这一层的失败方式几乎都是"静默少了一条"：一份没有 description 的文件进了索引
// 却永远不会被加载；索引顺序不稳定让 prompt 缓存每轮作废；search 把长文排在
// 前面而真正相关的那条掉出 limit。都不报错，只是 agent 忘了它本该记得的事。

#include "microtest.hpp"

#include "../src/frontmatter.hpp"
#include "mini_agent/memory.hpp"

#include <unistd.h>
#include <filesystem>
#include <fstream>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    Memory mem;

    Fixture() : root(make_root()), mem(root) {}
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    static fs::path make_root() {
        auto p = fs::temp_directory_path() /
                 ("mem_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(p);
        return p;
    }
    /// 直接写一个文件，绕过 Memory::write —— 测手写文件的解析。
    void seed(const std::string& stem, const std::string& text) const {
        fs::create_directories(root);
        std::ofstream(root / (stem + ".md"), std::ios::binary) << text;
    }
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

const char* kSample =
    "---\n"
    "name: git-commit-style\n"
    "description: 用户对提交信息格式的要求\n"
    "metadata:\n"
    "  type: feedback\n"
    "---\n"
    "\n"
    "Conventional Commits，正文解释 why。\n";

}  // namespace

// ── frontmatter ─────────────────────────────────────────────────────────────
TEST(frontmatter_splits_fields_from_body) {
    const auto fm = parse_frontmatter(kSample);
    CHECK(fm.has_value());
    CHECK(fm->fields.at("name") == "git-commit-style");
    CHECK(fm->fields.at("description") == "用户对提交信息格式的要求");
    CHECK_MSG(fm->fields.at("type") == "feedback",
              "缩进行的键要 trim 成顶层键，metadata: 那层嵌套才不用特殊处理");
    CHECK_MSG(fm->body == "Conventional Commits，正文解释 why。\n",
              "正文前的空行要去掉，否则每份 render 都以两个换行开头");
}

TEST(frontmatter_splits_on_the_first_colon_not_the_last) {
    const auto fm = parse_frontmatter("---\ndescription: 用法: 见下文\n---\nbody\n");
    // 按最后一个冒号切会把键切成 "description: 用法"
    CHECK_MSG(fm->fields.at("description") == "用法: 见下文", "值里可以有冒号");
}

TEST(frontmatter_strips_paired_quotes) {
    const auto fm = parse_frontmatter("---\ndescription: \"带引号\"\nname: 'x'\n---\nb");
    CHECK(fm->fields.at("description") == "带引号");
    CHECK(fm->fields.at("name") == "x");
}

TEST(frontmatter_requires_both_delimiters) {
    CHECK_MSG(!parse_frontmatter("没有 frontmatter 的普通文件\n").has_value(),
              "不以 --- 开头 → nullopt");
    CHECK_MSG(!parse_frontmatter("---\nname: x\n没有结束标记\n").has_value(),
              "没有结束的 --- → nullopt，不能把整个文件当 frontmatter 吞掉");
    CHECK(!parse_frontmatter("").has_value());
}

TEST(frontmatter_keeps_an_empty_body) {
    const auto fm = parse_frontmatter("---\nname: x\ndescription: d\n---\n");
    CHECK(fm.has_value());
    CHECK(fm->body.empty());
}

// ── 解析一份记忆文件 ────────────────────────────────────────────────────────
TEST(a_memory_without_a_description_is_discarded) {
    Fixture f;
    f.seed("good", kSample);
    f.seed("nodesc", "---\nname: nodesc\n---\n正文\n");

    const auto items = f.mem.items();
    CHECK_MSG(items.size() == 1,
              "⚠️ 没有 description 的要整条丢弃 —— 它会进索引占位置，"
              "而模型看到一条空描述永远不会去加载它，静默失效");
    CHECK(items.at(0).name == "git-commit-style");
}

TEST(a_missing_name_falls_back_to_the_filename) {
    Fixture f;
    f.seed("fallback-name", "---\ndescription: 有描述没名字\n---\n正文\n");
    CHECK_MSG(f.mem.items().at(0).name == "fallback-name",
              "手写的文件常忘了写 name，而文件名总是对的");
}

TEST(an_unknown_type_falls_back_to_reference) {
    Fixture f;
    f.seed("weird", "---\ndescription: d\nmetadata:\n  type: 火星类型\n---\nb\n");
    CHECK_MSG(f.mem.items().at(0).type == MemoryType::Reference,
              "未知类型退回最弱的那个，不是丢弃整条 —— 类型只影响召回权重");
}

TEST(non_markdown_files_are_ignored) {
    Fixture f;
    f.seed("real", kSample);
    fs::create_directories(f.root);
    std::ofstream(f.root / "notes.txt") << "---\ndescription: d\n---\nb";
    std::ofstream(f.root / ".DS_Store") << "junk";
    CHECK(f.mem.items().size() == 1);
}

// ── 索引 ────────────────────────────────────────────────────────────────────
TEST(the_index_order_is_stable) {
    Fixture f;
    for (const char* n : {"zebra", "alpha", "middle"})
        f.seed(n, std::string("---\nname: ") + n + "\ndescription: d\n---\nb\n");

    // ⚠️ index_text() 进 system prompt，缓存逐字节匹配前缀。目录遍历顺序不保证，
    //    不排序的话每次启动 system 的字节都可能不同 —— 缓存永远命不中，
    //    账单十倍，而功能完全正常。
    const auto first = f.mem.index_text();
    for (int i = 0; i < 3; ++i) {
        Memory again(f.root);
        CHECK_MSG(again.index_text() == first, "索引必须逐字节稳定");
    }
    const auto items = f.mem.items();
    CHECK(items.at(0).name == "alpha");
    CHECK(items.at(2).name == "zebra");
}

TEST(the_index_carries_the_description_not_the_body) {
    Fixture f;
    f.seed("x", "---\nname: x\ndescription: 什么时候该用它\n---\n正文很长很长\n");
    const auto idx = f.mem.index_text();
    CHECK(has(idx, "什么时候该用它"));
    CHECK_MSG(!has(idx, "正文很长很长"),
              "正文按需加载 —— 全塞进 system 的话每轮固定开销就是几千 token");
}

TEST(the_index_caps_and_says_so) {
    Fixture f;
    for (int i = 0; i < 10; ++i)
        f.seed("m" + std::to_string(i),
               "---\nname: m" + std::to_string(i) + "\ndescription: d\n---\nb\n");
    const auto idx = f.mem.index_text(3);
    CHECK_MSG(has(idx, "还有 7 条"), "截断了要说，否则模型以为那就是全部");
}

TEST(an_empty_store_contributes_nothing) {
    Fixture f;
    CHECK_MSG(f.mem.index_text().empty(),
              "没有记忆就不该往 system 里加一行标题 —— 那也是字节");
}

// ── 读写删 ──────────────────────────────────────────────────────────────────
TEST(write_then_read_back) {
    Fixture f;
    f.mem.write("style", "用户的提交信息偏好", "Conventional Commits\n", MemoryType::Feedback);

    const auto got = f.mem.get("style");
    CHECK(got.has_value());
    CHECK(got->description == "用户的提交信息偏好");
    CHECK(got->type == MemoryType::Feedback);
    CHECK(has(got->body, "Conventional Commits"));
}

TEST(write_is_visible_immediately) {
    Fixture f;
    CHECK(f.mem.items().empty());          // 先把缓存填上
    f.mem.write("new", "d", "b", MemoryType::User);
    CHECK_MSG(f.mem.items().size() == 1,
              "写完要重建索引 —— 否则这一轮写的记忆下一轮 system 里看不到");
    CHECK(has(f.mem.index_text(), "new"));
}

TEST(write_overwrites_by_name) {
    Fixture f;
    f.mem.write("k", "旧描述", "旧正文", MemoryType::User);
    f.mem.write("k", "新描述", "新正文", MemoryType::Project);
    CHECK(f.mem.items().size() == 1);
    CHECK(f.mem.get("k")->description == "新描述");
    CHECK(f.mem.get("k")->type == MemoryType::Project);
}

TEST(a_multiline_description_stays_on_one_line) {
    Fixture f;
    f.mem.write("k", "第一行\n第二行", "b", MemoryType::User);
    // 换行会毁掉 frontmatter 的行结构，整份文件就解析不出来了
    CHECK_MSG(f.mem.get("k").has_value(), "带换行的 description 不能让文件变得无法解析");
    CHECK(has(f.mem.get("k")->description, "第一行"));
    CHECK(has(f.mem.get("k")->description, "第二行"));
}

TEST(remove_deletes_and_reindexes) {
    Fixture f;
    f.mem.write("gone", "d", "b", MemoryType::User);
    f.mem.write("kept", "d", "b", MemoryType::User);

    CHECK(f.mem.remove("gone"));
    CHECK_MSG(f.mem.items().size() == 1, "删完要重建索引");
    CHECK(!f.mem.get("gone").has_value());
    CHECK_MSG(!f.mem.remove("gone"), "删一条不存在的返回 false，不是崩溃");
}

TEST(remove_deletes_the_file_get_would_return) {
    Fixture f;
    // ⚠️ 手写的文件可以声明一个和文件名不同的 name。remove 如果按
    //    root_/name.md 去猜，get 找得到而 remove 删不掉 —— 同一个名字
    //    两个方法给出不一致的答案，调用方看不出区别。
    f.seed("filename-differs", "---\nname: declared-name\ndescription: d\n---\n正文\n");

    CHECK_MSG(f.mem.get("declared-name").has_value(), "按 frontmatter 里的 name 查得到");
    CHECK_MSG(f.mem.remove("declared-name"), "get 找得到的，remove 就必须删得掉");
    CHECK(f.mem.items().empty());
}

// ── 检索 ────────────────────────────────────────────────────────────────────
TEST(search_weights_the_description_above_the_body) {
    Fixture f;
    // ⚠️ 反例：两条都含 "commit"，但一条在 description 里、一条只在正文里。
    //    不加权的话两条同分，排序退化成按名字 —— 真正相关的那条可能掉出 limit。
    f.mem.write("a-body-only", "跟提交无关的东西",
                "背景里提了一句 commit 而已\n", MemoryType::Reference);
    f.mem.write("z-in-description", "commit 信息的格式要求", "正文\n", MemoryType::Feedback);

    const auto hits = f.mem.search("commit", 5);
    CHECK(hits.size() == 2);
    CHECK_MSG(hits.at(0).name == "z-in-description",
              "description 命中要排在正文命中前面 —— 那句话就是为了回答"
              "「这条值不值得读」而写的");
}

TEST(search_weights_the_name_highest) {
    Fixture f;
    f.mem.write("commit-style", "无关描述", "无关正文", MemoryType::User);
    f.mem.write("other", "这里提到 commit 两次，commit", "commit", MemoryType::User);
    CHECK(f.mem.search("commit", 5).at(0).name == "commit-style");
}

TEST(a_long_body_cannot_win_by_length_alone) {
    Fixture f;
    std::string spam;
    for (int i = 0; i < 200; ++i) spam += "commit ";
    f.mem.write("a-spam", "无关", spam, MemoryType::Reference);
    f.mem.write("z-real", "commit 的格式", "正文", MemoryType::Feedback);
    CHECK_MSG(f.mem.search("commit", 5).at(0).name == "z-real",
              "正文命中要封顶，否则堆砌关键词的长文永远排第一");
}

TEST(search_honours_the_limit_and_ignores_case) {
    Fixture f;
    for (int i = 0; i < 6; ++i)
        f.mem.write("m" + std::to_string(i), "COMMIT 相关", "b", MemoryType::User);
    CHECK(f.mem.search("commit", 3).size() == 3);
    CHECK(f.mem.search("CoMmIt", 10).size() == 6);
}

TEST(search_returns_nothing_rather_than_everything) {
    Fixture f;
    f.mem.write("x", "d", "b", MemoryType::User);
    CHECK_MSG(f.mem.search("完全无关的词", 5).empty(),
              "没命中就返回空 —— 塞一堆不相关的进上下文比不返回更糟");
    CHECK_MSG(f.mem.search("", 5).empty(), "空查询不该召回全部");
}

TEST(search_is_stable_for_equal_scores) {
    Fixture f;
    for (const char* n : {"zz", "aa", "mm"})
        f.mem.write(n, "commit", "b", MemoryType::User);
    const auto a = f.mem.search("commit", 5);
    for (int i = 0; i < 3; ++i) {
        const auto b = f.mem.search("commit", 5);
        for (std::size_t k = 0; k < a.size(); ++k)
            CHECK_MSG(a.at(k).name == b.at(k).name, "同分要按名字定序，结果必须可复现");
    }
    CHECK(a.at(0).name == "aa");
}

// ── render ──────────────────────────────────────────────────────────────────
TEST(render_carries_the_type) {
    Fixture f;
    f.mem.write("k", "描述", "正文", MemoryType::Feedback);
    const auto r = f.mem.get("k")->render();
    CHECK(has(r, "k"));
    CHECK(has(r, "描述"));
    CHECK(has(r, "正文"));
    CHECK_MSG(has(r, "feedback"),
              "类型要带上 —— 模型看到 [feedback] 才知道这是「该怎么做事」"
              "而不是一条可以忽略的参考资料");
}

int main() { return mt::run_all(); }
