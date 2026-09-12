#pragma once
//
// Helpers available to @code{.test} blocks.
//
// A documented example should read as documentation, not as test scaffolding.
// Anything a block needs beyond one or two @setup lines belongs here instead.
//
#include <signal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "mini_agent/config.hpp"
#include "mini_agent/mcp.hpp"
#include "mini_agent/message.hpp"
#include "mini_agent/parser.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/prompt.hpp"
#include "mini_agent/skills.hpp"
#include "mini_agent/process.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/scheduler.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"
#include "mini_agent/tools/builtin.hpp"

namespace mini {

using namespace std::chrono_literals;   // 示例里能写 5s，而不是 std::chrono::seconds{5}

/// @brief A minimal Tool, so examples can exercise the base-class behaviour.
struct DocTool : Tool {
    std::string n;
    bool ro, perm;
    explicit DocTool(std::string name = "doc", bool read_only = true, bool needs_perm = false)
        : n(std::move(name)), ro(read_only), perm(needs_perm) {}
    std::string_view name() const override { return n; }
    std::string_view description() const override { return "a tool used in doc examples"; }
    Json input_schema() const override { return Json::object(); }
    bool read_only() const override { return ro; }
    bool requires_permission() const override { return perm; }
    ToolResult run(const Json&, ToolContext&) override { return ToolResult{.content = "ok"}; }
};

/// @brief A Config pointing at a scratch directory, already normalised.
inline Config doc_config(PermissionMode mode = PermissionMode::Ask) {
    Config cfg;
    cfg.workdir = std::filesystem::temp_directory_path() / "mini-agent-doc";
    std::filesystem::create_directories(cfg.workdir);
    cfg.permission_mode = mode;
    cfg.normalize();
    return cfg;
}

/// @brief A scratch directory examples can run commands in.
inline std::filesystem::path doc_workdir() { return doc_config().workdir; }

/// @brief A wired-up place to run a real tool: config, session, sandbox, context.
///
/// Each instance gets its own directory, so two examples in the same binary
/// cannot see each other's files. Without that, an example would depend on
/// which ones happened to run before it.
struct DocTools {
    std::filesystem::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    ToolContext ctx;

    DocTools() {
        static int seq = 0;
        root = std::filesystem::temp_directory_path() /
               ("mini-agent-doc-tools-" + std::to_string(++seq));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "work");
        cfg.workdir = std::filesystem::canonical(root / "work");
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
    }
    ~DocTools() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    ToolResult run(const ToolPtr& t, Json args) { return t->run(args, ctx); }

    /// @brief The file's whole content, for asserting on what a tool produced.
    std::string slurp(const std::string& rel) const {
        std::ifstream in(cfg.workdir / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }

    /// @brief Put a file there without going through a tool.
    ///
    /// @note Creates parent directories. An ofstream on a path whose directory
    ///       does not exist fails silently, and the example then reads as if
    ///       the tool under test had lost the file.
    void seed(const std::string& rel, const std::string& body) const {
        const auto p = cfg.workdir / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << body;
    }
};

/// @brief Path to the 30-line fake MCP server under tests/.
///
/// @note It sends a notification in the middle of the handshake on purpose,
///       so every example that goes through initialize() also shows the
///       client stepping over a message that is not its answer.
inline std::string doc_mock_mcp_server() {
    return (std::filesystem::path(MINI_AGENT_TEST_DIR) / "mock_mcp_server.py").string();
}

/// @brief Write an mcp.json into a fresh directory and return its path.
///
/// @param name        The mock server's name — it becomes the tool prefix.
/// @param with_broken Also list a server whose command does not exist, to show
///                    that one bad entry costs only its own tools.
///
/// @note A fresh directory per call, for the same reason DocTools has one:
///       examples must not see each other's files.
inline std::filesystem::path doc_mcp_config(const std::string& name, bool with_broken = false) {
    static int seq = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("mini-agent-doc-mcp-" + std::to_string(++seq));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Json servers = Json::object();
    servers[name] = Json{{"command", "python3"}, {"args", Json::array({doc_mock_mcp_server()})}};
    if (with_broken) servers["broken"] = Json{{"command", "no-such-program-xyz"}};
    const auto path = dir / "mcp.json";
    std::ofstream(path, std::ios::binary) << Json{{"mcpServers", servers}}.dump(2);
    return path;
}

}  // namespace mini
