// 【Stage 6】任务调度。注意：这一层**不认识 LLM**，测试塞 lambda 就能跑。
#include "mini_agent/scheduler.hpp"

#include "mini_agent/json.hpp"

namespace mini {

std::string_view to_string(TaskStatus) { todo("Stage 6: to_string(TaskStatus)"); }

Scheduler::Scheduler(unsigned max_workers) : max_workers_(max_workers) {}

Task& Scheduler::add(std::string, std::string, std::vector<std::string>, int, std::string) {
    todo("Stage 6: Scheduler::add —— id 重复要报错");
}

std::optional<std::string> Scheduler::validate() const {
    todo("Stage 6: Scheduler::validate —— DFS 三色标记，成环时错误里带上路径");
}

std::vector<Task*> Scheduler::ready() {
    todo("Stage 6: Scheduler::ready —— 依赖全 Done；上游失败则标 Blocked");
}

void Scheduler::run(const TaskRunner&, const TaskEventSink&) {
    todo("Stage 6: Scheduler::run —— 标准库没有 wait_any，见头文件里的提示");
}

std::string Scheduler::render() const { todo("Stage 6: Scheduler::render"); }

}  // namespace mini
