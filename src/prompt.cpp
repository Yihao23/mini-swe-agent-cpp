// 【Stage 4】输入 prompt 组装。
#include "mini_agent/prompt.hpp"

#include "mini_agent/memory.hpp"
#include "mini_agent/skills.hpp"

namespace mini {

const char* kIdentity = "TODO(Stage 4): 身份段 —— 工作方式、沟通风格、边界";
const char* kToolGuide = "TODO(Stage 4): 工具使用要点 —— 并行优先、edit 前先 read……";

std::vector<SystemBlock> build_system(const Config&, const SkillRegistry*, const Memory*,
                                      std::string_view, std::string_view) {
    // ⚠️ 这里**不许**放时间戳/uuid/todo 列表 —— 放了整段对话每轮重新计费
    // ⚠️ 最后一块要 cache_breakpoint = true
    todo("Stage 4: build_system");
}

std::string project_doc(const fs::path&, std::size_t) { todo("Stage 4: project_doc"); }

std::string reminder(std::string_view) { todo("Stage 4: reminder —— 包成 <system-reminder>"); }

std::string turn_context(BackgroundManager*, const Json&) { todo("Stage 4/6: turn_context"); }

}  // namespace mini
