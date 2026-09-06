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

/// @brief A task status as text, for render() and the event sink.
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

/// @brief Create a scheduler.
/// @param max_workers Concurrent task limit; 0 is clamped to 1 so the run loop
///        always makes progress.
Scheduler::Scheduler(unsigned max_workers)
    : max_workers_(max_workers == 0 ? 1u : max_workers) {}

/// @brief Add a node to the graph.
///
/// @param id         Unique within this graph.
/// @param prompt     What the task should do.
/// @param deps       Ids that must finish first.
/// @param priority   Higher goes first among ready tasks.
/// @param agent_type Which sub-agent profile to use.
/// @return Reference to the stored task.
/// @throws std::invalid_argument on a duplicate id.
///
/// @note Throwing here while validate() returns its errors is a deliberate
///       split: a duplicate id is the caller's bug — it leaves "which task does
///       this dependency point at" undecidable, and both overwriting and
///       ignoring would build a graph the caller did not ask for. A malformed
///       graph, by contrast, comes from the model and is routine.
/// @note `seq` records insertion order and breaks priority ties. Without it,
///       ties fall to the map's iteration order — the id's lexicographic order —
///       so naming tasks a/b instead of task1/task2 would change what runs
///       first, and nothing would be reproducible.
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

/// @brief Check the graph before running it.
///
/// @return nullopt when runnable, otherwise a description of what is wrong.
///
/// @warning Skipping this on a cyclic graph is not loud. ready() only picks
///          tasks whose dependencies are Done, so in an a→b→a cycle neither is
///          ever eligible, run() finds nothing to start and returns normally.
///          The caller reads that as success while both tasks sit at pending.
///
/// Cycle detection is a three-colour DFS:
///   - white: not visited
///   - grey:  on the current recursion path
///   - black: subtree fully explored
///
/// @note Two colours cannot tell a cycle from a shared dependency. In a diamond
///       (d→b→a, d→c→a) the node `a` is reached twice, and a plain "visited"
///       set — which only ever grows — reports the second visit as a cycle. The
///       question being asked is "is this node still on the stack", and grey is
///       what answers it; black means the subtree already returned and the
///       second visit is only sharing.
/// @note The grey nodes are exactly the current path, which is what makes the
///       full cycle reportable. "There is a cycle" gives the model nothing to
///       act on; "a → c → b → a" names the edge to break.
///
/// @code
/// // diamond — legal, and the common shape for a task graph
/// s.add("read", "read the code");
/// s.add("perf", "profile",  {"read"});
/// s.add("sec",  "audit",    {"read"});
/// s.add("sum",  "write up", {"perf", "sec"});
/// s.validate();   // nullopt — `read` is shared, not circular
/// @endcode
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

/// @brief Tasks that can start now, best first.
///
/// @return Pointers into tasks_, sorted by (priority desc, insertion asc).
///
/// @note Marks a task Blocked when an upstream failed, instead of leaving it
///       pending forever. The cascade needs no recursion: each call blocks the
///       tasks whose upstream just broke, and on the next call those are
///       themselves broken upstreams. Branches that do not depend on the
///       failure finish normally — one failure does not take the graph down.
/// @note Raw pointers are safe because std::map nodes keep their addresses
///       across insertions, and nothing is added during run(). Storing tasks in
///       a vector would invalidate every one of them on reallocation.
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

/// @brief Run the whole graph to completion.
///
/// @param runner   Executes one task; called from worker threads.
/// @param on_event Optional progress notifications ("start", "done", "failed").
///
/// Two nested loops: the outer one fills the free slots and then waits, the
/// inner one waits for whichever task finishes first. Collecting one result
/// returns to the outer loop, because that result may have unblocked others.
///
/// @warning std::async is called with an explicit std::launch::async. The
///          default policy permits deferred execution, and a deferred task only
///          runs at .get() — wait_for would return `deferred` forever and this
///          loop would never terminate.
///
/// @note There is no wait_any in the standard library. Completion is found by
///       polling every future with wait_for(0ms) and sleeping 1ms between
///       sweeps. Dropping the sleep costs a full core: 200ms of CPU versus 9ms
///       for the same 200ms of work, measured. A condition_variable and a
///       completion queue would remove the latency and the spin, at the cost of
///       introducing this class's only lock — not worth it while a task is an
///       LLM call measured in seconds.
/// @note Upstream results are gathered before the task starts, when those tasks
///       are already Done and their results will not change again. That is what
///       makes copying them into the lambda safe without a lock.
/// @note A task that throws becomes Failed rather than propagating: its
///       downstream gets Blocked and the rest of the graph carries on.
/// @note No lock anywhere. Worker threads only run the runner and return a
///       string; tasks_ is mutated solely on this thread while collecting.
///
/// @code
/// Scheduler s(3);
/// s.add("a", "..."); s.add("b", "..."); s.add("c", "...", {"a","b"});
/// s.run(runner);
/// // a and b start together; c starts once both are Done and receives
/// // {"a": <a's result>, "b": <b's result>}
/// @endcode
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

/// @brief A readable view of the graph and where each task stands.
///
/// @note Ordered by insertion rather than by id. That is the order the caller
///       built the graph in, which reads more naturally than alphabetical.
///
/// @code
/// // [   done] a            A
/// // [ failed] b            B  [b 炸了]
/// // [blocked] c            依赖b  ← b  [上游任务失败或被阻塞]
/// // [blocked] d            依赖c  ← c  [上游任务失败或被阻塞]
/// // [   done] e            只依赖a  ← a
/// @endcode
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
