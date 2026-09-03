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

class Session {
  public:
    Session();
    explicit Session(std::string id);

    // -- Stage 1 -------------------------------------------------------------
    void append(Message msg);              // 追加并落盘
    void add_user_text(std::string text);
    std::string last_assistant_text() const;

    const std::vector<Message>& messages() const { return messages_; }
    std::vector<Message>& messages() { return messages_; }

    /// path -> 上次读取时的 mtime。edit 的陈旧检查用。
    /// 想想：为什么是 mtime 而不是内容 hash？各有什么问题？
    std::map<std::string, std::filesystem::file_time_type>& read_files() { return read_files_; }

    // -- Stage 4 -------------------------------------------------------------
    /// 粗估上下文大小。提示：序列化后的字节数 / 4 就够用，别一上来就调 count_tokens。
    int estimated_tokens() const;

    /// 找一个安全的切分点，返回下标。
    ///
    /// ⚠️ Stage 4 唯一的技术难点：切分点之后的每个 tool_result 都必须能配上
    /// 它的 tool_use，否则下一轮请求直接 400。
    /// 提示：往前找最近一条"真正的用户输入"（Role::User 且 !has_tool_result）。
    std::size_t safe_split(std::size_t keep_recent) const;

    /// 把老历史换成一段纪要。返回是否真的压缩了。
    bool compact(LlmClient& llm, std::size_t keep_recent = 6);

    // -- 持久化 --------------------------------------------------------------
    Session& bind(const fs::path& dir);    // 返回 *this 方便链式调用
    void save() const;
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
extern const char* kCompactPrompt;   // TODO(Stage 4): 在 session.cpp 里写

}  // namespace mini
