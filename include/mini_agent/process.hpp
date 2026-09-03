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

struct ProcessResult {
    int exit_code = -1;
    std::string output;     // stdout + stderr 合并（模型要靠它排错，别分开）
    bool timed_out = false;
    bool spawn_failed = false;
    std::chrono::milliseconds duration{0};
};

/// 跑一条 shell 命令，最多等 timeout。
/// **不抛异常** —— 任何失败都反映在返回值里（这是 agent 的错误哲学：失败是值）。
/// TODO(Stage 2)
ProcessResult run_shell(const std::string& command,
                        const fs::path& cwd,
                        std::chrono::seconds timeout,
                        std::size_t max_output_bytes = 1u << 20);

}  // namespace mini
