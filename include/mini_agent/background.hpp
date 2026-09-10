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
// ── 全景：三条线，一把锁 ────────────────────────────────────────────────────
//
//   记号（沿用 UML 时序图，图例见 executor.hpp）：
//     ──▶ 同步，调用方阻塞    ──▷ 异步，调用方**不**等    ═══ 长期存在的线程
//
//                    主线程                            读线程（每任务一条）
//                    ──────                            ────────────────────
//   bash(run_in_
//   background)  ──▶ start()
//                      │ spawn_process()  ──▶ fork/pipe/setpgid/exec
//                      │                          │
//                      │                          └──▷ 子进程（独立进程组）
//                      │                                    │ 写 stdout/stderr
//                      │ 建 jthread ──────────────▷ ═══ pump(t, stop) ◀───┘
//                      │                                ┌── for(;;) ──────────┐
//                      ▼ 立刻返回 "bg_1"                 │ poll(read_fd, 100ms)│
//                                                       │ read() → 追加 buffer│
//   bash_output  ──▶ drain(id)  ──┐                     │ 超上界 → 丢一半      │
//   每轮开头     ──▶ notifications()│  ┌── mutex_ ───┐   │ EOF → waitpid, 标完成│
//   kill_task    ──▶ kill(id)      ├─▶│ buffer      │◀──┤ 收到 stop → 退出     │
//   ~Manager     ──▶ kill_all()   ─┘  │ cursor      │   └─────────────────────┘
//                                     │ finished    │
//                                     │ exit_code   │
//                                     │ reported    │
//                                     └─────────────┘
//
//   ⚠️ 主线程**从不**碰 read_fd，读线程**从不**碰 tasks_ 的增删。
//      锁只保护那五个字段 —— 它们才是两条线真正共享的东西。
//
// ── buffer 和 cursor 是**一对**，不是两个字段 ───────────────────────────────
//
//   buffer   ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░
//            └── 已交给模型 ──┘└─ 新的 ─┘
//                             ▲
//                           cursor
//
//   I1  0 ≤ cursor ≤ buffer.size()          ← 越界 substr 直接抛
//   I2  drain() 返回 buffer[cursor..]，然后 cursor = buffer.size()
//   I3  截断丢掉前 drop 字节 ⇒ cursor 同步减 drop（不够减就归零）
//
//   ⚠️ I3 少一句，I1 就破了。而且它**只在截断和 drain 交错时**才现形 ——
//      一次性吐完 1.4MB 的命令，所有截断都发生在第一次 drain 之前，cursor
//      全程是 0，substr(0) 照样有效。变异测试抓到过两版都是绿的假断言。
//
// ── 通知只发一次 ────────────────────────────────────────────────────────────
//
//   I4  reported 一旦为 true 就不再变回去
//   I5  finished == false 的任务，reported 必然 false
//
//   notifications() 是**有副作用的读**：它就地把 reported 置真。所以每轮
//   只能调一次，且调了就必须把结果用掉 —— 丢掉的话那条消息永远不会再来，
//   模型会一直等一个不存在的信号。
//
//   反过来重复发也不行：模型会认为命令跑了两遍，然后照着这个去撤销或重做。
//
// ── pid / read_fd / reader 是同一份资源的三个把手 ───────────────────────────
//
//        spawn_process()          建 jthread              pump 收尾
//             │                        │                      │
//   pid   ────┼──▶ ≥0 ────────────────┼──────────────────────┼──▶ 保留（供查阅）
//   read_fd ──┼──▶ ≥0 ────────────────┼──────────────────────┼──▶ close，置 -1
//   reader  ──┼────────────────▶ 在跑 ─┼──────────────────────┼──▶ 自然退出
//             │                        │                      │
//                                                         waitpid 之后
//
//   I6  三者同生：spawn 成功 ⟹ pid ≥ 0 且 read_fd ≥ 0 且 reader 在跑；
//       spawn 失败 ⟹ 三者都是"空"，任务直接标 finished
//   I7  read_fd 只由**读线程自己**关，而且是在它不再 read 之后。
//
//   ⚠️ I7 两个方向都会出事：
//
//     不关       每起一个任务泄漏一个 fd。任务被 reap_finished() 清掉之后，
//                fd 连同记录一起消失，再也没人能关 —— 跑久了撞上
//                RLIMIT_NOFILE，而报错发生在一个毫不相干的 open() 上。
//
//     别人关     读线程可能正卡在 read() 上。那个 fd 号会被立刻回收给下一个
//                open()，于是它读到的是**别人的数据**。这类 bug 没有症状，
//                只有莫名其妙的坏数据。
//
//   所以 close 写在 pump() 的收尾里，紧跟着 waitpid —— 那里既保证不会再读，
//   又保证还在读线程自己手上。
//
// ── 剩下四个字段不属于任何关系型不变量，这没问题 ────────────────────────────
//
//   id       其实有一条：它必须等于 tasks_ 里的那个 key（冗余存储的一致性）
//   label    ┐
//   command  ├ 纯展示数据 —— render_list() 和出错信息给人看的
//   started  ┘
//
//   ⚠️ 「每个字段都该属于至少一条不变量」这条规则要求的是**关系型**的
//      （牵涉两个以上字段，或字段与外部世界）。不加这个限定的话，任何字段
//      都能编一条"它是个字符串"来满足它，规则就永远为真、也永远没有信息。
//      承认"这只是被携带的数据"，比编一条弱不变量诚实。
//
// ── 两条上界，防的是同一种事 ────────────────────────────────────────────────
//
//   kMaxBuffer   256KB/任务   一个 watch 跑一天能吃光内存
//   kMaxRunning  8 个任务     陷进循环的模型能把线程和进程都开爆，
//                             而每一次调用单独看都是合理的
//   kMaxFinished 8 条记录     清理时 ⚠️ **跳过还没通知过的** —— 那条通知是
//                             模型能得到的唯一证据（I5 的另一半）
//
// ── C++ 的并发要点 ─────────────────────────────────────────────────────────
//   * 用 std::jthread：析构自动 join，还自带 stop_token。用裸 std::thread 的话，
//     manager 析构时线程还在跑 → std::terminate。
//   * 缓冲区被读线程写、被主线程读 → 必须加锁（mutex + 一次拷贝，别炫技做无锁）。
//   * 任务存在 std::map 里 —— 读线程捕获的是节点指针，而 map 的节点地址是稳定的。
//     换成 vector 的话一次扩容就让所有读线程的指针悬垂。
//   * 信号发给**进程组**（kill(-pid)）。`npm run dev` 起的孙子进程会攥着管道
//     写端不放 —— 只杀直接子进程，任务既不结束、管道也不关，读线程永远等不到 EOF。
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
