// 【Stage 4】上下文压缩与 prompt 组装。
//
// 这一层出错的方式都很安静：切错地方 → 下一轮 400 且历史已落盘，--continue
// 回来照样 400；system 里混进动态内容 → 缓存每轮作废，账单变三倍而功能完全正常。
// 所以断言重点不在"能不能压"，在"压完之后还能不能用"。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/prompt.hpp"
#include "mini_agent/session.hpp"

#include <unistd.h>
#include <filesystem>
#include <fstream>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

Config test_cfg() {
    Config cfg;
    cfg.workdir = fs::temp_directory_path() /
                  ("compact_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
    fs::remove_all(cfg.workdir);
    fs::create_directories(cfg.workdir);
    cfg.workdir = fs::canonical(cfg.workdir);
    cfg.normalize();
    return cfg;
}

Message user_text(std::string t) { return Message{Role::User, {TextBlock{std::move(t)}}}; }
Message asst_text(std::string t) { return Message{Role::Assistant, {TextBlock{std::move(t)}}}; }
Message asst_tool(std::string id, std::string name) {
    return Message{Role::Assistant, {ToolUseBlock{std::move(id), std::move(name), Json::object()}}};
}
Message tool_res(std::string id, std::string out) {
    return Message{Role::User, {ToolResultBlock{std::move(id), std::move(out), false}}};
}

/// 一份足够长、包含完整 tool 配对的历史。
/// 0 user  1 asst_tool  2 tool_result  3 asst  4 user  5 asst_tool  6 tool_result  7 asst
Session make_history() {
    Session s;
    s.messages() = {user_text("任务一"),      asst_tool("t1", "read"), tool_res("t1", "内容"),
                    asst_text("看完了"),      user_text("任务二"),     asst_tool("t2", "grep"),
                    tool_res("t2", "命中"),   asst_text("找到了")};
    return s;
}

/// 每个 tool_use 都能在**它之后**找到配对的 tool_result 吗？
bool pairing_intact(const Session& s) {
    for (std::size_t i = 0; i < s.messages().size(); ++i) {
        for (const auto& b : s.messages()[i].content) {
            const auto* tu = std::get_if<ToolUseBlock>(&b);
            if (!tu) continue;
            bool found = false;
            for (std::size_t j = i + 1; j < s.messages().size() && !found; ++j)
                for (const auto& c : s.messages()[j].content)
                    if (const auto* tr = std::get_if<ToolResultBlock>(&c))
                        if (tr->tool_use_id == tu->id) found = true;
            if (!found) return false;
        }
    }
    return true;
}
/// 反过来：每个 tool_result 都能在**它之前**找到发起它的 tool_use 吗？
bool no_orphan_results(const Session& s) {
    for (std::size_t i = 0; i < s.messages().size(); ++i) {
        for (const auto& b : s.messages()[i].content) {
            const auto* tr = std::get_if<ToolResultBlock>(&b);
            if (!tr) continue;
            bool found = false;
            for (std::size_t j = 0; j < i && !found; ++j)
                for (const auto& c : s.messages()[j].content)
                    if (const auto* tu = std::get_if<ToolUseBlock>(&c))
                        if (tu->id == tr->tool_use_id) found = true;
            if (!found) return false;
        }
    }
    return true;
}

/// 一个必定失败的客户端。FakeLlm 剧本用完会返回兜底文本 "done"，
/// 测不到「请求失败」这条路径 —— 而那正是最危险的一条：先删历史再发现
/// 总结没拿到，就是不可逆的数据丢失。
struct FailingLlm final : LlmClient {
    int calls = 0;
    std::expected<LlmResponse, LlmError> complete(const LlmRequest&) override {
        ++calls;
        return std::unexpected(LlmError{529, "overloaded_error", "服务过载"});
    }
    const Usage& usage() const override { return usage_; }
    Usage usage_;
};

bool has(const std::string& hay, std::string_view needle) {
    return hay.find(needle) != std::string::npos;
}
std::string all_text(const Session& s) {
    std::string out;
    for (const auto& m : s.messages()) out += text_of(m) + "\n";
    return out;
}

}  // namespace

