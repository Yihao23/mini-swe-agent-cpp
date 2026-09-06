// run_shell 的测试。重点不是"能跑通命令"，是 process.hpp 里点名的三个坑：
//   ① 父进程不 close 写端  → 读不到 EOF，命令早退了也要卡到超时
//   ② 不 setpgid           → 杀不掉孙子进程
//   ③ 截断后停止读         → 子进程阻塞在 write() 上，反被自己的截断逻辑挂死
// 前两条各有一个专门的用例，第三条藏在 huge_output_does_not_hang 里。

#include "microtest.hpp"

#include "mini_agent/process.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace mini;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

fs::path workspace() {
    const auto p = fs::temp_directory_path() / ("run_shell_" + std::to_string(::getpid()));
    fs::create_directories(p);
    return fs::canonical(p);
}

/// 命令仍然活着？kill(pid, 0) 不发信号，只做存在性 + 权限检查。
bool alive(pid_t pid) { return ::kill(pid, 0) == 0; }

long ms(std::chrono::milliseconds d) { return d.count(); }

}  // namespace

TEST(runs_a_command_and_reports_exit_zero) {
    const auto r = run_shell("echo hello", workspace(), 5s);
    CHECK(!r.spawn_failed);
    CHECK(!r.timed_out);
    CHECK_MSG(r.exit_code == 0, "echo 应该正常退出");
    CHECK_MSG(r.output == "hello\n", "输出要原样带回，含换行");
}

TEST(stderr_is_merged_into_output) {
    const auto r = run_shell("echo out; echo err >&2", workspace(), 5s);
    CHECK_MSG(r.output.find("out") != std::string::npos, "stdout 要在");
    CHECK_MSG(r.output.find("err") != std::string::npos, "stderr 也要在 —— 模型靠它排错");
}

TEST(exit_code_comes_back) {
    CHECK(run_shell("exit 42", workspace(), 5s).exit_code == 42);
    CHECK(run_shell("false", workspace(), 5s).exit_code == 1);
    CHECK_MSG(run_shell("no_such_command_xyz", workspace(), 5s).exit_code == 127,
              "命令找不到 = 127，沿用 shell 的约定");
}

TEST(signal_death_becomes_128_plus_signum) {
    const auto r = run_shell("kill -TERM $$", workspace(), 5s);
    CHECK_MSG(r.exit_code == 128 + SIGTERM, "被信号杀死要能和正常退出区分开");
    CHECK_MSG(!r.timed_out, "自己被杀不算超时");
}

TEST(command_runs_in_the_given_cwd) {
    const auto wd = workspace();
    const auto r = run_shell("pwd", wd, 5s);
    CHECK_MSG(r.output == wd.string() + "\n", "cwd 没生效的话 agent 会在错误的目录里干活");
}

TEST(bad_cwd_fails_without_throwing) {
    const auto r = run_shell("echo hi", "/no/such/dir/anywhere", 5s);
    CHECK_MSG(r.exit_code == 126, "chdir 失败 = 126");
    CHECK_MSG(!r.spawn_failed, "fork/pipe 都成功了，失败发生在 exec 之前的子进程里");
}

// ── 坑 ①：父进程必须 close 写端 ────────────────────────────────────────────
TEST(fast_command_returns_immediately_not_at_the_timeout) {
    const auto r = run_shell("echo quick", workspace(), 10s);
    CHECK(!r.timed_out);
    CHECK_MSG(ms(r.duration) < 2000,
              "父进程漏关写端的话，管道永远有写者，这里会卡满 10 秒");
}

TEST(timeout_kills_and_is_reported) {
    const auto r = run_shell("sleep 30", workspace(), 1s);
    CHECK_MSG(r.timed_out, "必须标成超时");
    CHECK_MSG(r.exit_code != 0, "超时不能看起来像成功");
    CHECK_MSG(ms(r.duration) < 3000, "1 秒的超时不该等到 30 秒");
    CHECK_MSG(r.output.find("超时") != std::string::npos, "输出里要有说明，模型才知道发生了什么");
}

