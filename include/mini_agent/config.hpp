#pragma once
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

inline constexpr const char* kMainModel = "claude-opus-5";
inline constexpr const char* kSubModel = "claude-sonnet-5";

enum class PermissionMode { ReadOnly, Ask, Auto, Yolo };

std::string_view to_string(PermissionMode m);
/// 解析失败返回 std::nullopt —— 不要抛异常，调用方要给用户一条可读的报错
std::optional<PermissionMode> permission_mode_from_string(std::string_view s);

struct Config {
    // --- 模型 ---
    std::string model = kMainModel;
    std::string subagent_model = kSubModel;
    int max_tokens = 16000;
    std::string effort = "high";   // low | medium | high | xhigh | max
    bool thinking = true;          // adaptive thinking
    bool show_thinking = true;     // 请求 summarized 思考摘要
    bool stream = true;

    // --- 循环控制 ---
    int max_steps = 40;
    int compact_at_tokens = 120000;

    // --- 工具执行 ---
    int tool_timeout_sec = 120;
    std::size_t max_output_chars = 30000;
    unsigned max_parallel_tools = 8;

    // --- 沙箱 ---
    PermissionMode permission_mode = PermissionMode::Ask;
    std::vector<std::string> allow_rules;
    std::vector<std::string> deny_rules;

    // --- 路径 ---
    fs::path workdir = fs::current_path();
    fs::path state_dir;            // 空 = <workdir>/.mini-agent

    // --- 功能开关 ---
    bool enable_memory = true;
    bool enable_skills = true;
    bool enable_mcp = true;
    bool enable_subagents = true;

    // -- 派生路径 ------------------------------------------------------------
    fs::path sessions_dir() const;
    fs::path memory_dir() const;
    /// 多个目录，靠前的优先：项目私有 / 项目内置 / 用户全局
    std::vector<fs::path> skills_dirs() const;
    fs::path mcp_config() const;

    void ensure_dirs() const;

    /// workdir 要 weakly_canonical()，state_dir 要填默认值。
    /// C++ 没有 __post_init__，所以显式调用 —— load() 里别忘了。
    void normalize();
};

/// 默认值 → 配置文件 → 环境变量。命令行覆盖由 cli.cpp 在返回后做。
/// TODO(Stage 0)
Config load_config(const fs::path& workdir);

}  // namespace mini
