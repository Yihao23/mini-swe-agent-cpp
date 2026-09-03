#pragma once
//
// 【Stage 4】输入 prompt 组装 —— 决定"模型每一轮看到什么"。
//
// 一条铁律：**system prompt 必须逐字节稳定**。
//
// 渲染顺序是 tools → system → messages，缓存是前缀逐字节匹配。
// 只要 system 里出现一个时间戳、一个 uuid、一个"当前 todo 列表"，
// 它后面的所有内容（也就是整段对话）每轮都要重新计费。
//
// 所以划一条线：
//   静态（身份、工具用法、skill/memory 索引、工作区路径）→ system，最后一块打缓存断点
//   动态（当前时间、后台任务通知、todo 变化）→ user 轮的 <system-reminder> 块
//
// 这也解释了一个你可能见过但没想明白的现象：为什么各种 agent 的"提醒"
// 总是以 <system-reminder> 出现在用户消息里，而不是写在 system prompt 里。
//
#include <string>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"

namespace mini {

class SkillRegistry;
class Memory;
class BackgroundManager;

extern const char* kIdentity;    // TODO(Stage 4): 身份段
extern const char* kToolGuide;   // TODO(Stage 4): 工具使用要点

/// 组装 system。**不许放任何随请求变化的东西** —— 时间戳走 turn_context()。
/// identity 非空时覆盖身份段（Stage 6 的子 agent 用）。
/// 最后一块要 cache_breakpoint = true。
/// TODO(Stage 4)
std::vector<SystemBlock> build_system(const Config& cfg,
                                      const SkillRegistry* skills = nullptr,
                                      const Memory* memory = nullptr,
                                      std::string_view extra = {},
                                      std::string_view identity = {});

/// 读项目约定：AGENTS.md / CLAUDE.md（存在才读，截断到几 KB）
std::string project_doc(const fs::path& workdir, std::size_t limit = 8000);

/// 动态上下文的包装：出现在 user 轮，缓存断点之后
std::string reminder(std::string_view text);

/// 每轮开始前收集的动态上下文（后台通知、todo）；没内容返回空串
/// TODO(Stage 4/6)
std::string turn_context(BackgroundManager* background, const Json& todos);

}  // namespace mini
