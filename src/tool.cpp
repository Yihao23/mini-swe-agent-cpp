// 【Stage 2】工具抽象与注册表。
//
// 契约（@brief / @param / @note / 可执行示例）全部写在 include/mini_agent/tool.hpp。
// 这里只留实现层面的注释：为什么这么写，换个写法会怎样。

#include "mini_agent/tool.hpp"
#include <algorithm>

namespace mini {

ToolResult ToolResult::error(std::string s) {
    return ToolResult{.content = std::move(s), .is_error = true};
}

std::string Tool::subject(const Json& args) const {
    if (!args.is_object()) return {};
    // items() 按 key 的字典序遍历 —— 多个字符串参数的工具必须 override，
    // 否则 write 会拿到 content 而不是 path。见 tool.hpp 的 @warning。
    for (const auto& [key, value] : args.items())
        if (value.is_string()) return value.get<std::string>();
    return {};
}

Json Tool::schema() const {
    return Json{{"name", name()}, {"description", description()}, {"input_schema", input_schema()}};
}

void ToolRegistry::add(ToolPtr t) { tools_.push_back(std::move(t)); }

Tool* ToolRegistry::get(std::string_view v) const {
    for (const auto& a : tools_)
        if (a->name() == v) return a.get();
    return nullptr;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> r;
    r.reserve(tools_.size());
    for (const auto& a : tools_) r.emplace_back(a->name());
    return r;
}

std::vector<Tool*> ToolRegistry::all() const {
    std::vector<Tool*> r;
    r.reserve(tools_.size());
    for (const auto& a : tools_) r.push_back(a.get());
    return r;
}

Json ToolRegistry::schemas() const {
    // 排序键是 name()，不是注册顺序 —— prompt cache 匹配的是字节级前缀。
    auto sorted = all();
    std::ranges::sort(sorted, {}, [](const Tool* t) { return t->name(); });

    Json out = Json::array();   // 必须是 array：object 上 push_back 会抛
    for (const Tool* t : sorted) out.push_back(t->schema());
    return out;
}

ToolRegistry ToolRegistry::subset(const std::vector<std::string>&) const {
    todo("Stage 6: ToolRegistry::subset —— 给子 agent 收窄权限");
}

std::size_t ToolRegistry::size() const { return tools_.size(); }

}  // namespace mini
