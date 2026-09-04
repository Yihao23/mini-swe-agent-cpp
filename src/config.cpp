// 【Stage 0】配置加载。

#include "mini_agent/config.hpp"

#include <cstdlib>
#include <fstream>

#include "mini_agent/json.hpp"

namespace mini {

std::string_view to_string(PermissionMode m) {
    // ⚠️ 必须和 permission_mode_from_string 逐字对齐 —— 两者是一对互逆函数，
    //    对不上就会「存出去的配置读不回来」，而且没有任何报错。
    switch (m) {
        case PermissionMode::ReadOnly: return "read-only";
        case PermissionMode::Ask:      return "ask";
        case PermissionMode::Auto:     return "auto";
        case PermissionMode::Yolo:     return "yolo";
    }
    return "ask";   // 不可达；没有它编译器会警告 control reaches end of non-void function
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
    fs::create_directories(state_dir);
    fs::create_directories(sessions_dir());
    fs::create_directories(memory_dir());
    if(!skills_dirs().empty()) fs::create_directories(skills_dirs().front());
}

void Config::normalize() {
    // workdir 要 weakly_canonical()；state_dir 空则填 <workdir>/.mini-agent
    workdir = fs::weakly_canonical(workdir);
    if (state_dir.empty()) state_dir = workdir / ".mini-agent";
}

Config load_config(const fs::path& workdir) {
    // 顺序：默认值 → .mini-agent/config.json → 环境变量
    //   MINI_AGENT_MODEL / MINI_AGENT_MODE / MINI_AGENT_EFFORT / MINI_AGENT_MAX_STEPS
    // 最后别忘了 normalize()。
       Config cfg;                       // ① 默认值：字段的 DMI 已经给了
      cfg.workdir = workdir;
      cfg.normalize();                  // ② 先 normalize 一次，才知道 state_dir 在哪


// ③ 配置文件（找不到/坏了都不是错误，用默认值继续）
      const fs::path file = cfg.state_dir / "config.json";
std::ifstream in(file);
      const Json j = Json::parse(in, nullptr, /*allow_exceptions=*/false);
      if (j.is_object()) {
          cfg.model             = j.value("model", cfg.model);
          cfg.subagent_model    = j.value("subagent_model", cfg.subagent_model);
          cfg.max_tokens        = j.value("max_tokens", cfg.max_tokens);
          cfg.effort            = j.value("effort", cfg.effort);
          cfg.max_steps         = j.value("max_steps", cfg.max_steps);
          cfg.compact_at_tokens = j.value("compact_at_tokens", cfg.compact_at_tokens);
          cfg.tool_timeout_sec  = j.value("tool_timeout_sec", cfg.tool_timeout_sec);
          cfg.allow_rules       = j.value("allow_rules", cfg.allow_rules);
          cfg.deny_rules        = j.value("deny_rules", cfg.deny_rules);
          cfg.enable_mcp        = j.value("enable_mcp", cfg.enable_mcp);
 if (auto m = permission_mode_from_string(j.value("permission_mode", std::string{})))
              cfg.permission_mode = *m;
      }

  // ④ 环境变量覆盖（优先级更高）

      auto env = [](const char* k) -> const char* {
          const char* v = std::getenv(k);
          return (v && *v) ? v : nullptr;
      };
      if (const char* v = env("MINI_AGENT_MODEL"))     cfg.model = v;
      if (const char* v = env("MINI_AGENT_EFFORT"))    cfg.effort = v;
      if (const char* v = env("MINI_AGENT_MAX_STEPS")) cfg.max_steps = std::atoi(v);
      if (const char* v = env("MINI_AGENT_MODE"))
          if (auto m = permission_mode_from_string(v)) cfg.permission_mode = *m;

      cfg.normalize();                  // ⑤ 配置文件可能改了 workdir/state_dir，再来一次
      return cfg; 


}

}  // namespace mini
