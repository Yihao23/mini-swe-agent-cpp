// 【Stage 1 + Stage 4】会话状态。

#include "mini_agent/session.hpp"

#include "mini_agent/llm.hpp"

#include <chrono>
#include <format>
#include <fstream>

namespace mini {

const char* kCompactPrompt = "TODO(Stage 4): 写压缩 prompt";

Session::Session()
{
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    id_ = std::format("sess_{:x}", ns);
}

Session::Session(std::string id)
:id_(std::move(id))
{}

void Session::append(Message m) 
{ 
messages_.push_back(std::move(m));
save();
}




void Session::add_user_text(std::string s) 
{ 
    messages_.push_back({.role = Role::User, .content = {TextBlock{std::move(s)}}});
    save();

}





std::string Session::last_assistant_text() const 
{ 
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    if (it->role == Role::Assistant) return text_of(*it);
    return {};
}

int Session::estimated_tokens() const {
    todo("Stage 4: Session::estimated_tokens —— 序列化字节数 / 4 就够用");
}

std::size_t Session::safe_split(std::size_t) const {
    // ⚠️ Stage 4 唯一的技术难点：切分点之后每个 tool_result 都要能配上 tool_use。
    // 提示：往前找最近一条 Role::User 且 !has_tool_result 的消息。
    todo("Stage 4: Session::safe_split");
}

bool Session::compact(LlmClient&, std::size_t) { todo("Stage 4: Session::compact"); }

Session& Session::bind(const fs::path& dir) { 
path_ = dir / (id_ + ".json");
return *this;


}

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
