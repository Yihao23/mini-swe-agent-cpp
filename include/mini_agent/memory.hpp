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
// ── 各部分怎么配合 ──────────────────────────────────────────────────────────
//
//                          磁盘
//                 .mini-agent/memory/*.md
//                            │
//                            │  parse_memory_file()   一个文件 → 一条 MemoryItem
//                            │  （没有 description 的整条丢弃）
//                            ▼
//                    rebuild_index()                  扫目录 + 按 name 排序
//                            │                        ⚠️ 排序不是为了好看，见下
//                            ▼
//                    ┌───────────────┐
//                    │    index_     │   mutable 缓存。四个读操作都会先看
//                    │  （已排序）    │   loaded_，没加载过就先 rebuild。
//                    └───────┬───────┘   这就是 rebuild_index() 是 const 的原因。
//                            │
//         ┌──────────────┬───┴────────┬──────────────┐
//         ▼              ▼            ▼              ▼
//     index_text()   search()      get()         items()
//         │              │            │
//         │              └─────┬──────┘
//         │                    ▼
//         │            MemoryItem::render()   name + [type] + description + body
//         │                    │
//         ▼                    ▼
//   build_system()       MemoryTool::run()
//   → system prompt      → tool_result
//   （每轮都发，只有       （模型主动要才发，
//     一行行的摘要）         正文全给）
//
// 写入侧只有两个入口，都以 rebuild_index() 收尾 —— 这一轮写的记忆，
// 下一轮的 system prompt 里就该看得见：
//
//     write()  ── 写 .md 文件 ──┐
//     remove() ── 删 .md 文件 ──┴──> rebuild_index()
//
// ── 方法在维护什么不变量 ────────────────────────────────────────────────────
//
// 三个字段，三条不变量。方法存在的理由就是**建立**或**依赖**它们：
//
//     root_          目录路径。构造后不再变，且目录一定存在（构造函数建的）
//     index_         已解析的条目
//     loaded_        index_ 到底能不能信
//
//   ┌── I1 ─────────────────────────────────────────────────────────────┐
//   │ loaded_ == true  ⟹  index_ 是 root_ 在「上次 rebuild 那一刻」的快照 │
//   │ loaded_ == false ⟹  index_ 的内容无意义，谁都不该读                 │
//   └───────────────────────────────────────────────────────────────────┘
//   ┌── I2 ─────────────────────────────────────────────────────────────┐
//   │ index_ 按 name 升序                                                │
//   └───────────────────────────────────────────────────────────────────┘
//   ┌── I3 ─────────────────────────────────────────────────────────────┐
//   │ index_ 里每一条的 description 都非空                                │
//   └───────────────────────────────────────────────────────────────────┘
//
//   建立者（只有一个）              依赖者（只读，先补齐再用）
//   ──────────────────             ──────────────────────────
//   rebuild_index()                items()  get()  search()  index_text()
//     clear + 扫盘 + 排序             全部以 `if (!loaded_) rebuild_index();` 开头
//     I1 I2 I3 一次全建立              —— 这就是四个 const 方法能修改缓存的原因，
//                                        也是 rebuild_index() 必须是 const 的原因
//
//   打破再修复者（写入侧）
//   ──────────────────────
//   write()   写 .md ─┐  写完磁盘变了 → I1 立刻失效
//   remove()  删 .md ─┴─> rebuild_index()   ← 必须收尾，否则这一轮写的记忆
//                                              下一轮 system prompt 里看不见
//
// ⚠️ I3 是 parse_memory_file() 在入口处保证的：没有 description 就整条丢弃。
//    放进去的话，它会占一个索引位置，而模型看到空描述永远不会去 load ——
//    写了等于没写，还白占每一轮的上下文。
//
// ⚠️ 有一条**没有**被保证：name 唯一。write() 用 name 当文件名所以它写的不会撞，
//    但手写的两个文件可以在 frontmatter 里声明同一个 name。此时 get() 返回
//    排序靠前的那条 —— 而 remove() 删的必须是**同一条**，所以它先 get() 拿到
//    item->path 再删，不是去拼 root_/name.md。这两个方法给出不一致答案的话，
//    调用方没有任何办法看出区别（实测过：get 找得到、remove 删不掉）。
//
// ⚠️ 为什么 I2（排序）是不变量而不是"顺手排一下"：index_text() 的输出进
//    system prompt，而 prompt 缓存是**逐字节匹配前缀**的。目录遍历顺序不保证，
//    不排序的话每次启动这段字节都可能不同 —— 缓存永远命不中，账单十倍，
//    而功能完全正常，没有任何东西会失败。
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

    /// @brief Parsed items, loaded lazily.
    /// @note mutable, which is why rebuild_index() and index_text() can be
    ///       const: the cache is not state the caller owns, it is a view of
    ///       what is on disk.
    mutable std::vector<MemoryItem> index_;
    mutable bool loaded_ = false;
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
