#pragma once
/// @file
/// @brief The single place the JSON type is allowed to appear.
//
// JSON 只在这一个文件里露脸一次。
//
// 纪律：`Json` 这个类型只允许出现在**边界**上 —— llm.cpp（组请求/解响应）、
// parser.cpp、config.cpp、mcp.cpp，以及工具的 args。业务逻辑里传的应该是
// Message / ToolResult / Decision 这些领域类型，不是 Json。
//
// 你会想违反它：`Json` 什么都能装，写起来快。代价是三个月后没人知道
// 某个函数到底期望什么字段 —— 编译器帮不上任何忙。
//
// ── Json 允许出现在哪 ───────────────────────────────────────────────────────
//
//        外面的世界                     边界（Json 合法）        里面（不许有 Json）
//   ─────────────────────           ────────────────────      ────────────────────
//     HTTP 响应        ──▶  llm.cpp / parser.cpp     ──▶  Message / ContentBlock
//     config.json      ──▶  config.cpp               ──▶  Config
//     .md frontmatter  ──▶  frontmatter.cpp          ──▶  MemoryItem / Skill
//     MCP 的 JSON-RPC  ──▶  mcp.cpp                  ──▶  ToolResult
//     模型给的工具参数  ──▶  Tool::run(const Json&)   ──▶  ToolResult / Decision
//                             ▲
//                             └── 只有这一处 Json 会往里走一层：
//                                 工具参数的 schema 是任意的，没法先定类型
//
//   判据：**跨进程边界的地方用 Json，进程内部传领域类型。**
//
//   ⚠️ 边界上还有一条规矩：`args.value(key, default)` 在 key 存在但类型不对时
//      **抛异常**，不是退回默认值。模型给的东西类型对不对没保证，所以
//      builtin.cpp 里用的是自己写的 str_arg / int_arg。
//
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace mini {

/// @brief The JSON type, aliased once so the dependency has one name.
///
/// @warning Allowed only at boundaries — llm.cpp, parser.cpp, config.cpp,
///          mcp.cpp, and a tool's arguments. Business logic passes Message,
///          ToolResult and Decision. Json holds anything, which reads as
///          convenience until three months later nobody can tell what fields a
///          function expects and the compiler cannot help.
using Json = nlohmann::json;

/// @brief Placeholder for a function that is not written yet.
/// @param what Which one, so the message says what to implement next.
/// @note Throws rather than returning a default: a silently wrong result is
///       harder to find than a stack trace naming the function.
[[noreturn]] inline void todo(std::string_view what) {
    throw std::logic_error("TODO — " + std::string(what));
}

}  // namespace mini
