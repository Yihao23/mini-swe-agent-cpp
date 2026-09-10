// 【Stage 6】后台任务。契约写在 include/mini_agent/background.hpp。

#include "mini_agent/background.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <format>
#include <mutex>
#include <thread>

#include "spawn.hpp"

namespace mini {
namespace {

/// 一个任务最多在内存里攒多少输出。
///
/// ⚠️ 必须有上界。一个 watch 进程跑一天能吐几个 GB —— 没有上界的话
///    agent 会被自己的后台任务撑爆内存，而那个任务本身完全正常。
constexpr std::size_t kMaxBuffer = 256u * 1024;

/// SIGTERM 之后给多久收拾，然后 SIGKILL。
constexpr auto kGrace = std::chrono::milliseconds(200);

/// 同时在跑的任务数上限。
///
/// ⚠️ 必须有。一个陷进循环的模型（「启动服务器 → 没反应 → 再启动一个」）
///    能把线程和进程都开爆，而**每一次调用单独看都是合理的** —— 这正是
///    max_steps、kMaxAgentDepth、max_parallel_tools 存在的同一个理由。
constexpr std::size_t kMaxRunning = 8;

/// 已结束的任务保留多少条。
///
/// ⚠️ tasks 只增不减的话，跑一整天会攒下几百条记录，render_list() 越来越长
///    而且大部分是模型早就读完的死任务。留最近几条，够查就行。
constexpr std::size_t kMaxFinished = 8;

}  // namespace

/// 一个后台任务的全部状态。
///
/// ⚠️ 除了 id / label / pid / reader，其余字段都被**两条线程**碰：
///    读线程往 buffer 写、改 finished/exit_code；主线程读 buffer、改 cursor。
///    所以每一次访问都要拿 Impl::mu。
struct BackgroundTask {
    std::string id;
    std::string label;
    std::string command;
    pid_t pid = -1;
    int read_fd = -1;

    std::string buffer;          ///< 已收到的全部输出（截断后是"最近的一段"）
    std::size_t cursor = 0;      ///< 已经交给模型的位置。和 buffer 是一对不变量
    bool truncated = false;      ///< 丢过老内容没有 —— 要告诉模型
    bool finished = false;
    int exit_code = -1;
    bool reported = false;       ///< notifications() 报过没有。⚠️ 只能报一次
    std::chrono::steady_clock::time_point started;

    std::jthread reader;         ///< ⚠️ 析构自动 join。裸 thread 会 std::terminate
};

struct BackgroundManager::Impl {
    std::mutex mu;
    std::map<std::string, BackgroundTask> tasks;
    int counter = 0;

    /// 清掉太老的、已结束的任务。调用方必须已经持有 mu。
    ///
    /// ⚠️ 只清「已结束 **且** 已经通知过」的。没通知过就清掉的话，
    ///    模型永远不知道那个任务结束了 —— 它还在等一条不会来的消息。
    /// ⚠️ 没读完的输出会跟着记录一起消失，所以按 id 顺序保留最近的几条：
    ///    模型刚 start 的那些最可能还要 drain。
    void reap_finished() {
        std::vector<std::string> done;
        for (const auto& [id, t] : tasks)
            if (t.finished && t.reported) done.push_back(id);
        if (done.size() <= kMaxFinished) return;
        // tasks 是 map，但 id 是 "bg_10" 这种字符串 —— 字典序不等于时间序。
        // 按 counter 顺序删最老的，所以拿数字部分排。
        std::ranges::sort(done, {}, [](const std::string& s) {
            return std::atol(s.c_str() + 3);   // 跳过 "bg_"
        });
        for (std::size_t i = 0; i + kMaxFinished < done.size(); ++i) tasks.erase(done[i]);
    }

