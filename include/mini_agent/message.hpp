#pragma once
//
// 【Stage 0】对话历史的数据模型。
//
// ── C++ 的第一个设计决定：variant 还是虚函数？──────────────────────────────
//
// content block 的类型是一个**封闭集合** —— 由 API 定义，你不会去扩展它。
// 封闭集合用 std::variant：
//     * 加一种块时，所有 std::visit 都会编译报错，逼你处理（穷尽性检查）
//     * 值语义，没有堆分配，拷贝/落盘都简单
//
// 而 Tool 是**开放集合** —— 用户随时会加自己的工具。开放集合才用虚函数（见 tool.hpp）。
//
// 这条判据在整个项目里反复出现：封闭用 variant，开放用虚函数。
//
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "mini_agent/json.hpp"

namespace mini {

enum class Role { User, Assistant };

struct TextBlock {
    std::string text;
};

struct ThinkingBlock {
    std::string thinking;
    std::string signature;  // ⚠️ 必须原样带回，API 会校验；改了就报错
};

struct RedactedThinkingBlock {
    std::string data;
};

struct ToolUseBlock {
    std::string id;    // toolu_...，回传 tool_result 时要对上
    std::string name;
    Json input;        // 工具参数是任意 schema，这里 Json 是合理的（边界）
};

struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
};

using ContentBlock =
    std::variant<TextBlock, ThinkingBlock, RedactedThinkingBlock, ToolUseBlock, ToolResultBlock>;

struct Message {
    Role role{};
    std::vector<ContentBlock> content;
};

// --- 序列化（在 src/message.cpp 实现）---------------------------------------
// TODO(Stage 1): 用 std::visit 分派，每种块产出 {"type": "...", ...}
Json to_json(const ContentBlock& block);
Json to_json(const Message& msg);
Json to_json(const std::vector<Message>& msgs);

/// 反向。未知 type 返回 nullopt —— 丢弃而不是猜结构：API 会加新块类型，
/// 伪造一个空 TextBlock 等于伪造历史。调用方负责跳过 nullopt。
std::optional<ContentBlock> block_from_json(const Json& j);
Message message_from_json(const Json& j);
std::vector<Message> messages_from_json(const Json& j);

// --- 小工具 -----------------------------------------------------------------
/// 把一条消息里所有 TextBlock 拼起来。
std::string text_of(const Message& msg);

/// 这条消息里有没有 tool_result 块（Stage 4 找压缩切分点要用）。
bool has_tool_result(const Message& msg);

std::string_view to_string(Role role);
}  // namespace mini
