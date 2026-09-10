#pragma once
//
// 【Stage 2 / 6】起一个子进程，把它的 stdout+stderr 接到一根管道上。
//
// 这是**内部头文件**（在 src/ 下，不在 include/），不算公开 API。
//
// run_shell()（阻塞，读到 EOF 或超时）和 BackgroundManager::start()（异步，
// 交给读线程）都要做同样的 fork/pipe/setpgid/exec，只是之后的处理不同。
// 抽出来是因为那段里有三个必踩的坑，复制两份的话两边会各自漂移 ——
// 而漂移的表现是「前台命令好好的，后台任务偶尔卡死」，两处代码看起来都对。
//
#include <sys/types.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mini {

namespace fs = std::filesystem;

/// 起好的子进程。
struct SpawnedProcess {
    pid_t pid = -1;      ///< 也是进程组 id —— 子进程 setpgid(0,0) 自成一组
    int read_fd = -1;    ///< 管道读端。调用方负责 close
    bool ok = false;     ///< false 时 error 里有原因，pid/read_fd 无效
    std::string error;
};

/// fork + exec 一条 shell 命令，stdout 和 stderr 都接到返回的 read_fd 上。
///
/// ⚠️ 三个必踩的坑，都在实现里处理了：
///   1. 父进程要 close 自己那份**写端**。不关的话管道永远有一个写者，
///      read() 等不到 EOF —— 前台命令会卡到超时，后台任务永远不算结束。
///   2. 子进程要 setpgid(0,0) 自成进程组。不然 `sleep 30 &` 起的孙子进程
///      杀不掉，还攥着管道写端不放。
///   3. fork 之后 exec 之前只能调 async-signal-safe 的函数 —— 不能 new、
///      不能抛、不能碰 iostream。所以 c_str() 在 fork 之前就取好。
///
/// **不抛异常** —— 失败反映在 ok/error 里。
SpawnedProcess spawn_process(const std::string& command, const fs::path& cwd);

/// 起好的双向子进程。
struct PipedProcess {
    pid_t pid = -1;      ///< 也是进程组 id
    int write_fd = -1;   ///< 写这里 → 子进程的 stdin。调用方负责 close
    int read_fd = -1;    ///< 读这里 ← 子进程的 stdout。调用方负责 close
    bool ok = false;
    std::string error;
};

/// fork + execvp 一个程序，stdin 和 stdout 各接一根管道。
///
/// 和 spawn_process 的三处不同，每一处都是 MCP 逼出来的：
///
///   1. **两根管道**，父进程既能写也能读 —— JSON-RPC 是一问一答。
///   2. **stderr 不合并**，原样继承父进程的。MCP server 往 stderr 打日志，
///      合进 stdout 就等于往 JSON-RPC 流里掺垃圾，下一次 getline 拿到的是
///      一行日志而不是一条消息。⚠️ 这是这个函数存在的**主要理由**。
///   3. **execvp 而不是 sh -c**。配置给的是 command + args 数组，直接 exec
///      省掉一层 shell，也就没有引号和 $ 展开的问题 —— 参数里带空格的路径
///      不会被拆开。
///
/// ⚠️ spawn_process 的三个坑这里一个不少：父进程要关掉两根管道各自的另一端、
///    子进程要 setpgid(0,0)、fork 到 exec 之间只能调 async-signal-safe 的东西
///    （所以 argv 在 fork 之前就搭好）。
///
/// **不抛异常** —— 失败反映在 ok/error 里。
PipedProcess spawn_piped(const std::string& command, const std::vector<std::string>& args,
                         const fs::path& cwd);

}  // namespace mini
