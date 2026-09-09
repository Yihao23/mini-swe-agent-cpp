// 【Stage 6】后台任务。
//
// 这一层是全项目唯一真正的异步：start() 返回时活还没干完。所以它的失败方式
// 和别处都不同 —— 数据竞争是概率性的（本地一百次都对，CI 上偶尔挂），
// 孤儿进程是不可见的（agent 退出了，dev server 还占着端口）。
//
// 断言重点在四处：cursor 推进对不对、缓冲区截断后 cursor 还有效、
// 每个任务只通知一次、析构杀干净。

#include "microtest.hpp"

#include "mini_agent/background.hpp"

#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace mini;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path workdir() {
    const auto p = fs::temp_directory_path() / ("bg_" + std::to_string(::getpid()));
    fs::create_directories(p);
    return fs::canonical(p);
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

/// 等到条件成立或超时。后台任务是异步的，不能假设它立刻完成。
template <class F>
bool wait_until(F&& cond, std::chrono::milliseconds limit = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cond()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return cond();
}

/// 进程还活着？kill(pid,0) 不发信号，只做存在性检查。
bool alive(pid_t pid) { return ::kill(pid, 0) == 0; }

}  // namespace

// ── 异步：start 立刻返回 ────────────────────────────────────────────────────
TEST(start_returns_immediately_while_the_command_is_still_running) {
    BackgroundManager bg;
    const auto t0 = std::chrono::steady_clock::now();
    const auto id = bg.start("sleep 5", workdir(), "慢命令");
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_MSG(!id.empty(), "要返回一个 task id");
    // ⚠️ 这就是它和 bash 工具的全部区别。阻塞的话这里要等 5 秒。
    CHECK_MSG(elapsed < 500ms, "start 必须立刻返回 —— 阻塞的话这一层就没有存在意义");
    CHECK_MSG(has(bg.render_list(), "运行中"), "命令应该还在跑");
}

TEST(ids_are_unique_and_sequential) {
    BackgroundManager bg;
    CHECK(bg.start("true", workdir(), "a") == "bg_1");
    CHECK(bg.start("true", workdir(), "b") == "bg_2");
}

// ── drain：只给新增 ─────────────────────────────────────────────────────────
TEST(drain_returns_only_what_is_new) {
    BackgroundManager bg;
    const auto id = bg.start("echo first; sleep 0.3; echo second", workdir(), "两段");

    CHECK(wait_until([&] { return has(bg.drain(id), ""); }, 100ms) || true);
    // 第一段
    CHECK_MSG(wait_until([&] { return has(bg.render_list(), "bg_1"); }), "任务要出现在列表里");
    std::string first;
    CHECK(wait_until([&] { first += bg.drain(id); return has(first, "first"); }));

    // ⚠️ 已经给过的不能再给一次。每次全给的话，一个 dev server 跑十分钟能吐
    //    几万行，几轮就把上下文撑爆，而新信息只有最后几行。
    std::string second;
    CHECK(wait_until([&] { second += bg.drain(id); return has(second, "second"); }));
    CHECK_MSG(!has(second, "first"), "第二次不该再返回第一段");
}

TEST(drain_is_empty_when_nothing_new) {
    BackgroundManager bg;
    const auto id = bg.start("echo once", workdir(), "一次");
    std::string all;
    CHECK(wait_until([&] { all += bg.drain(id); return has(all, "once"); }));
    CHECK_MSG(bg.drain(id).empty(), "没有新输出就返回空 —— drain 是一次性的");
}

TEST(drain_of_an_unknown_id_is_empty_not_a_crash) {
    BackgroundManager bg;
    CHECK(bg.drain("bg_999").empty());
    CHECK(bg.drain("").empty());
}

