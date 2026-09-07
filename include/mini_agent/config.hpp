#pragma once
/// @file
/// @brief Every knob, in one aggregate (Stage 0).
//
// 【Stage 0】配置 —— 所有旋钮集中在一个聚合类型里。
//
// 优先级：命令行 > 环境变量 > .mini-agent/config.json > 默认值。
//
// 字段已经列好（这是"结构"），load() 的实现是你的活。
//
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mini {

namespace fs = std::filesystem;

/// @brief Default model for the main agent.
inline constexpr const char* kMainModel = "claude-opus-5";
/// @brief Default model for sub-agents (Stage 6) — cheaper, and they do less.
inline constexpr const char* kSubModel = "claude-sonnet-5";

/// @brief The baseline the sandbox falls back to when no rule matches.
///
/// @note Four values, ordered from strictest to loosest. Which one applies is
///       decided per call in Sandbox::check, and only when the allow/deny rules
///       have nothing to say.
enum class PermissionMode {
    ReadOnly,   ///< Read-only tools pass; anything with side effects is denied.
    Ask,        ///< Everything is put to a human.
    Auto,       ///< Read-only passes silently; side effects are asked about.
    Yolo,       ///< Everything passes — except kDangerous, which is not configurable.
};

/// @brief The spelling used in config files and on the command line.
/// @param m The mode.
/// @return A stable, lowercase, hyphenated name.
/// @warning It must round-trip through permission_mode_from_string. The two
///          disagreeing once meant a config file wrote "read-only" and read
///          back "ask" — silently loosening the policy the operator set.
std::string_view to_string(PermissionMode m);

/// @brief Parse a mode name.
/// @param s Text from a config file or the command line.
/// @return nullopt when it is not a mode name.
/// @note nullopt rather than an exception: the string is hand-written, so a
///       typo has to produce a readable message instead of a stack trace.
std::optional<PermissionMode> permission_mode_from_string(std::string_view s);

/// @brief Every knob the agent has, in one aggregate.
///
/// @note A plain struct with public fields and no invariants of its own.
///       Normalisation is the one exception, and it is explicit: normalize()
///       has to be called after loading, because C++ has no __post_init__.
/// @note Held by const reference throughout — Sandbox, ToolContext and Agent
///       all point at the App's copy, which outlives them.
struct Config {
    // --- 模型 ---
    std::string model = kMainModel;          ///< Model for the main agent.
    std::string subagent_model = kSubModel;  ///< Model for sub-agents (Stage 6).
    int max_tokens = 16000;                  ///< Cap on one response.

    /// @brief Thinking budget: low | medium | high | xhigh | max.
    std::string effort = "high";
    bool thinking = true;                    ///< Ask for adaptive thinking.

    /// @brief Ask for a readable summary of the thinking.
    /// @note Separate from `thinking`: the model can reason without the
    ///       summary being sent back, which costs fewer tokens on the wire.
    bool show_thinking = true;

    /// @brief Stream the response.
    /// @note Also decides who emits TextEvent — see Agent::run, which skips it
    ///       when streaming because the SSE callback already did.
    bool stream = true;

    // --- 循环控制 ---
    /// @brief Ceiling on turns in one run().
    /// @warning Without it a model that keeps calling tools loops until the
    ///          money runs out. This is the only thing that stops it.
    int max_steps = 40;

    /// @brief Compact once the history is estimated above this.
    /// @note Set high on purpose. Compaction rewrites the front of the request
    ///       and therefore throws away the prompt cache, so compacting eagerly
    ///       costs more than the longer context it avoids.
    int compact_at_tokens = 120000;

    // --- 工具执行 ---
    int tool_timeout_sec = 120;          ///< Ceiling on one bash command.

    /// @brief Truncate tool output beyond this many bytes.
    /// @note One `cat` of a large file would otherwise fill the whole turn.
    std::size_t max_output_chars = 30000;

    /// @brief How many read-only tools may run at once.
    /// @note Only read_only() tools are eligible; anything with side effects
    ///       runs alone, because two of them racing on the same tree is not
    ///       something the model can reason about.
    unsigned max_parallel_tools = 8;

    // --- 沙箱 ---
    PermissionMode permission_mode = PermissionMode::Ask;  ///< Fallback when no rule matches.

    /// @brief Rules that permit, e.g. `Bash(git status:*)`, `Write(src/**)`.
    std::vector<std::string> allow_rules;

    /// @brief Rules that refuse. Consulted **before** allow, and before the
    ///        requires_permission() exemption.
    std::vector<std::string> deny_rules;

    // --- 路径 ---
    /// @brief The tree the agent may touch.
    /// @warning Every path from the model is resolved against this and checked
    ///          for containment. normalize() canonicalises it; skipping that
    ///          leaves `..` in the prefix and the containment test meaningless.
    fs::path workdir = fs::current_path();

    /// @brief Where sessions, memory and skills live. Empty means
    ///        `<workdir>/.mini-agent`, filled in by normalize().
    fs::path state_dir;

    // --- 功能开关 ---
    bool enable_memory = true;     ///< Stage 5 memory tools.
    bool enable_skills = true;     ///< Stage 5 skill loading.
    bool enable_mcp = true;        ///< Stage 7 external MCP servers.
    bool enable_subagents = true;  ///< Stage 6 sub-agents.

    // -- 派生路径 ------------------------------------------------------------
    /// @brief Where session files are written.
    /// @return `<state_dir>/sessions`.
    fs::path sessions_dir() const;

    /// @brief Where memory Markdown files live (Stage 5).
    /// @return `<state_dir>/memory`.
    fs::path memory_dir() const;

    /// @brief Directories searched for skills, most specific first.
    /// @return Project-private, then project-bundled, then user-global.
    /// @note Order is the precedence: a project skill shadows a global one of
    ///       the same name, so a repository can override what a user installed.
    std::vector<fs::path> skills_dirs() const;

    /// @brief Path to the MCP server list (Stage 7).
    /// @return `<state_dir>/mcp.json`, whether or not it exists.
    fs::path mcp_config() const;

    /// @brief Create the state directories if they are missing.
    /// @note Called once by App, not lazily by each writer — a tool failing on
    ///       a missing directory is a worse error message than it needs to be.
    void ensure_dirs() const;

    /// @brief Canonicalise workdir and fill in state_dir.
    ///
    /// @warning Must be called after loading and after any command-line
    ///          override. C++ has no __post_init__, so nothing enforces it —
    ///          and an un-normalised workdir makes every containment check
    ///          compare against a path that still contains `..`.
    ///
    /// @note Idempotent: calling it twice changes nothing, which is what lets
    ///       load_config() normalise after the file and again after the
    ///       environment.
    void normalize();
};

/// @brief Build a Config from defaults, the config file, then the environment.
///
/// @param workdir The directory to read `.mini-agent/config.json` from.
/// @return A normalised Config. Never throws.
///
/// @note The precedence is defaults → file → environment, and the command line
///       is applied by cli.cpp afterwards. Later sources overwrite earlier
///       ones, so the most specific wins.
/// @note normalize() runs twice: once after the file (which may set workdir)
///       and once at the end. It is idempotent, so the second call is free and
///       covers whatever the environment changed.
/// @note A malformed config file is skipped rather than fatal — the same
///       reasoning as a malformed permission rule. Starting with defaults
///       beats not starting.
Config load_config(const fs::path& workdir);

}  // namespace mini
