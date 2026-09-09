#pragma once
/// @file
/// @brief Executes a dependency graph of tasks, several at a time (Stage 6).
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
// ── 这是全项目唯一真开线程的地方 ────────────────────────────────────────────
//
//   记号（沿用 UML 时序图，详见 executor.hpp 的图例）：
//     ──▶  同步：调用方阻塞      ══▶  fork-join：内部开线程，调用方等全部结束
//
//   调用方 ──▶ run(runner)         ⚠️ **这一句是阻塞的**。它返回时每个任务
//        │                            都有最终状态了 —— 没有 future、没有回调。
//        ▼
//   ┌─ for (;;) ────────────────────────────────────────────────────────────┐
//   │                                                                        │
//   │  ① ready()  依赖全 Done 的任务，按 (priority 降序, seq 升序) 排好      │
//   │       │      顺带把「上游失败了」的标成 Blocked                        │
//   │       ▼                                                                │
//   │  ② 有空位就填（running.size() < max_workers）                          │
//   │       │  启动前先收集上游结果 —— 此刻它们都已 Done，之后不会再变        │
//   │       │                                                                │
//   │       ══▶ std::async(std::launch::async, runner(task, upstream))       │
//   │       │        ⚠️ 必须显式写 launch::async。默认策略允许 deferred，     │
//   │       │           那样任务只在 .get() 时才跑 —— 看起来并发，实际串行    │
//   │       ▼                                                                │
//   │  ③ 轮询等任意一个完成                                                  │
//   │       │  ⚠️ 标准库**没有 wait_any**。只能挨个 wait_for(0ms)，          │
//   │       │     一轮扫完都没好就 sleep 1ms 再来。                          │
//   │       │     忙等的话是 200ms CPU 对 9ms（实测）。                       │
//   │       │     condition_variable + 完成队列能去掉延迟和空转，代价是       │
//   │       │     引入这个类唯一的一把锁 —— 一个任务是秒级的 LLM 调用，不值。 │
//   │       ▼                                                                │
//   │  ④ 收结果：fut.get() 包在 try 里                                       │
//   │       │      正常     → status = Done,   result                        │
//   │       │      抛异常   → status = Failed, error   ⚠️ 不终止整张图        │
//   │       ▼                                                                │
//   │  ⑤ running 空了、也没有能启动的 → 跑完，退出循环                        │
//   └────────────────────────────────────────────────────────────────────────┘
//        │
//        ▼
//   tasks() 里每条都是终态：Done / Failed / Blocked
//
// ── 一把锁都没有，靠的是「不共享」──────────────────────────────────────────
//
//   工作线程做的事只有一件：跑 runner，返回一个 string。
//
//        主线程                          工作线程
//        ────────                        ────────
//        改 tasks_ 的 status/result      只读传进去的那份 Task 拷贝
//        改 running                      只写自己的 promise
//        发事件
//
//   ⚠️ 异常跨不过线程边界，所以错误得**攒成值**带回来 —— 这就是 Task 里
//      有 error 字段而不是往外抛的原因。fork-join 和普通同步调用的区别，
//      一大半体现在这里。
//
//   ⚠️ runner 会被**并发调用**。调用方塞进去的东西必须自己扛得住这一点。
//      TaskGraphTool 里那个 lambda 只捕获 ctx.spawn 的拷贝，不碰任何共享可变状态。
//
// ── 三种终态，三种含义 ──────────────────────────────────────────────────────
//
//     Done      跑完了，result 里有输出
//     Failed    自己抛了异常，error 里有原因
//     Blocked   ⚠️ **自己从来没跑过**，是上游断了
//
//   区分后两者是有用的：render() 能指出哪个是病根、哪些是并发症。
//   合成一个「失败」的话，调用方看到五个失败，得自己去猜哪一个是原因。
//
//   级联不用递归：每一轮 ready() 把「上游刚坏掉」的标成 Blocked，
//   它们下一轮就成了别人的坏上游。
//
// ── 三个不变量 ──────────────────────────────────────────────────────────────
//
//     tasks_        id → Task。map 而不是 vector：ready() 返回裸指针，
//                   而 map 的节点在插入别的条目时地址不变
//     max_workers_  并发上限，0 当 1
//     counter_      入图序号，同优先级时的稳定排序键
//
//   ┌── I1  一个任务启动时，它的每个上游都已 Done ───────────────────────┐
//   │ 维护者：ready() 只挑依赖全 Done 的。upstream 在启动前收集，         │
//   │        所以工作线程读到的是定稿，不会中途变。                       │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I2  同优先级的执行顺序稳定可复现 ────────────────────────────────┐
//   │ 维护者：add() 记 seq，ready() 按 (priority 降序, seq 升序) 排。     │
//   │ 没有 seq 的话平局由 map 的迭代顺序决定 —— 那是 id 的字典序，        │
//   │ 改个任务名整个执行顺序就变了，什么都不可复现。                      │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I3  tasks_ 只在主线程上被修改 ───────────────────────────────────┐
//   │ 这就是这个类里一把锁都没有的全部理由。                              │
//   │ 违反 → 数据竞争，而竞争是概率性的：本地跑一百次都对，CI 上偶尔挂。  │
//   └────────────────────────────────────────────────────────────────────┘
//
// ⚠️ run() **不自己调 validate()**。成环的图它会安安静静什么都不做就返回 ——
//    ready() 永远挑不出任务，循环第一轮就退出，调用方看到「成功」而所有任务
//    停在 Pending。调用方必须先 validate()，TaskGraphTool 就是这么做的。
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
    std::string id;                 ///< Unique within the graph; what deps point at.
    std::string prompt;             ///< What this task should do.
    std::vector<std::string> deps;  ///< Ids that must finish before this may start.
    int priority = 0;               ///< Higher runs first among ready tasks.
    std::string agent_type = "general";   ///< Which sub-agent profile to use.

    TaskStatus status = TaskStatus::Pending;  ///< Where it stands.
    std::string result;             ///< Set when Done; what the runner returned.
    std::string error;              ///< Set when Failed or Blocked; why.

    /// @brief Insertion order, the tie-break among equal priorities.
    /// @note Without it, ties resolve by the map's iteration order, which is
    ///       the id's lexicographic order — renaming a task would change the
    ///       execution order and nothing would be reproducible.
    int seq = 0;
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
    /// @code{.test}
    /// @setup Scheduler ok(2);
    /// @setup ok.add("a", "先做"); ok.add("b", "再做", {"a"});
    /// ok.validate().has_value()                     ==> false
    ///
    /// @setup Scheduler cyc(2);
    /// @setup cyc.add("a", "...", {"b"}); cyc.add("b", "...", {"a"});
    /// cyc.validate().has_value()                    ==> true
    /// // 报错要带上整条路径，否则三十个任务里没法查是哪条边
    /// (cyc.validate()->find("a") != std::string::npos)   ==> true
    /// (cyc.validate()->find("b") != std::string::npos)   ==> true
    ///
    /// @setup Scheduler miss(2);
    /// @setup miss.add("x", "...", {"nope"});
    /// (miss.validate()->find("nope") != std::string::npos)  ==> true
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
    /// @code{.test}
    /// @setup Scheduler s(3);
    /// @setup s.add("a", "analyse perf");
    /// @setup s.add("b", "analyse security");
    /// @setup s.add("c", "summarise", {"a", "b"});
    /// @setup s.run([](const Task& t, const std::map<std::string, std::string>& up) {
    /// @setup     return t.id + ":" + std::to_string(up.size());
    /// @setup });
    /// s.tasks().at("a").status   ==> TaskStatus::Done
    /// s.tasks().at("c").status   ==> TaskStatus::Done
    /// // a 和 b 没有上游，c 拿到两份上游结果
    /// s.tasks().at("a").result   ==> "a:0"
    /// s.tasks().at("c").result   ==> "c:2"
    ///
    /// // 上游失败 → 下游标 Blocked，而不是永远 Pending，也不拖垮整张图
    /// @setup Scheduler f(2);
    /// @setup f.add("bad", "会抛"); f.add("after", "依赖它", {"bad"});
    /// @setup f.run([](const Task& t, auto&&) -> std::string {
    /// @setup     if (t.id == "bad") throw std::runtime_error("boom");
    /// @setup     return "ok";
    /// @setup });
    /// f.tasks().at("bad").status     ==> TaskStatus::Failed
    /// f.tasks().at("after").status   ==> TaskStatus::Blocked
    /// @endcode
    ///
    /// C++ 实现提示：标准库没有 wait_any —— 轮询 future 的 wait_for(0ms)。
    void run(const TaskRunner& runner, const TaskEventSink& on_event = {});

    /// @brief The task map, for inspecting results after run().
    /// @return Every task by id, carrying its final status and output.
    const std::map<std::string, Task>& tasks() const { return tasks_; }

    /// @brief A human-readable view of the graph and its current state.
    /// @note Ordered by insertion, which matches how the caller thinks about
    ///       the graph better than the id's alphabetical order would.
    /// @return One line per task, in insertion order.
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

/// @brief The display name of a status.
/// @param s The status.
/// @return A stable lowercase name, used by render() and the event sink.
std::string_view to_string(TaskStatus s);

}  // namespace mini