// ── 缓冲区上界 ──────────────────────────────────────────────────────────────
TEST(a_huge_output_does_not_grow_without_bound) {
    BackgroundManager bg;
    // 分 20 批慢慢吐，总量约 1.4 MB，远超 256 KB 的上界
    const auto id = bg.start(
        "i=0; while [ $i -lt 20 ]; do yes filler | head -10000; sleep 0.02; i=$((i+1)); done",
        workdir(), "话痨");

    // ⚠️ 两个条件缺一不可，缺任何一个这条用例都测不到"截断时修正 cursor"：
    //
    //   ① 必须**边跑边 drain** —— 只在结束后读一次的话 cursor 全程是 0，
    //      截断后 substr(0) 照样有效。
    //   ② 输出必须**慢慢产生** —— `yes | head` 会在几毫秒内吐完 1.4MB，
    //      所有截断都发生在第一次 drain 之前，cursor 还是 0。
    //
    // 两版都被变异测试抓到过（把那句修正删掉，测试照样全绿）。
    // 现在的命令分 20 批、每批之间歇 20ms，drain 就能穿插进截断中间：
    // 那时 cursor 落在 [128KB, 256KB]，而截断把 buffer 砍到 128KB ——
    // 不修正的话下一次 substr(cursor) 直接抛 std::out_of_range。
    std::size_t total = 0;
    CHECK(wait_until([&] {
        total += bg.drain(id).size();          // 一边读，cursor 一直在往前推
        return has(bg.render_list(), "已结束");
    }, 15000ms));
    total += bg.drain(id).size();

    CHECK_MSG(total > 0, "截断之后 drain 还要能正常返回");
    CHECK_MSG(bg.drain(id).empty(), "全读完之后再 drain 应该是空的，cursor 推对了");
}

TEST(a_huge_output_is_capped_and_says_so) {
    BackgroundManager bg;
    const auto id = bg.start("yes filler | head -200000", workdir(), "话痨");
    CHECK(wait_until([&] { return has(bg.render_list(), "已结束"); }, 15000ms));

    const auto out = bg.drain(id);   // 全程没 drain 过，一次拿到截断后的全部
    CHECK_MSG(out.size() < 1024u * 1024, "缓冲区要有上界，不能把内存吃光");
    CHECK_MSG(has(out, "已丢弃"), "丢过内容要告诉模型，否则它以为读全了");
}

// ── 通知：每个任务只报一次 ──────────────────────────────────────────────────
TEST(a_finished_task_is_reported_exactly_once) {
    BackgroundManager bg;
    bg.start("echo done", workdir(), "小活");

    std::vector<std::string> first;
    CHECK(wait_until([&] { first = bg.notifications(); return !first.empty(); }));
    CHECK(has(first.at(0), "bg_1"));
    CHECK_MSG(has(first.at(0), "退出码"), "通知里要有退出码");

    // ⚠️ 报两次的话模型会以为那条命令跑了两遍，然后据此行动 ——
    //    撤销它，或者再做一遍。
    for (int i = 0; i < 3; ++i)
        CHECK_MSG(bg.notifications().empty(), "同一个任务不能报第二次");
}

TEST(a_running_task_is_not_reported) {
    BackgroundManager bg;
    bg.start("sleep 5", workdir(), "还在跑");
    CHECK_MSG(bg.notifications().empty(), "没结束的不该通知");
}

TEST(each_task_gets_its_own_notification) {
    BackgroundManager bg;
    bg.start("echo a", workdir(), "甲");
    bg.start("echo b", workdir(), "乙");
    std::vector<std::string> all;
    CHECK(wait_until([&] {
        for (auto& n : bg.notifications()) all.push_back(n);
        return all.size() >= 2;
    }));
    CHECK(all.size() == 2);
}

// ── 退出码 ──────────────────────────────────────────────────────────────────
TEST(the_exit_code_comes_back) {
    BackgroundManager bg;
    bg.start("exit 42", workdir(), "会失败");
    std::vector<std::string> notes;
    CHECK(wait_until([&] { notes = bg.notifications(); return !notes.empty(); }));
    CHECK_MSG(has(notes.at(0), "42"), "非零退出码要报出来 —— 模型得知道它失败了");
}

TEST(a_command_that_cannot_start_still_gets_an_id) {
    BackgroundManager bg;
    const auto id = bg.start("echo hi", "/no/such/dir/anywhere", "起不来");
    CHECK_MSG(!id.empty(), "起不来也要给个 id —— 模型拿到了它，得能查到原因");
    CHECK(wait_until([&] { return has(bg.render_list(), "已结束"); }));
}

