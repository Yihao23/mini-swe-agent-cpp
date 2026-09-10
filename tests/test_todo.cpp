// 【Stage 4】计划清单的两半：todo 工具往 ctx.todos 写，turn_context 每轮读回来。
//
// 这一对合起来才有意义，所以测试也成对写：光验证工具改了 ctx.todos 没用 ——
// 真正要证明的是**写进去的东西下一轮回得来**。
//
// 盯着三件事：
//   * 整表替换 —— 不是往里加，是整个换掉
//   * 同一时刻只能有一条 in_progress，否则"现在在做什么"没有答案
//   * 多余的键要丢掉，不然每轮回灌都把它们再喂一遍

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/prompt.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <unistd.h>
#include <filesystem>
#include <memory>
#include <string>

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
    ToolPtr todo = make_todo_tool();

    Fixture() {
        root = fs::temp_directory_path() /
               ("todo_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    ToolResult submit(Json items) {
        return todo->run(Json{{"todos", std::move(items)}}, ctx);
    }
    /// 下一轮开头模型真正会看到的那段文字。
    std::string next_turn() const { return turn_context(nullptr, ctx.todos); }
};

Json item(std::string content, std::string status) {
    return Json{{"content", std::move(content)}, {"status", std::move(status)}};
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── 声明 ────────────────────────────────────────────────────────────────────

TEST(todo_declares_itself_correctly) {
    Fixture f;
    CHECK(f.todo->name() == "todo");
    // ⚠️ 它改 ctx.todos，而 task 工具会整个拷贝 ctx。run_batch 今天是串行的，
    //    这个标记就是保证哪天并行了也没事。
    CHECK_MSG(!f.todo->read_only(), "会改 ctx.todos，不能和别的工具并发");
    CHECK_MSG(!f.todo->requires_permission(), "agent 自己的草稿纸，加闸门只会教它别做计划");
}

TEST(the_schema_pins_the_three_statuses) {
    Fixture f;
    // 状态是个封闭集合。schema 里不写死的话模型会自创 "doing"、"blocked"，
    // 而 run() 只能一条条退回去 —— 白花一轮。
    const auto e = f.todo->input_schema()["properties"]["todos"]["items"]
                          ["properties"]["status"]["enum"];
    CHECK(e.size() == 3);
    CHECK(has(e.dump(), "in_progress"));
}

// ── 写进去，下一轮读回来 ────────────────────────────────────────────────────

TEST(what_is_submitted_comes_back_next_turn) {
    Fixture f;
    CHECK(!f.submit(Json::array({item("写测试", "in_progress"), item("改文档", "pending")}))
               .is_error);

    // ⚠️ 这一条才是验收。工具改没改 ctx.todos 是内部细节；模型真正看到的是
    //    下一轮开头 turn_context 吐出来的这段。
    const auto seen = f.next_turn();
    CHECK(has(seen, "写测试"));
    CHECK(has(seen, "改文档"));
    CHECK(has(seen, "in_progress"));
}

TEST(the_result_echoes_the_committed_list) {
    Fixture f;
    // 回显不是客套：上面丢掉了多余的键，而下一轮才会再喂一次 ——
    // 中间这一步不回显，模型就是瞎的。
    const auto r = f.submit(Json::array({item("写测试", "in_progress")}));
    CHECK(has(r.content, "写测试"));
    CHECK(has(r.content, "in_progress"));
}

// ── 整表替换 ────────────────────────────────────────────────────────────────

TEST(a_submission_replaces_the_whole_list) {
    Fixture f;
    f.submit(Json::array({item("第一件", "pending"), item("第二件", "pending")}));
    CHECK(!f.submit(Json::array({item("第三件", "pending")})).is_error);

    const auto seen = f.next_turn();
    CHECK(has(seen, "第三件"));
    // ⚠️ 这两条是「替换」和「追加」唯一分得开的地方。少了它们，
    //    一个往里 append 的实现照样全绿。
    CHECK_MSG(!has(seen, "第一件"), "旧的要被整个换掉，不是往里加");
    CHECK_MSG(!has(seen, "第二件"), "旧的要被整个换掉，不是往里加");
}

TEST(an_empty_list_clears_the_plan) {
    Fixture f;
    f.submit(Json::array({item("干完了", "completed")}));
    const auto r = f.submit(Json::array());
    CHECK(!r.is_error);
    CHECK_MSG(!r.content.empty(), "ToolResult.content 不许为空");
    CHECK_MSG(f.next_turn().empty(), "清空之后下一轮不该再提 todo");
}

// ── 只能有一条在进行 ────────────────────────────────────────────────────────

TEST(only_one_entry_may_be_in_progress) {
    Fixture f;
    const auto r = f.submit(Json::array({item("甲", "in_progress"), item("乙", "in_progress")}));
    // ⚠️ 允许两条同时进行的话，「现在在做什么」就没有答案了 ——
    //    而那是模型看这张表的唯一理由。
    CHECK_MSG(r.is_error, "两条 in_progress 必须被拒");
    CHECK_MSG(has(r.content, "in_progress"), "错误信息要说清是哪条规则");
}

TEST(a_rejected_submission_leaves_the_old_plan_alone) {
    Fixture f;
    f.submit(Json::array({item("原计划", "in_progress")}));
    f.submit(Json::array({item("甲", "in_progress"), item("乙", "in_progress")}));
    // ⚠️ 校验失败必须是**原子**的：写了一半再报错，模型看到的表和它以为的对不上。
    CHECK_MSG(has(f.next_turn(), "原计划"), "被拒的提交不能动到已有的表");
}

TEST(many_completed_and_one_in_progress_is_fine) {
    Fixture f;
    CHECK(!f.submit(Json::array({item("甲", "completed"), item("乙", "completed"),
                                 item("丙", "in_progress"), item("丁", "pending")}))
               .is_error);
    CHECK(has(f.next_turn(), "丙"));
}

// ── 校验 ────────────────────────────────────────────────────────────────────

TEST(a_bogus_status_is_refused_by_name) {
    Fixture f;
    const auto r = f.submit(Json::array({item("甲", "doing")}));
    CHECK(r.is_error);
    // 错误信息要能直接照着改。笼统一句「参数不对」模型改不了，白花一轮。
    CHECK(has(r.content, "doing"));
    CHECK(has(r.content, "pending"));
}

TEST(a_missing_content_says_which_entry) {
    Fixture f;
    const auto r = f.submit(Json::array({item("甲", "pending"), Json{{"status", "pending"}}}));
    CHECK(r.is_error);
    CHECK_MSG(has(r.content, "第 2 条"), "要说第几条，不然模型不知道改哪个");
}

TEST(an_empty_content_is_refused) {
    Fixture f;
    CHECK(f.submit(Json::array({item("", "pending")})).is_error);
}

TEST(a_non_array_argument_is_refused) {
    Fixture f;
    CHECK(f.todo->run(Json{{"todos", "写测试"}}, f.ctx).is_error);
    CHECK(f.todo->run(Json::object(), f.ctx).is_error);
}

// ── 丢掉多余的键 ────────────────────────────────────────────────────────────

TEST(extra_keys_are_dropped_so_they_are_not_re_fed_every_turn) {
    Fixture f;
    Json fat = item("写测试", "pending");
    fat["id"] = "t-1";
    fat["priority"] = "high";
    fat["notes"] = "记得先看 sandbox.hpp";
    CHECK(!f.submit(Json::array({fat})).is_error);

    // ⚠️ turn_context 根本不读这些键。留着的话每一轮都把它们再喂一遍 ——
    //    一张长表能悄悄吃掉几百 token，而且模型永远看不到它们。
    const auto stored = f.ctx.todos[0];
    CHECK_MSG(stored.size() == 2, "只留 content 和 status");
    CHECK(!stored.contains("notes"));
}

int main() { return mt::run_all(); }
