// 【Stage 2】工具抽象与注册表。

#include "mini_agent/tool.hpp"
#include <algorithm>

namespace mini {

/// @brief Build a failed result.
///
/// @param s Why it failed. The model reads this and picks another approach, so
///        "file not found: a.py" beats "error".
/// @return A ToolResult with is_error set.
///
/// @note Forgetting the flag is the failure to watch for: the model then treats
///       the message as ordinary output and reasons from it as if the tool had
///       succeeded.
ToolResult ToolResult::error(std::string s) {
    return ToolResult{.content=std::move(s), .is_error = true};
}

/// @brief Default subject: the first string argument.
///
/// @param args This call's arguments.
/// @return The first string value found, or empty when there is none.
///
/// @warning "First" means first **alphabetically by key** — that is how nlohmann
///          iterates an object. Fine for a tool with one string parameter
///          (read has only `path`, grep only `pattern`). For anything with
///          several, override it: `write` would otherwise return `content`
///          rather than `path`, and the sandbox would match its rules against
///          the text being written.
///
/// @code
/// {"path":"a.py"}                       → "a.py"     ✓
/// {"limit":100,"path":"a.py"}           → "a.py"     ✓ numbers are skipped
/// {"content":"x = 1","path":"a.py"}     → "x = 1"    ✗ alphabetical order bites
/// []  or  {}  or  null                  → ""         (nothing to inspect)
/// @endcode
std::string Tool::subject(const Json& args) const {
         if (!args.is_object()) return {};
      for (const auto& [key, value] : args.items())
          if (value.is_string()) return value.get<std::string>();
      return {};
}

/// @brief This tool as one entry of the request's `tools` array.
/// @return `{"name":..., "description":..., "input_schema":...}`
///
/// @code
/// // {"name":"read",
/// //  "description":"读取文件内容，带行号。...",
/// //  "input_schema":{"type":"object","properties":{...},"required":["path"]}}
/// @endcode
Json Tool::schema() const { 
  return Json{{"name", name()}, {"description", description()}, {"input_schema",input_schema()}};
}

/// @brief Register a tool.
/// @param t Ownership is shared, so a Stage 6 subset can point at the same
///        instance without copying it.
void ToolRegistry::add(ToolPtr t) { 
    tools_.push_back(std::move(t));
}

/// @brief Find a tool by name.
///
/// @param v The name the model used.
/// @return The tool, or nullptr.
///
/// @note nullptr rather than an exception: the executor turns it into an error
///       result that lists the available names, and the model usually fixes its
///       own typo on the next turn.
/// @note Linear scan. With a dozen tools that is nothing next to the file I/O
///       the call is about to do.
Tool* ToolRegistry::get(std::string_view v) const {
     for (auto& a : tools_){
        if(a->name() == v ){return a.get();};
     }
     return nullptr;
}

/// @brief Every registered name, in registration order.
/// @note Used to tell the model what it could have called when it names
///       something that does not exist.
std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> r;
        r.reserve(tools_.size());

    for(auto& a : tools_){
        r.emplace_back(a->name());
    } 
    return r;   
}

/// @brief Non-owning pointers to every tool.
///
/// @note vector<Tool*> rather than vector<Tool&> — a container of references is
///       ill-formed, since a container needs to form pointers to its elements
///       and there is no such thing as a pointer to a reference. Values would
///       not work either: callers need to invoke run(), which is non-const.
std::vector<Tool*> ToolRegistry::all() const { 
    std::vector<Tool*> r;
    r.reserve(tools_.size());
    for(auto& a : tools_){
        r.push_back(a.get());
    }
    return  r;
}

/// @brief The `tools` array for a request, sorted by name.
///
/// @return A JSON **array** of full schemas.
///
/// @warning An object here rather than an array throws on push_back, and
///          `"tools": {...}` is not what the API accepts either.
/// @warning The sort is what keeps the prompt cache alive. Tool definitions sit
///          near the front of the request, caching matches a byte-exact prefix,
///          and registration order is not stable across builds or refactors.
///          Getting it wrong costs money quietly — nothing fails.
///
/// @code
/// // registered write, read, bash — emitted in this order:
/// // [ {"name":"bash",...}, {"name":"read",...}, {"name":"write",...} ]
/// @endcode
Json ToolRegistry::schemas() const {
auto sorted = all();
std::ranges::sort(sorted,{},[](const Tool* t){return t->name();});

Json out = Json::array();
    for(const Tool* t : sorted){
        out.push_back(t->schema());
    }
    return out;
}

ToolRegistry ToolRegistry::subset(const std::vector<std::string>&) const {
    todo("Stage 6: ToolRegistry::subset —— 给子 agent 收窄权限");
}

/// @brief How many tools are registered.
std::size_t ToolRegistry::size() const { return tools_.size(); }

}  // namespace mini
