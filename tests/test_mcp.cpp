// 【Stage 7】MCP 客户端。用 tests/mock_mcp_server.py 当对面。
//
// 这一层最容易写错的三件事，每件都有专门的用例：
//   * 响应要按 **id** 匹配。server 随时可能穿插发通知（没有 id 的消息），
//     不跳过的话，一条日志会被当成答案交给模型。mock server 故意在握手
//     中间插了一条。
//   * 握手收到响应还没完，必须再发 notifications/initialized。
//   * 一个 server 起不来，只能赔上它自己的工具 —— 不能让 agent 起不来。
//
// 另外几个用例把"对面根本不是 MCP server"的各种样子都试一遍：不存在的程序、
// 一言不发的、只会吐垃圾的、马上就退出的。它们的共同要求是**别卡死**。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"

#include <unistd.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace mini;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

const fs::path kMockServer = fs::path(MINI_AGENT_TEST_DIR) / "mock_mcp_server.py";

/// 一个"较真"的 server：没收到 notifications/initialized 之前什么都不给。
const fs::path kStrictServer = fs::path(MINI_AGENT_TEST_DIR) / "mock_mcp_strict.py";

struct Fixture {
    fs::path root;

    Fixture() {
        root = fs::temp_directory_path() /
               ("mcp_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    /// 写一个 mcp.json，返回它的路径。
    fs::path write_config(const std::string& body) const {
        const auto p = root / "mcp.json";
        std::ofstream(p, std::ios::binary) << body;
        return p;
    }
    /// 指向 mock server 的一份标准配置。
    fs::path mock_config(const std::string& name = "mock") const {
        return write_config(R"({"mcpServers":{")" + name +
                            R"(":{"command":"python3","args":[")" + kMockServer.string() +
                            R"("]}}})");
    }
};

std::shared_ptr<McpClient> mock_client() {
    return std::make_shared<McpClient>("mock", "python3",
                                       std::vector<std::string>{kMockServer.string()},
                                       fs::temp_directory_path());
}

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ── 握手 ────────────────────────────────────────────────────────────────────

TEST(the_handshake_steps_over_the_notification_in_the_middle) {
    const auto c = mock_client();
    const auto caps = c->initialize();
    // ⚠️ mock server 在回 initialize 之前先发了一条 notifications/message。
    //    按 id 匹配才能跳过它；不跳的话这里拿到的是那条日志，
    //    caps 里根本没有 protocolVersion。
    CHECK_MSG(caps.has_value(), caps ? "" : caps.error().c_str());
    CHECK(caps->value("protocolVersion", std::string{}) == "2024-11-05");
    CHECK(caps->contains("capabilities"));
}

TEST(the_server_is_told_the_handshake_is_finished) {
    // ⚠️ 收到 initialize 的响应还没完 —— 协议要求再发一条
    //    notifications/initialized。普通的 mock server 不等它，所以那一句
    //    删掉了也没人发现。这个 server 会等。
    //
    // ⚠️ 一处刻意的不忠实：真实 server 多半是**不答**，症状是卡到 15 秒超时。
    //    这个改成回一条 JSON-RPC error，好让用例毫秒级完成。代价是测的是
    //    "客户端确实发了那条通知"，而不是"不发会卡死"。
    McpClient c("strict", "python3", {kStrictServer.string()}, fs::temp_directory_path());
    CHECK(c.initialize().has_value());

    const auto tools = c.list_tools();
    CHECK_MSG(tools.has_value(),
              tools ? "" : ("握手后仍拿不到工具，多半是没发 initialized：" +
                            tools.error()).c_str());
    CHECK(tools->size() == 1);
}

TEST(tools_can_be_listed_after_the_handshake) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());
    const auto tools = c->list_tools();
    CHECK_MSG(tools.has_value(), tools ? "" : tools.error().c_str());
    CHECK(tools->size() == 1);
    CHECK((*tools)[0].value("name", std::string{}) == "echo");
    CHECK((*tools)[0].contains("inputSchema"));
}

