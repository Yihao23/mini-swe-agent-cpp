// 【Stage 2】带超时的子进程执行。整个项目最"系统编程"的一块。
//
// 契约写在 include/mini_agent/process.hpp。这里只讲实现上的取舍。

#include "mini_agent/process.hpp"

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

/// 距离 deadline 还剩多少毫秒，已经过了就是 0。
int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<Ms>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

/// 把 fd 读到 EOF 或 deadline 为止，最多留 limit 字节。
///
/// 超出 limit 之后**继续读、只是丢掉** —— 停下不读的话，子进程会阻塞在
/// write() 上永远不退出，超时机制反而被自己的截断逻辑触发。
/// @return true = 读到了 EOF（子进程那侧全部关闭了写端）
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

    int fds[2];
    if (::pipe(fds) != 0) {
        r.spawn_failed = true;
        r.output = std::string("pipe() 失败: ") + std::strerror(errno);
        return finish(std::move(r));
    }

    // execl 的参数在 fork 前就取好指针 —— fork 之后不能再碰分配器。
    const char* const cmd_c = command.c_str();
    const char* const cwd_c = cwd.c_str();

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        r.spawn_failed = true;
        r.output = std::string("fork() 失败: ") + std::strerror(e);
        return finish(std::move(r));
    }

    if (pid == 0) {
        // ── 子进程 ───────────────────────────────────────────────────────────
        // 从这里到 execl 只允许 async-signal-safe 调用：不能 new、不能抛、
        // 不能碰 iostream。出错一律 _exit()，不是 exit()（那会跑父进程的
        // atexit 处理器和析构函数）。
        ::setpgid(0, 0);            // 自成进程组，超时才能杀掉整棵树
        ::close(fds[0]);            // 读端用不着
        if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(126);
        if (::dup2(fds[1], STDERR_FILENO) < 0) ::_exit(126);   // 合并，模型要靠它排错
        ::close(fds[1]);
        if (::chdir(cwd_c) != 0) ::_exit(126);
        ::execl("/bin/sh", "sh", "-c", cmd_c, static_cast<char*>(nullptr));
        ::_exit(127);               // execl 只有失败才返回；127 = 沿用 shell 的约定
    }

    // ── 父进程 ───────────────────────────────────────────────────────────────
    ::setpgid(pid, pid);            // 和子进程里那次比赛，谁先成谁的；输的那个
                                    // 拿 EACCES/ESRCH，无所谓。两边都做才没有窗口期。
    // ⚠️ 必须关掉父进程这一侧的写端。留着的话管道永远有一个写者，
    //    read() 等不到 EOF，命令早退出了也要卡到超时。
    ::close(fds[1]);

    const auto deadline = started + timeout;
    bool truncated = false;
    const bool eof = drain(fds[0], r.output, max_output_bytes, deadline, truncated);

    if (!eof) {
        // 超时。先 SIGTERM 给它机会自己收尾，再 SIGKILL。
        // 杀的是 **-pgid**（负号 = 整个进程组），不是 pid —— 否则
        // `sleep 100 &` 起的孙子进程会活下来，还攥着管道写端不放。
        r.timed_out = true;
        ::kill(-pid, SIGTERM);
        if (!drain(fds[0], r.output, max_output_bytes, Clock::now() + kGrace, truncated)) {
            ::kill(-pid, SIGKILL);
            drain(fds[0], r.output, max_output_bytes, Clock::now() + kReap, truncated);
        }
    }
    ::close(fds[0]);

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
