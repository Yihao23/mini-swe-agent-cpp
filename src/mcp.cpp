// 【Stage 7】MCP 客户端（stdio JSON-RPC）。
#include "mini_agent/mcp.hpp"

namespace mini {

struct McpClient::Impl {
    // 至少要有：pid、读写 FILE*、请求 id 计数器、mutex、name
};

McpClient::McpClient(std::string, std::string, std::vector<std::string>, const fs::path&)
    : impl_(nullptr) {
    todo("Stage 7: McpClient 构造 —— 双向管道 + fork + exec");
}

McpClient::~McpClient() = default;

std::expected<Json, std::string> McpClient::initialize() {
    todo("Stage 7: initialize —— 然后别忘了发 notifications/initialized");
}

std::expected<Json, std::string> McpClient::list_tools() { todo("Stage 7: list_tools"); }

std::expected<std::pair<std::string, bool>, std::string> McpClient::call_tool(std::string_view,
                                                                             const Json&) {
    todo("Stage 7: call_tool —— ⚠️ 要跳过没有 id 的通知，读到 id 对上的那条");
}

const std::string& McpClient::name() const { todo("Stage 7: McpClient::name"); }
void McpClient::close() { todo("Stage 7: McpClient::close"); }

McpLoadResult load_mcp_servers(const fs::path&, const fs::path&) {
    todo("Stage 7: load_mcp_servers —— 某个 server 起不来只记 warning，不抛");
}

}  // namespace mini
