#pragma once
//
// Helpers available to @code{.test} blocks.
//
// A documented example should read as documentation, not as test scaffolding.
// Anything a block needs beyond one or two @setup lines belongs here instead.
//
#include <filesystem>
#include <memory>
#include <string>

#include "mini_agent/config.hpp"
#include "mini_agent/message.hpp"
#include "mini_agent/parser.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/scheduler.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

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

}  // namespace mini
