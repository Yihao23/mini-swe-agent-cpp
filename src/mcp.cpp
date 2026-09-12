// 【Stage 7】MCP 客户端（stdio JSON-RPC）。
//
// 契约写在 include/mini_agent/mcp.hpp。这里只讲实现上的取舍。
//
// ── 为什么自己攒行缓冲，而不用 FILE* + getline ──────────────────────────────
//
// 因为要**超时**。一个第三方 server 卡住不答，用阻塞的 getline 就是整个 agent
// 跟着卡死，而且没有任何提示。要超时就得 poll，而 poll 和 FILE* 混用是经典的坑：
// poll 看的是内核里的管道，看不见 FILE* 自己那层用户态缓冲 —— 数据可能已经躺在
// 缓冲区里，poll 却说"没得读"。
//
// 所以走裸 fd：poll 等一段时间 → read 到自己的 inbuf → 从 inbuf 里切出一行。
// 这也和 background.cpp 里的读法一致，两处一个套路。

#include "mini_agent/mcp.hpp"

#include "spawn.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <mutex>

namespace mini {
namespace {

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

/// 一次请求最多等多久。
///
/// ⚠️ 必须有。第三方 server 起不来、卡在自己的初始化里、或者干脆不认识这个
///    method —— 没有超时的话表现是 agent 整个不动，而且不打印任何东西。
constexpr Ms kRequestTimeout{15000};

/// close() 时给 server 多久自己退出，然后上信号。
constexpr Ms kShutdownGrace{500};

/// 单行长度上界。
///
/// ⚠️ 防的是"对面根本不是 MCP server"。exec 到一个会无限输出的东西（比如
///    `yes`），没有上界就是把内存吃光。一行 8MB 已经比任何真实响应都大。
constexpr std::size_t kMaxLine = 8u * 1024 * 1024;

/// @brief Milliseconds left before `deadline`, clamped at 0.
///
/// @param deadline Absolute time point.
/// @return What poll() takes as its timeout; 0 once the deadline has passed.
///
/// @code
///   remaining_ms(now + 1500ms)   →  1500（左右）
///   remaining_ms(now - 10ms)     →  0        ← 过期不返回负数：poll(-1) 是"永远等"
/// @endcode
int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<Ms>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

struct McpClient::Impl {
    std::string name;
    pid_t pid = -1;
    int write_fd = -1;
    int read_fd = -1;
    std::string inbuf;          ///< 读到但还没切成整行的字节
    long next_id = 1;
    bool closed = false;
    std::string spawn_error;    ///< 非空 = 进程压根没起来

    /// ⚠️ 一根管道上的 JSON-RPC 帧不能交错。两个线程同时发请求，
    ///    两条消息会拌在一起，而且响应也分不清是谁的。整个「发+等」是一个原子操作。
    std::mutex mu;

