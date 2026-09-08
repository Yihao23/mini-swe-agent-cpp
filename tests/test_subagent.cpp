// 【Stage 6】子 agent。
//
// 这个机制存在的唯一理由是**上下文隔离**：二十轮调查在父 agent 那里只花一段
// 文字。所以最要紧的断言不是"子 agent 跑起来了"，而是"它的历史没有漏进父的"。
// 漏了的话功能完全正常，只是每次派子 agent 都在往父的上下文里灌垃圾 ——
// 而那正是派它的理由。

#include "microtest.hpp"

#include "mini_agent/app.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/subagent.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    fs::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    ToolRegistry registry;
    ToolContext ctx;

    explicit Fixture(bool with_tools = true) {
        root = fs::temp_directory_path() /
               ("sub_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        if (with_tools)
            for (auto& t : builtin_tools(cfg)) registry.add(std::move(t));
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
        ctx.registry = &registry;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}
std::string all_text(const Session& s) {
    std::string out;
    for (const auto& m : s.messages()) out += text_of(m) + "\n";
    return out;
}

}  // namespace

// ── agent_types ─────────────────────────────────────────────────────────────
TEST(the_four_types_exist_and_are_findable) {
    for (const char* n : {"explorer", "coder", "reviewer", "general"})
        CHECK_MSG(find_agent_type(n) != nullptr, std::string("缺少类型 ") + n);
    CHECK(find_agent_type("nope") == nullptr);
    CHECK(find_agent_type("") == nullptr);
}

TEST(read_only_types_get_no_writing_tools) {
    for (const char* n : {"explorer", "reviewer"}) {
        const auto& tools = find_agent_type(n)->tools;
        // ⚠️ 白名单是这一层唯一的约束手段。给 explorer 一把 edit，
        //    「只读调查」这个承诺就只剩 prompt 在拦着了。
        for (const char* forbidden : {"write", "edit"})
            CHECK_MSG(std::ranges::find(tools, forbidden) == tools.end(),
                      std::string(n) + " 不该有 " + forbidden);
    }
    const auto& coder = find_agent_type("coder")->tools;
    CHECK_MSG(std::ranges::find(coder, "bash") != coder.end(), "coder 要能跑测试自验");
}

TEST(the_type_list_is_byte_stable) {
    // render_agent_types() 进 system prompt，缓存逐字节匹配前缀。
    const auto first = render_agent_types();
    for (int i = 0; i < 3; ++i) CHECK(render_agent_types() == first);
    CHECK(has(first, "explorer"));
    CHECK_MSG(has(first, "只读调查"), "描述要进去 —— 协调者靠它决定派谁");
}

TEST(the_rendered_list_omits_the_system_prompts) {
    const auto rendered = render_agent_types();
    // system 是给子 agent 自己看的，协调者不需要。塞进去只是白占每轮的固定开销。
    CHECK_MSG(!has(rendered, "你是只读调查员"), "system 段不该出现在索引里");
}

// ── ToolRegistry::subset ────────────────────────────────────────────────────
TEST(subset_shares_instances_rather_than_copying) {
    Fixture f;
    const auto sub = f.registry.subset({"read", "grep"});
    CHECK(sub.size() == 2);
    // ⚠️ 同一个实例。ReadTool 记录"读过哪些文件"的状态要在父子之间共享，
    //    拷贝的话子 agent read 过的文件父 agent 不认账。
    CHECK_MSG(sub.get("read") == f.registry.get("read"),
              "必须共享实例 —— 这就是工具存 shared_ptr 的理由");
}

TEST(subset_skips_names_it_does_not_know) {
    Fixture f;
    const auto sub = f.registry.subset({"read", "no_such_tool", "grep"});
    CHECK_MSG(sub.size() == 2, "认不出的名字跳过，不是报错");
    CHECK(sub.get("no_such_tool") == nullptr);
}

TEST(subset_of_nothing_is_empty) {
    Fixture f;
    CHECK(f.registry.subset({}).size() == 0);
}

// ── spawn_subagent：隔离 ────────────────────────────────────────────────────
TEST(the_subagents_history_does_not_reach_the_parent) {
    Fixture f;
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("子 agent 的结论")}, "end_turn"}});
    f.session.add_user_text("父 agent 的任务");
    const auto before = f.session.messages().size();

    const auto out = spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "去查点东西");

    CHECK(out == "子 agent 的结论");
    // ⚠️ 这是整个机制的意义所在。漏进来的话功能完全正常，只是每次派子 agent
    //    都在往父的上下文里灌垃圾 —— 而那正是派它的理由。
    CHECK_MSG(f.session.messages().size() == before,
              "子 agent 的历史一条都不该进父的 session");
    CHECK_MSG(!has(all_text(f.session), "去查点东西"),
              "连它的任务描述都不该进来");
}

TEST(the_subagent_gets_a_session_that_is_never_saved) {
    Fixture f;
    f.session.bind(f.cfg.sessions_dir());
    f.cfg.ensure_dirs();
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("结论")}, "end_turn"}});

    std::size_t before = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(f.cfg.sessions_dir())) ++before;
    spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");
    std::size_t after = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(f.cfg.sessions_dir())) ++after;

    CHECK_MSG(before == after, "子 agent 的历史不落盘 —— 它是一次性的");
}

