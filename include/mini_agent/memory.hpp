#pragma once
//
// 【Stage 5】长期记忆 —— 跨会话存活的知识，存成一堆 Markdown 文件。
//
//     .mini-agent/memory/
//         MEMORY.md            ← 索引，每条一行，随 system prompt 进上下文
//         prefers-pytest.md    ← 一条记忆一个文件，带 frontmatter
//
// **渐进式披露**：只有索引常驻上下文，正文要 agent 主动 load。
// 这是 memory 和 skills 共用的一招 —— 用一行摘要换一次按需加载。
// 一百条记忆也只占几百 token 的常驻预算。
//
// 存成 Markdown 而不是 sqlite/json 的理由很实际：人能直接读、git 能 diff、
// 出错时你能手改。
//
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mini {

namespace fs = std::filesystem;

enum class MemoryType { User, Feedback, Project, Reference };

struct MemoryItem {
    std::string name;
    std::string description;   // 一句话摘要，进索引
    MemoryType type = MemoryType::Reference;
    std::string body;
    fs::path path;

    std::string render() const;
};

class Memory {
  public:
    explicit Memory(fs::path root);

    std::vector<MemoryItem> items() const;
    std::optional<MemoryItem> get(std::string_view name) const;

    /// 关键词打分检索。
    /// 想想：什么时候值得上 embedding？（提示：上百条之前关键词都够用，别提前优化。）
    std::vector<MemoryItem> search(std::string_view query, std::size_t limit = 5) const;

    fs::path write(std::string_view name, std::string_view description,
                   std::string_view body, MemoryType type);
    bool remove(std::string_view name);

    void rebuild_index() const;

    /// 注入 system prompt 的一行式索引。
    /// ⚠️ 这里的描述是**给模型看的**：写"什么时候该用它"，不是"它是什么"。
    std::string index_text(std::size_t max_items = 40) const;

  private:
    fs::path root_;
};

/// frontmatter 解析。C++ 没有现成的 YAML 库可用（也不值得为几个字段引一个），
/// 手写：找 `---\n...\n---\n`，中间按 `key: value` 逐行切。
/// TODO(Stage 5)
std::optional<MemoryItem> parse_memory_file(const fs::path& path);

}  // namespace mini