// ── estimated_tokens ────────────────────────────────────────────────────────
TEST(estimated_tokens_is_zero_for_an_empty_session) {
    CHECK(Session{}.estimated_tokens() == 0);
}

TEST(estimated_tokens_grows_with_the_history) {
    Session s;
    s.messages().push_back(user_text("短"));
    const int small = s.estimated_tokens();
    s.messages().push_back(user_text(std::string(4000, 'x')));
    const int big = s.estimated_tokens();

    CHECK_MSG(small > 0, "非空会话不该估成 0");
    CHECK_MSG(big > small + 900,
              "4000 字节大约 1000 token —— 估算要跟内容的量级对上，否则阈值形同虚设");
}

// ── compact ─────────────────────────────────────────────────────────────────
TEST(compact_replaces_the_front_and_keeps_the_tail) {
    const auto cfg = test_cfg();
    auto s = make_history();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要：改了 a.cpp，还差测试")}, "end_turn"}});

    const auto before = s.messages().size();
    CHECK(s.compact(llm, /*keep_recent=*/3));

    CHECK_MSG(s.messages().size() < before, "压缩后应该更短");
    CHECK_MSG(has(all_text(s), "纪要：改了 a.cpp"), "模型给的总结要真的进历史");
    CHECK_MSG(has(all_text(s), "找到了"), "最近的消息要原样保留");
    CHECK_MSG(!has(all_text(s), "任务一"), "被压缩的那段应该没了");
    CHECK(s.compactions() == 1);
}

TEST(compaction_never_orphans_a_tool_pair) {
    // ⚠️ 这是整个 Stage 4 最要紧的一条。切在 tool_result 上，它的 tool_use
    //    会被压进纪要里消失 —— 下一轮请求 400，而历史已经落盘，--continue
    //    回来照样 400，整个会话永久报废。
    //
    // 必须遍历多个 keep_recent：「按 size-keep_recent 直接切」这种朴素实现
    // 在某些取值下碰巧也不会拆散配对（本例的 keep_recent=3 就是），
    // 单测一个值证明不了什么。keep_recent=2 才切在 tool_result 上。
    for (std::size_t keep = 1; keep <= 5; ++keep) {
        const auto cfg = test_cfg();
        FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要")}, "end_turn"}});
        auto s = make_history();
        s.compact(llm, keep);   // 压不压得动都行，压了就必须是完整的

        CHECK_MSG(no_orphan_results(s),
                  "keep_recent=" + std::to_string(keep) +
                      " 时留下了没有 tool_use 的 tool_result → 下一轮必然 400");
        CHECK_MSG(pairing_intact(s),
                  "keep_recent=" + std::to_string(keep) + " 时留下了没有结果的 tool_use");
    }
}

TEST(the_summary_is_marked_as_a_summary) {
    const auto cfg = test_cfg();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要正文")}, "end_turn"}});
    auto s = make_history();
    CHECK(s.compact(llm, 3));
    // 不加标记的话，模型会把这段机器生成的文字当成用户的新指令去执行。
    CHECK_MSG(has(all_text(s), "<system-reminder>"), "纪要要包在 system-reminder 里");
}

TEST(compact_asks_for_what_the_next_turn_needs) {
    const auto cfg = test_cfg();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要")}, "end_turn"}});
    auto s = make_history();
    CHECK(s.compact(llm, 3));

    // 压缩指令必须问到"未解决的问题"和"已排除的方案"。只让它写"做了什么"，
    // agent 会重做已经做过的事，或者绕回已经排除的死胡同。
    const auto& sent = llm.calls().at(0).dump();
    CHECK_MSG(has(sent, "未解决"), "纪要要包含尚未解决的问题");
    CHECK_MSG(has(sent, "排除"), "纪要要包含已经排除的方案");
}

