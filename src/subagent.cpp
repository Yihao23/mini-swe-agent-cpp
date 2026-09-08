// 【Stage 6】多 agent。契约写在 include/mini_agent/subagent.hpp。

#include "mini_agent/subagent.hpp"

#include <algorithm>
#include <format>

#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"

namespace mini {

const std::vector<AgentType>& agent_types() {
    // 顺序固定：render_agent_types() 的输出进 system prompt，缓存逐字节匹配前缀。
    // 换成 map 或者按名字排序也行，重点是**别依赖任何会变的东西**。
    static const std::vector<AgentType> kTypes = {
        {
            .name = "explorer",
            // ⚠️ description 是写给**协调者模型**看的：它靠这段话决定把什么活
            //    交出去。写"只读调查"没用，要写清"什么时候该用它、它会给回什么"。
            .description =
                "只读调查：在陌生代码里找东西、回答「X 在哪 / 谁调用了 Y / 这段怎么工作」。"
                "返回结论和 `文件:行号`，不改任何文件。"
                "需要读很多文件才能回答一个问题时用它 —— 那些文件不会进你的上下文。",
            .tools = {"read", "glob", "grep"},
            .system = "你是只读调查员。用 glob/grep 定位、用 read 确认，然后给出结论。\n"
                      "结论要带 `文件:行号`，让调用者能直接跳过去核对。\n"
                      "不要改文件，你也没有改文件的工具。不确定的地方明说不确定。",
            .use_main_model = false,
            .max_steps = 20,
        },
        {
            .name = "coder",
            .description =
                "独立完成一处改动并自验：给它一个明确的、边界清楚的修改任务，"
                "它会改完并跑测试确认。任务含糊或者跨很多文件的话别用它 —— 自己做。",
            .tools = {"read", "write", "edit", "glob", "grep", "bash"},
            .system = "你独立完成一处改动。改完必须跑那条会失败的命令（测试、构建）确认。\n"
                      "跑不通就改到跑通，或者说明为什么跑不通 —— 别把没验证的改动报成完成。",
            .use_main_model = false,
            .max_steps = 30,
        },
        {
            .name = "reviewer",
            .description =
                "只读评审：给它一段 diff 或几个文件，它报告问题，不动手改。"
                "想要「第二双眼睛」而不是「帮我改」的时候用。",
            .tools = {"read", "glob", "grep", "bash"},
            .system = "你评审代码，只报告不修改。\n"
                      "每条问题要给出 `文件:行号` 和一个具体的失败场景 —— "
                      "说不出「什么输入会让它出错」的意见就别提。",
            .use_main_model = false,
            .max_steps = 20,
        },
        {
            .name = "general",
            .description = "工具齐全，边界不清的任务。前三种都不合适时才用。",
            .tools = {"read", "write", "edit", "glob", "grep", "bash"},
            .system = {},              // 用默认身份段
            .use_main_model = true,    // 边界不清 → 值得用好模型
            .max_steps = 30,
        },
    };
    return kTypes;
}

const AgentType* find_agent_type(std::string_view name) {
    const auto& all = agent_types();
    const auto it = std::ranges::find(all, name, &AgentType::name);
    return it == all.end() ? nullptr : &*it;
}

std::string render_agent_types() {
    // ⚠️ 只放 name + description，不放 system —— 那是给子 agent 自己看的，
    //    协调者不需要，塞进去只是白占每一轮的固定开销。
    std::string out = "Sub-agent types (hand a self-contained job to one; only its "
                      "conclusion comes back, not its transcript):\n";
    for (const auto& t : agent_types())
        out += std::format("  {} — {}\n", t.name, t.description);
    return out;
}

std::string spawn_subagent(const Config& cfg, LlmClient& llm, Sandbox& sandbox,
                           const ToolContext& parent_ctx, std::string_view agent_type,
                           std::string_view task_prompt) {
    const AgentType* type = find_agent_type(agent_type);
    if (!type) {
        std::string names;
        for (const auto& t : agent_types()) names += (names.empty() ? "" : ", ") + t.name;
        return "没有名为 " + std::string(agent_type) + " 的子 agent 类型。可用: " + names;
    }
    // ⚠️ 深度上限。没有它，一个认定"再派一个就好了"的模型会一直派下去，
    //    每一层的开销都乘在上一层上。
    if (parent_ctx.depth + 1 >= kMaxAgentDepth)
        return "已达子 agent 嵌套上限，这一层请自己完成";

    if (!parent_ctx.registry) return "没有工具注册表，无法派子 agent";
    ToolRegistry narrowed = parent_ctx.registry->subset(type->tools);

    // ⚠️ 全新的 Session，而且**不 bind** —— 子 agent 的历史不落盘。
    //    共享父的 session 的话，子 agent 二十轮探索会全部落进父 agent
    //    下一轮要发出去的那份历史里，而派子 agent 的意义正好是避免这件事。
    Session sub_session;

    // 复制父的 ctx 再收窄：memory / skills / background 继续共享（那些是资源），
    // 换掉的是 session 和 registry（那些是"这一轮的工作区"）。
    ToolContext sub_ctx = parent_ctx;
    sub_ctx.session = &sub_session;
    sub_ctx.registry = &narrowed;
    sub_ctx.spawn = {};              // 清空 → 子 agent 没有 task 工具可用
    sub_ctx.depth = parent_ctx.depth + 1;
    sub_ctx.todos = Json::array();   // todo 是"这一轮的计划"，不该继承

    AgentOptions opts;
    opts.name = type->name;
    opts.model = type->use_main_model ? std::string{} : cfg.subagent_model;
    opts.max_steps = type->max_steps;
    opts.identity = type->system;

    // 事件不往上传：父 agent 的终端不该被子 agent 的工具调用刷屏。
    Agent sub(cfg, llm, narrowed, sandbox, sub_session, sub_ctx, {}, std::move(opts));
    const std::string conclusion = sub.run(task_prompt);

    // 只有结论跨回来。二十轮调查在父 agent 那里的代价是一段文字。
    return conclusion.empty() ? "(子 agent 没有产出结论)" : conclusion;
}

}  // namespace mini
