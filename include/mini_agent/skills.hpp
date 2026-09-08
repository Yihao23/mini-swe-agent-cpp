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
// ── 各部分怎么配合 ──────────────────────────────────────────────────────────
//
//   dirs_ = { 项目私有, 项目内置, 用户全局 }        ← 顺序就是优先级
//        │        │          │
//        │        │          └── skills/code-review/SKILL.md
//        │        └───────────── .agent/skills/code-review/SKILL.md
//        └────────────────────── .mini-agent/skills/code-review/SKILL.md
//                     │
//                     │  parse_skill_file()   一个文件夹 → 一个 Skill
//                     │  （没有 description 的整条丢弃）
//                     ▼
//              reload()                       按 dirs_ 顺序扫，先到的赢
//                     │                       ⚠️ emplace 不覆盖，见下
//                     ▼
//             ┌─────────────────┐
//             │     skills_     │  map，天然有序 → index_text() 的字节稳定
//             │   name → Skill  │  （memory 那边用 vector，所以要显式 sort）
//             └────────┬────────┘
//                      │
//          ┌───────────┼───────────┐
//          ▼           ▼           ▼
//     index_text()   get()       all()
//          │           │
//          │           ▼
//          │    Skill::render()   正文 + **同目录其他文件的清单**
//          │           │           ↑ skill 是文件夹，这一步是它和 memory
//          │           │             唯一实质不同的地方
//          ▼           ▼
//    build_system()  SkillTool::run()
//    → system prompt → tool_result
//    （每轮都发，       （模型主动要才发，
//      一行行的摘要）     手册全文 + 附件清单）
//                              │
//                              ▼
//                      模型看到 "code-review/checklist.md"
//                      → 用 read 工具打开     ← 第二层按需加载
//
// 没有写入侧 —— skill 由人维护，agent 只读。reload() 是唯一的重建入口，
// 供 CLI 的 /skills 命令用：跑着的时候加一个 skill 不用重启，
// 那是它们值得手写的前提。
//
// ── 方法在维护什么不变量 ────────────────────────────────────────────────────
//
// 两个字段，三条不变量：
//
//     dirs_      搜索路径，按优先级排好。构造后不再变
//     skills_    name → Skill
//
//   ┌── I1 ─────────────────────────────────────────────────────────────┐
//   │ skills_ 里每个 name 只有一条，且是**靠前目录**里的那一份            │
//   └───────────────────────────────────────────────────────────────────┘
//   ┌── I2 ─────────────────────────────────────────────────────────────┐
//   │ 遍历 skills_ 得到的顺序稳定（map 有序，白拿）                       │
//   └───────────────────────────────────────────────────────────────────┘
//   ┌── I3 ─────────────────────────────────────────────────────────────┐
//   │ 每一条的 name 和 description 都非空                                │
//   └───────────────────────────────────────────────────────────────────┘
//
//   建立者（只有一个）        依赖者（纯读，什么都不用先做）
//   ──────────────────       ────────────────────────────
//   reload()                 all()   get()   index_text()
//     clear + 按序扫盘          直接读 skills_ —— 不像 memory 那边要
//     I1 I2 I3 一次全建立        先 `if (!loaded_)`，因为构造函数已经
//     构造函数也调它             调过 reload()，不变量从一开始就成立
//
// ⚠️ I1 靠 map::emplace 实现 —— 它遇到已存在的键**什么都不做**。dirs_ 按
//    「项目 → 全局」排，于是先扫到的项目版本留下，后扫到的全局版本被忽略。
//    换成 insert_or_assign 语义就反了：别人机器上的全局配置会覆盖仓库里
//    的约定，而两边代码看起来一模一样。
//
// ⚠️ I2 是 map 白送的，memory 那边用 vector 就得显式 sort。两处的理由相同：
//    index_text() 进 system prompt，缓存逐字节匹配前缀，顺序一变整段对话
//    每轮全价 —— 而功能完全正常，没有任何东西会失败。
//
// ⚠️ 和 memory 一样，有一条**没有**被保证：文件夹名和 frontmatter 里的 name
//    可以不一致（`skills/pdf/SKILL.md` 里写 `name: pdf-processing`）。
//    这里不构成问题，因为 skill 没有删除操作 —— 所有查找都只认 name 一条路，
//    不存在 memory 那种「get 按 name 查、remove 按文件名猜」的分歧。
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
