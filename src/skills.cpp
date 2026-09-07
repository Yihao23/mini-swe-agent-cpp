// 【Stage 5】Skill 插件。契约写在 include/mini_agent/skills.hpp。

#include "mini_agent/skills.hpp"

#include <algorithm>
#include <format>

#include "frontmatter.hpp"

namespace mini {

std::string Skill::render() const {
    std::string out = std::format("# {}\n{}\n\n{}", name, description, body);

    // ⚠️ 列出同目录的其他文件。skill 是一个**文件夹**，里面常有脚本、模板、
    //    示例数据 —— 模型不知道它们存在就用不上，而正文里往往只写"运行
    //    convert.py"，不写它在哪。列出来一行，省掉一轮 glob。
    std::error_code ec;
    std::vector<std::string> siblings;
    for (const auto& e : fs::directory_iterator(dir(), ec)) {
        if (ec) break;
        const auto fname = e.path().filename().string();
        if (fname == "SKILL.md") continue;
        siblings.push_back(e.is_directory(ec) ? fname + "/" : fname);
    }
    if (siblings.empty()) return out;

    std::ranges::sort(siblings);   // 顺序稳定：文件系统的遍历顺序不保证
    out += "\n\n同目录下还有（需要时用 read 打开）:\n";
    for (const auto& s : siblings) out += "  " + dir().filename().string() + "/" + s + "\n";
    return out;
}

SkillRegistry::SkillRegistry(std::vector<fs::path> dirs) : dirs_(std::move(dirs)) { reload(); }

void SkillRegistry::reload() {
    skills_.clear();
    std::error_code ec;
    // ⚠️ 按 dirs_ 顺序扫，先到的赢。emplace 遇到同名不覆盖 —— 这正是我们要的：
    //    靠前的目录是项目私有的，靠后的是用户全局的，项目应该能盖住全局。
    //    换成 insert_or_assign 就反了，全局的会覆盖项目的。
    for (const auto& dir : dirs_) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            // 只是省掉一次无谓的开文件尝试 —— 「skill 必须是文件夹」这条规矩
            // 其实是下面那句路径拼接保证的：对 loose.md 会去开
            // loose.md/SKILL.md，不存在，照样跳过。
            if (!e.is_directory(ec)) continue;
            if (auto s = parse_skill_file(e.path() / "SKILL.md"))
                skills_.emplace(s->name, std::move(*s));
        }
    }
}

std::vector<const Skill*> SkillRegistry::all() const {
    std::vector<const Skill*> out;
    out.reserve(skills_.size());
    // map 本身有序，所以输出顺序稳定 —— index_text() 进 system prompt，
    // 缓存逐字节匹配前缀，顺序变了整段对话每轮全价。
    for (const auto& [_, s] : skills_) out.push_back(&s);
    return out;
}

const Skill* SkillRegistry::get(std::string_view name) const {
    const auto it = skills_.find(name);   // less<> 让 string_view 直接查
    return it == skills_.end() ? nullptr : &it->second;
}

std::string SkillRegistry::index_text() const {
    if (skills_.empty()) return {};
    std::string out = "Skills (load one by name when the task matches):\n";
    for (const auto& [name, s] : skills_)
        out += std::format("  {} — {}\n", name, s.description);
    return out;
}

std::optional<Skill> parse_skill_file(const fs::path& skill_md) {
    const auto fm = parse_frontmatter_file(skill_md.string());
    if (!fm) return std::nullopt;

    Skill s;
    s.path = skill_md;
    // name 缺失时用**文件夹名**兜底，不是文件名 —— SKILL.md 每个 skill 都叫这个。
    const auto n = fm->fields.find("name");
    s.name = n != fm->fields.end() ? n->second : skill_md.parent_path().filename().string();
    if (s.name.empty()) return std::nullopt;

    // ⚠️ 和 memory 同一条规矩：没有 description 就整条丢弃。它是索引里唯一
    //    能让模型判断"这个 skill 跟当前任务有没有关系"的东西，空的等于永远不会被加载。
    const auto d = fm->fields.find("description");
    if (d == fm->fields.end() || d->second.empty()) return std::nullopt;
    s.description = d->second;

    s.body = fm->body;
    return s;
}

}  // namespace mini
