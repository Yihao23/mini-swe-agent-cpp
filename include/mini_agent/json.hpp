#pragma once
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
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace mini {

using Json = nlohmann::json;

/// 骨架占位。实现完一个函数就把对应的 todo() 删掉。
[[noreturn]] inline void todo(std::string_view what) {
    throw std::logic_error("TODO — " + std::string(what));
}

}  // namespace mini
