#pragma once
//
// 【Stage 2】带超时的子进程执行 —— Python 一行 subprocess.run(timeout=) 的东西，
// C++ 要你自己写。这是整个项目里最"系统编程"的一块，也是最值得亲手写一遍的。
//
// ── 为什么不能用 popen() ────────────────────────────────────────────────────
//   * 拿不到 pid → 超时了没法 kill
//   * 只能单向拿 stdout（stderr 得靠 shell 重定向）
//   * pclose() 的返回值要用 WEXITSTATUS 解，还容易漏掉信号退出的情况
//
// ── 该怎么做 ────────────────────────────────────────────────────────────────
//   1. pipe()           建管道
//   2. fork()           子进程：dup2 把 stdout/stderr 都接到管道写端，
//                                 close 掉不用的 fd，setpgid() 自成进程组，
//                                 execl("/bin/sh", "sh", "-c", cmd, nullptr)
//   3. 父进程：close 写端，poll() 读端 + 计算剩余超时
//   4. 超时：kill(-pgid, SIGTERM) → 等一小会儿 → SIGKILL
//      （杀**进程组**，不是单个 pid —— 否则 `sleep 100 &` 起的孙子进程会活下来）
//   5. waitpid() 收尸，WIFEXITED/WEXITSTATUS 取退出码
//
// ── 三个必踩的坑 ────────────────────────────────────────────────────────────
//   * 不 close 父进程里的写端 → read 永远等不到 EOF，超时机制形同虚设
//   * 不 setpgid → 杀不掉整棵进程树
//   * fork 之后、exec 之前只能调 async-signal-safe 的函数（别在那儿 new / 打日志）
//
#include <chrono>
#include <filesystem>
#include <string>

namespace mini {

namespace fs = std::filesystem;

/// @brief What one command run produced.
///
/// @note Four ways a run can go wrong and they are kept apart on purpose:
///       a non-zero `exit_code` (the command ran and failed), `timed_out`
///       (it never finished), `spawn_failed` (pipe or fork failed, so nothing
///       ran at all), and exit_code 126/127 (the child got as far as chdir or
///       exec and no further). The model picks a different next move for each.
struct ProcessResult {
    /// @brief The command's exit status; -1 when it was killed for timing out.
    /// @note A signal death is reported as 128 + signum, the shell convention,
    ///       so `kill -9` shows up as 137 rather than as an ordinary failure.
    int exit_code = -1;

    /// @brief stdout and stderr, interleaved as they were produced.
    /// @note Merged, not two fields. A compiler error is on stderr and the line
    ///       it refers to is on stdout; split apart, the ordering that makes
    ///       them readable is lost.
    /// @note Truncated at max_output_bytes, with a note appended saying so.
    std::string output;

    bool timed_out = false;      ///< Killed at the deadline rather than exiting.
    bool spawn_failed = false;   ///< pipe() or fork() failed; the command never ran.
    std::chrono::milliseconds duration{0};   ///< Wall clock, start to reap.
};

/// @brief Run one shell command and collect its output, with a deadline.
///
/// @param command          Passed to `/bin/sh -c`, so pipes, globs and `&&`
///                         all work — and so does everything else a shell can
///                         do. The caller is responsible for having vetted it.
/// @param cwd              Working directory for the command.
/// @param timeout          How long to wait before killing the process group.
/// @param max_output_bytes Keep at most this much output; the rest is read and
///                         discarded so the child never blocks writing.
/// @return What happened. See ProcessResult for how the failure modes differ.
///
/// @warning **Never throws.** Every failure is a value — a tool that cannot run
///          one command must hand the model something it can work around, not
///          end the run. This is the same rule as ToolResult::is_error.
/// @warning The command is not sanitised here. Permission checks belong to
///          Sandbox::check_command, which the executor calls first.
///
/// @note The whole process **group** is killed on a timeout, not just the
///       child. `sleep 30 & echo done` exits the shell at once while the
///       grandchild keeps running — and keeps the pipe open, so the parent
///       would never see EOF. SIGTERM, 200ms of grace, then SIGKILL.
/// @note Output past max_output_bytes is still read, just dropped. Stopping the
///       reads leaves the child blocked in write() while the parent waits for
///       EOF: the truncation logic would deadlock the thing it protects.
/// @note The beginning of the output is what survives truncation. What a
///       command is doing is stated at the top; the tail is usually repetition.
///
/// @code{.test}
/// @setup const auto wd = doc_workdir();
/// run_shell("echo hello", wd, 5s).output        ==> "hello\n"
/// run_shell("echo hello", wd, 5s).exit_code     ==> 0
/// run_shell("exit 42", wd, 5s).exit_code        ==> 42
/// run_shell("no_such_cmd_xyz", wd, 5s).exit_code ==> 127
/// run_shell("kill -TERM $$", wd, 5s).exit_code  ==> 128 + SIGTERM
/// run_shell("echo out; echo err >&2", wd, 5s).output ==> "out\nerr\n"
/// run_shell("pwd", wd, 5s).output               ==> wd.string() + "\n"
/// @endcode
///
/// A command that outruns its deadline, and one that outruns its output budget:
///
/// @code{.test}
/// @setup const auto wd = doc_workdir();
/// @setup const auto slow = run_shell("echo before; sleep 30", wd, 1s);
/// slow.timed_out                                ==> true
/// (slow.output.find("before") != std::string::npos)  ==> true
/// (slow.duration < 3000ms)                      ==> true
/// @setup const auto big = run_shell("echo HEAD; yes filler | head -100000", wd, 5s, 512);
/// big.timed_out                                 ==> false
/// big.exit_code                                 ==> 0
/// (big.output.rfind("HEAD", 0) == 0)            ==> true
/// (big.output.size() < 4096)                    ==> true
/// @endcode
ProcessResult run_shell(const std::string& command,
                        const fs::path& cwd,
                        std::chrono::seconds timeout,
                        std::size_t max_output_bytes = 1u << 20);

}  // namespace mini
