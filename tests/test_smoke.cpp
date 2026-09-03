//
// 冒烟测试 —— **这份文件就是规格说明**：每个用例锁住一条设计不变量。
//
// 全部离线（FakeLlm），不需要 API key，跑完不到一秒。
// 你的任务是让它们从红变绿。
//
//     cmake --build build && ./build/test_smoke
//
// 行为上的"标准答案"在 ../mini-swe-agent/（Python 参考实现），可以跑起来对照：
//     cd ../mini-swe-agent && MINI_AGENT_SRC=$PWD/reference python3 tests/test_smoke.py
//
// ── 一个 C++ 特有的注意点 ──────────────────────────────────────────────────
// FakeLlm 持有 `const Config&`。测试里 cfg 是局部变量，App 拿到的是它的拷贝 ——
// 两个对象值相同但地址不同。只要 cfg 活得比 app 长就没问题（这里是的）。
// 如果你把 FakeLlm 的构造挪进一个临时对象里，就会拿到悬垂引用 —— 这类生命周期
// 问题 Python 版本根本不存在，是 C++ 要自己盯的部分。
//
#include <filesystem>
#include <fstream>
#include <string>

#include "microtest.hpp"
#include "mini_agent/app.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/message.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/scheduler.hpp"
#include "mini_agent/session.hpp"

using namespace mini;
namespace fs = std::filesystem;

// --- 夹具 -------------------------------------------------------------------
namespace {

fs::path make_workspace() {
    auto dir = fs::temp_directory_path() / ("mini-agent-test-" + std::to_string(::getpid()) +
                                            "-" + std::to_string(mt::registry().size()));
    fs::create_directories(dir);
    std::ofstream(dir / "hello.py") << "def greet(name):\n    return f'hi {name}'\n";
    std::ofstream(dir / "README.md") << "# demo\n";
    return dir;
}

Config test_config(const fs::path& workdir, PermissionMode mode = PermissionMode::Yolo) {
    Config cfg;
    cfg.workdir = workdir;
    cfg.permission_mode = mode;
    cfg.stream = false;
    cfg.enable_mcp = false;
    cfg.normalize();
    return cfg;
}

/// 取第 index 条消息的第一个块，并断言它是 T 类型
template <class T>
const T& block_at(const Session& s, std::size_t msg_index, std::size_t block_index = 0) {
    const auto& blocks = s.messages().at(msg_index).content;
    return std::get<T>(blocks.at(block_index));
}

}  // namespace

// ===========================================================================
// Stage 1 —— 主循环
// ===========================================================================

TEST(loop_runs_tool_then_answers) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg, std::vector<FakeLlm::Turn>{
                 {{FakeBlock::tool("read", {{"path", "hello.py"}})}, "tool_use"},
                 {{FakeBlock::text_block("这是一个打招呼函数。")}, "end_turn"},
             });
    App app(cfg, std::move(llm));

    auto out = app.agent().run("看看 hello.py");

    CHECK(out.find("打招呼") != std::string::npos);
    // user / assistant(tool_use) / user(tool_result) / assistant(text)
    CHECK(app.session().messages().size() == 4);
    CHECK(app.session().messages()[2].role == Role::User);
    CHECK_MSG(std::holds_alternative<ToolResultBlock>(app.session().messages()[2].content[0]),
              "工具结果必须作为 tool_result 块回传");
}

TEST(all_tool_results_in_one_message) {
    // ⚠️ 拆成多条会让模型逐渐学会不再并行调工具
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg, std::vector<FakeLlm::Turn>{
                 {{FakeBlock::tool("glob", {{"pattern", "**/*.py"}}),
                   FakeBlock::tool("grep", {{"pattern", "greet"}})},
                  "tool_use"},
                 {{FakeBlock::text_block("找到了")}, "end_turn"},
             });
    App app(cfg, std::move(llm));
    app.agent().run("找找 greet");

    CHECK(app.session().messages().size() == 4);
    CHECK_MSG(app.session().messages()[2].content.size() == 2,
              "两个工具的结果必须打进同一条 user 消息");
}

TEST(max_steps_guard) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    cfg.max_steps = 3;
    std::vector<FakeLlm::Turn> script(
        10, {{FakeBlock::tool("read", {{"path", "hello.py"}})}, "tool_use"});
    auto* fake = new FakeLlm(cfg, script);
    App app(cfg, std::unique_ptr<LlmClient>(fake));

    app.agent().run("一直读");
    CHECK_MSG(fake->calls().size() == 3, "步数上限没生效");
}

