#pragma once
/// @file
/// @brief Conversation state: history, persistence, compaction (Stage 1 + 4).
//
// 【Stage 1 + Stage 4】会话状态 —— 历史 + 落盘 + 上下文压缩。
//
// Stage 1 只要：append / user / save。
// Stage 4 回来补：estimated_tokens / safe_split / compact。
//
// ── 五个字段，三类职责 ──────────────────────────────────────────────────────
//
//     id_           会话标识，也是文件名的主干。构造后不变
//     messages_     对话历史 —— 这个类存在的理由
//     read_files_   路径 → 读取时的 mtime。read 写、edit 读，两个工具靠它对话
//     compactions_  压缩过几次。落盘、/usage 显示
//     path_         写到哪。空 = 内存会话，不落盘
//
//   三类职责挤在一个类里，因为它们操作同一份 messages_：
//
//        追加        append / add_user_text         每轮
//        落盘        bind / save / load             每次 append 自动
//        压缩        estimated_tokens / safe_split / compact    超阈值时
//
// ── 一轮里谁在动它 ──────────────────────────────────────────────────────────
//
//     Agent::run()  ──append(assistant 轮)──┐
//                   ──append(工具结果)──────┤
//                   ──compact()─────────────┤
//     ReadTool      ──read_files_[p] = mtime┤──> messages_ / read_files_
//     EditTool      ──查 read_files_[p]─────┤
//     WriteTool     ──同上────────────────── ┘
//
//   ⚠️ 工具是通过 ToolContext::session 摸到它的，而循环用的是自己那个引用。
//      两者必须是**同一个对象** —— 不是的话，read 记的时间戳进了 A、edit
//      去 B 查，「改之前要先读」这条规矩静默失效，而历史看起来完全正常。
//      这条 Session 自己保证不了，见 loop.hpp 的 P2。
//
// ── 压缩：切在哪，为什么 ────────────────────────────────────────────────────
//
//   estimated_tokens() > compact_at_tokens
//        │
//        ▼
//   safe_split(keep_recent)      从 size-keep_recent 往**前**找
//        │                        第一个「真正的用户输入」（User 且没有 tool_result）
//        │
//        │   0  user "任务一"        ← 安全，但可能落在保留窗口里
//        │   1  assistant tool_use
//        │   2  user tool_result     ← 不安全！切这里 1 的 tool_use 就成孤儿
//        │   3  assistant "做完了"
//        │   4  user "任务二"        ← 安全
//        │   5  assistant "在做"
//        │
//        ├─ 返回 0 = 找不到安全切分点 → 这次不压（不是错误，历史还会变长）
//        ▼
//   compact(llm, keep_recent)
//        │  ① 前 split 条 + 一条压缩指令 → 发给模型（不给工具）
//        │  ② 拿到纪要**之后**才动历史 ⚠️ 反过来就是不可逆的数据丢失
//        │  ③ 前 split 条换成一条包着 <system-reminder> 的 user 消息
//        ▼
//   [纪要] + [最近 keep_recent 条]        compactions_ +1，落盘
//
//   ⚠️ 为什么「真正的用户输入」是安全的：它标志新一轮的开始，此刻之前所有
//      tool_use 都已经拿到结果了（否则模型还在等工具，不会把话筒交回来）。
//      切在那里，每一对 tool_use/tool_result 要么整对留下、要么整对进纪要。
//
//   ⚠️ 切错的代价是**永久的**：下一轮请求 400，而历史已经落盘，
//      --continue 回来照样 400。整个会话报废。
//
// ── 三个不变量 ──────────────────────────────────────────────────────────────
//
//   ┌── I1  每个 tool_result 在它前面都能找到配对的 tool_use ─────────────┐
//   │ 维护者：safe_split() 只返回真正的用户输入的下标；                   │
//   │        Agent::handle_interrupt() 给未完成的 tool_use 补 error 结果。│
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I2  path_ 非空时，磁盘上那份 == messages_ ───────────────────────┐
//   │ 维护者：append() 每次自己 save()，不指望调用方记得。                │
//   │ 手段：写临时文件再 rename —— 每轮都写，写一半崩掉不是稀罕事，       │
//   │      而截断的文件会把整个历史带走。rename 在同一文件系统内是原子的。│
//   │ 例外：直接改 messages() 那个非 const 重载**不会**落盘。压缩和测试   │
//   │      用它，所以 compact() 自己以 save() 收尾。                     │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I3  read_files_ 的键是**解析后的绝对路径** ──────────────────────┐
//   │ 违反 → read 存 "a.py"、edit 查 "/work/a.py"，刚读完的文件被报成    │
//   │        没读过。两边都得先过 sandbox.resolve_path()。               │
//   └────────────────────────────────────────────────────────────────────┘
//
// ⚠️ mtime 而不是内容哈希：一次 stat 对每次检查都读一遍整个文件。它会误报
//    （touch 一下没改内容也会触发）也会漏报（同一时间戳精度内的两次修改）。
//    对「别覆盖别人的改动」这件事，误报的代价是重读一次，漏报的代价是别人的活。
//
#include <filesystem>
#include <map>
#include <optional>
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
    /// @param id The id read back from the file.
    explicit Session(std::string id);

    // -- Stage 1 -------------------------------------------------------------
    /// @brief Append a message and write the session to disk.
    /// @note Saving here rather than leaving it to the caller: forgetting one
    ///       save loses the turn, and there is no signal that it happened.
    /// @param msg The turn to record.
    void append(Message msg);              // 追加并落盘

    /// @brief Append a plain user turn and save.
    /// @param text What the user typed.
    void add_user_text(std::string text);

    /// @brief The most recent assistant text, searching backwards.
    /// @return Empty when no assistant turn has produced text yet.
    std::string last_assistant_text() const;

    /// @brief The history, for reading.
    /// @return Every turn in order, oldest first.
    const std::vector<Message>& messages() const { return messages_; }

    /// @brief The history, for writing.
    /// @return A mutable reference.
    /// @warning Mutating through this does **not** save. append() exists so
    ///          that the common path cannot forget; this overload is for
    ///          compaction and for tests that build a history directly.
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
    /// @return Resolved absolute path → the mtime read recorded for it.
    std::map<std::string, std::filesystem::file_time_type>& read_files() { return read_files_; }

    // -- Stage 4 -------------------------------------------------------------
    /// @brief Rough size of the history in tokens.
    ///
    /// @note Serialised bytes / 4 is close enough to decide when to compact.
    ///       The exact count_tokens endpoint costs a round trip per check, and
    ///       the threshold has slack in it anyway.
    ///
    /// 粗估上下文大小。提示：序列化后的字节数 / 4 就够用，别一上来就调 count_tokens。
    /// @return Roughly how many tokens the history would cost.
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
    /// @code{.test}
    /// // 0 user "task1"      ← 安全，但落在 keep_recent 窗口里
    /// // 1 assistant tool_use
    /// // 2 user tool_result  ← 不安全：切在这里会让 1 的 tool_use 变成孤儿
    /// // 3 assistant "done"
    /// // 4 user "task2"      ← 安全
    /// // 5 assistant "working"
    /// @setup Session s;
    /// @setup s.messages() = {
    /// @setup     Message{Role::User, {TextBlock{"task1"}}},
    /// @setup     Message{Role::Assistant, {ToolUseBlock{"t1", "read", Json::object()}}},
    /// @setup     Message{Role::User, {ToolResultBlock{"t1", "out", false}}},
    /// @setup     Message{Role::Assistant, {TextBlock{"done"}}},
    /// @setup     Message{Role::User, {TextBlock{"task2"}}},
    /// @setup     Message{Role::Assistant, {TextBlock{"working"}}}};
    /// // 从下标 3 往前扫，只找到落在窗口内的 0 → 返回 0，这次不压
    /// s.safe_split(3)   ==> 0u
    /// // 窗口收窄到 1，下标 4 就在窗口外了，切在那条真正的用户输入上
    /// s.safe_split(1)   ==> 4u
    /// // ⚠️ 永远不会返回 2 —— 那是 tool_result，切在那里下一轮请求必然 400
    /// (s.safe_split(2) != 2u)   ==> true
    /// (s.safe_split(4) != 2u)   ==> true
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
    /// @param path The file to read.
    static Session load(const fs::path& path);

    /// @brief This session's id, which is also its filename stem.
    /// @return The id, e.g. "sess_17f3a2b1c".
    const std::string& id() const { return id_; }

    /// @brief How many times this session has been compacted.
    /// @return The count; shown by /usage, and persisted with the session.
    int compactions() const { return compactions_; }

    /// @brief Where this session writes itself.
    /// @return The bound path, or empty for an in-memory session.
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
/// @brief The instruction handed to the model when compacting.
///
/// @warning It summarises the history that is about to be **deleted**. Whatever
///          it fails to ask for is gone: a file already edited, an approach
///          already ruled out. The agent then redoes the work, or walks back
///          into the dead end — and nothing about that is visible in the code,
///          only in a long run where it keeps revisiting the same file.
///
/// @note Asking for "unresolved problems" and "approaches already ruled out"
///       is what separates a useful handover from a progress report. A summary
///       of what was done, with no statement of what is left, leaves the agent
///       with no next step to take.
extern const char* kCompactPrompt;

/// @brief The session file in `dir` that was written most recently — what
///        `--continue` resumes.
///
/// @param dir A sessions directory, usually Config::sessions_dir().
/// @return Its path; nullopt when the directory is missing or holds no session.
///
/// @warning Ordered by **last write time**, not by filename. An id is the
///          creation timestamp, so sorting names finds the session started most
///          recently. But a session is saved on every turn: resume an old one,
///          work in it, quit — and `--continue` must come back to that one, not
///          to a newer session nobody has touched since.
///
/// @note Only `*.json`. save() writes `<id>.json.tmp` and renames it; a crash
///       between the two leaves the temp file behind, and it is always the
///       newest thing in the directory.
/// @note Ties on write time fall back to the filename, so the answer does not
///       depend on directory iteration order.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup const auto dir = t.cfg.sessions_dir();
/// latest_session(dir).has_value()                                      ==> false
/// @setup Session older; older.bind(dir).save();
/// @setup Session newer; newer.bind(dir).save();
/// @setup std::filesystem::last_write_time(older.path(), std::filesystem::last_write_time(newer.path()) + 1h);
/// latest_session(dir) == older.path()                                  ==> true
/// @endcode
std::optional<fs::path> latest_session(const fs::path& dir);

}  // namespace mini
