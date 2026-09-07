#pragma once
/// @file
/// @brief Knowledge that outlives a session, stored as Markdown (Stage 5).
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

/// @brief What kind of thing a memory records.
///
/// @note The type steers recall: a `feedback` note about how to work should
///       surface on almost any task, a `reference` only when its subject comes
///       up. Without the distinction everything scores alike and the index
///       becomes noise.
enum class MemoryType {
    User,       ///< Who the user is: role, expertise, preferences.
    Feedback,   ///< How to work — corrections and confirmed approaches.
    Project,    ///< Ongoing work and constraints not derivable from the code.
    Reference,  ///< Pointers outward: URLs, dashboards, tickets.
};

/// @brief One remembered fact, stored as a Markdown file with frontmatter.
///
/// @note One fact per file. Merging several into one means recall drags in
///       three irrelevant things to deliver the one that matched.
struct MemoryItem {
    std::string name;          ///< Slug; also the filename stem.

    /// @brief One line, used to decide relevance during recall.
    /// @warning ⚠️ Written **for the model**: it says *when this is worth
    ///          reading*, not what it is. "How the user wants commits
    ///          formatted" earns a read; "commit notes" does not.
    std::string description;

    MemoryType type = MemoryType::Reference;   ///< Steers recall; see MemoryType.
    std::string body;          ///< The fact itself.
    fs::path path;             ///< The file it came from.

    /// @brief The item as the model should see it.
    /// @return Title, type and body, formatted for injection.
    std::string render() const;
};

/// @brief Knowledge that outlives a session, kept as Markdown on disk.
///
/// @note Markdown files rather than a database: the user can read them, edit
///       them, and put them under version control. A memory the user cannot
///       inspect is one they cannot correct.
class Memory {
  public:
    /// @brief Open a memory store.
    /// @param root Directory holding the Markdown files; created if missing.
    explicit Memory(fs::path root);

    /// @brief Every stored item.
    /// @return All items, in name order.
    std::vector<MemoryItem> items() const;

    /// @brief Fetch one by name.
    /// @param name The slug.
    /// @return nullopt when there is no such memory.
    std::optional<MemoryItem> get(std::string_view name) const;

    /// @brief Find items relevant to a query.
    ///
    /// @param query Keywords, usually taken from the task.
    /// @param limit How many to return at most.
    /// @return The best matches, best first.
    ///
    /// @note Keyword scoring, with a hit in the description weighted above one
    ///       in the body — the description was written to answer "is this
    ///       worth reading", so a match there means more.
    /// @note Embeddings are not worth it until there are hundreds of items.
    ///       They add a model call, a cache and a similarity index to a problem
    ///       that grep solves at this size.
    std::vector<MemoryItem> search(std::string_view query, std::size_t limit = 5) const;

    /// @brief Store or replace a memory.
    ///
    /// @param name        The slug; an existing one is overwritten.
    /// @param description One line saying when this is worth reading.
    /// @param body        The fact.
    /// @param type        Which kind.
    /// @return The path written.
    ///
    /// @note Rebuilds the index, so the next turn's system prompt sees it.
    fs::path write(std::string_view name, std::string_view description,
                   std::string_view body, MemoryType type);

    /// @brief Delete one.
    /// @param name The slug.
    /// @return false when there was no such memory.
    /// @note Deleting matters as much as writing: a memory that turned out to
    ///       be wrong keeps being recalled until someone removes it.
    bool remove(std::string_view name);

    /// @brief Re-read the directory.
    /// @note const because the index is a cache, not state the caller owns.
    void rebuild_index() const;

    /// @brief The index injected into the system prompt.
    ///
    /// @param max_items Cap, so a large store cannot dominate the prompt.
    /// @return One line per item: name and description, no bodies.
    ///
    /// @warning ⚠️ These descriptions are **for the model**. They must say
    ///          *when to use it*, not *what it is* — the model reads this line
    ///          and nothing else before deciding whether to load the body.
    /// @warning Part of the cached prefix, so it must be byte-stable. It
    ///          changes only when a memory is written or removed.
    std::string index_text(std::size_t max_items = 40) const;

  private:
    fs::path root_;
};

/// @brief Read one memory file.
///
/// @param path The Markdown file.
/// @return nullopt when it is missing or its frontmatter will not parse.
///
/// @note The frontmatter parser is hand-written: find `---\n...\n---\n`, then
///       split each line on the first `: `. C++ has no YAML in the standard
///       library, and pulling one in for four fields would break this
///       project's two-dependency rule.
/// @note Shared with skills. Two hand-rolled parsers would drift, and the drift
///       would appear as a skill that loads beside a memory that does not, for
///       no visible reason.
std::optional<MemoryItem> parse_memory_file(const fs::path& path);

}  // namespace mini
