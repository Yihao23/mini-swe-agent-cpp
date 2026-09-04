//
// Agent 主循环 —— 锁住"一轮对话"的形状。
//
//     cmake --build build && ./build/test_loop
//
// 这些用例手工把 Agent 的六个依赖接起来，不经过 App。两个用处：
//   1. Stage 7 之前就能验证循环本身（test_smoke 全都卡在 App 构造）
//   2. App 写好后拿它当对照 —— 如果 App 的接线错了（比如 ctx.session 指向
//      另一个 Session 对象），行为会和这里对不上
//
#include <memory>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"

using namespace mini;

// --- 夹具 -------------------------------------------------------------------
namespace {

/// 固定输出的假工具。真的 read/grep 是 Stage 2 的活，这里只要能被调用。
struct StubTool : Tool {
    std::string n, out;
    bool ro, throws;
    StubTool(std::string name, std::string output, bool read_only = true, bool throws_ = false)
        : n(std::move(name)), out(std::move(output)), ro(read_only), throws(throws_) {}

    std::string_view name() const override { return n; }
    std::string_view description() const override { return "stub"; }
    Json input_schema() const override { return Json::object(); }
    bool read_only() const override { return ro; }
    ToolResult run(const Json&, ToolContext&) override {
        if (throws) throw std::runtime_error("工具炸了");
        return ToolResult{.content = out};
    }
};

/// Agent 的六个依赖必须一起活着，而且 ctx.session 要指向同一个 Session
/// （loop.hpp:47）。把它们绑成一个对象，测试里就不会写错。
struct Harness {
    Config cfg;
    ToolRegistry registry;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    ToolContext ctx;
    std::unique_ptr<FakeLlm> llm;
    std::unique_ptr<Agent> agent;
    std::vector<std::string> event_kinds;

    explicit Harness(std::vector<FakeLlm::Turn> script, int max_steps = 40) {
        cfg.workdir = ".";
        cfg.stream = false;                       // 非流式，才会补发 TextEvent
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.max_steps = max_steps;
        cfg.normalize();

        registry.add(std::make_shared<StubTool>("read", "def greet(): pass"));
        registry.add(std::make_shared<StubTool>("grep", "3 处匹配"));
        registry.add(std::make_shared<StubTool>("boom", "", true, /*throws=*/true));

        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;                   // ⚠️ 必须是同一个 Session
        ctx.registry = &registry;

        llm = std::make_unique<FakeLlm>(cfg, std::move(script));
        agent = std::make_unique<Agent>(cfg, *llm, registry, *sandbox, session, ctx,
                                        [this](const AgentEvent& e) { record(e); });
    }

    void record(const AgentEvent& e) {
        event_kinds.push_back(std::visit(
            [](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, TextEvent>) return "text";
                else if constexpr (std::is_same_v<T, ThinkingEvent>) return "thinking";
                else if constexpr (std::is_same_v<T, ToolCallEvent>) return "call";
                else if constexpr (std::is_same_v<T, ToolResultEvent>) return "result";
                else return "stop";
            },
            e));
    }

    std::size_t count(std::string_view kind) const {
        std::size_t n = 0;
        for (const auto& k : event_kinds) n += (k == kind);
        return n;
    }
};

FakeLlm::Turn tool_turn(std::string name, Json input = Json::object()) {
    return {{FakeBlock::tool(std::move(name), std::move(input))}, "tool_use"};
}
FakeLlm::Turn text_turn(std::string text) {
    return {{FakeBlock::text_block(std::move(text))}, "end_turn"};
}

}  // namespace

// ===========================================================================
// 一轮工具调用 + 一轮回答
// ===========================================================================

TEST(loop_runs_a_tool_then_answers) {
    Harness h{{tool_turn("read", {{"path", "hello.py"}}), text_turn("这是一个打招呼函数。")}};

    const auto out = h.agent->run("看看 hello.py");

    CHECK(out == "这是一个打招呼函数。");
    CHECK_MSG(h.session.messages().size() == 4,
              "user / assistant(tool_use) / user(tool_result) / assistant");
    CHECK(h.session.messages().at(0).role == Role::User);
    CHECK(h.session.messages().at(1).role == Role::Assistant);
    CHECK(h.session.messages().at(2).role == Role::User);
    CHECK_MSG(has_tool_result(h.session.messages().at(2)), "工具结果作为 tool_result 块回传");
}

