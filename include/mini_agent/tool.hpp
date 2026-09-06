#pragma once
//
// 【Stage 2】工具抽象 —— 一个工具 = JSON Schema（给模型看）+ run()（给你跑）。
//
// 这个抽象设计对一次，后面五个阶段都在复用：
// 本地文件操作、bash、子 agent、MCP 远程工具，全是这一个形状。
//
// ── C++ 的第二个设计决定：谁拥有 Tool？──────────────────────────────────────
//
// 注册表存 shared_ptr<Tool>，不是 unique_ptr。理由不是偷懒：
// Stage 6 的子 agent 要拿主注册表的**子集**（explorer 只给只读工具），
// 两个注册表指向同一批工具实例 —— 这是真正的共享所有权。
//
// ToolContext 则相反：里面全是**非拥有裸指针**。它是一个"借来的引用包"，
// 生命周期由 App 保证（所有被指向的对象都活得比一次 agent run 长）。
//
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mini_agent/json.hpp"

namespace mini {

struct Config;
class Sandbox;
class Session;
class Memory;
class SkillRegistry;
class BackgroundManager;
class ToolRegistry;

/// @brief What a tool returns.
///
/// @note Failure is `is_error`, not an exception. A tool that cannot read one
///       file should hand the model a reason it can work around, not end the
///       whole run.
/// @note `metadata` is for the UI (line counts, match counts) and never reaches
///       the API.
struct ToolResult {
    std::string content;
    bool is_error = false;   // ← 是值，不是异常。见 BUILD-GUIDE Stage 0
    Json metadata = Json::object();

    /// @brief Build a failed result.
    ///
    /// @param msg Why it failed. The model reads this and picks another
    ///        approach, so "file not found: a.py" beats "error".
    /// @return A ToolResult with is_error set.
    ///
    /// @note Forgetting the flag is the failure to watch for: the model then
    ///       treats the message as ordinary output and reasons from it as if
    ///       the tool had succeeded.
    ///
    /// @code{.test}
    /// ToolResult::error("file not found").is_error      ==> true
    /// ToolResult::error("file not found").content       ==> "file not found"
    /// ToolResult::error("x").metadata.is_object()       ==> true
    /// ToolResult{.content = "ok"}.is_error              ==> false
    /// @endcode
    static ToolResult error(std::string msg);
};

/// @brief Launches a sub-agent: spawn(agent_type, prompt) → its conclusion.
/// @note Empty means sub-agents are not available in this context.
///
/// 子 agent 启动器：spawn(agent_type, prompt) -> 结论文本。Stage 6 才填。
using SpawnFn = std::function<std::string(std::string_view, std::string_view)>;

/// @brief Everything a tool can reach at run time.
///
/// Every tool needs different things — read wants the workdir, bash wants the
/// sandbox, memory tools want the store. Giving each its own parameters would
/// leave them with different signatures, and they could no longer share one
/// interface. So they all take this and use the two or three fields that
/// concern them.
///
/// @warning Non-owning pointers throughout. Whatever they point at must outlive
///          the agent run; App is what guarantees that. Nothing here manages a
///          lifetime.
/// @note The Stage 5/6 fields stay null until those stages land — which is why
///       they are pointers rather than references.
///
/// 工具运行时能摸到的一切。**全是非拥有指针**，不负责任何生命周期。
struct ToolContext {
    const Config* cfg = nullptr;      // 契约：非空
    Sandbox* sandbox = nullptr;       // 契约：非空
    Session* session = nullptr;
    Memory* memory = nullptr;         // Stage 5
    SkillRegistry* skills = nullptr;  // Stage 5
    BackgroundManager* background = nullptr;  // Stage 6
    ToolRegistry* registry = nullptr;
    SpawnFn spawn;                    // Stage 6；空 = 不允许派子 agent
    Json todos = Json::array();
    int depth = 0;                    // agent 层级，用来禁止无限嵌套
};

// ── 两个布尔标记不是一回事（最容易搞错的地方）──────────────────────────────
//   read_only()            不改本地文件 → executor 敢并发跑
//   requires_permission()  有副作用 → 必须过权限闸
// 一个抓 URL 的工具是 read_only=true（可并发）但 requires_permission=true（出网要过闸）。
/// @brief The one shape every capability takes.
///
/// Three members describe the tool to the model, two tell the executor how to
/// schedule it, and run() does the work. File I/O, bash, sub-agents and remote
/// MCP tools all fit this — get it right once and five stages reuse it.
///
/// An open set, so virtuals rather than a variant: users add their own tools
/// and no enumeration could stay complete.
class Tool {
  public:
    virtual ~Tool() = default;