TEST(a_remote_tool_can_be_called) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());
    const auto r = c->call_tool("echo", Json{{"text", "你好"}});
    CHECK_MSG(r.has_value(), r ? "" : r.error().c_str());
    CHECK_MSG(has(r->first, "你好"), "content 里的 text 块要被拼出来");
    CHECK_MSG(!r->second, "isError 为 false");
}

TEST(several_calls_reuse_one_server) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());
    // ⚠️ id 每次都要递增，而且每次都要读到自己那条。共用一个计数器写错的话，
    //    第二次调用会拿到第一次的响应 —— 而两次响应长得很像，很难看出来。
    for (int i = 0; i < 5; ++i) {
        const auto r = c->call_tool("echo", Json{{"text", std::to_string(i)}});
        CHECK(r.has_value());
        CHECK_MSG(has(r->first, std::to_string(i)), "第 i 次要拿到第 i 次的答案");
    }
}

TEST(the_client_reports_its_name) {
    CHECK(mock_client()->name() == "mock");
}

// ── 对面不是 MCP server 的各种样子 ──────────────────────────────────────────

TEST(a_command_that_does_not_exist_fails_instead_of_hanging) {
    McpClient c("nope", "definitely-not-a-real-program-xyz", {}, fs::temp_directory_path());
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = c.initialize();
    // execvp 失败时子进程 _exit(127)，管道随之关闭 → 我们读到 EOF。
    // ⚠️ 要的是"很快失败"，不是"等满 15 秒超时"。
    CHECK(!r.has_value());
    CHECK_MSG(std::chrono::steady_clock::now() - t0 < 3s, "要立刻失败，不能等到超时");
}

TEST(a_server_that_exits_at_once_fails_instead_of_hanging) {
    McpClient c("quiet", "true", {}, fs::temp_directory_path());
    const auto r = c.initialize();
    CHECK(!r.has_value());
    CHECK_MSG(has(r.error(), "initialize"), "错误信息要说清是哪一步挂的");
}

TEST(junk_on_stdout_is_skipped_not_taken_as_an_answer) {
    // 有些 server 会往 stdout 打启动横幅。读不懂的一行要跳过继续读，
    // 而不是当场放弃 —— 否则一条无害的横幅就废掉整个 server。
    const auto script =
        "import sys,json\n"
        "sys.stdout.write('starting up...\\n'); sys.stdout.flush()\n"
        "for line in sys.stdin:\n"
        "    m=json.loads(line)\n"
        "    if m.get('id') is not None:\n"
        "        sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':m['id'],"
        "'result':{'protocolVersion':'2024-11-05','capabilities':{}}})+'\\n')\n"
        "        sys.stdout.flush()\n";
    McpClient c("banner", "python3", {"-c", script}, fs::temp_directory_path());
    const auto r = c.initialize();
    CHECK_MSG(r.has_value(), r ? "" : r.error().c_str());
}

TEST(an_error_response_is_reported_not_treated_as_a_result) {
    const auto script =
        "import sys,json\n"
        "for line in sys.stdin:\n"
        "    m=json.loads(line)\n"
        "    if m.get('id') is not None:\n"
        "        sys.stdout.write(json.dumps({'jsonrpc':'2.0','id':m['id'],"
        "'error':{'code':-32601,'message':'method not found'}})+'\\n')\n"
        "        sys.stdout.flush()\n";
    McpClient c("angry", "python3", {"-c", script}, fs::temp_directory_path());
    const auto r = c.initialize();
    CHECK(!r.has_value());
    // ⚠️ JSON-RPC 的 error 是一条**成功送达的响应**，很容易被当成 result
    //    收下来。收下来的话模型会拿着一个空对象继续推理。
    CHECK(has(r.error(), "method not found"));
}

TEST(closing_twice_is_fine) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());
    c->close();
    c->close();      // 幂等 —— 析构函数还会再调一次
    // 关掉之后再请求要报错，不能卡住也不能崩。
    CHECK(!c->list_tools().has_value());
}

// ── load_mcp_servers ────────────────────────────────────────────────────────

