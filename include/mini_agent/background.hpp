#pragma once
/// @file
/// @brief Long-running commands that must not block the main loop (Stage 6).
//
// 【Stage 6】后台任务 —— 不阻塞主循环的长命令（dev server、watch、跑全套测试）。
//
// 关键设计：agent 循环是同步的，但世界不是。
// 后台进程的输出由**读取线程**收进缓冲区，主循环在每轮开始前抽取状态变化
// （"某某任务结束了"），作为 <system-reminder> 注入下一轮。
// 于是 agent 不需要轮询，事件会自己找上门。
//
// ── C++ 的并发要点 ─────────────────────────────────────────────────────────
//   * 用 std::jthread：析构自动 join，还自带 stop_token。用裸 std::thread 的话，
//     manager 析构时线程还在跑 → std::terminate。
//   * 缓冲区被读线程写、被主线程读 → 必须加锁（mutex + 一次拷贝，别炫技做无锁）。
//   * cursor（已交给模型的位置）也在锁里 —— 它和 buffer 是一对不变量。
//   * 缓冲区要有上界。一个 watch 进程跑一天能吃光内存；超了丢最老的一半，
//     记得同步修正 cursor，否则会重复输出或跳过内容。
//
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mini {

namespace fs = std::filesystem;

struct BackgroundTask;   // 实现细节关在 .cpp 里（含 pid、jthread、mutex）

/// @brief Long-running commands that must not block the agent loop.
///
/// A dev server, a file watcher, a full test suite — things that run for
/// minutes while the agent gets on with something else. bash would hold the
/// turn hostage for the whole duration.
///
/// @warning Owns child processes. The destructor kills them; without that, a
///          crashed agent leaves servers holding ports and watchers holding
///          inotify handles, and nothing else knows they exist.
///
/// @note pimpl, because the implementation carries pids, jthreads and a mutex,
///       and none of that belongs in a header every layer includes.
/// @note Non-copyable: two managers owning one pid would each try to kill it.
class BackgroundManager {
  public:
    BackgroundManager();

    /// @brief Kills every task still running.
    /// @warning Not optional. A leaked child holds a port or a file handle and
    ///          outlives the process that could have told you about it.
    ~BackgroundManager();

    BackgroundManager(const BackgroundManager&) = delete;
    BackgroundManager& operator=(const BackgroundManager&) = delete;

    /// @brief Start a command in the background.
    ///
    /// @param command The shell command.
    /// @param cwd     Working directory.
    /// @param label   Short name shown in listings.
    /// @return A task id: "bg_1", "bg_2", ... — how everything else refers to it.
    std::string start(const std::string& command, const fs::path& cwd, std::string label);

    /// @brief Output produced since the last drain, advancing the cursor.
    ///
    /// @param task_id From start().
    /// @return New output only; empty when nothing arrived.
    ///
    /// @note The cursor is what keeps this from re-reporting output the model
    ///       has already read. Returning everything each time would refill the
    ///       context with the same lines every turn.
    std::string drain(std::string_view task_id);

    /// @brief A human-readable list of tasks and their state.
    /// @return One line per task; empty when there are none.
    std::string render_list() const;

    /// @brief Terminate one task and its process group.
    /// @param task_id From start(). Unknown ids are ignored.
    void kill(std::string_view task_id);

    /// @brief Terminate every task. Called by the destructor.
    void kill_all();

    /// @brief Tasks that finished and have not been reported yet.
    ///
    /// @return One message per newly finished task; empty on a quiet turn.
    ///
    /// @warning ⚠️ A task is reported **once**. Report it twice and the model
    ///          concludes the command ran twice — and it will act on that,
    ///          undoing work or duplicating it.
    ///
    /// @note Called at the top of each turn, and the result goes through
    ///       turn_context() into a `<system-reminder>` — never into the system
    ///       prompt, which has to stay byte-stable.
    std::vector<std::string> notifications();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mini
