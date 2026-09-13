#pragma once
/// @file
/// @brief MCP client over stdio — external processes contributing tools (Stage 7).
//
// 【Stage 7】MCP 客户端（stdio 传输）—— 把外部进程提供的工具接进本 agent 的工具表。
//
// 去掉所有术语：**JSON-RPC 2.0，一行一条消息，走子进程的 stdin/stdout**。
//
//     → initialize                  告诉对方我是谁、协议版本
//     ← 服务端能力
//     → notifications/initialized   通知（没有回复）
//     → tools/list                  拿工具表
//     → tools/call                  调用
//
// 接进来之后，每个远程工具包成一个普通的 Tool ——
// executor 和 sandbox 完全不知道它是远程的。**这就是 Stage 2 把 Tool 抽象
// 设计对的回报。**
//
// ── C++ 要自己处理的两件事 ─────────────────────────────────────────────────
//   * 双向管道：pipe() 两次 + fork + dup2，父进程拿到可读可写的两个 fd。
//     用 FILE* + fdopen 会比裸 read/write 好写（能用 getline）。
//   * 响应匹配：服务端可能穿插发通知（没有 id），必须循环读到 id 对上的那条才返回。
//     tests/mock_mcp_server.py 会故意在握手中间插一条通知来测你这一点。
//
// ── 握手和一次调用 ──────────────────────────────────────────────────────────
//
//   记号见 executor.hpp 的图例：──▶ 同步 / ══▶ fork-join / ──▷ 异步
//
//   load_mcp_servers(config_path, cwd)
//        │  读 .mini-agent/mcp.json；文件不存在**不是错误**
//        │
//        └─ 对每个 server ──▶ McpClient(name, command, args, cwd)
//                                  │  fork + 两根管道（stdin / stdout）
//                                  ▼
//                             ──▶ initialize()
//                                  │  发 {"method":"initialize", ...}
//                                  │  收 capabilities
//                                  │  ⚠️ **然后必须再发一条**
//                                  │     notifications/initialized
//                                  │     不发的话，等它的 server 会一直不答，
//                                  │     症状是**卡住**而不是报错
//                                  ▼
//                             ──▶ list_tools()
//                                  │  → [{name, description, inputSchema}, ...]
//                                  ▼
//                          包一层 Tool，名字加前缀
//                          mcp__<server>__<tool>
//        │
//        ▼
//   McpLoadResult{ tools, clients, errors }
//        │        │        │
//        │        │        └── 起不来的 server 记在这，**不抛**。
//        │        │            丢掉一个 server 的工具，好过整个 agent 起不来
//        │        └── 一个工具都不提供的 server 靠它活着。
//        │            有工具的 server 不靠它：每个工具自己持有 client 的
//        │            shared_ptr，只要工具还在注册表里，server 就在
//        └── 直接 registry.add() 就能用
//
//   模型调用一个远程工具：
//
//        tool->run(args, ctx) ──▶ client->call_tool(name, args)
//             │                       │  写一行 JSON-RPC 到 stdin
//             │                       │  从 stdout 读，直到 id 对上
//             │                       │  ⚠️ 中间可能夹着**通知**（没有 id 的消息），
//             │                       │     要跳过它们继续读。不跳的话，
//             │                       │     一条日志会被当成答案返回给模型
//             │                       ▼
//             │                  {content_text, is_error}
//             ▼
//        ToolResult
//
// ── 名字为什么要加前缀 ──────────────────────────────────────────────────────
//
//     filesystem server  提供 read_file
//     github server      也提供 read_file
//         → mcp__filesystem__read_file / mcp__github__read_file
//
//   两个好处：不撞名；权限规则能按 server 写 —— deny 规则 `Mcp__github__*`
//   盖住 github 这个 server 的全部工具。Rule::matches 对工具名也走 glob，
//   这条规则才生效；以前是逐字符比较，它什么都匹配不到。
//
// ── 远程工具的两个标记都取保守值 ────────────────────────────────────────────
//
//     read_only()            false   ┐ 第三方 server 干什么我们不知道，
//     requires_permission()  true    ┘ 安全的假设就是最贵的那个
//
//   代价是远程工具永远不并发、永远过闸。相比"它可能在删你的文件"，这不算什么。
//
// ── 两条这一层保证不了的 ────────────────────────────────────────────────────
//
//   * server 实际做了什么。两个标记取保守值只决定"要不要问、能不能并发"，
//     问过之后它删了什么，这一层看不见。
//   * 工具名。它来自第三方 server，里面要是带 `*` `?` `[`，想精确点名它的
//     权限规则会被当成通配。
//
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mini_agent/tool.hpp"

