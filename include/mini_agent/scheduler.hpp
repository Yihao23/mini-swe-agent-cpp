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

enum class TaskStatus { Pending, Running, Done, Failed, Blocked };

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

using TaskRunner = std::function<std::string(const Task&, const std::map<std::string, std::string>&)>;
using TaskEventSink = std::function<void(std::string_view kind, const Task&)>;

class Scheduler {
  public:
    explicit Scheduler(unsigned max_workers = 4);

    Task& add(std::string id, std::string prompt, std::vector<std::string> deps = {},
              int priority = 0, std::string agent_type = "general");

    /// 依赖存在 + 无环。DFS 三色标记；成环时错误信息里要带上路径，否则没法查。
    /// 失败返回错误描述（不抛异常 —— 这是模型给的任务图，出错是常态不是意外）。
    /// TODO(Stage 6)
    std::optional<std::string> validate() const;

    /// 跑完整张图。
    ///
    /// 调度策略：
    ///   * 依赖全 Done 才进 ready 队列
    ///   * ready 按 (priority 降序, seq 升序) 出队
    ///   * 最多 max_workers 个并发
    ///   * 某任务失败 → 依赖它的标记 Blocked，不拖垮整张图
    ///
    /// C++ 实现提示：std::vector<std::future<std::string>> + 一个"等任意一个完成"
    /// 的循环。标准库没有 wait_any —— 最简单的做法是轮询 future 的
    /// wait_for(0ms) == future_status::ready，配一个很短的 sleep。
    /// 想做得漂亮就用 condition_variable + 完成队列。
    /// TODO(Stage 6)
    void run(const TaskRunner& runner, const TaskEventSink& on_event = {});

    const std::map<std::string, Task>& tasks() const { return tasks_; }
    std::string render() const;

  private:
    std::vector<Task*> ready();

    std::map<std::string, Task> tasks_;
    unsigned max_workers_;
    int counter_ = 0;
};

std::string_view to_string(TaskStatus s);

}  // namespace mini
