#pragma once
//
// 【Stage 1 + Stage 4】会话状态 —— 历史 + 落盘 + 上下文压缩。
//
// Stage 1 只要：append / user / save。
// Stage 4 回来补：estimated_tokens / safe_split / compact。
//
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "mini_agent/message.hpp"

namespace mini {

namespace fs = std::filesystem;
class LlmClient;

/// @brief The conversation history, plus what it takes to persist and shrink it.
///
/// Shared state that several layers touch: the loop appends turns, tools record
/// which files they read, compaction rewrites the front of it.
///
/// @warning ToolContext::session must point at the *same* Session the loop uses.
///          Two instances means edit's staleness check reads a read_files map
///          that read never wrote to, and the "read before edit" rule silently
///          stops working while the history itself looks fine.
class Session {
  public:
    /// @brief Fresh session with a generated id.
    Session();

    /// @brief Session with a specific id — used when resuming from disk.
    explicit Session(std::string id);

    // -- Stage 1 -------------------------------------------------------------
    /// @brief Append a message and write the session to disk.
    /// @note Saving here rather than leaving it to the caller: forgetting one
    ///       save loses the turn, and there is no signal that it happened.
    void append(Message msg);              // 追加并落盘

    /// @brief Append a plain user turn and save.
    void add_user_text(std::string text);

    /// @brief The most recent assistant text, searching backwards.
    /// @return Empty when no assistant turn has produced text yet.
    std::string last_assistant_text() const;

    const std::vector<Message>& messages() const { return messages_; }
    std::vector<Message>& messages() { return messages_; }

    /// @brief Resolved absolute path → mtime at the time it was read.
    ///
    /// How read and edit communicate: read records, edit checks. The key is the
    /// resolved path so both sides agree — storing "a.py" while edit looks up
    /// "/work/a.py" would report a file as unread right after reading it.
    ///
    /// @note mtime rather than a content hash: one stat versus reading the whole
    ///       file on every check. It over-reports (a touch with no edit trips it)
    ///       and can under-report within a filesystem's timestamp resolution.
    ///       For "do not overwrite someone else's change", a false alarm costs
    ///       one re-read; a miss costs their work.
    ///
    /// path -> 上次读取时的 mtime。edit 的陈旧检查用。
    std::map<std::string, std::filesystem::file_time_type>& read_files() { return read_files_; }

    // -- Stage 4 -------------------------------------------------------------
    /// @brief Rough size of the history in tokens.
    ///
    /// @note Serialised bytes / 4 is close enough to decide when to compact.
    ///       The exact count_tokens endpoint costs a round trip per check, and
    ///       the threshold has slack in it anyway.
    ///
    /// 粗估上下文大小。提示：序列化后的字节数 / 4 就够用，别一上来就调 count_tokens。
    int estimated_tokens() const;

    /// @brief Index where the history can be cut without breaking tool pairing.
    ///
    /// @param keep_recent Minimum number of trailing messages to preserve.
    /// @return The cut index, or 0 meaning "do not compact".
    ///
    /// @warning Everything before the returned index gets replaced by a summary.
    ///          Cutting at a tool_result orphans its tool_use and the next
    ///          request fails — permanently, since the history is already on
    ///          disk and --continue reproduces it.
    ///
    /// @note A genuine user turn is safe because it starts a round: by then
    ///       every earlier tool_use has its result, so each pair either survives
    ///       whole or is summarised whole.
    /// @note Searches backwards. Forwards would find a later user turn and leave
    ///       fewer than keep_recent messages, discarding the context the model
    ///       is working in right now.
    ///
    /// @code
    /// // history:  0 user "task1"        ← safe, but inside keep_recent
    /// //           1 assistant tool_use
    /// //           2 user tool_result    ← unsafe
    /// //           3 assistant "done"
    /// //           4 user "task2"        ← safe
    /// //           5 assistant "working"
    /// s.safe_split(3);   // scans back from index 3, finds nothing → 0 (no compaction)
    /// // with two more messages appended, index 4 falls outside the window and is used
    /// @endcode
    ///
    /// 找一个安全的切分点，返回下标。
    std::size_t safe_split(std::size_t keep_recent) const;

    /// @brief Replace the older history with a summary.
    ///
    /// @param llm         Used to write the summary; a cheaper model is fine.
    /// @param keep_recent Messages to preserve verbatim at the end.
    /// @return false when no safe split point was available.
    ///
    /// @note Compaction invalidates the prompt cache — the front of the request
    ///       changes. That is unavoidable, and the reason compact_at_tokens is
    ///       set high rather than compacting eagerly.
    ///
    /// 把老历史换成一段纪要。返回是否真的压缩了。
    bool compact(LlmClient& llm, std::size_t keep_recent = 6);

    // -- 持久化 --------------------------------------------------------------
    /// @brief Decide where this session will be written. Does not touch disk.
    /// @param dir Directory; the filename becomes `<id>.json`.
    /// @return *this, so it chains.
    /// @note Separate from save() because append() saves on every turn and
    ///       should not have to know where the path came from.
    Session& bind(const fs::path& dir);    // 返回 *this 方便链式调用

    /// @brief Write the session to its bound path. A no-op when unbound.
    ///
    /// @note Writes a temp file and renames it. append() saves every turn, so a
    ///       crash mid-write is not remote — and a truncated file would take the
    ///       whole history with it. rename is atomic within a filesystem.
    /// @note An empty path means an in-memory session: tests and sub-agents rely
    ///       on this to avoid leaving files behind.
    void save() const;

    /// @brief Read a session back from a file.
    ///
    /// @return A fresh empty session if the file is missing or malformed. Its
    ///         path stays unset in that case, so the damaged file is left in
    ///         place rather than overwritten.
    /// @note path_ is set to the file it came from, not rebuilt from the id —
    ///       a session whose filename was changed still writes back to itself.
    static Session load(const fs::path& path);

    const std::string& id() const { return id_; }
    int compactions() const { return compactions_; }
    const fs::path& path() const { return path_; }

  private:
    std::string id_;
    std::vector<Message> messages_;
    std::map<std::string, std::filesystem::file_time_type> read_files_;
    int compactions_ = 0;
    fs::path path_;
};

/// 压缩用的 prompt。要保留什么、丢掉什么由你定 —— 这个 prompt 的质量直接决定
/// 长任务能不能接着干。（留：目标、已做的决定、改过的文件、验证过的事实、没做完的事。
///   丢：寒暄、被否决方案的细节、过时的中间状态。）
/// @brief The prompt that produces a compaction summary.
///
/// @note Its quality decides whether a long task can continue: what it drops is
///       gone for good. Keep the goal, decisions taken, files touched, facts
///       verified, work outstanding; drop pleasantries, rejected approaches,
///       superseded intermediate state.
extern const char* kCompactPrompt;   // TODO(Stage 4): 在 session.cpp 里写

}  // namespace mini
