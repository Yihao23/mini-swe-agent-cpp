// 【Stage 0】配置加载。

#include "mini_agent/config.hpp"

#include <cstdlib>

#include "mini_agent/json.hpp"

namespace mini {

std::string_view to_string(PermissionMode) {
    todo("Stage 0: to_string(PermissionMode)");
}

std::optional<PermissionMode> permission_mode_from_string(std::string_view s) {
    if (s == "read-only") return PermissionMode::ReadOnly;
    if (s == "ask") return PermissionMode::Ask;
    if (s == "auto") return PermissionMode::Auto;
    if (s == "yolo") return PermissionMode::Yolo;
    return std::nullopt;
}

fs::path Config::sessions_dir() const {
    return state_dir / "sessions";
}

fs::path Config::memory_dir() const {
    return state_dir / "memory";
}

std::vector<fs::path> Config::skills_dirs() const {
    // 靠前的优先：<state_dir>/skills、<workdir>/skills、~/.mini-agent/skills
    std::vector<fs::path> dirs = {  
        state_dir / "skills",
        workdir / "skills"};
    if (const char* home = std::getenv("HOME"); home && *home) 
        dirs.push_back(fs::path(home) / ".mini-agent/skills");
    return dirs;
}

fs::path Config::mcp_config() const {
    return state_dir / "mcp.json";
}

void Config::ensure_dirs() const {
    todo("Stage 0: Config::ensure_dirs");
}

void Config::normalize() {
    // workdir 要 weakly_canonical()；state_dir 空则填 <workdir>/.mini-agent
    workdir = fs::weakly_canonical(workdir);
    if (state_dir.empty()) state_dir = workdir / ".mini-agent";
}

Config load_config(const fs::path&) {
    // 顺序：默认值 → .mini-agent/config.json → 环境变量
    //   MINI_AGENT_MODEL / MINI_AGENT_MODE / MINI_AGENT_EFFORT / MINI_AGENT_MAX_STEPS
    // 最后别忘了 normalize()。
    todo("Stage 0: load_config");
}

}  // namespace mini