TEST(a_missing_config_file_is_not_an_error) {
    Fixture f;
    const auto r = load_mcp_servers(f.root / "没有这个文件.json", f.root);
    CHECK(r.tools.empty());
    CHECK_MSG(r.errors.empty(), "没配 MCP 是常态，不该报警告");
}

TEST(servers_from_the_config_contribute_prefixed_tools) {
    Fixture f;
    const auto r = load_mcp_servers(f.mock_config(), f.root);
    CHECK_MSG(r.errors.empty(), r.errors.empty() ? "" : r.errors[0].c_str());
    CHECK(r.tools.size() == 1);
    // ⚠️ 前缀是两个 server 都提供 read_file 时不撞名的唯一保证，
    //    也是权限规则能写 deny Mcp__github__* 的前提。
    CHECK(r.tools[0]->name() == "mcp__mock__echo");
    CHECK(r.clients.size() == 1);
}

TEST(the_server_name_goes_into_the_prefix) {
    Fixture f;
    const auto r = load_mcp_servers(f.mock_config("github"), f.root);
    CHECK(r.tools.size() == 1);
    CHECK_MSG(r.tools[0]->name() == "mcp__github__echo", "前缀取的是配置里的名字");
}

TEST(a_remote_tool_behaves_like_any_other_tool) {
    Fixture f;
    const auto r = load_mcp_servers(f.mock_config(), f.root);
    CHECK(r.tools.size() == 1);
    const auto& t = r.tools[0];

    // ⚠️ 两个标记都取最保守的值。第三方 server 干什么我们不知道。
    CHECK_MSG(!t->read_only(), "不知道它改不改文件 → 不并发");
    CHECK_MSG(t->requires_permission(), "不知道它有什么副作用 → 必须过闸");
    CHECK(t->input_schema().contains("properties"));

    Config cfg;
    cfg.workdir = f.root;
    cfg.permission_mode = PermissionMode::Yolo;
    cfg.normalize();
    Sandbox sandbox(cfg);
    Session session;
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.sandbox = &sandbox;
    ctx.session = &session;

    const auto res = t->run(Json{{"text", "隔着管道"}}, ctx);
    CHECK(!res.is_error);
    CHECK(has(res.content, "隔着管道"));
}

TEST(one_broken_server_does_not_take_down_the_others) {
    Fixture f;
    f.write_config(R"({"mcpServers":{
        "broken":{"command":"definitely-not-a-real-program-xyz"},
        "mock":{"command":"python3","args":[")" + kMockServer.string() + R"("]}}})");
    const auto r = load_mcp_servers(f.root / "mcp.json", f.root);
    // ⚠️ 这一条是整个 load 函数的设计理由。抛异常的话，mcp.json 里一行写错
    //    就让 agent 起不来 —— 代价比收益大得多。
    CHECK_MSG(r.tools.size() == 1, "好的那个照样能用");
    CHECK(r.tools[0]->name() == "mcp__mock__echo");
    CHECK_MSG(r.errors.size() == 1, "坏的那个记一条警告");
    CHECK(has(r.errors[0], "broken"));
}

TEST(a_malformed_config_is_a_warning_not_a_crash) {
    Fixture f;
    f.write_config("{ 这不是 JSON");
    const auto r = load_mcp_servers(f.root / "mcp.json", f.root);
    CHECK(r.tools.empty());
    CHECK(r.errors.size() == 1);
}

TEST(a_server_entry_without_a_command_is_a_warning) {
    Fixture f;
    f.write_config(R"({"mcpServers":{"oops":{"args":["x"]}}})");
    const auto r = load_mcp_servers(f.root / "mcp.json", f.root);
    CHECK(r.tools.empty());
    CHECK(r.errors.size() == 1);
    CHECK(has(r.errors[0], "oops"));
}

// ── 并发：task_graph 的几个子 agent 可能同时调用同一个 server 的工具 ─────────

