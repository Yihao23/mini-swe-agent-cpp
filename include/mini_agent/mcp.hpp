#pragma once
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
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mini_agent/tool.hpp"

namespace mini {

namespace fs = std::filesystem;

inline constexpr const char* kMcpProtocolVersion = "2024-11-05";

class McpClient {
  public:
    McpClient(std::string name, std::string command, std::vector<std::string> args,
              const fs::path& cwd);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    /// 失败返回错误描述 —— 一个外部 server 起不来不该拖垮整个 agent
    std::expected<Json, std::string> initialize();
    std::expected<Json, std::string> list_tools();
    std::expected<std::pair<std::string, bool>, std::string> call_tool(std::string_view name,
                                                                      const Json& args);
    const std::string& name() const;
    void close();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct McpLoadResult {
    std::vector<ToolPtr> tools;
    std::vector<std::shared_ptr<McpClient>> clients;   // 要保活：工具持有 client
    std::vector<std::string> errors;
};

/// 读 .mini-agent/mcp.json，连所有 server。
/// 远程工具名字形如 `mcp__filesystem__read_file`（前缀防重名）。
/// 远程工具的副作用未知 → read_only=false, requires_permission=true，保守处理。
/// TODO(Stage 7)
McpLoadResult load_mcp_servers(const fs::path& config_path, const fs::path& cwd);

}  // namespace mini
