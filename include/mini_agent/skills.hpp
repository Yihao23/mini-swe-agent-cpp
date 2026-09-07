#pragma once
/// @file
/// @brief Skills — folders holding a manual for one kind of job (Stage 5).
//
// 【Stage 5】Skill 插件 —— 用文件夹装的"专项操作手册"。
//
//     skills/code-review/
//         SKILL.md       ← 必须有，frontmatter 带 name / description
//         checklist.md   ← 可选附件，由 SKILL.md 正文引用，模型自己去 read
//
// 加载策略和 memory 完全一样（渐进式披露）：
//   常驻上下文：  - code-review: 代码评审清单，改动 PR 前用
//   模型判断相关：skill(name="code-review") → 完整手册进上下文
//   手册里再引用别的文件 → 第二层按需加载
//
// 和 memory 的区别只有两点：skill 不跨会话变化，且 agent 不能自己写入。
//
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mini {

namespace fs = std::filesystem;

/// @brief One skill: a folder holding a manual for a particular kind of job.
///
/// @note Name and description live in the system prompt; the body is loaded
///       only when the model asks for it. That split is the whole point —
///       ten skills inlined in full would put thousands of tokens into every
///       turn's fixed cost, nearly all of them irrelevant.
struct Skill {
    std::string name;         ///< How the model asks for it.

    /// @brief When to use it, written for the model.
    /// @note This is what goes into the index, so it is the only thing the
    ///       model sees before deciding to load the body. Vague wording here
    ///       means the skill never gets used.
    std::string description;

    std::string body;         ///< The manual itself; loaded on demand.
    fs::path path;            ///< Where SKILL.md lives.

    /// @brief The folder containing this skill.
    /// @return The parent of `path`; sibling files live here.
    fs::path dir() const { return path.parent_path(); }

    /// @brief The body, plus a listing of the other files beside it.
    /// @return Text ready to hand to the model.
    /// @note The listing matters: a skill is a folder, and its scripts and
    ///       templates are useless if the model does not know they are there
    ///       to be read.
    std::string render() const;
};

/// @brief The skills available, gathered from several directories.
class SkillRegistry {
  public:
    /// @brief Build a registry over a search path.
    /// @param dirs Searched in order, most specific first.
    /// @note Order is precedence: a project skill shadows a global one of the
    ///       same name, so a repository can override what a user installed.
    explicit SkillRegistry(std::vector<fs::path> dirs);

    /// @brief Rescan the directories.
    /// @note A skill added while the agent is running should be usable without
    ///       restarting it — that is what makes them worth editing by hand.
    void reload();

    /// @brief Every skill found.
    /// @return Non-owning pointers, in name order.
    std::vector<const Skill*> all() const;

    /// @brief Look one up.
    /// @param name What the model asked for.
    /// @return nullptr when there is no such skill.
    const Skill* get(std::string_view name) const;

    /// @brief The index that goes into the system prompt.
    /// @return One line per skill: name and description, no bodies.
    /// @warning Must be byte-stable — it is part of the cached prefix. Which
    ///          is why the underlying map is ordered rather than a hash map.
    std::string index_text() const;

    /// @brief How many skills were found.
    /// @return The count.
    std::size_t size() const { return skills_.size(); }

  private:
    std::vector<fs::path> dirs_;
    std::map<std::string, Skill, std::less<>> skills_;   // less<> 支持 string_view 查找
};

/// @brief Read one SKILL.md.
///
/// @param skill_md Path to the file.
/// @return nullopt when it is missing or its frontmatter will not parse.
///
/// @note The frontmatter parsing is the same as memory's. Written once and
///       shared — two hand-rolled YAML-ish parsers would drift, and the drift
///       would show up as a skill that loads and a memory that does not, for
///       reasons nobody could see.
std::optional<Skill> parse_skill_file(const fs::path& skill_md);

}  // namespace mini