TEST(the_subagent_only_gets_its_whitelisted_tools) {
    Fixture f;
    // FakeLlm 把请求记下来，包括 tools 数组 —— 直接断言发出去的是什么。
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("结论")}, "end_turn"}});
    spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");

    const auto& sent = llm.calls().at(0).at("tools");
    std::vector<std::string> names;
    for (const auto& t : sent) names.push_back(t.value("name", std::string{}));
    CHECK_MSG(std::ranges::find(names, "read") != names.end(), "explorer 该有 read");
    CHECK_MSG(std::ranges::find(names, "edit") == names.end(),
              "explorer 不该有 edit —— 白名单必须真的生效");
    CHECK_MSG(std::ranges::find(names, "bash") == names.end(), "也不该有 bash");
}

TEST(the_subagent_cannot_spawn_again) {
    Fixture f;
    // ctx.spawn 在子 agent 的上下文里被清空，且 depth 加一 —— 两道保险。
    f.ctx.depth = kMaxAgentDepth - 1;
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("结论")}, "end_turn"}});
    const auto out = spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");
    CHECK_MSG(has(out, "嵌套上限"), "到了深度上限要拒绝，而不是再派一层");
    CHECK_MSG(llm.calls().empty(), "拒绝了就不该浪费一次 API 调用");
}

TEST(the_subagent_keeps_sharing_memory_and_skills) {
    Fixture f;
    // 换掉的是"这一轮的工作区"（session/registry），共享的是"资源"。
    Memory mem(f.cfg.memory_dir());
    f.ctx.memory = &mem;
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("结论")}, "end_turn"}});
    spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");
    CHECK_MSG(f.ctx.memory == &mem, "父的 ctx 不该被改动");
}

TEST(an_unknown_type_lists_the_alternatives) {
    Fixture f;
    FakeLlm llm(f.cfg, {});
    const auto out = spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "typo-name", "任务");
    CHECK_MSG(has(out, "explorer"), "报错要列出可用的类型，模型才能自纠");
    CHECK(llm.calls().empty());
}

TEST(a_cheaper_model_is_used_unless_the_type_says_otherwise) {
    Fixture f;
    f.cfg.model = "big-model";
    f.cfg.subagent_model = "small-model";

    FakeLlm llm1(f.cfg, {{{FakeBlock::text_block("x")}, "end_turn"}});
    spawn_subagent(f.cfg, llm1, *f.sandbox, f.ctx, "explorer", "任务");
    // explorer 的 use_main_model 是 false
    CHECK(llm1.calls().size() == 1);

    FakeLlm llm2(f.cfg, {{{FakeBlock::text_block("x")}, "end_turn"}});
    spawn_subagent(f.cfg, llm2, *f.sandbox, f.ctx, "general", "任务");
    CHECK(llm2.calls().size() == 1);
    // 模型名在 LlmRequest 里，FakeLlm 没记；这里只确认两条路径都走通了。
    CHECK_MSG(find_agent_type("explorer")->use_main_model == false, "便宜模型");
    CHECK_MSG(find_agent_type("general")->use_main_model == true, "边界不清用好模型");
}

TEST(the_subagent_system_prompt_replaces_the_identity) {
    Fixture f;
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("结论")}, "end_turn"}});
    spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");

    std::string sys;
    for (const auto& b : llm.calls().at(0).at("system")) sys += b.value("text", std::string{});
    CHECK_MSG(has(sys, "只读调查员"), "类型自己的 system 段要生效");
    CHECK_MSG(!has(sys, "You are a software engineering agent"),
              "identity 是覆盖不是叠加 —— 子 agent 是另一个角色");
}

TEST(an_empty_conclusion_says_so) {
    Fixture f;
    FakeLlm llm(f.cfg, {{{FakeBlock::text_block("")}, "end_turn"}});
    const auto out = spawn_subagent(f.cfg, llm, *f.sandbox, f.ctx, "explorer", "任务");
    CHECK_MSG(!out.empty(), "空结论要给个说明，不能返回空串让父 agent 自己猜");
}

// ── task 工具 ───────────────────────────────────────────────────────────────
TEST(task_tool_declares_itself_correctly) {
    const auto t = make_task_tool();
    CHECK(t->name() == "task");
    CHECK_MSG(!t->read_only(), "coder 子 agent 会改文件");
    CHECK_MSG(t->requires_permission(), "派子 agent 要过闸");
    CHECK_MSG(t->subject(Json{{"agent_type", "coder"}, {"prompt", "p"}}) == "coder",
              "审查对象是类型 —— 规则要能写 deny Task(coder)");
}

TEST(task_tool_refuses_when_spawning_is_off) {
    Fixture f;
    const auto t = make_task_tool();
    // ctx.spawn 为空 = 这里已经是子 agent 了，或者功能关掉了
    Json args{{"agent_type", "explorer"}, {"prompt", "任务"}};
    CHECK(t->run(args, f.ctx).is_error);
}

