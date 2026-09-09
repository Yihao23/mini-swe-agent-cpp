// 【Stage 6】后台任务的三条工具路径：bash(run_in_background)、bash_output、kill_task，
// 以及 turn_context() 把完成通知注入下一轮。
//
// BackgroundManager 自己在 test_background.cpp 里测。这里盯的是**工具层**：
//   * 前台/后台这条分叉在沙箱**之后** —— 后台跑的东西没人盯着，闸门更不能松
//   * bash_output 只给新的（cursor），且"没有新输出"要能和"id 打错了"分开
//   * ctx.background == nullptr（子 agent 里就是）时，两个工具都要礼貌拒绝
//   * 通知**只发一次** —— 发两次模型会以为命令跑了两遍，然后照着这个去行动

#include "microtest.hpp"

#include "mini_agent/background.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/prompt.hpp"
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
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    BackgroundManager bg;
    ToolContext ctx;
    ToolPtr bash = make_bash_tool();
    ToolPtr bash_output = make_bash_output_tool();
    ToolPtr kill_task = make_kill_task_tool();

    Fixture() {
        root = fs::temp_directory_path() /
               ("s6t_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        cfg.ensure_dirs();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
        ctx.background = &bg;
    }
    // ⚠️ bg 在 ctx 之前声明，所以 bg 最后析构 —— 子进程被杀在 ctx 失效之后。
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    ToolResult run_bg(const std::string& cmd) {
        return bash->run(Json{{"command", cmd}, {"run_in_background", true}}, ctx);
    }
    ToolResult read(const std::string& id) {
        return bash_output->run(Json{{"task_id", id}}, ctx);
    }
    ToolResult list() { return bash_output->run(Json::object(), ctx); }
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

/// 从 "任务 id: bg_1" 里把 id 抠出来。
std::string task_id_of(const ToolResult& r) {
    return r.metadata.value("task_id", std::string{});
}

template <class F>
bool wait_until(F&& cond, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return cond();
}

}  // namespace

// ── 声明 ────────────────────────────────────────────────────────────────────

TEST(the_two_tools_declare_themselves_correctly) {
    Fixture f;
    CHECK(f.bash_output->name() == "bash_output");
    CHECK_MSG(f.bash_output->read_only(), "只是读缓冲区");
    CHECK_MSG(!f.bash_output->requires_permission(),
              "命令启动时已经过闸了；再拦一道只会教模型绕开这个工具");

    CHECK(f.kill_task->name() == "kill_task");
    CHECK_MSG(!f.kill_task->read_only(), "杀 dev server 是真的副作用");
    CHECK_MSG(f.kill_task->requires_permission(), "有副作用就要过闸");
}

TEST(kill_task_is_reviewed_by_task_id) {
    Fixture f;
    // 规则要能写 deny kill_task(bg_1) —— 审查对象必须是 id 本身。
    CHECK(f.kill_task->subject(Json{{"task_id", "bg_3"}}) == "bg_3");
}

TEST(bash_advertises_the_background_switch) {
    Fixture f;
    // 模型只从 description + schema 知道有这条路。schema 里没有 = 它不会用。
    CHECK(has(std::string(f.bash->description()), "run_in_background"));
    CHECK(f.bash->input_schema()["properties"].contains("run_in_background"));
}

// ── 后台启动 ────────────────────────────────────────────────────────────────

TEST(background_bash_returns_at_once_instead_of_waiting) {
    Fixture f;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = f.run_bg("sleep 5");
    const auto took = std::chrono::steady_clock::now() - t0;

    CHECK_MSG(!r.is_error, "启动成功");
    CHECK_MSG(!task_id_of(r).empty(), "metadata 里要带 task_id，UI 靠它认任务");
    CHECK_MSG(has(r.content, "bg_"), "正文里也要有 id —— 模型读的是正文");
    // ⚠️ 这一条才是整个 Stage 6 的理由。同一条命令走前台会占满 5 秒。
    CHECK_MSG(took < 1s, "必须立刻返回，不能等命令结束");
    f.bg.kill_all();
}

TEST(a_foreground_bash_still_blocks_and_returns_output) {
    Fixture f;
    // 反面对照：不加参数时行为一个字都不能变。
    const auto r = f.bash->run(Json{{"command", "echo 前台"}}, f.ctx);
    CHECK(!r.is_error);
    CHECK(has(r.content, "前台"));
    CHECK_MSG(task_id_of(r).empty(), "前台没有任务 id");
}

TEST(the_background_switch_must_be_a_real_boolean) {
    Fixture f;
    // 模型给错类型是家常便饭。字符串 "true" 不算 —— 退回前台，而不是抛异常。
    const auto r = f.bash->run(Json{{"command", "echo x"}, {"run_in_background", "true"}}, f.ctx);
    CHECK(!r.is_error);
    CHECK_MSG(has(r.content, "x"), "当成前台跑了");
    CHECK(task_id_of(r).empty());
}

// ── 读输出 ──────────────────────────────────────────────────────────────────

TEST(bash_output_returns_only_what_is_new) {
    Fixture f;
    // ⚠️ 标记必须**不出现在命令原文里**。render_list() 会把命令印出来，而
    //    "没有新输出" 那条路径附带整张列表 —— 直接 `echo 第一段` 的话，
    //    第一次轮询就能在列表里看到"第一段"，断言在任何输出产生之前就绿了。
    //    这就是变异测试反复抓到的那种空断言：正确实现和坏实现都能通过。
    std::ofstream(f.cfg.workdir / "a.txt", std::ios::binary) << "第一段\n";
    std::ofstream(f.cfg.workdir / "b.txt", std::ios::binary) << "第二段\n";
    const auto id = task_id_of(f.run_bg("cat a.txt; sleep 0.4; cat b.txt"));

    std::string first;
    CHECK(wait_until([&] { first = f.read(id).content; return has(first, "第一段"); }));

    std::string second;
    CHECK(wait_until([&] { second = f.read(id).content; return has(second, "第二段"); }));
    // ⚠️ 这一条是 cursor 存在的全部理由。少了它，每轮都把整段历史再喂一遍。
    CHECK_MSG(!has(second, "第一段"), "已经读过的不能再给一遍");
}