    /// @brief Serialise one message and write it as a single line.
    ///
    /// @param msg A JSON-RPC request or notification.
    /// @return false when the pipe is broken — the server has exited.
    ///
    /// @code
    ///   send_line({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})
    ///
    ///   写进 server stdin 的字节：
    ///   {"id":2,"jsonrpc":"2.0","method":"tools/list","params":{}}\n
    /// @endcode
    ///
    /// @note Loops on partial writes. A pipe accepts at most 64KB at a time,
    ///       and a tools/call carrying a large file would otherwise be cut in
    ///       half mid-frame.
    bool send_line(const Json& msg) {
        const std::string line = msg.dump() + "\n";
        std::size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = ::write(write_fd, line.data() + sent, line.size() - sent);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// @brief Take one line off the server's stdout, waiting at most until `deadline`.
    ///
    /// @param deadline Absolute; shared across every line of one request.
    /// @param err      Filled in when nullopt is returned.
    /// @return The line without its newline; nullopt on timeout, EOF or error.
    ///
    /// @code
    ///   管道里到了：  {"method":"notifications/message",...}\n{"id":1,"res
    ///
    ///   第 1 次调用 → {"method":"notifications/message",...}
    ///                inbuf 剩下 {"id":1,"res            ← 半行留着，不丢
    ///   第 2 次调用 → poll 等更多字节 → 拼成整行再返回
    ///
    ///   server 退出、inbuf 为空 → nullopt，err = "server 已退出（stdout 到达 EOF）"
    ///   15 秒没等到换行符       → nullopt，err = "等 server 响应超时"
    /// @endcode
    std::optional<std::string> read_line(Clock::time_point deadline, std::string& err) {
        for (;;) {
            // 先看 inbuf 里有没有现成的一行 —— 上一次 read 很可能一次读回来好几行。
            if (const auto nl = inbuf.find('\n'); nl != std::string::npos) {
                std::string line = inbuf.substr(0, nl);
                inbuf.erase(0, nl + 1);
                return line;
            }
            if (inbuf.size() > kMaxLine) {
                err = "server 输出的一行超过 8MB，当它不是 MCP server";
                return std::nullopt;
            }

            const int wait = remaining_ms(deadline);
            if (wait == 0) {
                err = "等 server 响应超时";
                return std::nullopt;
            }
            pollfd pfd{read_fd, POLLIN, 0};
            const int n = ::poll(&pfd, 1, wait);
            if (n < 0) {
                if (errno == EINTR) continue;
                err = std::string("poll 失败: ") + std::strerror(errno);
                return std::nullopt;
            }
            if (n == 0) {
                err = "等 server 响应超时";
                return std::nullopt;
            }

            char buf[4096];
            const ssize_t got = ::read(read_fd, buf, sizeof buf);
            if (got < 0) {
                if (errno == EINTR) continue;
                err = std::string("read 失败: ") + std::strerror(errno);
                return std::nullopt;
            }
            if (got == 0) {
                // EOF：server 退出了。它退出前写的最后一行可能没有换行符。
                if (!inbuf.empty()) {
                    std::string line = std::move(inbuf);
                    inbuf.clear();
                    return line;
                }
                err = "server 已退出（stdout 到达 EOF）";
                return std::nullopt;
            }
            inbuf.append(buf, static_cast<std::size_t>(got));
        }
    }

    /// @brief Send one request and return the `result` of the response with its id.
    ///
    /// @param method The JSON-RPC method, e.g. "tools/list".
    /// @param params Its params object.
    /// @return The `result` member; an error for a JSON-RPC `error`, a broken
    ///         pipe, or a timeout.
    ///
    /// @code
    ///   request("initialize", {...})
    ///
    ///   → {"id":1,"jsonrpc":"2.0","method":"initialize","params":{...}}
    ///   ← starting up...                                     读不懂 → 跳过
    ///   ← {"jsonrpc":"2.0","method":"notifications/message"}  没有 id → 跳过
    ///   ← {"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05"}}
    ///                                                        id 对上 → 返回 result
    ///
    ///   ← {"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"method not found"}}
    ///       → unexpected("initialize 返回错误 -32601: method not found")
    /// @endcode
    ///
    /// ⚠️ 这个循环是这一层的核心。server 随时可能穿插发**通知** —— 没有 id 的
    ///    消息，比如日志、进度。不跳过它们的话，一条日志会被当成答案交给模型。
    ///    mock_mcp_server.py 专门在握手中间插了一条来测这件事。
    std::expected<Json, std::string> request(std::string_view method, Json params) {
        if (!spawn_error.empty()) return std::unexpected(spawn_error);
        if (closed) return std::unexpected("server 已关闭");

        std::lock_guard lock(mu);
        const long id = next_id++;
        const Json msg{{"jsonrpc", "2.0"}, {"id", id},
                       {"method", std::string(method)}, {"params", std::move(params)}};
        if (!send_line(msg))
            return std::unexpected(std::format("写 {} 请求失败：管道已断", method));

        const auto deadline = Clock::now() + kRequestTimeout;
        for (;;) {
            std::string err;
            const auto line = read_line(deadline, err);
            if (!line) return std::unexpected(std::format("{}：{}", method, err));
            if (line->empty()) continue;

            Json in;
            try {
                in = Json::parse(*line);
            } catch (const std::exception& e) {
                // ⚠️ 一行读不懂**不能**直接放弃 —— 有些 server 会往 stdout 打
                //    非 JSON 的启动横幅。跳过继续读，超时才是终止条件。
                (void)e;
                continue;
            }
            // 没有 id = 通知。跳过。
            if (!in.is_object() || !in.contains("id") || in["id"].is_null()) continue;
            // id 对不上 = 别人的响应（不该发生，但对面爱怎么写是它的事）。跳过。
            if (!in["id"].is_number_integer() || in["id"].get<long>() != id) continue;

            if (in.contains("error")) {
                const auto& e = in["error"];
                return std::unexpected(std::format(
                    "{} 返回错误 {}: {}", method,
                    e.value("code", 0), e.value("message", std::string{"(没有 message)"})));
            }
            return in.value("result", Json::object());
        }
    }

    /// @brief Send a notification: no id, and nothing comes back.
    ///
    /// @param method E.g. "notifications/initialized".
    /// @param params Its params object.
    /// @return false when the pipe is broken.
    ///
    /// @code
    ///   notify("notifications/initialized", {})
    ///   → {"jsonrpc":"2.0","method":"notifications/initialized","params":{}}
    ///   ← （什么都不会回来 —— 所以这里不能去读，读就是卡到超时）
    /// @endcode
    bool notify(std::string_view method, Json params) {
        if (!spawn_error.empty() || closed) return false;
        std::lock_guard lock(mu);
        return send_line(Json{{"jsonrpc", "2.0"},
                              {"method", std::string(method)},
                              {"params", std::move(params)}});
    }
};

// ─────────────────────────────────────────────────────────────────────────────

/// @brief Spawn the server; record, rather than throw, a failure to start.
///
/// Contract in mcp.hpp. What happens underneath:
///
/// @code
///   McpClient("github", "npx", {"-y", "@mcp/github"}, "/repo")
///     → spawn_piped: fork + execvp("npx", ["npx","-y","@mcp/github"])
///         子进程 stdin  ← 我们的 write_fd
///         子进程 stdout → 我们的 read_fd
///         子进程 stderr   原样继承（不进管道）
///
///   McpClient("nope", "no-such-program-xyz", {}, "/repo")
///     → fork 成功、execvp 失败、子进程 _exit(127)
///     → 构造照样返回；第一次请求读到 EOF，报"server 已退出"
/// @endcode
McpClient::McpClient(std::string name, std::string command, std::vector<std::string> args,
                     const fs::path& cwd)
    : impl_(std::make_unique<Impl>()) {
    impl_->name = std::move(name);

    const PipedProcess p = spawn_piped(command, args, cwd);
    if (!p.ok) {
        // ⚠️ 构造函数**不抛**。一个起不来的 server 只该赔上它自己的工具，
        //    而不是让整个 agent 起不来。错误存下来，第一次请求时如实返回。
        impl_->spawn_error = std::format("启动 {} 失败: {}", command, p.error);
        return;
    }
    impl_->pid = p.pid;
    impl_->write_fd = p.write_fd;
    impl_->read_fd = p.read_fd;
}

/// @brief Shuts the server down through close().
McpClient::~McpClient() {
    if (impl_) close();
}

/// @brief The handshake: one request, then one notification.
///
/// @code
///   → {"id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05",
///                                             "capabilities":{},"clientInfo":{...}}}
///   ← {"id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}}}}
///   → {"method":"notifications/initialized","params":{}}     ⚠️ 少了这条 server 不干活
///
///   返回：{"protocolVersion":"2024-11-05","capabilities":{"tools":{}}}
/// @endcode
std::expected<Json, std::string> McpClient::initialize() {
    auto caps = impl_->request(
        "initialize",
        Json{{"protocolVersion", kMcpProtocolVersion},
             {"capabilities", Json::object()},
             {"clientInfo", Json{{"name", "mini-swe-agent-cpp"}, {"version", "0.1"}}}});
    if (!caps) return caps;

    // ⚠️ 收到响应还没完 —— 协议要求再发一条 notifications/initialized。
    //    不发的话，那些等着它才开始服务的 server 会一直不答，而症状是**卡住**，
    //    不是报错：下一次 tools/list 一路等到 15 秒超时。
    if (!impl_->notify("notifications/initialized", Json::object()))
        return std::unexpected("发 notifications/initialized 失败：管道已断");

    return caps;
}

/// @brief Fetch the tool list and check that it is an array.
///
/// @code
///   → {"id":2,"method":"tools/list","params":{}}
///   ← {"id":2,"result":{"tools":[{"name":"echo","inputSchema":{...}}]}}
///   返回：[{"name":"echo","inputSchema":{...}}]
///
///   ← {"id":2,"result":{}}
///   返回：unexpected("tools/list 的响应里没有 tools 数组")
/// @endcode
std::expected<Json, std::string> McpClient::list_tools() {
    auto r = impl_->request("tools/list", Json::object());
    if (!r) return r;
    if (!r->contains("tools") || !(*r)["tools"].is_array())
        return std::unexpected("tools/list 的响应里没有 tools 数组");
    return (*r)["tools"];
}

/// @brief Call one remote tool and flatten its content blocks to text.
///
/// @code
///   call_tool("echo", {"text":"hi"})
///   → {"id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}
///   ← {"id":3,"result":{"content":[{"type":"text","text":"echo: hi"}],"isError":false}}
///   返回：{"echo: hi", false}
///
///   ← {"id":3,"result":{"content":[{"type":"image",...}],"isError":false}}
///   返回：{"(server 没有返回文本内容)", false}     ← 空串会被 API 拒掉
///
///   call_tool("echo", "not an object")
///   → arguments 被换成 {}                         ← 协议要求它是对象
/// @endcode
std::expected<std::pair<std::string, bool>, std::string> McpClient::call_tool(
    std::string_view name, const Json& args) {
    auto r = impl_->request("tools/call",
                            Json{{"name", std::string(name)},
                                 {"arguments", args.is_object() ? args : Json::object()}});
    if (!r) return std::unexpected(r.error());

    // MCP 的返回是一个 content 块数组（可能有 text、image 等）。模型这边只吃
    // 文本，所以把 text 块拼起来。⚠️ 一个块都没有时给一句占位 ——
    // ToolResult.content 不许为空，API 会拒。
    std::string text;
    if (r->contains("content") && (*r)["content"].is_array()) {
        for (const auto& block : (*r)["content"]) {
            if (!block.is_object()) continue;
            if (block.value("type", std::string{}) == "text")
                text += block.value("text", std::string{});
        }
    }
    if (text.empty()) text = "(server 没有返回文本内容)";

    return std::pair{std::move(text), r->value("isError", false)};
}

/// @brief The name given at construction; `"github"` for `mcp__github__*` tools.
const std::string& McpClient::name() const { return impl_->name; }

/// @brief Ask the server to leave, then make it.
///
/// @code
///   close()
///     ① close(write_fd)            server 在 stdin 上读到 EOF，正常情况下自己退出
///     ② waitpid(WNOHANG) 轮询 500ms
///     ③ 还没走 → kill(-pgid, SIGTERM)，100ms 后 SIGKILL
///     ④ close(read_fd)，pid = -1
///
///   close()   再调一次 → 直接返回（closed 已经是 true）
/// @endcode
void McpClient::close() {
    if (impl_->closed) return;      // 幂等 —— 析构函数也会调
    impl_->closed = true;
    if (impl_->pid < 0) return;     // 压根没起来

    // ⚠️ 关 stdin 是**正常的收场方式**：server 在 stdin 上读到 EOF 就该自己退。
    //    直接发信号也能杀掉，但那样 server 没机会保存状态、清理临时文件。
    if (impl_->write_fd >= 0) {
        ::close(impl_->write_fd);
        impl_->write_fd = -1;
    }

    // 给它一小会儿自己退。WNOHANG 轮询，不阻塞。
    const auto deadline = Clock::now() + kShutdownGrace;
    int status = 0;
    bool reaped = false;
    while (Clock::now() < deadline) {
        const pid_t r = ::waitpid(impl_->pid, &status, WNOHANG);
        if (r == impl_->pid) { reaped = true; break; }
        if (r < 0 && errno != EINTR) { reaped = true; break; }
        ::usleep(10 * 1000);
    }
    if (!reaped) {
        // 不肯走就动手。杀**进程组** —— npx 起的 node 是孙子进程。
        ::kill(-impl_->pid, SIGTERM);
        ::usleep(100 * 1000);
        ::kill(-impl_->pid, SIGKILL);
        while (::waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {}
    }

    if (impl_->read_fd >= 0) {
        ::close(impl_->read_fd);
        impl_->read_fd = -1;
    }
    impl_->pid = -1;
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// 一个远程工具，长得和本地工具一模一样。
///
/// @code
///   McpTool(client, "mcp__github__search", "search", "Search issues", schema)
///     name()                →  "mcp__github__search"   模型和规则看到的
///     run({"q":"bug"}, ctx) →  client->call_tool("search", {"q":"bug"})
///                                                      server 看到的是没前缀的名字
/// @endcode
///
/// ⚠️ executor 和 sandbox 完全看不出它是远程的 —— 这就是 Stage 2 把 Tool
///    抽象成纯虚接口的回报：加一整类新能力，两个调用方一行都不用改。
class McpTool final : public Tool {
  public:
    McpTool(std::shared_ptr<McpClient> client, std::string prefixed, std::string remote,
            std::string description, Json schema)
        : client_(std::move(client)),
          prefixed_(std::move(prefixed)),
          remote_(std::move(remote)),
          description_(std::move(description)),
          schema_(std::move(schema)) {}

    std::string_view name() const override { return prefixed_; }
    std::string_view description() const override { return description_; }
    Json input_schema() const override { return schema_; }

    // ⚠️ 两个都取最保守的值。第三方 server 干什么我们不知道，而"它可能在删
    //    你的文件"这个假设错了只是慢一点，反过来错了是丢数据。
    bool read_only() const override { return false; }
    bool requires_permission() const override { return true; }

    /// @brief Forward the call; keep transport failure and tool failure apart.
    ///
    /// @param args Forwarded to the server untouched.
    /// @return The server's text with its isError, or an error for a broken pipe
    ///         or timeout.
    ///
    /// @code
    ///   server 回 {"content":[...], "isError":true}  →  ToolResult{正文, is_error=true}
    ///   管道断了 / 超时                              →  ToolResult::error("MCP 调用失败：…")
    /// @endcode
    ToolResult run(const Json& args, ToolContext&) override {
        const auto r = client_->call_tool(remote_, args);
        // 两种失败要分开：传输层挂了（管道断、超时）和 server 说这次调用失败。
        // 前者模型重试也没用，后者它多半能改参数再来。
        if (!r) return ToolResult::error(std::format("MCP 调用失败：{}", r.error()));
        return ToolResult{.content = r->first, .is_error = r->second};
    }

  private:
    std::shared_ptr<McpClient> client_;   ///< ⚠️ 工具活着，server 就活着
    std::string prefixed_, remote_, description_;
    Json schema_;
};

}  // namespace

/// @brief Read mcp.json, start every server in it, collect their tools.
///
/// Contract in mcp.hpp. Input and output:
///
/// @code
///   .mini-agent/mcp.json
///   {"mcpServers": {
///      "github": {"command": "npx", "args": ["-y", "@mcp/github"]},
///      "broken": {"command": "no-such-program-xyz"},
///      "oops":   {"args": ["x"]}
///   }}
///
///   返回
///     tools   = [mcp__github__search, mcp__github__get_issue, ...]
///     clients = [github]
///     errors  = ["MCP server broken 握手失败：initialize：server 已退出（stdout 到达 EOF）",
///                "MCP server oops 少了 command"]
///
///   文件不存在 → {tools=[], clients=[], errors=[]}      ← 没配 MCP 是常态，不报警
/// @endcode
McpLoadResult load_mcp_servers(const fs::path& config_path, const fs::path& cwd) {
    McpLoadResult out;

    std::error_code ec;
    if (!fs::exists(config_path, ec)) return out;   // 没配就是没配，不是错误

    Json cfg;
    {
        std::ifstream in(config_path, std::ios::binary);
        if (!in) {
            out.errors.push_back(std::format("读不了 {}", config_path.string()));
            return out;
        }
        try {
            in >> cfg;
        } catch (const std::exception& e) {
            out.errors.push_back(std::format("{} 不是合法的 JSON: {}",
                                             config_path.string(), e.what()));
            return out;
        }
    }
    if (!cfg.is_object() || !cfg.contains("mcpServers") || !cfg["mcpServers"].is_object()) {
        out.errors.push_back(std::format("{} 里没有 mcpServers 对象", config_path.string()));
        return out;
    }

    for (const auto& [server_name, spec] : cfg["mcpServers"].items()) {
        // ⚠️ 每个 server 的失败都在这一轮里就地记账、continue。一个起不来的
        //    server 只该赔上它自己的工具 —— 为了 mcp.json 里的一行写错就不让
        //    agent 启动，代价比收益大得多。
        if (!spec.is_object() || !spec.contains("command") || !spec["command"].is_string()) {
            out.errors.push_back(std::format("MCP server {} 少了 command", server_name));
            continue;
        }
        std::vector<std::string> args;
        if (spec.contains("args") && spec["args"].is_array())
            for (const auto& a : spec["args"])
                if (a.is_string()) args.push_back(a.get<std::string>());

        auto client = std::make_shared<McpClient>(server_name, spec["command"].get<std::string>(),
                                                  std::move(args), cwd);
        if (const auto caps = client->initialize(); !caps) {
            out.errors.push_back(std::format("MCP server {} 握手失败：{}",
                                             server_name, caps.error()));
            continue;
        }
        const auto tools = client->list_tools();
        if (!tools) {
            out.errors.push_back(std::format("MCP server {} 列不出工具：{}",
                                             server_name, tools.error()));
            continue;
        }

        for (const auto& t : *tools) {
            if (!t.is_object()) continue;
            const auto remote = t.value("name", std::string{});
            if (remote.empty()) continue;
            Json schema = t.value("inputSchema", Json::object());
            if (!schema.is_object()) schema = Json::object();
            out.tools.push_back(std::make_shared<McpTool>(
                client,
                // ⚠️ 前缀。两个 server 都提供 read_file 时不撞名，而且权限规则
                //    能精确到某一个 server：deny Mcp__github__*
                std::format("mcp__{}__{}", server_name, remote), remote,
                t.value("description", std::format("{} 提供的 {}", server_name, remote)),
                std::move(schema)));
        }
        // ⚠️ 必须留着。工具持有 shared_ptr，理论上够了 —— 但一个不提供任何
        //    工具的 server 就没人持有它，会当场析构。存在这里才活得下去。
        out.clients.push_back(std::move(client));
    }

    return out;
}

}  // namespace mini