    /// 读线程的主体：把管道排空，倒进 buffer。
    ///
    /// ⚠️ 这条线程存在的唯一理由是**别让管道满**。管道缓冲区只有 64KB，
    ///    满了子进程就阻塞在 write() 上 —— 一个 dev server 会就此冻住，
    ///    而表现是"网页打不开"，跟缓冲区设计看起来毫无关系。
    void pump(BackgroundTask* t, std::stop_token stop) {
        char buf[4096];
        for (;;) {
            if (stop.stop_requested()) break;
            const ssize_t got = ::read(t->read_fd, buf, sizeof buf);
            if (got < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (got == 0) break;   // EOF：所有写端都关了 → 进程结束了

            std::lock_guard g(mu);
            t->buffer.append(buf, static_cast<std::size_t>(got));

            // ⚠️ 截断时必须同步修正 cursor。不修的话它会大于新的 buffer 长度，
            //    下次 substr(cursor) 直接抛；或者更糟 —— 从错误的位置开始，
            //    重复或跳过一大段输出。
            if (t->buffer.size() > kMaxBuffer) {
                const std::size_t drop = t->buffer.size() - kMaxBuffer / 2;
                t->buffer.erase(0, drop);
                t->cursor = t->cursor > drop ? t->cursor - drop : 0;
                t->truncated = true;
            }
        }

        // 收尸。waitpid 在这条线程上做 —— 主线程不该为了拿退出码而阻塞。
        int status = 0;
        while (::waitpid(t->pid, &status, 0) < 0 && errno == EINTR) {}

        std::lock_guard g(mu);
        t->finished = true;
        if (WIFEXITED(status)) t->exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) t->exit_code = 128 + WTERMSIG(status);

        // ⚠️ 管道读端在这里关，而且**只能**在这里关。
        //
        //   * 只能在这里：这条线程是唯一读它的人。主线程去关的话，我们可能
        //     正卡在 read() 上 —— 而那个 fd 号会被立刻回收给下一个 open()，
        //     于是这条线程读到的是别人的数据。这类 bug 没有任何症状可循。
        //   * 必须在这里：不关就是每起一个后台任务泄漏一个 fd。任务被
        //     reap_finished() 清掉之后，那个 fd 连同它的记录一起消失，
        //     再也没人能关。跑久了会撞上 RLIMIT_NOFILE，而报错发生在
        //     一个完全无关的 open() 上。
        //
        //   置 -1 是为了让 I6 可检验：pid < 0 ⟺ read_fd < 0。
        if (t->read_fd >= 0) {
            ::close(t->read_fd);
            t->read_fd = -1;
        }
    }
};

BackgroundManager::BackgroundManager() : impl_(std::make_unique<Impl>()) {}

BackgroundManager::~BackgroundManager() {
    // ⚠️ 不杀干净的话，agent 退出后留下一堆孤儿：dev server 占着端口、
    //    watcher 占着 inotify 句柄，而没有任何东西知道它们存在。
    kill_all();
    // jthread 的析构会 request_stop + join，所以这里不用手动等。
}

std::string BackgroundManager::start(const std::string& command, const fs::path& cwd,
                                     std::string label) {
    {
        // ⚠️ 名额检查要在 spawn 之前。反过来的话进程已经起来了才发现超额，
        //    要么泄漏一个，要么得把刚起的杀掉 —— 两种都比不起它更糟。
        std::lock_guard lk(impl_->mu);
        impl_->reap_finished();
        std::size_t running = 0;
        for (const auto& [id, t] : impl_->tasks) running += !t.finished;
        if (running >= kMaxRunning) return {};   // 空 id = 满了，调用方给模型一条说明
    }

    const SpawnedProcess sp = spawn_process(command, cwd);

    std::lock_guard lk(impl_->mu);
    const std::string id = "bg_" + std::to_string(++impl_->counter);

    BackgroundTask t;
    t.id = id;
    t.label = label.empty() ? command : std::move(label);
    t.command = command;
    t.started = std::chrono::steady_clock::now();

    if (!sp.ok) {
        // 起不来也建一条记录：模型拿到了 id，得能查到它为什么没起来。
        t.buffer = sp.error;
        t.finished = true;
        t.exit_code = -1;
        impl_->tasks.emplace(id, std::move(t));
        return id;
    }
    t.pid = sp.pid;
    t.read_fd = sp.read_fd;

    auto [it, _] = impl_->tasks.emplace(id, std::move(t));
    BackgroundTask* task = &it->second;

    // ⚠️ 握着锁起线程。pump 第一件事就是拿这把锁，所以它会阻塞到 start() 返回 ——
    //    不会死锁，而且这样 reader 的赋值也在锁里，不会和 kill_all() 的
    //    request_stop() 撞上。放锁之后再赋值就是一个数据竞争，而竞态是概率性的：
    //    本地跑一百次都对，CI 上偶尔挂。
    //
    // ⚠️ 指针 task 指进 map。std::map 的节点在插入别的条目时地址不变，
    //    这是它能被跨线程持有的前提 —— 换成 vector 就是悬垂指针。
    task->reader = std::jthread([this, task](std::stop_token st) { impl_->pump(task, st); });
    return id;
}

std::string BackgroundManager::drain(std::string_view task_id) {
    std::lock_guard g(impl_->mu);
    const auto it = impl_->tasks.find(std::string(task_id));
    if (it == impl_->tasks.end()) return {};
    BackgroundTask& t = it->second;

    // ⚠️ 取内容和推进 cursor 必须在同一把锁里。分开的话，两步之间读线程
    //    又追加了内容，那段会被跳过 —— 永远不会交给模型。
    std::string out = t.buffer.substr(t.cursor);
    t.cursor = t.buffer.size();

    if (t.truncated && !out.empty()) {
        out = "[更早的输出已丢弃]\n" + out;
        t.truncated = false;   // 只说一次
    }
    return out;
}

std::string BackgroundManager::render_list() const {
    std::lock_guard g(impl_->mu);
    if (impl_->tasks.empty()) return {};

    std::string out = "后台任务:\n";
    for (const auto& [id, t] : impl_->tasks) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - t.started).count();
        const std::size_t pending = t.buffer.size() - t.cursor;
        out += std::format("  {} [{}] {} —— 跑了 {}s，{} 字节没读\n", id,
                           t.finished ? std::format("已结束 exit={}", t.exit_code)
                                      : std::string("运行中"),
                           t.label, secs, pending);
    }
    return out;
}