TEST(tool_error_becomes_result_not_crash) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg, std::vector<FakeLlm::Turn>{
                 {{FakeBlock::tool("read", {{"path", "不存在.py"}})}, "tool_use"},
                 {{FakeBlock::text_block("文件不存在")}, "end_turn"},
             });
    App app(cfg, std::move(llm));

    auto out = app.agent().run("读一个不存在的文件");
    CHECK(block_at<ToolResultBlock>(app.session(), 2).is_error);
    CHECK(out == "文件不存在");
}

// ===========================================================================
// Stage 2 —— 工具与执行器
// ===========================================================================

TEST(edit_requires_read_first) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg,
        std::vector<FakeLlm::Turn>{
            {{FakeBlock::tool("edit", {{"path", "hello.py"}, {"old_string", "hi"},
                                       {"new_string", "hello"}})},
             "tool_use"},
            {{FakeBlock::text_block("done")}, "end_turn"},
        });
    App app(cfg, std::move(llm));
    app.agent().run("把 hi 改成 hello");

    const auto& r = block_at<ToolResultBlock>(app.session(), 2);
    CHECK(r.is_error);
    CHECK_MSG(r.content.find("read") != std::string::npos, "报错要指出必须先 read");
}

TEST(edit_after_read_succeeds) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg,
        std::vector<FakeLlm::Turn>{
            {{FakeBlock::tool("read", {{"path", "hello.py"}})}, "tool_use"},
            {{FakeBlock::tool("edit", {{"path", "hello.py"}, {"old_string", "hi "},
                                       {"new_string", "hello "}})},
             "tool_use"},
            {{FakeBlock::text_block("改好了")}, "end_turn"},
        });
    App app(cfg, std::move(llm));
    app.agent().run("把 hi 改成 hello");

    std::ifstream in(wd / "hello.py");
    std::string content((std::istreambuf_iterator<char>(in)), {});
    CHECK(content.find("hello {name}") != std::string::npos);
}

TEST(tool_schemas_are_sorted) {
    // ⚠️ 工具表渲染在 prompt 最前面，顺序一变整个缓存前缀就废了
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    App app(cfg, std::make_unique<FakeLlm>(cfg, std::vector<FakeLlm::Turn>{}));

    auto schemas = app.registry().schemas();
    std::string prev;
    for (const auto& s : schemas) {
        std::string name = s.at("name").get<std::string>();
        CHECK_MSG(prev <= name, "工具顺序不稳定会打穿缓存前缀");
        prev = name;
    }
}

// ===========================================================================
// Stage 3 —— 沙箱
// ===========================================================================

TEST(path_escape_blocked) {
    auto wd = make_workspace();
    auto cfg = test_config(wd);
    auto llm = std::make_unique<FakeLlm>(
        cfg, std::vector<FakeLlm::Turn>{
                 {{FakeBlock::tool("read", {{"path", "../../../etc/passwd"}})}, "tool_use"},
                 {{FakeBlock::text_block("读不到")}, "end_turn"},
             });
    App app(cfg, std::move(llm));
    app.agent().run("读 /etc/passwd");

    CHECK(block_at<ToolResultBlock>(app.session(), 2).is_error);
}

TEST(dangerous_command_denied) {
    auto wd = make_workspace();
    auto cfg = test_config(wd, PermissionMode::ReadOnly);
    auto llm = std::make_unique<FakeLlm>(
        cfg, std::vector<FakeLlm::Turn>{
                 {{FakeBlock::tool("bash", {{"command", "rm -rf /"}})}, "tool_use"},
                 {{FakeBlock::text_block("被拦了")}, "end_turn"},
             });
    App app(cfg, std::move(llm));
    app.agent().run("删库");

    CHECK(block_at<ToolResultBlock>(app.session(), 2).is_error);
}

TEST(command_is_split_before_checking) {
    // ⚠️ 这一条不做，前面所有权限设计都是装饰
    auto segs = Sandbox::split_command("ls && rm -rf / ; echo done | grep x");
    CHECK_MSG(segs.size() == 4, "必须按 && || ; | 拆段");
}

TEST(sandbox_rules_three_states) {
    auto wd = make_workspace();
    auto cfg = test_config(wd, PermissionMode::Ask);
    cfg.allow_rules = {"Bash(git status:*)"};
    Sandbox sb(cfg, {});   // 无 asker = 非交互

    CHECK(sb.check_command("git status --short").allowed());
    CHECK(sb.check_command("npm publish").action == Action::Ask);
    CHECK(!sb.check_command("sudo rm -rf /").allowed());

    auto rule = Rule::parse("Write(src/**)");
    CHECK(rule.has_value());
    CHECK(rule->matches("Write", "src/a/b.py"));
}