TEST(compact_sends_no_tools) {
    const auto cfg = test_cfg();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要")}, "end_turn"}});
    auto s = make_history();
    CHECK(s.compact(llm, 3));
    const auto sent = llm.calls().at(0);
    // 给了工具，模型可能真的去调 —— 这里只要一段文字。
    CHECK_MSG(!sent.contains("tools") || sent.at("tools").empty(),
              "压缩请求不该带工具");
}

TEST(a_failed_request_leaves_the_history_untouched) {
    FailingLlm llm;
    auto s = make_history();
    const auto before = s.messages().size();

    CHECK_MSG(!s.compact(llm, 3), "请求失败就该返回 false");
    CHECK_MSG(s.messages().size() == before,
              "⚠️ 绝不能先删历史再发现总结没拿到 —— 那是不可逆的数据丢失");
    CHECK_MSG(has(all_text(s), "任务一"), "原文必须一个字不少地留着");
    CHECK(s.compactions() == 0);
    CHECK_MSG(llm.calls == 1, "该试过一次");
}

TEST(an_empty_summary_leaves_the_history_untouched) {
    const auto cfg = test_cfg();
    // 请求成功了，但模型什么都没说。空纪要比不压缩糟糕得多 ——
    // 那等于把整段历史换成一句空话。
    FakeLlm llm(cfg, {{{FakeBlock::text_block("")}, "end_turn"}});
    auto s = make_history();
    const auto before = s.messages().size();

    CHECK_MSG(!s.compact(llm, 3), "空纪要要当成失败");
    CHECK(s.messages().size() == before);
    CHECK(s.compactions() == 0);
}

TEST(compact_declines_when_there_is_no_safe_split) {
    const auto cfg = test_cfg();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要")}, "end_turn"}});
    Session s;
    s.messages() = {user_text("只有一轮"), asst_text("回答")};
    CHECK_MSG(!s.compact(llm, 6), "保留窗口比历史还长 → 不压，不是错误");
    CHECK_MSG(llm.calls().empty(), "没有切分点就不该浪费一次 API 调用");
}

TEST(compacting_twice_keeps_shrinking) {
    const auto cfg = test_cfg();
    FakeLlm llm(cfg, {{{FakeBlock::text_block("纪要一")}, "end_turn"},
                      {{FakeBlock::text_block("纪要二")}, "end_turn"}});
    auto s = make_history();
    CHECK(s.compact(llm, 3));
    s.messages().push_back(user_text("任务三"));
    s.messages().push_back(asst_text("好"));
    CHECK_MSG(s.compact(llm, 2), "长历史应该能压第二次");
    CHECK(s.compactions() == 2);
    CHECK(no_orphan_results(s));
}

// ── build_system ────────────────────────────────────────────────────────────
TEST(system_is_byte_stable_across_calls) {
    const auto cfg = test_cfg();
    // ⚠️ 这是这一层最贵的错误。system 渲染在请求最前面，缓存逐字节匹配前缀 ——
    //    混进一个时间戳，它后面的整段对话每轮都要重新计费，而功能完全正常，
    //    没有任何东西会失败。
    std::string first;
    for (const auto& b : build_system(cfg)) first += b.text;
    for (int i = 0; i < 5; ++i) {
        std::string again;
        for (const auto& b : build_system(cfg)) again += b.text;
        CHECK_MSG(again == first, "system 必须逐字节稳定 —— 变一个字节缓存就作废");
    }
}

TEST(only_the_last_system_block_carries_the_cache_breakpoint) {
    const auto cfg = test_cfg();
    const auto blocks = build_system(cfg);
    CHECK(!blocks.empty());
    for (std::size_t i = 0; i + 1 < blocks.size(); ++i)
        CHECK_MSG(!blocks[i].cache_breakpoint, "断点打在中间 → 后面几块每轮全价");
    CHECK_MSG(blocks.back().cache_breakpoint, "最后一块要打断点，一次缓存 tools + system");
}

