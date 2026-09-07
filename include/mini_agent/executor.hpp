#pragma once
/// @file
/// @brief Runs a batch of tool_use blocks: authorise, parallelise, time out (Stage 2).
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

/// @brief Runs the tools a turn asked for, and turns every outcome into a result.
///
/// @note This is where the permission gate is consulted, and the only place.
///       Tools make no authorisation decisions of their own — spread that
///       across a dozen tools and there is no way to tell whether the coverage
///       is complete.
/// @note Holds references, not copies: whatever they point at must outlive the
///       agent run, which App guarantees.
class Executor {
  public:
    /// @brief Wire the executor to a registry and a context.
    /// @param registry  Where tool names are resolved.
    /// @param ctx       ⚠️ Held by reference; must outlive this executor.
    /// @param on_event  Progress notifications; empty is fine.
    Executor(ToolRegistry& registry, ToolContext& ctx, EventSink on_event = {});

    /// @brief Run one tool call.
    ///
    /// @param call What the model asked for.
    /// @return A result, always — including for every failure.
    ///
    /// @warning **Never throws.** A tool that throws, a name that does not
    ///          exist, a call the sandbox refuses: each becomes a result the
    ///          model can read and work around. An exception here would end the
    ///          run over one bad call, and the model would never learn why.
    ///
    /// @note An unknown name comes back listing the available ones, so the
    ///       model can correct its own typo on the next turn rather than
    ///       guessing again.
    /// @note catch(std::exception&) and catch(...) are both present. The first
    ///       can report what(); the second is what stops a throw of some other
    ///       type from taking the process down.
    ToolResultEvent run_one(const ToolCallEvent& call);

    /// @brief Run a batch of calls from one turn.
    ///
    /// @param calls Every tool_use block the response contained.
    /// @return One result per call, in the same order.
    ///
    /// @warning Order must be preserved. Results are matched to calls by
    ///          tool_use_id, but a reordered batch makes the transcript
    ///          unreadable for anyone debugging it.
    ///
    /// @note Concurrency is all-or-nothing: the batch runs in parallel only
    ///       when **every** tool in it is read_only(). One writer means the
    ///       whole batch is serialised, because two tools racing on the same
    ///       tree is not something the model can reason about.
    std::vector<ToolResultEvent> run_batch(const std::vector<ToolCallEvent>& calls);

  private:
    ToolRegistry& registry_;
    ToolContext& ctx_;
    EventSink on_event_;
};

/// @brief Cut oversized tool output down to size.
///
/// @param text  What the tool produced.
/// @param limit Maximum bytes to keep.
/// @return The text unchanged when it fits, otherwise a shortened version that
///         says how much was dropped.
///
/// @warning `text.size() - limit` underflows when the text is shorter than the
///          limit — size_t is unsigned, and the difference becomes astronomical.
///          Compare before subtracting. This project has made that mistake
///          twice.
///
/// @note Cuts on a UTF-8 character boundary. Splitting a multi-byte sequence
///       produces bytes the API rejects, and the failure names the request
///       rather than the truncation.
/// @note Says how much was removed. Silently shortened output reads to the
///       model as the whole answer, and it draws conclusions from a fragment.
std::string truncate_output(std::string_view text, std::size_t limit);

}  // namespace mini