TEST(task_tool_validates_its_arguments) {
    Fixture f;
    f.ctx.spawn = [](std::string_view, std::string_view) { return std::string("ok"); };
    const auto t = make_task_tool();
    CHECK(t->run(Json{{"agent_type", "explorer"}}, f.ctx).is_error);
    CHECK(t->run(Json{{"prompt", "p"}}, f.ctx).is_error);
    CHECK(t->run(Json{{"agent_type", 42}, {"prompt", "p"}}, f.ctx).is_error);
    CHECK(!t->run(Json{{"agent_type", "explorer"}, {"prompt", "p"}}, f.ctx).is_error);
}

// ── task_graph 工具 ─────────────────────────────────────────────────────────
TEST(task_graph_runs_the_graph_and_passes_upstream_results) {
    Fixture f;
    std::vector<std::string> seen;
    f.ctx.spawn = [&seen](std::string_view, std::string_view prompt) {
        seen.emplace_back(prompt);
        return "结论(" + std::string(prompt.substr(0, 1)) + ")";
    };
    const auto t = make_task_graph_tool();
    Json args{{"tasks", Json::array({
        Json{{"id", "a"}, {"prompt", "A 任务"}},
        Json{{"id", "b"}, {"prompt", "B 任务"}, {"deps", Json::array({"a"})}},
    })}};
    const auto r = t->run(args, f.ctx);

    CHECK(!r.is_error);
    CHECK(r.metadata.at("done") == 2);
    // ⚠️ 上游结论要拼进下游的 prompt，否则 b 拿不到 a 干了什么 ——
    //    有依赖却不传结果，这个图就只是个执行顺序，白建了。
    const auto b_prompt = std::ranges::find_if(seen, [](const std::string& p) {
        return p.starts_with("B");
    });
    CHECK(b_prompt != seen.end());
    CHECK_MSG(has(*b_prompt, "结论(A)"), "下游要看到上游的结论");
}

TEST(a_cyclic_graph_is_refused_before_running) {
    Fixture f;
    int spawned = 0;
    f.ctx.spawn = [&spawned](std::string_view, std::string_view) { ++spawned; return std::string("x"); };
    const auto t = make_task_graph_tool();
    Json args{{"tasks", Json::array({
        Json{{"id", "a"}, {"prompt", "A"}, {"deps", Json::array({"b"})}},
        Json{{"id", "b"}, {"prompt", "B"}, {"deps", Json::array({"a"})}},
    })}};
    const auto r = t->run(args, f.ctx);

    // ⚠️ 不 validate 的话 run() 会安安静静什么都不做就返回 —— 调用方看到
    //    "成功"，而所有任务停在 pending。必须在跑之前报出来。
    CHECK_MSG(r.is_error, "成环要报错");
    CHECK_MSG(has(r.content, "a") && has(r.content, "b"), "报错要带上环的路径");
    CHECK_MSG(spawned == 0, "报错了就一个子 agent 都不该派");
}

TEST(a_missing_dependency_is_refused) {
    Fixture f;
    f.ctx.spawn = [](std::string_view, std::string_view) { return std::string("x"); };
    const auto t = make_task_graph_tool();
    Json args{{"tasks", Json::array({
        Json{{"id", "a"}, {"prompt", "A"}, {"deps", Json::array({"nope"})}},
    })}};
    const auto r = t->run(args, f.ctx);
    CHECK(r.is_error);
    CHECK(has(r.content, "nope"));
}

TEST(task_graph_validates_its_arguments) {
    Fixture f;
    f.ctx.spawn = [](std::string_view, std::string_view) { return std::string("x"); };
    const auto t = make_task_graph_tool();
    CHECK(t->run(Json::object(), f.ctx).is_error);
    CHECK(t->run(Json{{"tasks", "not-an-array"}}, f.ctx).is_error);
    CHECK(t->run(Json{{"tasks", Json::array({Json{{"id", "a"}}})}}, f.ctx).is_error);
    CHECK_MSG(t->run(Json{{"tasks", Json::array({
                  Json{{"id", "a"}, {"prompt", "p"}},
                  Json{{"id", "a"}, {"prompt", "q"}}})}}, f.ctx).is_error,
              "重复 id 要报错 —— 依赖指向哪一个就无从判断了");
}

// ── App 接线 ────────────────────────────────────────────────────────────────
TEST(app_registers_the_subagent_tools_and_wires_spawn) {
    Fixture f(/*with_tools=*/false);
    Config cfg = f.cfg;
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));
    CHECK(app.registry().get("task") != nullptr);
    CHECK(app.registry().get("task_graph") != nullptr);
}

TEST(app_omits_them_when_subagents_are_off) {
    Fixture f(/*with_tools=*/false);
    Config cfg = f.cfg;
    cfg.enable_subagents = false;
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));
    CHECK(app.registry().get("task") == nullptr);
    CHECK(app.registry().get("task_graph") == nullptr);
}

int main() { return mt::run_all(); }