TEST(concurrent_calls_on_one_client_each_get_their_own_answer) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());

    // ⚠️ 每个线程**调很多次**，而不是一次。mock server 一次应答只要几毫秒，
    //    每个线程只调一次的话，访问在时间上可能根本不交错 —— task_graph 那条
    //    并发测试就栽过：只调一次时去掉锁，TSan 一份报告都不出。
    const int kThreads = 4, kCalls = 25;
    std::atomic<int> wrong{0}, failed{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < kCalls; ++i) {
                const std::string text = std::format("t{}-{}", t, i);
                const auto r = c->call_tool("echo", Json{{"text", text}});
                // 每次文本都不同。锁失效时帧会在管道里交错，或者 A 读到 B 的响应 ——
                // 那样拿回来的就不是自己发出去的那段文字。
                if (!r) ++failed;
                else if (r->first != "echo: " + text) ++wrong;
            }
        });
    for (auto& th : ts) th.join();

    CHECK_MSG(failed == 0, "并发调用不该失败");
    CHECK_MSG(wrong == 0, "每个线程都要拿回自己那一次的答案");
}

TEST(closing_while_another_thread_is_calling_neither_crashes_nor_hangs) {
    const auto c = mock_client();
    CHECK(c->initialize().has_value());

    std::atomic<bool> saw_ok{false};
    std::atomic<int> after_close_ok{0};
    std::atomic<bool> closed{false};
    std::thread caller([&] {
        // 一直调到出错为止（最多 2000 次，免得出 bug 时停不下来）
        for (int i = 0; i < 2000; ++i) {
            // ⚠️ 在调用**开始之前**看标志。调用返回之后再看的话，一次在 close()
            //    之前就已经成功的调用，可能恰好在主线程置位之后才检查 ——
            //    被误算成「关闭之后还成功了」，这条用例就会偶发地红。
            const bool started_after_close = closed.load();
            const auto r = c->call_tool("echo", Json{{"text", "x"}});
            if (!r) return;
            saw_ok = true;
            if (started_after_close) ++after_close_ok;
        }
    });

    // 等调用方真的在跑，再从另一个线程关掉。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!saw_ok && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(saw_ok.load());

    const auto t0 = std::chrono::steady_clock::now();
    c->close();
    closed = true;
    caller.join();

    // ⚠️ close() 要等正在进行的那次请求结束，而不是在它脚下把 fd 关掉。
    //    在脚下关掉的话，那个 fd 号会被本进程下一个 open() 复用，
    //    正在 read() 的线程读到的就是别的文件 —— 没有症状，只有坏数据。
    CHECK_MSG(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5),
              "close() 不能卡住");
    CHECK_MSG(after_close_ok == 0, "close() 返回之后，调用不该再成功");
    CHECK(!c->call_tool("echo", Json{{"text", "x"}}).has_value());
}

// ── 按 server 写权限规则 ────────────────────────────────────────────────────

TEST(a_deny_rule_on_the_server_prefix_blocks_its_remote_tools) {
    Fixture f;
    const auto r = load_mcp_servers(f.mock_config(), f.root);
    CHECK(r.tools.size() == 1);
    const auto& tool = *r.tools[0];

    auto make_cfg = [&](std::vector<std::string> deny) {
        Config cfg;
        cfg.workdir = f.root;
        cfg.permission_mode = PermissionMode::Yolo;   // 没有规则命中就放行
        cfg.deny_rules = std::move(deny);
        cfg.normalize();
        return cfg;
    };

    // 反面对照：没有规则时 yolo 放行。否则下面的 Deny 可能根本不是规则造成的。
    const Config open = make_cfg({});
    Sandbox open_sb(open);
    CHECK(open_sb.authorize(tool, Json{{"text", "hi"}}).action == Action::Allow);

    // ⚠️ 前缀之所以存在，就是为了能按 server 写规则。以前 Rule::matches 对工具名
    //    逐字符比较，这条规则解析成功、不报任何警告，然后什么都拦不住。
    const Config closed = make_cfg({"Mcp__mock__*"});
    Sandbox closed_sb(closed);
    CHECK(closed_sb.warnings().empty());
    CHECK_MSG(closed_sb.authorize(tool, Json{{"text", "hi"}}).action == Action::Deny,
              "deny Mcp__mock__* 要真的拦住 mock 这个 server 的工具");
}

int main() { return mt::run_all(); }
