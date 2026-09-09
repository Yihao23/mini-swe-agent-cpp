// 【Stage 2】带超时的子进程执行。整个项目最"系统编程"的一块。
//
// 契约（@brief / @param / @warning / 可执行示例）写在
// include/mini_agent/process.hpp。这里只讲实现上的取舍：为什么用这个系统调用、
// 换个写法会怎么坏。

#include "mini_agent/process.hpp"

#include "spawn.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace mini {
namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

/// SIGTERM 之后给进程组多久收拾，然后上 SIGKILL。
constexpr Ms kGrace{200};
/// SIGKILL 之后再排干管道多久 —— 内核回收 fd 不是瞬间的。
constexpr Ms kReap{200};

/// @brief Milliseconds left before `deadline`, clamped at 0.
/// @note Returns int because that is what poll() takes; 0 means "return
///       immediately", which is exactly the wanted behaviour at the deadline.
int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<Ms>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

/// @brief Read fd until EOF or the deadline, keeping at most `limit` bytes.
///
/// @param fd        The pipe's read end.
/// @param out       Collected output; appended to, never cleared.
/// @param limit     Keep this much; the rest is read and dropped.
/// @param deadline  Absolute, so the grace and reap passes can each get a
///                  short one of their own without touching the caller's.
/// @param truncated Set to true once anything has been dropped.
/// @return true when EOF was reached — every writer closed the pipe, which
///         means the whole process group is done. false means the deadline
///         won.
///
/// @note Reading continues past `limit`, the excess is simply discarded.
///       Stopping the reads leaves the child blocked in write() while this
///       function waits for EOF, so the truncation logic would hang the
///       command it is meant to bound.
/// @note EINTR is retried rather than treated as failure: any signal delivered
///       to this process — SIGCHLD when the command exits, SIGWINCH when the
///       terminal is resized — interrupts poll and read.
bool drain(int fd, std::string& out, std::size_t limit, Clock::time_point deadline,
           bool& truncated) {
    char buf[4096];
    for (;;) {
        const int wait = remaining_ms(deadline);
        // ⚠️ 硬出口。光靠下面 poll 返回 0 不够：deadline 过了但管道里还有数据时，
        //    poll 照样报「可读」，循环能不能结束就取决于「每轮都真的消费掉数据」。
        //    把终止保证挂在循环体的行为上太脆 —— 直接查时间。
        if (wait == 0) return false;
        pollfd pfd{fd, POLLIN, 0};
        const int n = ::poll(&pfd, 1, wait);
        if (n < 0) {
            if (errno == EINTR) continue;   // 信号打断，不是错误
            return false;
        }
        if (n == 0) return false;           // deadline 到了，还没 EOF

        const ssize_t got = ::read(fd, buf, sizeof buf);
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return true;          // EOF

        const auto room = out.size() < limit ? limit - out.size() : 0;
        const auto take = std::min(room, static_cast<std::size_t>(got));
        out.append(buf, take);
        if (take < static_cast<std::size_t>(got)) truncated = true;
    }
}

}  // namespace

ProcessResult run_shell(const std::string& command, const fs::path& cwd,
                        std::chrono::seconds timeout, std::size_t max_output_bytes) {
    const auto started = Clock::now();
    ProcessResult r;
    auto finish = [&r, started](ProcessResult&& out) {
        out.duration = std::chrono::duration_cast<Ms>(Clock::now() - started);
        return out;
    };

    // fork / pipe / setpgid / exec 抽在 spawn.hpp 里 —— BackgroundManager 用同一段。
    // 那三个坑（父进程漏关写端、不 setpgid、fork 后只能调 async-signal-safe）
    // 在一处解决，两边不会漂移。
    const SpawnedProcess sp = spawn_process(command, cwd);
    if (!sp.ok) {
        r.spawn_failed = true;
        r.output = sp.error;
        return finish(std::move(r));
    }
    const pid_t pid = sp.pid;
    const int read_fd = sp.read_fd;

    const auto deadline = started + timeout;
    bool truncated = false;
    const bool eof = drain(read_fd, r.output, max_output_bytes, deadline, truncated);

    if (!eof) {
        // 超时。先 SIGTERM 给它机会自己收尾，再 SIGKILL。
        // 杀的是 **-pgid**（负号 = 整个进程组），不是 pid —— 否则
        // `sleep 100 &` 起的孙子进程会活下来，还攥着管道写端不放。
        r.timed_out = true;
        ::kill(-pid, SIGTERM);
        if (!drain(read_fd, r.output, max_output_bytes, Clock::now() + kGrace, truncated)) {
            ::kill(-pid, SIGKILL);
            drain(read_fd, r.output, max_output_bytes, Clock::now() + kReap, truncated);
        }
    }
    ::close(read_fd);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}   // 收尸，别留僵尸

    if (r.timed_out) {
        r.exit_code = -1;
        r.output += "\n[超时 " + std::to_string(timeout.count()) + "s，进程组已被终止]";
    } else if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);   // shell 的约定
    }

    if (truncated)
        r.output += "\n[输出超过 " + std::to_string(max_output_bytes) + " 字节，已截断]";

    return finish(std::move(r));
}

}  // namespace mini
