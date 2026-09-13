// 【Stage 1 + Stage 4】会话状态。

#include "mini_agent/session.hpp"

#include "mini_agent/llm.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

namespace mini {

/// 交给模型的压缩指令。
///
/// ⚠️ 它总结的是**将要被丢掉**的那段历史，所以宁可啰嗦也别漏。模型看不到原文
///    之后只剩这段文字 —— 漏掉一个已经改过的文件、一个已经排除的方案，它就会
///    重做一遍，甚至把改对的东西改回去。
///
/// 「未解决的问题」和「已经排除的方案」这两条尤其重要：前者是它接下来要干的活，
/// 后者防止它绕回死胡同。只写「做了什么」的纪要会让 agent 原地打转。
///
/// @note 声明在 session.hpp 里是为了让测试能断言它问到了这两条 —— 一份漏掉
///       它们的压缩 prompt 会让 agent 重做已经做过的事，而这件事从代码上
///       完全看不出来，只在跑长任务时表现为"它怎么又在改这个文件"。
const char* kCompactPrompt =
    "以上是一段较长的工作记录，即将被这份纪要取代 —— 原文你之后看不到了。\n"
    "请写一份交接纪要，覆盖：\n"
    "1. 用户的目标，以及中途提出的修正\n"
    "2. 已经查明的事实：文件路径、函数名、关键结论（写具体，别写「看了一些文件」）\n"
    "3. 已经做出的改动：改了哪些文件、改了什么\n"
    "4. 尚未解决的问题，以及下一步打算\n"
    "5. 已经试过并排除的方案，和排除的原因\n"
    "\n"
    "只输出纪要正文，不要寒暄。";

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
    // 字节数 / 4。英文大约 4 字节一个 token，中文和 JSON 结构会偏差不少，
    // 但这个值只用来决定"该不该压缩了"，阈值本身留了余量。
    // 精确的 count_tokens 端点每次检查要多一次往返 —— 每轮一次，不值。
    if (messages_.empty()) return 0;
    return static_cast<int>(to_json(messages_).dump().size() / 4);
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

bool Session::compact(LlmClient& llm, std::size_t keep_recent) {
    const std::size_t split = safe_split(keep_recent);
    if (split == 0) return false;   // 没有安全切分点，这次不压

    // ① 把要丢掉的那一段单独拿出来，末尾追加压缩指令。
    //    用 vector 的拷贝而不是引用：下面要往里 push 一条指令消息，
    //    而且 llm.complete 期间 messages_ 必须保持原样 —— 请求失败时要能原封不动地退回。
    std::vector<Message> older(messages_.begin(), messages_.begin() + static_cast<long>(split));
    older.push_back(Message{Role::User, {TextBlock{kCompactPrompt}}});

    // ② 不给工具。给了模型可能真的去调，而这里只要一段文字。
    LlmRequest req;
    req.messages = &older;
    req.tools = Json::array();

    const auto resp = llm.complete(req);
    if (!resp) return false;   // ⚠️ 失败就原样返回，绝不能把历史删了再发现总结没拿到

    std::string summary;
    for (const auto& b : resp->content)
        if (const auto* t = std::get_if<TextBlock>(&b)) summary += t->text;
    if (summary.empty()) return false;   // 空纪要比不压缩糟糕得多

    // ③ 用一条 user 消息取代前 split 条。
    //    做成独立消息而不是塞进后面那条：连续两条 user 是允许的（实测过），
    //    而塞进别人的消息会把用户原话和机器生成的纪要混在一起，
    //    之后再压缩时分不清哪句是谁说的。
    //    包成 <system-reminder>：模型对这个标签的语义有先验，不会当成用户指令去执行。
    Message note{Role::User,
                 {TextBlock{"<system-reminder>\n以下是此前对话的纪要（原文已省略）：\n\n" +
                            summary + "\n</system-reminder>"}}};

    std::vector<Message> compacted;
    compacted.reserve(messages_.size() - split + 1);
    compacted.push_back(std::move(note));
    for (std::size_t i = split; i < messages_.size(); ++i) compacted.push_back(messages_[i]);
    messages_ = std::move(compacted);

    ++compactions_;
    save();
    return true;
}

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

std::optional<fs::path> latest_session(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;

    std::optional<fs::path> best;
    fs::file_time_type best_time{};
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto& e = *it;
        // ⚠️ 只认 .json。save() 先写 <id>.json.tmp 再 rename，两步之间崩溃会留下
        //    .tmp —— 而它永远是目录里最新的那个文件。
        if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;

        const auto t = e.last_write_time(ec);
        if (ec) { ec.clear(); continue; }   // 读 mtime 失败（刚被删掉？）就跳过这一个

        // ⚠️ 按**最后写入时间**，不按文件名。id 是创建时刻，按名字排得到的是
        //    「最近创建的」；会话每轮都落盘，mtime 才是「最近用过的」。
        //    时间相同再比文件名，免得结果取决于目录遍历顺序。
        if (!best || t > best_time || (t == best_time && e.path() > *best)) {
            best = e.path();
            best_time = t;
        }
    }
    return best;
}

}  // namespace mini