TEST(every_turn_resends_the_whole_history) {
    // 模型没有记忆。少发一条，它就不知道自己刚做过什么。
    Harness h{{tool_turn("read"), text_turn("好了")}};
    h.agent->run("开始");

    CHECK(h.llm->calls().size() == 2);
    CHECK_MSG(h.llm->calls().at(0).value("messages", Json::array()).size() == 1,
              "第一轮只有用户那句话");
    CHECK_MSG(h.llm->calls().at(1).value("messages", Json::array()).size() == 3,
              "第二轮要带上 assistant 的 tool_use 和 tool_result");
}

TEST(parallel_tool_calls_come_back_in_one_message) {
    // ⚠️ 拆成多条会让模型逐渐学会不再并行调工具。
    Harness h{{{{FakeBlock::tool("read", Json::object()), FakeBlock::tool("grep", Json::object())}, "tool_use"},
               text_turn("找到了")}};
    h.agent->run("找一下");

    CHECK(h.session.messages().size() == 4);
    CHECK_MSG(h.session.messages().at(2).content.size() == 2,
              "两个工具的结果必须打进同一条 user 消息");
}

TEST(answer_without_tools_ends_in_one_turn) {
    Harness h{{text_turn("不用查，我知道")}};
    const auto out = h.agent->run("你好");

    CHECK(out == "不用查，我知道");
    CHECK_MSG(h.session.messages().size() == 2, "只有 user + assistant");
    CHECK(h.llm->calls().size() == 1);
}

// ===========================================================================
// 系统块与请求组装
// ===========================================================================

TEST(system_prompt_is_byte_stable_across_turns) {
    // ⚠️ system 渲染在请求最前面。变一个字节，整个请求的缓存前缀就作废。
    Harness h{{tool_turn("read"), tool_turn("grep"), text_turn("完成")}};
    h.agent->run("多跑几轮");

    CHECK(h.llm->calls().size() == 3);
    const auto first = h.llm->calls().at(0).value("system", Json::array()).dump();
    for (const auto& c : h.llm->calls())
        CHECK_MSG(c.value("system", Json::array()).dump() == first,
                  "每一轮的 system 必须逐字节相同");
}

TEST(tool_schema_is_stable_across_turns) {
    Harness h{{tool_turn("read"), text_turn("完成")}};
    h.agent->run("跑");

    const auto first = h.llm->calls().at(0).value("tools", Json::array()).dump();
    CHECK(h.llm->calls().at(1).value("tools", Json::array()).dump() == first);
}

TEST(system_prompt_is_identical_across_agent_instances) {
    // ⚠️ 上一条只能证明「一次 run 内每轮相同」—— 因为 system 是循环外算一次的，
    //    那天然成立。真正要防的是 system **内容本身**不稳定：时间戳、随机 id、
    //    进程 pid。那种东西每次启动都不一样，缓存永远命中不了，而且
    //    「每轮相同」的断言完全看不出来。
    //    两个独立实例产出的 system 必须逐字节相同。
    Harness a{{text_turn("x")}};
    Harness b{{text_turn("y")}};
    a.agent->run("同样的输入");
    b.agent->run("同样的输入");

    CHECK_MSG(a.llm->calls().at(0).value("system", Json::array()).dump() ==
                  b.llm->calls().at(0).value("system", Json::array()).dump(),
              "两个 Agent 实例的 system 必须完全一致 —— 不许含时间戳或随机值");
}

TEST(tool_schema_is_identical_across_agent_instances) {
    Harness a{{text_turn("x")}};
    Harness b{{text_turn("y")}};
    a.agent->run("hi");
    b.agent->run("hi");

    CHECK(a.llm->calls().at(0).value("tools", Json::array()).dump() ==
          b.llm->calls().at(0).value("tools", Json::array()).dump());
}