TEST(nothing_new_is_told_apart_from_a_wrong_id) {
    Fixture f;
    const auto id = task_id_of(f.run_bg("sleep 5"));

    const auto quiet = f.read(id);
    CHECK_MSG(!quiet.content.empty(), "content 不能为空 —— API 会拒");
    CHECK(has(quiet.content, "没有新输出"));
    // ⚠️ 两种情况都返回空字符串的话，模型会对着一个打错的 id 一直等下去。
    //    附上列表，真相就摆在眼前了。
    CHECK_MSG(has(quiet.content, id), "列表里有这个任务 = id 是对的，只是还没输出");

    const auto typo = f.read("bg_999");
    CHECK_MSG(!has(typo.content, "bg_999"), "列表里没有 bg_999 = id 打错了");
    f.bg.kill_all();
}

TEST(bash_output_without_an_id_lists_everything) {
    Fixture f;
    const auto a = task_id_of(f.run_bg("sleep 5"));
    const auto b = task_id_of(f.run_bg("sleep 5"));
    const auto r = f.list();
    CHECK(has(r.content, a));
    CHECK(has(r.content, b));
    f.bg.kill_all();
}

TEST(the_listing_is_not_empty_when_there_are_no_tasks) {
    Fixture f;
    CHECK_MSG(!f.list().content.empty(), "ToolResult.content 不许为空");
}

// ── 停止 ────────────────────────────────────────────────────────────────────

TEST(kill_task_stops_one_task) {
    Fixture f;
    const auto id = task_id_of(f.run_bg("sleep 29"));
    const auto r = f.kill_task->run(Json{{"task_id", id}}, f.ctx);
    CHECK(!r.is_error);
    CHECK_MSG(wait_until([&] { return has(f.list().content, "已结束"); }),
              "杀完之后列表里要看得出来它停了");
}

TEST(kill_task_all_stops_every_task) {
    Fixture f;
    f.run_bg("sleep 29");
    f.run_bg("sleep 29");
    CHECK(!f.kill_task->run(Json{{"task_id", "all"}}, f.ctx).is_error);
    CHECK(wait_until([&] { return !has(f.list().content, "运行中"); }));
}

TEST(killing_an_unknown_id_is_not_an_error) {
    Fixture f;
    // 契约：未知 id 静默忽略。报错的话模型会以为出事了，然后去"修"一个不存在的问题。
    const auto r = f.kill_task->run(Json{{"task_id", "bg_999"}}, f.ctx);
    CHECK(!r.is_error);
}

TEST(kill_task_needs_an_id) {
    Fixture f;
    CHECK(f.kill_task->run(Json::object(), f.ctx).is_error);
}

// ── 子 agent 里没有后台任务 ──────────────────────────────────────────────────

TEST(all_three_paths_refuse_without_a_manager) {
    Fixture f;
    f.ctx.background = nullptr;          // 子 agent 的 ctx 就长这样

    const auto started = f.run_bg("sleep 5");
    CHECK_MSG(started.is_error, "不能默默地当前台跑 —— 那会把子 agent 卡住 5 秒");
    CHECK(f.bash_output->run(Json{{"task_id", "bg_1"}}, f.ctx).is_error);
    CHECK(f.kill_task->run(Json{{"task_id", "bg_1"}}, f.ctx).is_error);

    // 但前台 bash 照常能用 —— 收窄的只是后台这条路。
    CHECK(!f.bash->run(Json{{"command", "echo ok"}}, f.ctx).is_error);
}

// ── 通知注入 ────────────────────────────────────────────────────────────────

TEST(a_finished_task_is_injected_into_the_next_turn_exactly_once) {
    Fixture f;
    const auto id = task_id_of(f.run_bg("echo 干完了"));

    std::string ctxt;
    CHECK_MSG(wait_until([&] {
                  ctxt = turn_context(&f.bg, Json::array());
                  return has(ctxt, id);
              }),
              "任务结束后，下一轮开头要能看到它");

    // ⚠️ 第二次必须是空的。重复通知 = 模型认为命令跑了两遍，然后照着这个
    //    去撤销或重做 —— 这是这一层最贵的一种错。
    CHECK_MSG(!has(turn_context(&f.bg, Json::array()), id), "只通知一次");
}

TEST(turn_context_survives_a_null_manager) {
    Fixture f;
    // Stage 6 关掉时 background 是 nullptr，turn_context 照样要能跑。
    const auto s = turn_context(nullptr, Json::array());
    CHECK(s.empty());
}

TEST(notifications_and_todos_coexist) {
    Fixture f;
    const auto id = task_id_of(f.run_bg("echo 干完了"));
    Json todos = Json::array({Json{{"content", "写测试"}, {"status", "in_progress"}}});

    std::string ctxt;
    CHECK(wait_until([&] {
        ctxt = turn_context(&f.bg, todos);
        return has(ctxt, id);
    }));
    CHECK_MSG(has(ctxt, "写测试"), "两种动态内容要能同时出现，不能互相顶掉");
}

int main() { return mt::run_all(); }