// ── kill ────────────────────────────────────────────────────────────────────
TEST(kill_stops_the_whole_process_group) {
    BackgroundManager bg;
    // sleep 是 sh fork 出来的孙子。只 kill(pid) 的话它会活下来，
    // 还攥着管道写端 —— 读线程永远等不到 EOF。
    const auto id = bg.start("sleep 29 & echo PID=$!; wait", workdir(), "有孙子");

    std::string out;
    CHECK(wait_until([&] { out += bg.drain(id); return has(out, "PID="); }));
    const auto at = out.find("PID=");
    const pid_t grandchild = static_cast<pid_t>(std::atol(out.c_str() + at + 4));
    CHECK(grandchild > 0);
    CHECK(alive(grandchild));

    bg.kill(id);
    CHECK_MSG(wait_until([&] { return !alive(grandchild); }),
              "孙子进程还活着 —— 说明杀的是 pid 不是 -pgid");
}

TEST(kill_of_an_unknown_id_is_a_no_op) {
    BackgroundManager bg;
    bg.kill("bg_999");    // 不该崩
    bg.kill("");
    CHECK(true);
}

// ── 析构 ────────────────────────────────────────────────────────────────────
TEST(kill_all_kills_the_whole_group) {
    BackgroundManager bg;
    const auto id = bg.start("sleep 29 & echo PID=$!; wait", workdir(), "孙子候选");
    std::string out;
    CHECK(wait_until([&] { out += bg.drain(id); return has(out, "PID="); }));
    const pid_t grandchild = static_cast<pid_t>(std::atol(out.c_str() + out.find("PID=") + 4));
    CHECK(alive(grandchild));

    // ⚠️ 直接测 kill_all() 本身，不走析构。析构里还有 jthread join 等别的动作，
    //    最后孙子会被别的路径带走 —— 那条测的是「析构之后没有孤儿」这个结果，
    //    对"靠哪一步达成"不敏感。变异测试抓到过：kill_all 改成杀 pid，
    //    析构那条照样绿。
    bg.kill_all();
    CHECK_MSG(wait_until([&] { return !alive(grandchild); }, 2000ms),
              "kill_all 必须杀进程组 —— 杀 pid 的话 `npm run dev` fork 出的"
              "孙子进程会活下来，还攥着管道写端");
}

TEST(the_destructor_leaves_no_orphans) {
    pid_t grandchild = 0;
    {
        BackgroundManager bg;
        const auto id = bg.start("sleep 29 & echo PID=$!; wait", workdir(), "孤儿候选");
        std::string out;
        CHECK(wait_until([&] { out += bg.drain(id); return has(out, "PID="); }));
        grandchild = static_cast<pid_t>(std::atol(out.c_str() + out.find("PID=") + 4));
        CHECK(alive(grandchild));
    }   // ← 析构
    // ⚠️ 不杀干净的话，agent 退出后留下一堆孤儿：dev server 占着端口、
    //    watcher 占着 inotify 句柄，而没有任何东西知道它们存在。
    CHECK_MSG(wait_until([&] { return !alive(grandchild); }),
              "析构必须杀干净，否则留下没人知道的孤儿进程");
}

TEST(the_destructor_does_not_hang_on_a_long_running_task) {
    const auto t0 = std::chrono::steady_clock::now();
    {
        BackgroundManager bg;
        bg.start("sleep 60", workdir(), "很久");
        bg.start("sleep 60", workdir(), "也很久");
    }   // ← 析构。jthread 会 join，所以读线程必须能被叫停
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK_MSG(elapsed < 5000ms, "析构不能卡在 join 上等一分钟");
}

// ── 列表 ────────────────────────────────────────────────────────────────────
TEST(render_list_is_empty_when_there_is_nothing) {
    BackgroundManager bg;
    CHECK(bg.render_list().empty());
}

TEST(render_list_distinguishes_running_from_finished) {
    BackgroundManager bg;
    bg.start("echo quick", workdir(), "快的");
    bg.start("sleep 5", workdir(), "慢的");
    CHECK(wait_until([&] { return has(bg.render_list(), "已结束"); }));
    const auto list = bg.render_list();
    CHECK(has(list, "运行中"));
    CHECK(has(list, "快的"));
    CHECK(has(list, "慢的"));
}