// ===========================================================================
// 失败与边界
// ===========================================================================

TEST(a_throwing_tool_becomes_a_result_not_a_crash) {
    // 工具失败要变成"失败了，原因是 X"喂回模型，让它自己纠错。
    Harness h{{tool_turn("boom"), text_turn("那我换个方法")}};
    const auto out = h.agent->run("试试");

    CHECK(out == "那我换个方法");
    CHECK(h.session.messages().size() == 4);
    const auto& res = std::get<ToolResultBlock>(h.session.messages().at(2).content.at(0));
    CHECK_MSG(res.is_error, "抛异常的工具要标成 is_error");
    CHECK_MSG(res.content.find("工具炸了") != std::string::npos, "原因要带给模型");
}

TEST(an_unknown_tool_becomes_a_result_not_a_crash) {
    Harness h{{tool_turn("nonexistent"), text_turn("换一个")}};
    h.agent->run("试试");

    const auto& res = std::get<ToolResultBlock>(h.session.messages().at(2).content.at(0));
    CHECK(res.is_error);
    CHECK_MSG(res.content.find("read") != std::string::npos,
              "要列出可用工具名，模型下一轮才能自纠");
}

TEST(step_ceiling_stops_an_endless_tool_loop) {
    // 模型可能一直调同一个工具。没有上限就一直烧钱，而且只有看账单才发现。
    std::vector<FakeLlm::Turn> forever;
    for (int i = 0; i < 20; ++i) forever.push_back(tool_turn("read"));

    Harness h{std::move(forever), /*max_steps=*/3};
    h.agent->run("停不下来");

    CHECK_MSG(h.llm->calls().size() == 3, "最多问模型 max_steps 次");
    CHECK_MSG(h.session.messages().size() == 1 + 3 * 2, "每轮追加 assistant + tool_result 两条");
}

TEST(agent_options_override_the_config_step_limit) {
    std::vector<FakeLlm::Turn> forever;
    for (int i = 0; i < 20; ++i) forever.push_back(tool_turn("read"));

    Harness h{std::move(forever), /*cfg.max_steps=*/40};
    AgentOptions opts;
    opts.max_steps = 2;
    Agent limited(h.cfg, *h.llm, h.registry, *h.sandbox, h.session, h.ctx, {}, opts);
    limited.run("停不下来");

    CHECK_MSG(h.llm->calls().size() == 2, "opts.max_steps 非 0 时压过 cfg.max_steps");
}

TEST(empty_user_input_does_not_append_a_message) {
    // 子 agent 和 --continue 会用空输入继续已有历史。
    Harness h{{text_turn("接着上次")}};
    h.session.add_user_text("之前说过的话");
    h.agent->run();

    CHECK_MSG(h.session.messages().size() == 2, "空输入不该再插一条空的 user 消息");
}

// ===========================================================================
// 事件流
// ===========================================================================

TEST(events_cover_the_whole_turn) {
    Harness h{{tool_turn("read"), text_turn("好了")}};
    h.agent->run("跑");

    CHECK_MSG(h.count("call") == 1, "工具调用要通知 UI");
    CHECK_MSG(h.count("result") == 1, "工具结果也要");
    CHECK_MSG(h.count("stop") == 1, "结束时发一个 StopEvent");
}

TEST(non_streaming_emits_the_text_once) {
    // ⚠️ 流式时 on_text 已经吐过文本了，非流式才补发 —— 否则用户看到两遍。
    Harness h{{text_turn("只说一次")}};      // cfg.stream = false
    h.agent->run("hi");

    CHECK_MSG(h.count("text") == 1, "非流式：补发一次 TextEvent");
}

TEST(agent_works_without_an_event_sink) {
    // on_event 是 std::function，可以是空的。每处调用前都判空了才行。
    Harness h{{tool_turn("read"), text_turn("好了")}};
    Agent silent(h.cfg, *h.llm, h.registry, *h.sandbox, h.session, h.ctx);   // 不传 on_event
    const auto out = silent.run("跑");

    CHECK_MSG(out == "好了", "空 EventSink 不该让循环崩掉");
}

int main() { return mt::run_all(); }
