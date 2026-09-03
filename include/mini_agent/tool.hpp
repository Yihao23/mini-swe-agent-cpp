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

struct ToolResult {
    std::string content;
    bool is_error = false;   // ← 是值，不是异常。见 BUILD-GUIDE Stage 0
    Json metadata = Json::object();

    static ToolResult error(std::string msg);
};

/// 子 agent 启动器：spawn(agent_type, prompt) -> 结论文本。Stage 6 才填。
using SpawnFn = std::function<std::string(std::string_view, std::string_view)>;

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
class Tool {
  public:
    virtual ~Tool() = default;

    virtual std::string_view name() const = 0;
    virtual std::string_view description() const = 0;   // 唯一告诉模型"何时该用"的地方
    virtual Json input_schema() const = 0;

    virtual bool read_only() const { return false; }
    virtual bool requires_permission() const { return true; }

    /// 这次调用的"审查对象" —— 沙箱只看这个字符串做规则匹配。
    /// 默认取第一个字符串参数；bash 覆盖成 command，文件类覆盖成 path。
    virtual std::string subject(const Json& args) const;

    virtual ToolResult run(const Json& args, ToolContext& ctx) = 0;

    /// {name, description, input_schema}
    Json schema() const;
};

using ToolPtr = std::shared_ptr<Tool>;

class ToolRegistry {
  public:
    void add(ToolPtr tool);
    Tool* get(std::string_view name) const;      // 找不到返回 nullptr
    std::vector<std::string> names() const;
    std::vector<Tool*> all() const;

    /// 给 API 的 tools 数组。
    /// ⚠️ 必须**按名字排序**。工具定义渲染在 prompt 最前面，顺序一变缓存前缀就废了。
    Json schemas() const;

    /// 挑出一部分组成新注册表 —— Stage 6 给子 agent 收窄权限用。
    ToolRegistry subset(const std::vector<std::string>& names) const;

    std::size_t size() const;

  private:
    // 想想用什么容器：要按名字查，又要按名字排序输出。
    std::vector<ToolPtr> tools_;   // TODO(Stage 2): 也许换成 std::map？
};

}  // namespace mini
