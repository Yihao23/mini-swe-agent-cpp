#pragma once
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

struct Skill {
    std::string name;
    std::string description;
    std::string body;
    fs::path path;          // SKILL.md 的路径

    fs::path dir() const { return path.parent_path(); }
    /// 正文 + 同目录下其他文件的清单（让模型知道还能 read 什么）
    std::string render() const;
};

class SkillRegistry {
  public:
    /// dirs 靠前的优先：项目私有 / 项目内置 / 用户全局。同名怎么办？想清楚。
    explicit SkillRegistry(std::vector<fs::path> dirs);

    void reload();
    std::vector<const Skill*> all() const;
    const Skill* get(std::string_view name) const;
    std::string index_text() const;
    std::size_t size() const { return skills_.size(); }

  private:
    std::vector<fs::path> dirs_;
    std::map<std::string, Skill, std::less<>> skills_;   // less<> 支持 string_view 查找
};

/// 和 memory 的 frontmatter 解析是同一套逻辑 —— 抽出来复用，别写两遍。
/// TODO(Stage 5)
std::optional<Skill> parse_skill_file(const fs::path& skill_md);

}  // namespace mini
