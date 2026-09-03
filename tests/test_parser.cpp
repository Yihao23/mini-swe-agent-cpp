//
// 输出解析 —— 锁住 agent 循环两端的 wire format 契约。
//
//     cmake --build build && ./build/test_parser
//
// 这一层的 bug 有个共同特征：**编译期看不出来，运行时也不崩**，
// 只表现为 API 返回 400 或者模型行为慢慢变差。所以只能靠断言守着。
//
// 两条最硬的约束：
//   1. 写回历史的 assistant 消息必须**原样**保留所有块（含 thinking 的 signature）
//   2. tool_use 和 tool_result 必须靠 id 配上对，且所有结果打进**一条** user 消息
//
#include <string>
#include <vector>

#include "microtest.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/message.hpp"
#include "mini_agent/parser.hpp"

using namespace mini;

// --- 夹具 -------------------------------------------------------------------
namespace {

LlmResponse response(std::vector<ContentBlock> blocks, std::string stop) {
    LlmResponse r;
    r.content = std::move(blocks);
    r.stop_reason = std::move(stop);
    return r;
}

/// 取序列化后第 i 个 content 块，测试要断言真正发出去的 JSON 长什么样。
Json wire_block(const Message& m, std::size_t i) {
    return to_json(m).at("content").at(i);
}

}  // namespace

// ===========================================================================
// parse —— 一趟遍历产出四样东西
// ===========================================================================

TEST(parse_sorts_blocks_into_text_thinking_and_calls) {
    auto r = response({ThinkingBlock{"让我想想", "sig_abc"},
                       TextBlock{"我来读一下"},
                       ToolUseBlock{"toolu_1", "read", {{"path", "a.py"}}}},
                      "tool_use");
    auto p = parse(r);

    CHECK(p.text == "我来读一下");
    CHECK(p.thinking == "让我想想");
    CHECK(p.tool_calls.size() == 1);
    CHECK(p.tool_calls.at(0).id == "toolu_1");
    CHECK(p.tool_calls.at(0).name == "read");
    CHECK(p.tool_calls.at(0).input.value("path", std::string{}) == "a.py");
    CHECK(p.stop_reason == "tool_use");
}

TEST(parse_message_keeps_every_block_verbatim) {
    // ⚠️ 写回历史的 message 必须是**原样的全部块**，不能只挑 text 和 tool_use。
    //    漏了块，下一轮请求的历史就和模型实际说过的话对不上。
    auto r = response({ThinkingBlock{"思考", "sig_abc"},
                       TextBlock{"回答"},
                       ToolUseBlock{"toolu_1", "read", {{"path", "a.py"}}}},
                      "tool_use");
    auto p = parse(r);

    CHECK_MSG(p.message.content.size() == 3, "三种块一个都不能丢");
    CHECK_MSG(p.message.role == Role::Assistant, "模型说的话是 assistant 轮");
}

TEST(parse_preserves_thinking_signature) {
    // ⚠️ signature 必须原样带回，API 会校验 —— 改了或丢了直接报错。
    auto p = parse(response({ThinkingBlock{"思考内容", "sig_xyz_123"}}, "end_turn"));

    const auto& tb = std::get<ThinkingBlock>(p.message.content.at(0));
    CHECK_MSG(tb.signature == "sig_xyz_123", "signature 必须一字不差");
    CHECK(wire_block(p.message, 0).value("signature", std::string{}) == "sig_xyz_123");
}

TEST(parse_concatenates_multiple_text_blocks) {
    auto p = parse(response({TextBlock{"前半"}, TextBlock{"后半"}}, "end_turn"));
    CHECK(p.text == "前半后半");
    CHECK_MSG(p.message.content.size() == 2, "拼接只影响 p.text，历史里仍是两个块");
}

TEST(parse_handles_empty_content) {
    auto p = parse(response({}, "end_turn"));
    CHECK(p.text.empty());
    CHECK(p.tool_calls.empty());
    CHECK(p.message.content.empty());
    CHECK(!p.wants_tools());
}

// ===========================================================================
// wants_tools —— 循环要不要继续
// ===========================================================================

TEST(wants_tools_false_when_no_tool_calls) {
    CHECK(!parse(response({TextBlock{"就说句话"}}, "end_turn")).wants_tools());
}