TEST(system_mentions_the_workdir_and_the_tools) {
    const auto cfg = test_cfg();
    std::string all;
    for (const auto& b : build_system(cfg)) all += b.text + "\n";
    CHECK(has(all, cfg.workdir.string()));
    CHECK_MSG(has(all, "read") && has(all, "edit") && has(all, "grep"),
              "模型得知道有哪些工具、什么时候用哪个");
}

TEST(identity_and_extra_reach_the_system_prompt) {
    const auto cfg = test_cfg();
    std::string all;
    for (const auto& b : build_system(cfg, nullptr, nullptr, "额外说明", "我是子 agent"))
        all += b.text + "\n";
    CHECK(has(all, "我是子 agent"));
    CHECK(has(all, "额外说明"));
    CHECK_MSG(!has(all, "You are a software engineering agent."),
              "给了 identity 就该覆盖默认身份段，不是叠加");
}

TEST(no_empty_system_blocks) {
    const auto cfg = test_cfg();
    for (const auto& b : build_system(cfg))
        CHECK_MSG(!b.text.empty(), "空块也是字节，缓存按字节匹配");
}

// ── project_doc ─────────────────────────────────────────────────────────────
TEST(project_doc_reads_the_conventions_file) {
    const auto cfg = test_cfg();
    std::ofstream(cfg.workdir / "AGENTS.md") << "提交前必须跑 ctest\n";
    const auto doc = project_doc(cfg.workdir);
    CHECK(has(doc, "提交前必须跑 ctest"));
    CHECK_MSG(has(doc, "AGENTS.md"), "要说明这段规矩是从哪读来的");
}

TEST(project_doc_prefers_the_first_name_it_finds) {
    const auto cfg = test_cfg();
    std::ofstream(cfg.workdir / "AGENTS.md") << "第一份\n";
    std::ofstream(cfg.workdir / "CLAUDE.md") << "第二份\n";
    const auto doc = project_doc(cfg.workdir);
    CHECK(has(doc, "第一份"));
    CHECK_MSG(!has(doc, "第二份"), "两份都读会让同一段规矩出现两次");
}

TEST(project_doc_truncates_instead_of_giving_up) {
    const auto cfg = test_cfg();
    std::ofstream(cfg.workdir / "CLAUDE.md") << std::string(50000, 'x');
    const auto doc = project_doc(cfg.workdir, 1000);
    CHECK_MSG(doc.size() < 2000, "一份 50KB 的约定文件不能整个塞进 system");
    CHECK_MSG(has(doc, "截断"), "截断了要说，否则模型以为读全了");
}

TEST(project_doc_is_empty_when_there_is_none) {
    CHECK(project_doc(test_cfg().workdir).empty());
}

// ── reminder / turn_context ─────────────────────────────────────────────────
TEST(reminder_wraps_but_not_when_empty) {
    CHECK(has(reminder("后台任务完成"), "<system-reminder>"));
    CHECK(has(reminder("后台任务完成"), "后台任务完成"));
    CHECK_MSG(reminder("").empty(), "空内容不该产出一个空标签 —— 那也是字节");
}

TEST(turn_context_is_empty_when_nothing_changed) {
    CHECK_MSG(turn_context(nullptr, Json::array()).empty(),
              "没有动态内容就不该注入任何东西");
}

TEST(turn_context_reports_todos) {
    Json todos = Json::array();
    todos.push_back({{"content", "写测试"}, {"status", "pending"}});
    todos.push_back({{"content", "改文档"}, {"status", "done"}});
    const auto ctx = turn_context(nullptr, todos);
    CHECK(has(ctx, "写测试"));
    CHECK(has(ctx, "改文档"));
    CHECK(has(ctx, "pending"));
}

int main() { return mt::run_all(); }
