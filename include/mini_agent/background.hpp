#pragma once
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

class BackgroundManager {
  public:
    BackgroundManager();
    ~BackgroundManager();                    // 析构要杀干净所有子进程

    BackgroundManager(const BackgroundManager&) = delete;
    BackgroundManager& operator=(const BackgroundManager&) = delete;

    /// 返回 task_id（"bg_1"、"bg_2"…）
    std::string start(const std::string& command, const fs::path& cwd, std::string label);

    /// 上次 drain 之后的新增输出，并推进游标
    std::string drain(std::string_view task_id);
    std::string render_list() const;

    void kill(std::string_view task_id);
    void kill_all();

    /// 主循环每轮开头调。已结束但没上报过的任务 → 一条通知。
    /// ⚠️ 每个任务只能通知**一次**，否则模型会以为跑了两遍。
    std::vector<std::string> notifications();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mini