TEST(readonly_is_not_permissionless) {
    // read_only 只表示"不改本地文件"，不等于免授权。
    // 免授权的开关是 requires_permission()==false；一个只读但会出网的工具仍要过闸。
    struct Fetch final : Tool {
        std::string_view name() const override { return "fetch"; }
        std::string_view description() const override { return "抓 URL"; }
        Json input_schema() const override { return Json::object(); }
        bool read_only() const override { return true; }
        bool requires_permission() const override { return true; }
        ToolResult run(const Json&, ToolContext&) override { return {}; }
    };
    struct Peek final : Tool {
        std::string_view name() const override { return "peek"; }
        std::string_view description() const override { return "看本地"; }
        Json input_schema() const override { return Json::object(); }
        bool read_only() const override { return true; }
        bool requires_permission() const override { return false; }
        ToolResult run(const Json&, ToolContext&) override { return {}; }
    };

    auto wd = make_workspace();
    auto cfg = test_config(wd, PermissionMode::Ask);
    Sandbox sb(cfg, {});   // 非交互 → Ask 应该落到 Deny

    CHECK(sb.authorize(Fetch{}, Json{{"url", "https://x"}}).action == Action::Deny);
    CHECK(sb.authorize(Peek{}, Json::object()).allowed());
}

// ===========================================================================
// Stage 4 —— 上下文工程
// ===========================================================================

TEST(compaction_splits_on_user_boundary) {
    // ⚠️ 切分点不能把 tool_use 和它的 tool_result 拆散，否则下一轮直接 400
    Session s;
    s.messages() = {
        {Role::User, {TextBlock{"任务1"}}},
        {Role::Assistant, {ToolUseBlock{"t1", "read", Json::object()}}},
        {Role::User, {ToolResultBlock{"t1", "x", false}}},
        {Role::Assistant, {TextBlock{"好了"}}},
        {Role::User, {TextBlock{"任务2"}}},
        {Role::Assistant, {TextBlock{"在做"}}},
    };

    auto split = s.safe_split(3);
    const auto& first_kept = s.messages().at(split);
    CHECK(first_kept.role == Role::User);
    CHECK_MSG(!has_tool_result(first_kept), "切分点落在了 tool_result 上");
}

// ===========================================================================
// Stage 6 —— 调度器（注意：这一层不认识 LLM，所以测试塞 lambda 就够了）
// ===========================================================================

TEST(scheduler_respects_dependencies) {
    Scheduler sched(3);
    sched.add("a", "任务A");
    sched.add("b", "任务B");
    sched.add("c", "汇总", {"a", "b"});

    std::vector<std::string> order;
    std::mutex m;
    sched.run([&](const Task& t, const std::map<std::string, std::string>& up) {
        {
            std::lock_guard lk(m);
            order.push_back(t.id);
        }
        return t.id + " 完成 (上游 " + std::to_string(up.size()) + ")";
    });

    CHECK(order.size() == 3);
    CHECK_MSG(order[2] == "c", "c 依赖 a 和 b，必须最后跑");
    CHECK(sched.tasks().at("c").result.find("上游 2") != std::string::npos);
}

TEST(scheduler_detects_cycle) {
    Scheduler sched;
    sched.add("a", "A", {"b"});
    sched.add("b", "B", {"a"});
    auto err = sched.validate();
    CHECK_MSG(err.has_value(), "应该检测到依赖成环");
}

// ===========================================================================
// TODO: 下面这些留给你自己补（照着上面的样子写，Python 版有对应实现可参考）
//
//   memory_roundtrip                写入 → 索引 → 检索
//   skills_progressive_disclosure   索引里只有一行，load 之后才有正文
//   background_task_notified_once   后台任务结束通知只发一次
//   subagent_has_isolated_context   子 agent 用全新 Session，只回传结论
//   system_prompt_is_byte_stable    连续两次 build_system 结果完全一致
//   system_prompt_has_no_timestamp  system 里不许出现日期
//   mcp_handshake                   见 test_mcp.cpp（用 tests/mock_mcp_server.py）
//   interrupt_pairs_tool_results    中断时未完成的 tool_use 要补 error 结果
// ===========================================================================

int main() { return mt::run_all(); }
