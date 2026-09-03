// 【Stage 5】Skill 插件。
#include "mini_agent/skills.hpp"

#include "mini_agent/json.hpp"

namespace mini {

std::string Skill::render() const {
    todo("Stage 5: Skill::render —— 正文 + 同目录其他文件清单");
}

SkillRegistry::SkillRegistry(std::vector<fs::path> dirs) : dirs_(std::move(dirs)) { reload(); }

void SkillRegistry::reload() {
    todo("Stage 5: SkillRegistry::reload —— 扫 */SKILL.md，靠前目录优先");
}

std::vector<const Skill*> SkillRegistry::all() const { todo("Stage 5: SkillRegistry::all"); }
const Skill* SkillRegistry::get(std::string_view) const { todo("Stage 5: SkillRegistry::get"); }
std::string SkillRegistry::index_text() const { todo("Stage 5: SkillRegistry::index_text"); }

std::optional<Skill> parse_skill_file(const fs::path&) { todo("Stage 5: parse_skill_file"); }

}  // namespace mini