TEST(partial_output_survives_a_timeout) {
    const auto r = run_shell("echo before; sleep 30", workspace(), 1s);
    CHECK(r.timed_out);
    CHECK_MSG(r.output.find("before") != std::string::npos,
              "超时前已经产出的东西要留下 —— 那往往正是排错需要的");
}

// ── 坑 ②：必须杀进程组，不是单个 pid ───────────────────────────────────────
TEST(timeout_kills_the_whole_process_group) {
    // sleep 是子进程 fork 出来的孙子。只 kill(pid) 的话它会活下来，
    // 而且攥着管道写端不放 —— 下一次 run_shell 都可能受影响。
    const auto r = run_shell("sleep 29 & echo PID=$!; wait", workspace(), 1s);
    CHECK(r.timed_out);

    const auto at = r.output.find("PID=");
    CHECK_MSG(at != std::string::npos, "孙子进程的 pid 要能拿到");
    const pid_t grandchild = static_cast<pid_t>(std::atol(r.output.c_str() + at + 4));
    CHECK(grandchild > 0);

    // 内核回收要一点时间，给 200ms
    for (int i = 0; i < 20 && alive(grandchild); ++i) ::usleep(10000);
    CHECK_MSG(!alive(grandchild), "孙子进程还活着 —— 说明杀的是 pid 不是 -pgid");
}

// ── 坑 ③：截断之后仍要继续排干管道 ─────────────────────────────────────────
TEST(truncated_command_still_runs_to_completion) {
    // 约 700KB 输出，然后**自己退出**。选这条命令是有讲究的：截断后如果不再
    // read()，子进程会阻塞在 write() 上永远退不出去 —— 我们等 EOF、它等我们读。
    // 换成 `yes`（永不结束）就测不出来了：那种命令正确实现也会超时，
    // 两边答案一样，什么都证明不了。
    const auto r = run_shell("echo HEAD; yes filler | head -100000", workspace(), 5s, 512);
    CHECK_MSG(!r.timed_out, "命令自己会结束 —— 超时说明截断后停止了排干管道");
    CHECK_MSG(r.exit_code == 0, "应该正常退出");
    CHECK_MSG(ms(r.duration) < 2000, "丢弃多余输出很快，不该拖到超时");
    CHECK_MSG(r.output.size() < 4096, "必须截断，不能把内存吃光");
    CHECK_MSG(r.output.find("截断") != std::string::npos, "要说明发生了截断");
    CHECK_MSG(r.output.rfind("HEAD", 0) == 0, "保留开头 —— 命令是干什么的写在前面");
}

TEST(endless_output_still_honours_the_timeout) {
    // `yes` 永不停止，只能靠超时收场。和上一条互补：那条证明"会读完"，
    // 这条证明"不会读到天荒地老"。
    const auto r = run_shell("yes hello", workspace(), 1s, /*max_output_bytes=*/1024);
    CHECK_MSG(r.timed_out, "永不结束的命令必须被超时拦下");
    CHECK_MSG(ms(r.duration) < 3000, "1 秒的超时不该拖成十几秒");
    CHECK_MSG(r.output.size() < 4096, "边丢边读，内存不能涨");
}

TEST(empty_output_is_fine) {
    const auto r = run_shell("true", workspace(), 5s);
    CHECK(r.exit_code == 0);
    CHECK(r.output.empty());
    CHECK(!r.timed_out);
}

TEST(no_zombies_left_behind) {
    for (int i = 0; i < 5; ++i) (void)run_shell("true", workspace(), 5s);
    // 所有子进程都 waitpid 过了，这里应该无子可收
    CHECK_MSG(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD,
              "有僵尸进程没收 —— 长时间跑的 agent 会耗尽进程表");
}

int main() { return mt::run_all(); }