void BackgroundManager::kill(std::string_view task_id) {
    std::lock_guard g(impl_->mu);
    const auto it = impl_->tasks.find(std::string(task_id));
    if (it == impl_->tasks.end() || it->second.finished || it->second.pid <= 0) return;

    // ⚠️ 杀进程**组**（负号），不是 pid。`npm run dev` 会 fork 出一堆子进程，
    //    只杀它自己的话那些孙子进程会活下来，还攥着管道写端 ——
    //    读线程永远等不到 EOF。和 run_shell 同一个坑。
    ::kill(-it->second.pid, SIGTERM);
    it->second.reader.request_stop();
}

void BackgroundManager::kill_all() {
    std::vector<pid_t> pids;
    {
        std::lock_guard g(impl_->mu);
        for (auto& [id, t] : impl_->tasks) {
            if (!t.finished && t.pid > 0) pids.push_back(t.pid);
            t.reader.request_stop();
        }
    }
    // ⚠️ 放锁之后再等。读线程要拿这把锁才能收尾，握着锁去 sleep 就死锁了。
    for (const pid_t p : pids) ::kill(-p, SIGTERM);
    if (!pids.empty()) std::this_thread::sleep_for(kGrace);
    for (const pid_t p : pids) ::kill(-p, SIGKILL);   // 已经死了的话是 ESRCH，无所谓
}

std::vector<std::string> BackgroundManager::notifications() {
    std::lock_guard g(impl_->mu);
    std::vector<std::string> out;
    for (auto& [id, t] : impl_->tasks) {
        if (!t.finished || t.reported) continue;

        // ⚠️ 就地置位，不能等调用方回来说"我报过了"—— 那是个会被忘掉的约定。
        //    报两次的话模型会以为那条命令跑了两遍，然后据此行动：撤销它，
        //    或者再做一遍。
        t.reported = true;
        const std::size_t pending = t.buffer.size() - t.cursor;
        out.push_back(std::format("后台任务 {} ({}) 已结束，退出码 {}。{}", id, t.label,
                                  t.exit_code,
                                  pending ? std::format("有 {} 字节输出没读，用 bash_output 取。",
                                                        pending)
                                          : std::string("没有新输出。")));
    }
    return out;
}

}  // namespace mini
