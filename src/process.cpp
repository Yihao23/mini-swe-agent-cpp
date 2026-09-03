// 【Stage 2】带超时的子进程执行。整个项目最"系统编程"的一块。
//
// 步骤见 process.hpp 的注释。三个必踩的坑：
//   * 不 close 父进程里的写端 → read 永远等不到 EOF
//   * 不 setpgid → 杀不掉整棵进程树（`sleep 100 &` 的孙子进程会活下来）
//   * fork 之后 exec 之前只能调 async-signal-safe 的函数

#include "mini_agent/process.hpp"

#include "mini_agent/json.hpp"

namespace mini {

ProcessResult run_shell(const std::string&, const fs::path&, std::chrono::seconds, std::size_t) {
    todo("Stage 2: run_shell —— pipe/fork/setpgid/execl + poll 超时 + 杀进程组");
}

}  // namespace mini