    /// @brief Tool name, as the model will call it.
    virtual std::string_view name() const = 0;

    /// @brief When to use this tool.
    /// @note The only place the model learns that. Vague wording here shows up
    ///       as the model reaching for read where grep was meant.
    virtual std::string_view description() const = 0;   // 唯一告诉模型"何时该用"的地方

    /// @brief JSON Schema for the arguments.
    /// @note Listing a parameter in `required` saves a round trip — without it
    ///       the model may omit it and need a second turn to supply it.
    virtual Json input_schema() const = 0;

    /// @brief Does this tool leave local files alone?
    /// @note Only decides whether the executor may run it concurrently, and
    ///       which way a permission mode defaults. Not an exemption.
    virtual bool read_only() const { return false; }

    /// @brief Does this call need authorisation?
    /// @note The actual exemption, and distinct from read_only(). A tool that
    ///       only reads but reaches the network is read-only and still has to
    ///       pass the gate. Defaults to true so a new tool errs safe.
    /// @note Even an exempt tool is still matched against deny rules —
    ///       "no need to ask" is not "unconstrained".
    virtual bool requires_permission() const { return true; }

    /// @brief The one string the sandbox matches its rules against.
    ///
    /// @param args This call's arguments.
    /// @return What is being acted on — a path, a command, a URL.
    ///
    /// @warning The default takes the first string argument, and nlohmann
    ///          iterates keys **alphabetically**. For `write`, `content` sorts
    ///          ahead of `path`, so the default hands the sandbox the text being
    ///          written instead of the file being written to, and a rule like
    ///          Write(src/**) never matches. Any tool with more than one string
    ///          parameter must override this. `edit` does.
    ///
    /// @code{.test}
    /// @setup const DocTool t;
    /// t.subject(Json{{"path","a.py"}})                        ==> "a.py"
    /// t.subject(Json{{"limit",100},{"path","a.py"}})          ==> "a.py"
    /// t.subject(Json{{"content","x = 1"},{"path","a.py"}})    ==> "x = 1"
    /// t.subject(Json::array())                                ==> ""
    /// t.subject(Json::object())                               ==> ""
    /// t.subject(Json())                                       ==> ""
    /// @endcode
    ///
    /// @note The third assertion pins the trap deliberately: `content` sorts
    ///       ahead of `path`, so a tool with several string parameters gets the
    ///       wrong subject unless it overrides this.
    ///
    /// 这次调用的"审查对象" —— 沙箱只看这个字符串做规则匹配。
    virtual std::string subject(const Json& args) const;

    /// @brief Do the work.
    ///
    /// @param args What the model supplied. Validate it — the schema is guidance,
    ///        not a guarantee.
    /// @param ctx  Config, sandbox, session and whatever else this tool needs.
    /// @return The result, is_error set on failure.
    ///
    /// @note May throw — the executor catches everything — but returning
    ///       ToolResult::error is better: the message stays clean, and the
    ///       distinction between an expected failure and a bug survives.
    virtual ToolResult run(const Json& args, ToolContext& ctx) = 0;

