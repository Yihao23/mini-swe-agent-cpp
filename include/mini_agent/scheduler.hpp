#pragma once
//
// 【Stage 6】任务调度 —— 把一个大目标拆成带依赖的任务图，按拓扑序 + 优先级并发执行。
//
// ── 这一层刻意**不认识 LLM** ────────────────────────────────────────────────
//
// 它只知道 `runner(task, upstream_results) -> string`。
// 于是测试时能塞一个 lambda 进去，0.5 秒验证完拓扑序、并发、环检测，不烧一分钱 token。
//
// **任何时候你能把某一层的 LLM 依赖切掉，都值得这么做。** 这是整个项目里
// 最容易被忽略、但对开发速度影响最大的一条。
//
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mini {

/// @brief Where a task stands.
///
/// @note `Blocked` is distinct from `Failed`: the task itself never ran, an
///       upstream broke. Keeping them apart is what lets the render show which
///       failure was the cause and which were consequences.
enum class TaskStatus { Pending, Running, Done, Failed, Blocked };

/// @brief One node of the task graph.
///
/// @note A plain struct with public fields rather than a class: the scheduler
///       is the only writer, and everything else — render(), the event sink,
///       the caller reading results — only reads. No invariant needs guarding
///       from the outside.
/// @note status, result and error are related (Done implies a result, Failed
///       implies an error) and a variant would make the illegal combinations
///       unrepresentable. Flat fields win here because the type is read far
///       more often than written, and every read would otherwise need a visit.
struct Task {
    std::string id;
    std::string prompt;
    std::vector<std::string> deps;
    int priority = 0;
    std::string agent_type = "general";

    TaskStatus status = TaskStatus::Pending;
    std::string result;
    std::string error;
    int seq = 0;             // 入图顺序，同优先级时的稳定排序键
};

/// @brief How one task is executed: runner(task, upstream results) → its output.
///
/// @note This signature is why the layer does not know about LLMs. A test hands
///       over a lambda and verifies topological order, concurrency and cycle
///       detection in half a second without spending a token. Cutting the LLM
///       dependency out of a layer is worth doing wherever it is possible.
/// @note Called from worker threads, possibly several at once — whatever the
///       caller puts in here has to tolerate that.
using TaskRunner = std::function<std::string(const Task&, const std::map<std::string, std::string>&)>;
/// @brief Progress notifications: kind is "start", "done" or "failed".
/// @note A string rather than an enum — three values, used only for display.
using TaskEventSink = std::function<void(std::string_view kind, const Task&)>;

/// @brief Runs a graph of dependent tasks, several at a time.
///
/// @note No lock anywhere. Worker threads only run the runner and return a
///       string; every mutation of the task map happens on the main thread as
///       it collects results. Confining the shared mutable state to one thread
///       removes the need for synchronisation rather than managing it.
class Scheduler {
  public:
    /// @brief Create a scheduler.
    /// @param max_workers How many tasks may run at once; 0 is treated as 1.
    explicit Scheduler(unsigned max_workers = 4);

    /// @brief Add a node to the graph.
    ///
    /// @param id         Unique within this graph.
    /// @param prompt     What the task should do.
    /// @param deps       Ids that must finish first.
    /// @param priority   Higher runs first among ready tasks.
    /// @param agent_type Which sub-agent profile to use.
    /// @return A reference to the stored task.
    ///
    /// @throws std::invalid_argument on a duplicate id.
    ///
    /// @note Throwing here, while validate() returns its errors, is deliberate:
    ///       a duplicate id is the caller's bug — it makes "which task does this
    ///       dependency point at" undecidable — whereas a malformed graph comes
    ///       from the model and is routine.
    /// @note Records an insertion sequence number, used to break priority ties.
    ///       Without it, ties resolve by the map's iteration order, which is the
    ///       id's lexicographic order — renaming a task would change the
    ///       execution order and nothing would be reproducible.
    Task& add(std::string id, std::string prompt, std::vector<std::string> deps = {},
              int priority = 0, std::string agent_type = "general");

    /// @brief Check that every dependency exists and the graph is acyclic.
    ///
    /// @return nullopt when the graph is runnable, otherwise a description.
    ///
    /// @warning A cyclic graph fails worse than it sounds. ready() only picks
    ///          tasks whose dependencies are all Done, so in an a→b→a cycle
    ///          neither ever becomes eligible, run() finds nothing to start and
    ///          returns normally. The caller sees success while both tasks sit
    ///          at pending — silent, not loud.
    ///
    /// @note Returns a value rather than throwing: the graph comes from the
    ///       model, so a bad one is routine. The description is fed back so it
    ///       can revise and retry.
    /// @note The message names the full cycle. "There is a cycle" leaves the
    ///       model nothing to act on across thirty tasks; "a → c → b → a" says
    ///       which edge to break.
    ///
    /// @code
    /// sched.add("a", "...", {"b"});
    /// sched.add("b", "...", {"a"});
    /// sched.validate();   // "依赖成环: a → b → a"
    ///
    /// sched.add("x", "...", {"nope"});
    /// sched.validate();   // "任务 \"x\" 依赖了不存在的任务 \"nope\""
    /// @endcode
    ///
    /// 依赖存在 + 无环。DFS 三色标记；成环时错误信息里要带上路径，否则没法查。
    std::optional<std::string> validate() const;

    /// 跑完整张图。
    ///
    /// 调度策略：
    ///   * 依赖全 Done 才进 ready 队列
    ///   * ready 按 (priority 降序, seq 升序) 出队
    ///   * 最多 max_workers 个并发
    ///   * 某任务失败 → 依赖它的标记 Blocked，不拖垮整张图
    ///
    /// @brief Run the whole graph.
    ///
    /// @param runner   Executes one task; called from worker threads.
    /// @param on_event Optional progress notifications.
    ///
    /// @note Does not call validate() itself. On a cyclic graph it returns
    ///       having done nothing, quietly — call validate() first.
    /// @note std::async is invoked with an explicit std::launch::async. Under
    ///       the default policy an implementation may defer, in which case
    ///       wait_for returns `deferred` forever and the polling loop never
    ///       finishes.
    /// @note There is no wait_any in the standard library, so completion is
    ///       detected by polling each future with wait_for(0ms) and sleeping
    ///       1ms between sweeps. Busy-waiting instead costs a full core: 200ms
    ///       of CPU against 9ms, measured. A condition_variable and a completion
    ///       queue would remove both the latency and the spin, at the price of
    ///       introducing this class's only lock — not worth it while a task is
    ///       an LLM call measured in seconds.
    ///
    /// @code
    /// Scheduler s(3);
    /// s.add("a", "analyse perf");
    /// s.add("b", "analyse security");
    /// s.add("c", "summarise", {"a", "b"});
    /// s.run([](const Task& t, const std::map<std::string,std::string>& up) {
    ///     return t.id + " done (" + std::to_string(up.size()) + " upstream)";
    /// });
    /// // a and b run concurrently, c waits for both and receives both results
    /// @endcode
    ///
    /// C++ 实现提示：标准库没有 wait_any —— 轮询 future 的 wait_for(0ms)。
    void run(const TaskRunner& runner, const TaskEventSink& on_event = {});

    /// @brief The task map, for inspecting results after run().
    const std::map<std::string, Task>& tasks() const { return tasks_; }

    /// @brief A human-readable view of the graph and its current state.
    /// @note Ordered by insertion, which matches how the caller thinks about
    ///       the graph better than the id's alphabetical order would.
    std::string render() const;

  private:
    /// @brief Tasks eligible to start now, best first.
    ///
    /// @return Pointers into tasks_, sorted by (priority desc, insertion asc).
    ///
    /// @note Also marks a task Blocked when an upstream failed, rather than
    ///       leaving it pending forever. The cascade happens across rounds
    ///       without recursion: each pass blocks the tasks whose upstream just
    ///       broke, and they become broken upstreams for the next pass.
    /// @note Returns raw pointers because std::map nodes keep their addresses
    ///       when other entries are inserted, and nothing is added during run().
    ///       A vector<Task> would invalidate them on reallocation.
    std::vector<Task*> ready();

    std::map<std::string, Task> tasks_;
    unsigned max_workers_;
    int counter_ = 0;
};

std::string_view to_string(TaskStatus s);

}  // namespace mini
