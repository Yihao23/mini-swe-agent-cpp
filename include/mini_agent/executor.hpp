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

// ── 一批工具调用怎么跑完 ────────────────────────────────────────────────────
//
//   调用性质的记号（整个项目通用，沿用 UML 时序图）：
//
//                        调用方等吗   内部开线程   出错时错误在哪
//     ──▶  同步            等          不开        当场返回
//     ══▶  fork-join       **等**      开          在工作线程里发生，
//                                                  得攒成值带回主线程
//     ──▷  异步            不等        —          调用方早走了，只能记下来
//                                                  等它回来问
//
//     ⚠️ fork-join 对调用方来说**和同步一样是阻塞的** —— 它把活分给几条线程
//        同时做，然后等全部汇合才返回。没有 future、没有回调。单独一个记号
//        是因为它内部有并发，而并发决定了错误怎么传、要不要加锁：
//          ──▶  没有并发，不用锁
//          ══▶  Scheduler 的做法是**根本不共享**：工作线程只跑 runner 返回
//               一个 string，tasks_ 全在主线程上改 —— 所以那个类里一把锁都没有
//          ──▷  必须加锁：读线程在往 buffer 写，主线程在 drain 它
//
//     实线 + **实心**箭头 = 同步、**空心**箭头 = 异步，是 UML 的约定。
//     双线那个是自定义的 —— UML 活动图用一条粗横杠表示 fork/join，那个记号
//     在纵向流程图里好用，横向的调用链里会打断阅读。
//     虚线留给 UML 的原意「返回」，所以这里不拿它表示异步。这些图也从不画
//     返回箭头 —— 返回值一律写成下一行的 ▼ 加一段文字。
//
//   Agent::run()
//        │
//        ──▶ run_batch(calls)          ⚠️ 现在是**串行**的，见文末
//              │
//              └─ 对每个 call ──▶ run_one(call)
//                                   │
//                                   ├─ registry.get(name) ── 没有 ──> error 结果
//                                   │                                 + 列出可用工具名
//                                   ├──▶ sandbox.authorize(tool, args)
//                                   │       Deny ──> error 结果（带 reason）
//                                   ├──▶ tool->run(args, ctx)
//                                   │       try / catch(std::exception&) / catch(...)
//                                   ├─ truncate_output(结果, max_output_chars)
//                                   └─ 记耗时、发 ToolResultEvent
//              │
//              ▼
//        vector<ToolResultEvent>       ⚠️ 顺序必须和入参一致
//              │
//        ──▶ tool_result_message(...)  打成**一条** user Message
//
// ── 为什么 run_one 永不抛 ───────────────────────────────────────────────────
//
//   工具不存在、参数类型不对、沙箱拒绝、工具自己抛异常 —— 四种都变成一条
//   is_error 的结果交回去。抛出来的话，一次坏调用会终结整个 run，
//   而模型永远不知道为什么。
//
//   两个 catch 都要：catch(std::exception&) 能拿到 what()；catch(...) 挡住
//   别的类型的 throw，不让它掀掉进程。
//
// ── 并发：写着的和做着的不一样 ──────────────────────────────────────────────
//
//   ⚠️ 现在 run_batch 是**串行**的：`for (c : calls) out.push_back(run_one(c));`
//
//   打算做成：全部工具都 read_only() 才并发（fork-join，`══▶`），
//   否则串行。判据是全或无 —— 一个写工具就把整批降级，因为两个工具
//   在同一棵树上竞争不是模型能推理的东西。
//
//   模型经常一轮要三四个 read/grep，串行意味着三四倍的墙上时间。
//   做的时候要点：std::async 必须显式写 std::launch::async（默认策略允许
//   deferred，那是假并发），而且结果顺序要和入参一致。
//
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
    /// @warning ⚠️ Currently serial. The intended rule is all-or-nothing —
    ///          parallel only when **every** tool is read_only(), since one
    ///          writer means two tools could race on the same tree, which is
    ///          not something the model can reason about — but that is not
    ///          implemented yet. A turn asking for three greps takes three
    ///          times as long as it needs to.
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
