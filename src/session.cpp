// 【Stage 1 + Stage 4】会话状态。

#include "mini_agent/session.hpp"

#include "mini_agent/llm.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

namespace mini {

const char* kCompactPrompt = "TODO(Stage 4): 写压缩 prompt";

/// @brief Fresh session with an id derived from the current time.
///
/// @note A nanosecond timestamp in hex. Two sessions created in the same tick
///       would collide; system_clock rarely resolves that finely, so in practice
///       it takes a tight loop or two processes starting together. A counter or
///       random suffix would close it if that ever shows up.
Session::Session()
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    id_ = std::format("sess_{:x}", ns);
}

/// @brief Session with a given id — the resume path uses this.
Session::Session(std::string id)
:id_(std::move(id))
{}

/// @brief Append a message and persist immediately.
///
/// @note Saving here instead of leaving it to callers: a forgotten save loses
///       the turn silently, and the loop appends from several places.
void Session::append(Message m) 
{ 
messages_.push_back(std::move(m));
save();
}




/// @brief Append a plain user turn and persist.
/// @param s What the user typed.
void Session::add_user_text(std::string s) 
{ 
    messages_.push_back({.role = Role::User, .content = {TextBlock{std::move(s)}}});
    save();

}





/// @brief The latest assistant text, scanning backwards.
/// @return Empty if no assistant turn has produced text.
/// @note Reverse iteration stops at the first match instead of walking the
///       whole history.
std::string Session::last_assistant_text() const 
{ 
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    if (it->role == Role::Assistant) return text_of(*it);
    return {};
}

int Session::estimated_tokens() const {
    todo("Stage 4: Session::estimated_tokens —— 序列化字节数 / 4 就够用");
}

std::size_t Session::safe_split(std::size_t keep_recent) const {
    // ⚠️ 切分点之后每个 tool_result 都要能配上它的 tool_use。切在 tool_result 上，
    //    它的 tool_use 会被压进纪要里消失 —— 下一轮请求 400，而历史已经落盘，
    //    --continue 回来照样 400，整个会话永久报废。
    //
    // 「真正的用户输入」之所以安全：它标志新一轮的开始，此刻之前的所有 tool_use
    // 都已经拿到结果了（否则模型还在等工具，不会把话筒交回来）。切在那里，
    // 每一对 tool_use/tool_result 要么整对留下、要么整对进纪要，不会被拆散。
    if (messages_.size() <= keep_recent) return 0;

    // 只能往前找：往后找会让保留区少于 keep_recent，把模型当前的工作上下文压掉。
    // keep_recent==0 时 size-0 == size 会越界，夹到最后一条。
    const std::size_t start = std::min(messages_.size() - keep_recent, messages_.size() - 1);
    for (std::size_t i = start; i > 0; --i) {
        const Message& m = messages_[i];
        if (m.role == Role::User && !has_tool_result(m)) return i;
    }

    // 两个约束冲突（唯一的安全切分点在 keep_recent 范围内）→ 这次不压。
    // 返回 0 表示空区间 [0,0)，调用方不需要额外的错误通道。
    // 历史还会继续变长，下次触发时那个切分点自然就落在范围外了。
    return 0;
}

bool Session::compact(LlmClient&, std::size_t) { todo("Stage 4: Session::compact"); }

/// @brief Set where this session will be saved. Touches no files.
/// @param dir Directory; the file becomes `<dir>/<id>.json`.
/// @return *this, for chaining.
Session& Session::bind(const fs::path& dir) { 
path_ = dir / (id_ + ".json");
return *this;


}

/// @brief Serialise the session to its bound path; no-op when unbound.
///
/// @note Temp file plus rename. append() saves on every turn, so the window
///       where a crash could truncate the file is hit constantly — and a
///       half-written file destroys the history it was replacing. rename is
///       atomic within one filesystem.
/// @note An empty path means an in-memory session. Tests and sub-agents use
///       that to avoid leaving files behind.
///
/// @code
/// Session s;
/// s.bind(cfg.sessions_dir());        // .mini-agent/sessions/sess_18cc....json
/// s.add_user_text("hello");          // saved here, and after every append
/// @endcode
void Session::save() const {
    if (path_.empty()) return;
    fs::create_directories(path_.parent_path());

    // 先写临时文件再 rename。rename 在同一文件系统上是原子的 —— 否则每次 append
    // 都有一个「写到一半崩溃，连原历史一起毁掉」的窗口。
    const fs::path tmp = path_.string() + ".tmp";
    {
        std::ofstream out(tmp);
        out << Json{{"id", id_},
                    {"compactions", compactions_},
                    {"messages", to_json(messages_)}}
                   .dump(2);
    }
    fs::rename(tmp, path_);
}

/// @brief Read a session back from disk.
///
/// @param p The session file.
/// @return The session, or a fresh empty one if the file is missing or corrupt.
///
/// @note Parsing with exceptions off: a truncated file — power loss, full disk —
///       should not stop the agent from starting. The failure path leaves path_
///       unset, so the damaged file stays put instead of being overwritten by
///       the next save.
/// @note path_ is assigned directly rather than going through bind(), which
///       would rebuild the name from the id. A session file that was renamed
///       still writes back to where it was read from.
///
/// @code
/// Session s = Session::load(dir / "sess_abc.json");
/// // s.id(), s.messages(), s.compactions() restored; s.path() == that file
/// s.add_user_text("continue");   // appends to the same file
/// @endcode
Session Session::load(const fs::path& p) {
    std::ifstream in(p);

    // 文件不存在 / 内容损坏不该炸掉进程：解析失败就当成一个全新会话。
    // 注意这时 path_ 留空，save() 不会写 —— 坏文件保留在原地供排查。
    const Json j = Json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return Session{};

    Session s{j.value("id", std::string{})};
    s.messages_    = messages_from_json(j.value("messages", Json::array()));
    s.compactions_ = j.value("compactions", 0);
    s.path_        = p;
    return s;
}

}  // namespace mini