// ── 上限与清理 ──────────────────────────────────────────────────────────────
TEST(there_is_a_cap_on_concurrent_tasks) {
    BackgroundManager bg;
    const auto wd = workdir();
    std::size_t started = 0;
    for (int i = 0; i < 20; ++i)
        if (!bg.start("sleep 10", wd, "占位").empty()) ++started;

    // ⚠️ 一个陷进循环的模型（「启动服务器 → 没反应 → 再启动一个」）能把
    //    线程和进程都开爆，而每一次调用单独看都是合理的。
    CHECK_MSG(started > 0, "至少要能起几个");
    CHECK_MSG(started < 20, "必须有上限 —— 否则模型能无限开进程");
    CHECK_MSG(bg.start("sleep 10", wd, "第 21 个").empty(),
              "满了要返回空 id，让调用方能给模型一条说明");
}

TEST(a_freed_slot_can_be_reused) {
    BackgroundManager bg;
    const auto wd = workdir();
    std::vector<std::string> ids;
    for (int i = 0; i < 20; ++i) {
        auto id = bg.start("sleep 10", wd, "占位");
        if (id.empty()) break;
        ids.push_back(id);
    }
    CHECK(!ids.empty());
    CHECK_MSG(bg.start("true", wd, "被挡住").empty(), "先确认确实满了");

    bg.kill(ids.front());
    CHECK_MSG(wait_until([&] { return !bg.start("true", wd, "补位").empty(); }),
              "杀掉一个之后名额要能腾出来");
}

TEST(old_finished_tasks_are_reaped) {
    BackgroundManager bg;
    const auto wd = workdir();
    for (int i = 0; i < 20; ++i) {
        bg.start("true", wd, "短命 " + std::to_string(i));
        // 等它结束并通知过 —— 只有「已结束且已通知」的才会被清
        wait_until([&] { return !bg.notifications().empty(); }, 2000ms);
    }
    const auto list = bg.render_list();
    const auto lines = std::ranges::count(list, '\n');
    // ⚠️ tasks 只增不减的话，跑一整天会攒下几百条，render_list 越来越长
    //    而且大部分是模型早就读完的死任务。
    CHECK_MSG(lines < 15, "已结束的旧任务要清掉，列表不能无限长");
    CHECK_MSG(lines > 1, "但不能全清 —— 最近几条还要能查");
}

TEST(an_unreported_task_is_never_reaped) {
    BackgroundManager bg;
    const auto wd = workdir();
    const auto first = bg.start("echo important", wd, "还没通知过");
    CHECK(wait_until([&] { return has(bg.render_list(), "已结束"); }));

    // 故意不调 notifications()，然后塞满。逐个等它结束 —— 不等的话
    // kMaxRunning 会挡住后面的，攒不够 kMaxFinished 条，清理根本不触发。
    for (int i = 0; i < 20; ++i) {
        const auto nid = bg.start("true", wd, "噪音 " + std::to_string(i));
        if (!nid.empty())
            wait_until([&] { return has(bg.render_list(), "已结束 exit=0"); }, 2000ms);
    }
    // ⚠️ 没通知过就清掉的话，模型永远不知道那个任务结束了 ——
    //    它还在等一条不会来的消息。
    CHECK_MSG(has(bg.render_list(), first), "没通知过的任务不能被清掉");
}

// ── 并发 ────────────────────────────────────────────────────────────────────
TEST(concurrent_drains_do_not_corrupt_anything) {
    BackgroundManager bg;
    const auto id = bg.start("yes line | head -20000", workdir(), "边写边读");

    // 主线程一直在 drain，读线程一直在写。没有锁的话这里是数据竞争。
    std::string collected;
    std::thread other([&] {
        for (int i = 0; i < 200; ++i) {
            (void)bg.render_list();
            std::this_thread::sleep_for(1ms);
        }
    });
    CHECK(wait_until([&] { collected += bg.drain(id); return has(bg.render_list(), "已结束"); },
                     10000ms));
    other.join();
    collected += bg.drain(id);
    CHECK_MSG(!collected.empty(), "边写边读要能拿到内容");
    CHECK_MSG(has(collected, "line"), "内容不能是乱的");
}

int main() { return mt::run_all(); }