TEST(wants_tools_true_on_tool_use) {
    auto p = parse(response({ToolUseBlock{"toolu_1", "read", {}}}, "tool_use"));
    CHECK(p.wants_tools());
}

TEST(wants_tools_false_when_model_ended_the_turn) {
    // end_turn 表示模型认为对话结束。此时即便带着 tool_use 块也不该再执行 ——
    // 循环要停，不会有下一轮，执行了反而多出一条配不上对的 tool_result。
    auto p = parse(response({ToolUseBlock{"toolu_1", "read", {}}}, "end_turn"));
    CHECK(!p.wants_tools());
}

// ===========================================================================
// tool_result_message —— 回传给模型的格式
// ===========================================================================

TEST(all_results_go_into_exactly_one_user_message) {
    // ⚠️ 拆成多条会让模型逐渐学会不再并行调工具。
    auto m = tool_result_message({{.id = "toolu_1", .name = "glob", .output = "a.py"},
                                  {.id = "toolu_2", .name = "grep", .output = "命中 3 处"}});

    CHECK_MSG(m.role == Role::User, "工具结果以 user 身份回传");
    CHECK_MSG(m.content.size() == 2, "两个结果必须在同一条消息里");
}

TEST(result_carries_the_tool_use_id) {
    // ⚠️ 少一个配对，下一轮请求直接 400。
    auto m = tool_result_message({{.id = "toolu_42", .name = "read", .output = "内容"}});

    const auto& b = std::get<ToolResultBlock>(m.content.at(0));
    CHECK_MSG(b.tool_use_id == "toolu_42", "tool_use_id 必须原样回传");
    CHECK(wire_block(m, 0).value("tool_use_id", std::string{}) == "toolu_42");
}

TEST(is_error_appears_only_when_true) {
    // 线上格式：省略即 false。带一个 "is_error": false 不会报错，但不是标准写法。
    auto m = tool_result_message({{.id = "toolu_1", .name = "read", .output = "ok", .is_error = false},
                                  {.id = "toolu_2", .name = "bash", .output = "炸了", .is_error = true}});

    CHECK_MSG(!wire_block(m, 0).contains("is_error"), "成功的结果不该带 is_error 字段");
    CHECK_MSG(wire_block(m, 1).value("is_error", false), "失败的结果必须带 is_error: true");
    CHECK(std::get<ToolResultBlock>(m.content.at(1)).is_error);
}

TEST(empty_output_gets_a_placeholder) {
    // 空 content 让模型分不清「执行成功但没输出」和「什么都没发生」。
    auto m = tool_result_message({{.id = "toolu_1", .name = "write", .output = ""}});

    const auto& b = std::get<ToolResultBlock>(m.content.at(0));
    CHECK_MSG(!b.content.empty(), "空输出必须换成占位串");
}

TEST(ui_only_fields_stay_out_of_the_wire_format) {
    // name / duration_sec 是给 UI 显示的，不属于 API 契约。
    auto m = tool_result_message(
        {{.id = "toolu_1", .name = "read", .output = "x", .is_error = false, .duration_sec = 1.5}});

    const Json j = wire_block(m, 0);
    CHECK(!j.contains("name"));
    CHECK(!j.contains("duration_sec"));
}

// ===========================================================================
// 端到端：两个函数产出的 id 必须对得上
// ===========================================================================

TEST(tool_use_ids_pair_up_with_results) {
    // 这是整条循环最容易断的地方：parse 取出的 id 要能原封不动送回去。
    auto p = parse(response({ToolUseBlock{"toolu_a", "read", {}},
                             ToolUseBlock{"toolu_b", "grep", {}}},
                            "tool_use"));

    std::vector<ToolResultEvent> results;
    for (const auto& c : p.tool_calls)
        results.push_back({.id = c.id, .name = c.name, .output = "结果"});

    auto reply = tool_result_message(results);
    CHECK(reply.content.size() == p.tool_calls.size());
    for (std::size_t i = 0; i < reply.content.size(); ++i)
        CHECK_MSG(std::get<ToolResultBlock>(reply.content.at(i)).tool_use_id == p.tool_calls.at(i).id,
                  "第 i 个结果必须配上第 i 个 tool_use");
}

int main() { return mt::run_all(); }
