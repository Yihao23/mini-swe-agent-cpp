#pragma once
//
// 【Stage 2】执行器 —— 拿到一批 tool_use，负责授权、并发、超时、截断、错误封装。
//
// 三条规则：
//   1. 一次响应里的多个 tool_use 要一起执行、结果一起回传
//   2. 只有全部只读时才并发；有副作用的混在里面一律串行
//   3. 工具失败不是异常 —— 转成 is_error 结果喂回模型，让它自己纠错
//
// ── C++ 的并发注意事项 ──────────────────────────────────────────────────────
//   * 用 std::async 一定要显式写 std::launch::async。默认策略允许"延迟执行"，
//     那样你以为在并发，其实是在 .get() 的时候顺序跑完的 —— 静默失去并行。
//   * 结果顺序必须和入参一致（先收集 future 再统一 get，别边跑边 push_back）。
//   * 并发跑的工具共享 ToolContext。谁会写 session.read_files()？想清楚要不要加锁。
//
#include <vector>

#include "mini_agent/parser.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

class Executor {
  public:
    Executor(ToolRegistry& registry, ToolContext& ctx, EventSink on_event = {});

    /// 执行一个工具调用。**任何情况下都要返回 ToolResultEvent，不能抛异常。**
    ///   1. 工具不存在 → error 结果 + 列出可用工具名（帮模型自纠）
    ///   2. 【Stage 3】过闸 sandbox.authorize()
    ///   3. try 调 run，catch(...) → error 结果
    ///   4. 截断输出、记录耗时
    /// TODO(Stage 2)
    ToolResultEvent run_one(const ToolCallEvent& call);

    /// 一批。并发判据：**所有**工具都 read_only 才并发。
    /// TODO(Stage 2)
    std::vector<ToolResultEvent> run_batch(const std::vector<ToolCallEvent>& calls);

  private:
    ToolRegistry& registry_;
    ToolContext& ctx_;
    EventSink on_event_;
};

/// 截断超长输出。
/// 想想：只留头部，还是头尾都留？要不要告诉模型"我截断了多少"？
/// （一个 grep 可能返回 10 万行，而尾部往往最新最相关。）
std::string truncate_output(std::string_view text, std::size_t limit);

}  // namespace mini
