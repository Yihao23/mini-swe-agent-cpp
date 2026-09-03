//
// 会话持久化 —— 锁住 save/load 的 round-trip 契约。
//
//     cmake --build build && ./build/test_session
//
// 这些用例存在的理由：save() 和 load() 是**两处独立的代码**，它们对
// JSON 结构的理解必须一致。编译器完全帮不上忙 —— key 拼错、漏读一个字段，
// 都只在真正存盘再读回来时才暴露。所以每加一个持久化字段，这里就该多一条断言。
//
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "microtest.hpp"
#include "mini_agent/message.hpp"
#include "mini_agent/session.hpp"

using namespace mini;
namespace fs = std::filesystem;

// --- 夹具 -------------------------------------------------------------------
namespace {

/// 每次调用给一个全新的空目录。带 pid 是为了 ctest 并行时不互相踩。
fs::path make_dir() {
    static int n = 0;
    auto dir = fs::temp_directory_path() /
               ("mini-agent-session-" + std::to_string(::getpid()) + "-" + std::to_string(++n));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// 一段有代表性的历史：文本 + 工具调用 + 工具结果，覆盖三种块。
void fill(Session& s) {
    s.add_user_text("看看 hello.py");
    s.append({.role = Role::Assistant,
              .content = {TextBlock{"我来读一下"},
                          ToolUseBlock{"toolu_01", "read", {{"path", "hello.py"}}}}});
    s.append({.role = Role::User,
              .content = {ToolResultBlock{"toolu_01", "print('hi')", false}}});
}

}  // namespace

// ===========================================================================
// round-trip
// ===========================================================================

TEST(session_roundtrip_preserves_messages) {
    Session s;
    s.bind(make_dir());
    fill(s);

    Session t = Session::load(s.path());

    CHECK(t.id() == s.id());
    CHECK_MSG(t.messages().size() == 3, "三条消息一条都不能少");
    CHECK(t.messages().at(0).role == Role::User);
    CHECK(t.messages().at(1).role == Role::Assistant);
    CHECK(text_of(t.messages().at(0)) == "看看 hello.py");
    CHECK(text_of(t.messages().at(1)) == "我来读一下");
}

TEST(session_roundtrip_preserves_tool_blocks) {
    // ⚠️ tool_use 和 tool_result 靠 id 配对。任何一端在存盘时丢失，
    //    下一轮请求就会因为 tool_use 没有配对结果被 API 拒绝（400）。
    Session s;
    s.bind(make_dir());
    fill(s);

    Session t = Session::load(s.path());

    const auto& use = std::get<ToolUseBlock>(t.messages().at(1).content.at(1));
    CHECK(use.id == "toolu_01");
    CHECK(use.name == "read");
    CHECK_MSG(use.input.value("path", std::string{}) == "hello.py", "工具参数必须原样带回");

    const auto& res = std::get<ToolResultBlock>(t.messages().at(2).content.at(0));
    CHECK_MSG(res.tool_use_id == use.id, "tool_result 必须还能配上它的 tool_use");
    CHECK(res.content == "print('hi')");
    CHECK(res.is_error == false);
}

TEST(session_roundtrip_preserves_compactions) {
    // compactions_ 不影响任何表面行为，最容易在存盘时被忘掉。
    // 它是私有的、compact() 又还没实现，所以直接造一份带非零值的文件 ——
    // 用默认值 0 测等于什么都没测（0 == 0 恒成立）。
    auto dir = make_dir();
    const auto path = dir / "sess_x.json";
    std::ofstream(path) << R"({"id":"sess_x","compactions":3,"messages":[]})";

    Session t = Session::load(path);
    CHECK_MSG(t.compactions() == 3, "load 必须读回 compactions");

    t.add_user_text("再来一轮");   // 触发 save
    Session u = Session::load(path);
    CHECK_MSG(u.compactions() == 3, "save 必须把 compactions 写回去");
}

TEST(session_load_restores_path_so_it_keeps_saving) {
    Session s;
    s.bind(make_dir());
    fill(s);
    const auto path = s.path();

    Session t = Session::load(path);
    CHECK_MSG(t.path() == path, "从哪读的就该写回哪");

    t.add_user_text("继续");
    Session u = Session::load(path);
    CHECK_MSG(u.messages().size() == 4, "load 回来的 session 必须还能继续落盘");
}

// ===========================================================================
// 落盘时机与安全性
// ===========================================================================

TEST(append_persists_immediately) {
    // 调用方不该需要记得手动 save()（忘了就丢历史）。
    Session s;
    s.bind(make_dir());
    CHECK(!fs::exists(s.path()));

    s.add_user_text("你好");
    CHECK_MSG(fs::exists(s.path()), "append/add_user_text 之后文件必须已经在磁盘上");
}

TEST(save_leaves_no_temp_file) {
    // save() 走「写 .tmp 再 rename」保证原子性 —— rename 必须真的发生，
    // 否则会在状态目录里堆一地 .tmp。
    Session s;
    s.bind(make_dir());
    fill(s);

    CHECK(fs::exists(s.path()));
    CHECK_MSG(!fs::exists(s.path().string() + ".tmp"), "不该留下 .tmp 残留");
}

TEST(unbound_session_writes_nothing) {
    // path_ 为空 = 纯内存会话。测试和子 agent 靠这个不产生文件。
    auto dir = make_dir();
    Session s;                 // 没有 bind
    fill(s);

    CHECK(s.path().empty());
    CHECK_MSG(fs::is_empty(dir), "没 bind 过的 session 不该写任何文件");
    CHECK_MSG(s.messages().size() == 3, "但内存里的历史照常工作");
}

// ===========================================================================
// 坏输入
// ===========================================================================

TEST(load_survives_corrupt_file) {
    // 会话文件可能被截断（断电、磁盘满）。加载一个坏文件不该炸掉整个进程。
    auto dir = make_dir();
    const auto bad = dir / "bad.json";
    std::ofstream(bad) << "{ this is not json";

    Session t = Session::load(bad);       // 不抛异常就算过
    CHECK(t.messages().empty());
    CHECK_MSG(t.path().empty(), "解析失败时不接管该文件 —— 坏文件留在原地供排查");
}

TEST(load_survives_missing_file) {
    Session t = Session::load(make_dir() / "not_here.json");
    CHECK(t.messages().empty());
}

TEST(load_tolerates_missing_fields) {
    // 老版本写的文件可能没有新加的字段。缺字段该走默认值，不该抛。
    auto dir = make_dir();
    const auto partial = dir / "partial.json";
    std::ofstream(partial) << R"({"id": "sess_old"})";

    Session t = Session::load(partial);
    CHECK(t.id() == "sess_old");
    CHECK(t.messages().empty());
    CHECK(t.compactions() == 0);
}

// ===========================================================================
// 小工具
// ===========================================================================

TEST(last_assistant_text_finds_the_latest_one) {
    Session s;
    s.append({.role = Role::Assistant, .content = {TextBlock{"第一次回答"}}});
    s.add_user_text("再说一遍");
    s.append({.role = Role::Assistant, .content = {TextBlock{"第二次回答"}}});

    CHECK(s.last_assistant_text() == "第二次回答");
}

TEST(last_assistant_text_empty_when_none) {
    Session s;
    s.add_user_text("只有用户说话");
    CHECK(s.last_assistant_text().empty());
}

int main() { return mt::run_all(); }
