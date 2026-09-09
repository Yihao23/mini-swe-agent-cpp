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

}  // namespace mini
