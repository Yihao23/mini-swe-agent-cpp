#pragma once
//
// 【Stage 1】Agent 主循环 —— 整个项目的心脏，也应该是最短的文件之一。
//
// 核心就这十行（写完回来对照；如果你的 run() 比这复杂很多，说明有东西漏进来了）：
//
//     for (;;) {
//         auto resp   = llm.complete(req);
//         auto parsed = parse(*resp);
//         session.append(parsed.message);
//         if (!parsed.wants_tools()) return parsed.text;
//         auto results = executor.run_batch(parsed.tool_calls);
//         session.append(tool_result_message(results));
//     }
//
// 围绕它的一切都是流程控制：
//   步数上限     防止无限循环烧钱                       (Stage 1)
//   事件回调     UI 拿到流式文本 / 工具调用 / 结果       (Stage 1)
//   中断处理     Ctrl-C 变成历史里的一条记录，不是崩溃   (Stage 1)
//   上下文压缩   超过阈值把老历史换成纪要                (Stage 4)
//   动态注入     每轮开始前塞后台通知、todo              (Stage 4/6)
//
// 子 agent 复用的就是这个类，只是换一套 tools 和一个更便宜的 model (Stage 6)。
//
#include <csignal>
#include <string>

#include "mini_agent/executor.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

class Sandbox;

struct AgentOptions {
    std::string name = "main";
    std::string model;          // 空 = cfg.model
    int max_steps = 0;          // 0 = cfg.max_steps
    std::string system_extra;
    std::string identity;       // 子 agent 覆盖身份段
};

class Agent {
  public:
    /// 注意 ctx 和 session 的关系：ctx.session 必须指向**同一个** session 对象，
    /// 否则工具看到的历史和循环用的历史会对不上。
    Agent(const Config& cfg, LlmClient& llm, ToolRegistry& registry, Sandbox& sandbox,
          Session& session, ToolContext& ctx, EventSink on_event = {}, AgentOptions opts = {});

    /// 跑到模型不再要工具为止，返回最终文本。
    ///
    /// TODO(Stage 1): 骨架 + 步数上限 + 事件 + 中断
    /// TODO(Stage 4): 循环开头判断是否压缩；注入动态上下文
    ///
    /// 注意：流式模式下 on_text 已经吐过文本了，非流式才补发 TextEvent，否则重复输出。
    std::string run(std::string_view user_input = {});

    bool interrupted() const { return interrupted_; }

  private:
    std::vector<SystemBlock> build_system_blocks() const;
    void inject_turn_context();

    /// Ctrl-C：把"被打断"写进历史，而不是让进程崩掉。
    ///
    /// ⚠️ 关键：要给每个**未完成的 tool_use** 补一个 error 类型的 tool_result，
    /// 否则下一轮请求会因为 tool_use 没有配对结果被 API 拒绝。
    ///
    /// C++ 里怎么接 Ctrl-C：signal handler 里只能改 volatile sig_atomic_t 标志位
    /// （不能 new、不能加锁、不能打日志），循环在安全点检查它。
    std::string handle_interrupt(const std::vector<ToolCallEvent>& pending);

    const Config& cfg_;
    LlmClient& llm_;
    ToolRegistry& registry_;
    Sandbox& sandbox_;
    Session& session_;
    ToolContext& ctx_;
    EventSink on_event_;
    AgentOptions opts_;
    Executor executor_;
    bool interrupted_ = false;
};

/// 全局中断标志。main() 里 std::signal(SIGINT, ...) 设置它，循环检查它。
extern volatile std::sig_atomic_t g_interrupt;

}  // namespace mini
