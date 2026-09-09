// 起子进程。契约见 src/spawn.hpp。

#include "spawn.hpp"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mini {

SpawnedProcess spawn_process(const std::string& command, const fs::path& cwd) {
    SpawnedProcess out;

    int fds[2];
    if (::pipe(fds) != 0) {
        out.error = std::string("pipe() 失败: ") + std::strerror(errno);
        return out;
    }

    // execl 的参数在 fork 前就取好指针 —— fork 之后不能再碰分配器。
    const char* const cmd_c = command.c_str();
    const char* const cwd_c = cwd.c_str();

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(fds[0]);
        ::close(fds[1]);
        out.error = std::string("fork() 失败: ") + std::strerror(e);
        return out;
    }

    if (pid == 0) {
        // ── 子进程 ───────────────────────────────────────────────────────────
        // 从这里到 execl 只允许 async-signal-safe 调用：不能 new、不能抛、
        // 不能碰 iostream。出错一律 _exit()，不是 exit()（那会跑父进程的
        // atexit 处理器和析构函数）。
        ::setpgid(0, 0);            // 自成进程组，超时/kill 才能杀掉整棵树
        ::close(fds[0]);            // 读端用不着
        if (::dup2(fds[1], STDOUT_FILENO) < 0) ::_exit(126);
        if (::dup2(fds[1], STDERR_FILENO) < 0) ::_exit(126);   // 合并，模型靠它排错
        ::close(fds[1]);
        if (::chdir(cwd_c) != 0) ::_exit(126);
        ::execl("/bin/sh", "sh", "-c", cmd_c, static_cast<char*>(nullptr));
        ::_exit(127);               // execl 只有失败才返回；127 = 沿用 shell 的约定
    }

    // ── 父进程 ───────────────────────────────────────────────────────────────
    ::setpgid(pid, pid);            // 和子进程里那次比赛，谁先成谁的；输的拿 EACCES，无所谓
    // ⚠️ 必须关掉父进程这一侧的写端。留着的话管道永远有一个写者，
    //    read() 等不到 EOF —— 前台命令卡到超时，后台任务永远不算结束。
    ::close(fds[1]);

    out.pid = pid;
    out.read_fd = fds[0];
    out.ok = true;
    return out;
}

}  // namespace mini