    /// @brief This tool as one entry of the API's `tools` array.
    /// @return `{"name":..., "description":..., "input_schema":...}`
    ///
    /// @code
    /// // {"name":"read",
    /// //  "description":"读取文件内容，带行号。...",
    /// //  "input_schema":{"type":"object","properties":{...},"required":["path"]}}
    /// @endcode
    Json schema() const;
};

using ToolPtr = std::shared_ptr<Tool>;

/// @brief The tools available to one agent.
class ToolRegistry {
  public:
    /// @brief Register a tool.
    /// @param tool Ownership is shared, so a Stage 6 subset can point at the
    ///        same instance without copying it.
    void add(ToolPtr tool);

    /// @brief Look a tool up by name.
    ///
    /// @param name The name the model used.
    /// @return nullptr when there is no such tool. The executor turns that into
    ///         an error result listing what is available, so the model can
    ///         correct its own typo on the next turn.
    ///
    /// @note Linear scan. With a dozen tools that is nothing next to the file
    ///       I/O the call is about to do.
    ///
    /// @code{.test}
    /// @setup ToolRegistry r;
    /// @setup r.add(std::make_shared<DocTool>("read"));
    /// (r.get("read") != nullptr)                    ==> true
    /// (r.get("nope") == nullptr)                    ==> true
    /// (r.get("") == nullptr)                        ==> true
    /// r.size()                                      ==> 1u
    /// @endcode
    Tool* get(std::string_view name) const;      // 找不到返回 nullptr

    /// @brief Every registered name, in registration order.
    /// @note Used to tell the model what it could have called when it names
    ///       something that does not exist.
    std::vector<std::string> names() const;

    /// @brief Non-owning pointers to every tool.
    ///
    /// @note vector<Tool*> rather than vector<Tool&> — a container of references
    ///       is ill-formed, since a container needs to form pointers to its
    ///       elements and there is no such thing as a pointer to a reference.
    ///       Values would not work either: callers need to invoke run(), which
    ///       is non-const.
    std::vector<Tool*> all() const;

    /// @brief The `tools` array for a request.
    ///
    /// @return A JSON array of complete schemas, **sorted by name**.
    ///
    /// @warning An object here rather than an array throws on push_back, and
    ///          `"tools": {...}` is not what the API accepts either.
    /// @warning The sort is not cosmetic. Tool definitions sit near the front of
    ///          the request and prompt caching matches a byte-exact prefix, so a
    ///          different order means paying full price for system and tools
    ///          every turn. Registration order is not stable across builds or
    ///          refactors. Nothing reports it — it shows up on the bill.
    ///
    /// @code{.test}
    /// @setup ToolRegistry r;
    /// @setup r.add(std::make_shared<DocTool>("write"));
    /// @setup r.add(std::make_shared<DocTool>("read"));
    /// @setup r.add(std::make_shared<DocTool>("bash"));
    /// @setup const Json s = r.schemas();
    /// s.is_array()                                  ==> true
    /// s.size()                                      ==> 3u
    /// s.at(0).value("name", std::string{})          ==> "bash"
    /// s.at(1).value("name", std::string{})          ==> "read"
    /// s.at(2).value("name", std::string{})          ==> "write"
    /// s.at(0).contains("description")               ==> true
    /// s.at(0).contains("input_schema")              ==> true
    /// r.schemas().dump()                            ==> s.dump()
    /// ToolRegistry{}.schemas().is_array()           ==> true
    /// @endcode
    ///
    /// 给 API 的 tools 数组。
    Json schemas() const;

    /// @brief A registry holding only the named tools.
    ///
    /// @note This is why tools are held by shared_ptr: the parent registry and
    ///       the subset point at the same instances and neither owns them
    ///       outright.
    ///
    /// 挑出一部分组成新注册表 —— Stage 6 给子 agent 收窄权限用。
    ToolRegistry subset(const std::vector<std::string>& names) const;

    /// @brief How many tools are registered.
    std::size_t size() const;

  private:
    // 想想用什么容器：要按名字查，又要按名字排序输出。
    std::vector<ToolPtr> tools_;   // TODO(Stage 2): 也许换成 std::map？
};

}  // namespace mini
