// 【Stage 6】任务调度。注意：这一层**不认识 LLM**，测试塞 lambda 就能跑。
#include "mini_agent/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>

#include "mini_agent/json.hpp"

namespace mini {

std::string_view to_string(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending: return "pending";
        case TaskStatus::Running: return "running";
        case TaskStatus::Done:    return "done";
        case TaskStatus::Failed:  return "failed";
        case TaskStatus::Blocked: return "blocked";
    }
    return "pending";   // 不可达；没有它编译器警告 control reaches end
}

Scheduler::Scheduler(unsigned max_workers)
    : max_workers_(max_workers == 0 ? 1u : max_workers) {}

Task& Scheduler::add(std::string id, std::string prompt, std::vector<std::string> deps,
                     int priority, std::string agent_type) {
    // id 重复是**调用方的编程错误**，不是模型给的数据错误 —— 后者由 validate()
    // 以返回值报告。这里抛异常：重复 id 会让依赖指向哪一个任务变得不确定，
    // 悄悄覆盖或忽略都会产出一张与调用方意图不符的图。
    if (tasks_.contains(id)) throw std::invalid_argument("任务 id 重复: " + id);

    Task& t = tasks_[id];
    t.id = std::move(id);
    t.prompt = std::move(prompt);
    t.deps = std::move(deps);
    t.priority = priority;
    t.agent_type = std::move(agent_type);
    t.seq = counter_++;   // 入图顺序，同优先级时的稳定排序键
    return t;
}

std::optional<std::string> Scheduler::validate() const {
    // ① 依赖必须存在。悬空依赖会让那个任务永远等一个不会完成的上游。
    for (const auto& [id, t] : tasks_)
        for (const auto& d : t.deps)
            if (!tasks_.contains(d))
                return "任务 \"" + id + "\" 依赖了不存在的任务 \"" + d + "\"";

    // ② 无环。DFS 三色标记：白=没访问过，灰=在当前递归路径上，黑=子树已走完。
    //    碰到灰色就是环 —— 沿着当前路径回溯，把环的完整路径报出来，
    //    否则用户只知道"有环"，在几十个任务里根本找不到是哪几个。
    enum class Color { White, Gray, Black };
    std::map<std::string, Color> color;
    for (const auto& [id, t] : tasks_) color[id] = Color::White;

    std::vector<std::string> path;
    std::function<std::optional<std::string>(const std::string&)> dfs =
        [&](const std::string& id) -> std::optional<std::string> {
        color[id] = Color::Gray;
        path.push_back(id);

        for (const auto& d : tasks_.at(id).deps) {
            if (color[d] == Color::Gray) {
                const auto start = std::ranges::find(path, d);
                std::string cycle;
                for (auto it = start; it != path.end(); ++it) cycle += *it + " → ";
                return "依赖成环: " + cycle + d;
            }
            if (color[d] == Color::White)
                if (auto err = dfs(d)) return err;
        }

        path.pop_back();
        color[id] = Color::Black;
        return std::nullopt;
    };

    for (const auto& [id, t] : tasks_)
        if (color[id] == Color::White)
            if (auto err = dfs(id)) return err;

    return std::nullopt;
}

std::vector<Task*> Scheduler::ready() {
    std::vector<Task*> out;

    for (auto& [id, t] : tasks_) {
        if (t.status != TaskStatus::Pending) continue;

        bool blocked = false;
        bool all_done = true;
        for (const auto& d : t.deps) {
            const auto it = tasks_.find(d);
            if (it == tasks_.end()) { blocked = true; break; }   // validate 该先拦住
            const TaskStatus s = it->second.status;
            if (s == TaskStatus::Failed || s == TaskStatus::Blocked) { blocked = true; break; }
            if (s != TaskStatus::Done) all_done = false;
        }

        // 上游失败 → 这个任务永远等不到，标 Blocked 而不是让它挂着。
        // 一处失败不该拖垮整张图：不依赖它的分支照常跑完。
        if (blocked) {
            t.status = TaskStatus::Blocked;
            t.error = "上游任务失败或被阻塞";
            continue;
        }
        if (all_done) out.push_back(&t);
    }

    // 高优先级先出队；同优先级按入图顺序 —— seq 让调度结果可复现，
    // 否则 map 的遍历顺序（按 id 字典序）会决定谁先跑。
    std::ranges::sort(out, [](const Task* a, const Task* b) {
        if (a->priority != b->priority) return a->priority > b->priority;
        return a->seq < b->seq;
    });
    return out;
}

void Scheduler::run(const TaskRunner& runner, const TaskEventSink& on_event) {
    struct Slot {
        Task* task;
        std::future<std::string> fut;
    };
    std::vector<Slot> running;

    auto emit = [&on_event](std::string_view kind, const Task& t) {
        if (on_event) on_event(kind, t);
    };

    for (;;) {
        // 有空位就填：ready() 已按 (priority 降序, seq 升序) 排好
        for (Task* t : ready()) {
            if (running.size() >= max_workers_) break;

            // 上游结果在启动前收集 —— 此刻它们都已 Done，之后不会再变
            std::map<std::string, std::string> upstream;
            for (const auto& d : t->deps) upstream[d] = tasks_.at(d).result;

            t->status = TaskStatus::Running;
            emit("start", *t);

            // ⚠️ 必须显式写 std::launch::async。默认策略允许 deferred，
            //    那样任务只在 .get() 时才跑 —— 看起来在并发，实际串行。
            running.push_back(
                {t, std::async(std::launch::async,
                               [&runner, t, up = std::move(upstream)] { return runner(*t, up); })});
        }

        if (running.empty()) break;   // 没有在跑的，也没有能启动的 → 整张图跑完了

        // 等任意一个完成。标准库没有 wait_any，用 wait_for(0) 轮询 + 短暂让出。
        for (;;) {
            bool collected = false;
            for (std::size_t i = 0; i < running.size(); ++i) {
                if (running[i].fut.wait_for(std::chrono::milliseconds(0)) !=
                    std::future_status::ready)
                    continue;

                Task* t = running[i].task;
                try {
                    t->result = running[i].fut.get();
                    t->status = TaskStatus::Done;
                    emit("done", *t);
                } catch (const std::exception& e) {
                    // 一个任务抛异常不该终止整张图 —— 标 Failed，下游会被标 Blocked
                    t->error = e.what();
                    t->status = TaskStatus::Failed;
                    emit("failed", *t);
                } catch (...) {
                    t->error = "未知异常";
                    t->status = TaskStatus::Failed;
                    emit("failed", *t);
                }
                running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
                collected = true;
                break;
            }
            if (collected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

std::string Scheduler::render() const {
    std::vector<const Task*> sorted;
    sorted.reserve(tasks_.size());
    for (const auto& [id, t] : tasks_) sorted.push_back(&t);
    std::ranges::sort(sorted, {}, [](const Task* t) { return t->seq; });

    std::string out;
    for (const Task* t : sorted) {
        out += std::format("[{:>7}] {:<12} {}", to_string(t->status), t->id, t->prompt);
        if (!t->deps.empty()) {
            out += "  ← ";
            for (std::size_t i = 0; i < t->deps.size(); ++i)
                out += (i ? ", " : "") + t->deps[i];
        }
        if (!t->error.empty()) out += "  [" + t->error + "]";
        out += "\n";
    }
    return out;
}

}  // namespace mini