namespace mini {

namespace fs = std::filesystem;

/// @brief The MCP revision this client speaks, sent during the handshake.
inline constexpr const char* kMcpProtocolVersion = "2024-11-05";

/// @brief One external MCP server, spoken to over stdio.
///
/// @warning Owns a child process and two pipes. The destructor closes them;
///          a leaked server is a process nobody knows about, still holding
///          whatever it opened.
///
/// @note Non-copyable — two clients writing to one pipe interleave their
///       JSON-RPC frames and neither gets a coherent answer.
/// @note pimpl: the implementation carries a pid, two file descriptors and a
///       request counter, none of which belongs in a header.
class McpClient {
  public:
    /// @brief Start a server process.
    /// @param name    How its tools are prefixed, e.g. "filesystem".
    /// @param command The executable.
    /// @param args    Its arguments.
    /// @param cwd     Working directory for the child.
    ///
    /// @note Does not throw. A command that cannot be started is recorded and
    ///       reported by the first request, so one bad server costs its own
    ///       tools rather than the agent's startup.
    ///
    /// @code{.test}
    /// @setup McpClient bad("nope", "no-such-program-xyz", {}, doc_workdir());
    /// bad.initialize().has_value()   ==>   false
    /// bad.name()                     ==>   "nope"
    /// @endcode
    McpClient(std::string name, std::string command, std::vector<std::string> args,
              const fs::path& cwd);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    /// @brief Perform the handshake.
    ///
    /// @return The server's capabilities, or a description of what went wrong.
    ///
    /// @warning `notifications/initialized` must be sent after the response
    ///          arrives. Servers that wait for it will answer nothing until it
    ///          does, and the symptom is a hang rather than an error.
    ///
    /// @note An error is returned rather than thrown: one external server that
    ///       will not start should cost its own tools, not the whole agent.
    ///
    /// The mock server sends a `notifications/message` **before** its answer.
    /// The capabilities below are only reachable because the client matched the
    /// response by id and stepped over that notification:
    ///
    /// @code{.test}
    /// @setup McpClient c("mock", "python3", {doc_mock_mcp_server()}, doc_workdir());
    /// @setup const auto caps = c.initialize();
    /// caps.has_value()                                   ==>   true
    /// caps->value("protocolVersion", std::string{})      ==>   "2024-11-05"
    /// caps->contains("capabilities")                     ==>   true
    /// @endcode
    std::expected<Json, std::string> initialize();

    /// @brief Ask what tools the server offers.
    /// @return The `tools` array, or an error description.
    ///
    /// @code{.test}
    /// @setup McpClient c("mock", "python3", {doc_mock_mcp_server()}, doc_workdir());
    /// @setup (void)c.initialize();
    /// @setup const auto tools = c.list_tools();
    /// tools->size()                                      ==>   1u
    /// (*tools)[0].value("name", std::string{})           ==>   "echo"
    /// (*tools)[0].contains("inputSchema")                ==>   true
    /// @endcode
    std::expected<Json, std::string> list_tools();

    /// @brief Invoke one of the server's tools.
    ///
    /// @param name The tool's own name, without this client's prefix.
    /// @param args Arguments, forwarded as-is.
    /// @return The output text and whether it is an error, or a transport-level
    ///         error description.
    ///
    /// @warning ⚠️ Responses must be matched by request id. A server may
    ///          interleave notifications, which carry no id — read past them
    ///          until the id matches, or a log line arrives where an answer was
    ///          expected.
    ///
    /// The server answers with an array of content blocks; the text blocks are
    /// joined into one string, and `isError` becomes the second member:
    ///
    /// @code{.test}
    /// @setup McpClient c("mock", "python3", {doc_mock_mcp_server()}, doc_workdir());
    /// @setup (void)c.initialize();
    /// @setup const auto r = c.call_tool("echo", Json{{"text", "hi"}});
    /// r.has_value()      ==>   true
    /// r->first           ==>   "echo: hi"
    /// r->second          ==>   false
    /// @endcode
    ///
    /// Each call gets its own id, so a second call reads its own answer and not
    /// the previous one — the two replies below differ only in their text:
    ///
    /// @code{.test}
    /// @setup McpClient c("mock", "python3", {doc_mock_mcp_server()}, doc_workdir());
    /// @setup (void)c.initialize();
    /// c.call_tool("echo", Json{{"text", "one"}})->first   ==>   "echo: one"
    /// c.call_tool("echo", Json{{"text", "two"}})->first   ==>   "echo: two"
    /// @endcode
    std::expected<std::pair<std::string, bool>, std::string> call_tool(std::string_view name,
                                                                      const Json& args);

    /// @brief The server's name, used as the tool-name prefix.
    /// @return The name given at construction.
    const std::string& name() const;

    /// @brief Shut the server down. Idempotent; the destructor calls it.
    ///
    /// @note Closes the server's stdin first and gives it a moment to exit on
    ///       its own; signals come only if it will not. A request after close()
    ///       is an error, not a hang.
    /// @note Safe to call while another thread is inside a request: it waits
    ///       for that request to finish — at most the request timeout — rather
    ///       than closing the pipes under it. A descriptor closed mid-read is
    ///       reused by the next open() in the process, and the reader then reads
    ///       someone else's file.
    ///
    /// @code{.test}
    /// @setup McpClient c("mock", "python3", {doc_mock_mcp_server()}, doc_workdir());
    /// @setup (void)c.initialize();
    /// @setup c.close();
    /// @setup c.close();
    /// c.list_tools().has_value()   ==>   false
    /// @endcode
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief What loading the MCP configuration produced.
struct McpLoadResult {
    std::vector<ToolPtr> tools;   ///< Ready to register, names already prefixed.

    /// @brief Every started server, including those that offer no tools.
    /// @note A server with tools does not depend on this vector: each McpTool
    ///       holds its client through a shared_ptr, so the server lives as
    ///       long as any of its tools does. A server with no tools has nothing
    ///       else holding it and would be shut down the moment loading ends.
    ///       Keeping them here also leaves App one place to reach every server.
    std::vector<std::shared_ptr<McpClient>> clients;

    /// @brief Servers that would not start. Non-fatal; shown via App::warnings.
    std::vector<std::string> errors;
};

/// @brief Read the MCP configuration and connect to every server in it.
///
/// @param config_path `.mini-agent/mcp.json`; a missing file is not an error.
/// @param cwd         Working directory for the server processes.
/// @return The tools, the clients keeping them alive, and any failures.
///
/// @warning Remote tools declare `read_only = false` and
///          `requires_permission = true`. What a third-party server does is
///          unknown, and the safe assumption is the expensive one.
///
/// @note Tool names are prefixed `mcp__<server>__<tool>` so two servers
///       offering `read_file` do not collide, and so a permission rule can
///       name one server's tools specifically.
/// @note A server that fails to start is recorded in `errors` rather than
///       thrown. Losing one server's tools beats not starting.
///
/// No configuration file is the common case, and it is not a warning:
///
/// @code{.test}
/// @setup const auto none = load_mcp_servers(doc_workdir() / "no-such-mcp.json", doc_workdir());
/// none.tools.empty()    ==>   true
/// none.errors.empty()   ==>   true
/// @endcode
///
/// A server contributes its tools under a prefixed name, with both flags at
/// their most conservative:
///
/// @code{.test}
/// @setup const auto r = load_mcp_servers(doc_mcp_config("github"), doc_workdir());
/// r.tools.size()                      ==>   1u
/// r.tools[0]->name()                  ==>   "mcp__github__echo"
/// r.tools[0]->read_only()             ==>   false
/// r.tools[0]->requires_permission()   ==>   true
/// r.clients.size()                    ==>   1u
/// @endcode
///
/// One broken entry costs only its own tools:
///
/// @code{.test}
/// @setup const auto mixed = load_mcp_servers(doc_mcp_config("github", true), doc_workdir());
/// mixed.tools.size()                                        ==>   1u
/// mixed.errors.size()                                       ==>   1u
/// mixed.errors[0].find("broken") != std::string::npos       ==>   true
/// @endcode
McpLoadResult load_mcp_servers(const fs::path& config_path, const fs::path& cwd);

}  // namespace mini
