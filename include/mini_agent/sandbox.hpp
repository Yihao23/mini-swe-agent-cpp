#pragma once
//
// 【Stage 3】沙箱 / 权限层 —— 决定"这次工具调用允不允许跑"。
//
// 最小可用的隔离方案：**权限规则 + 子进程**，没有容器依赖。三道关卡：
//   1. 模式    readonly / ask / auto / yolo，全局基线
//   2. 规则    allow / deny，形如 `Bash(git status:*)`、`Write(src/**)`
//   3. 硬检查  路径必须落在 workdir 内；危险命令永远拒绝
//
// **一条铁律：工具不做权限判断。** executor 在调用前统一问这一层。
// 权限散在各个工具里，你永远不知道覆盖全不全。
//
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/json.hpp"

namespace mini {

class Tool;

enum class Action { Allow, Deny, Ask };   // ← 三态，不是布尔。Ask 是最重要的那个

struct Decision {
    Action action = Action::Ask;
    std::string reason;

    bool allowed() const { return action == Action::Allow; }
};

/// 一条权限规则：`Bash(git status:*)` / `Write(src/**)` / `Bash`
struct Rule {
    std::string tool;
    std::optional<std::string> pattern;   // nullopt = 整个工具都匹配

    /// 约定：结尾 `:*` 表示前缀匹配（`git status:*` → glob `git status*`）
    /// 解析失败返回 nullopt，别抛异常 —— 用户配置文件写错要给可读报错
    static std::optional<Rule> parse(std::string_view text);

    /// 工具名要对上，pattern 走 glob。
    /// C++ 没有内建 fnmatch 的 std 版本 —— 用 <fnmatch.h>（POSIX），
    /// 或者自己写 20 行递归匹配 `*` 和 `?`（想练手就自己写）。
    bool matches(std::string_view tool_name, std::string_view subject) const;
};

/// 交互确认回调。返回值三态：拒绝 / 允许一次 / 本会话都允许
enum class Confirm { Deny, Once, Always };
using AskFn = std::function<Confirm(std::string_view tool, std::string_view subject,
                                    std::string_view reason)>;

class Sandbox {
  public:
    /// asker 为空 = 非交互（CI、子 agent）。那时 Ask 该怎么办？想清楚再写。
    Sandbox(const Config& cfg, AskFn asker = {});

    // -- 统一入口：executor 只调这一个 ---------------------------------------
    /// 顺序：requires_permission=false 直接放行 → bash 走 check_command，
    ///       其余走 check → 最后 confirm 把 Ask 变成 Allow/Deny
    /// TODO(Stage 3)
    Decision authorize(const Tool& tool, const Json& args);

    // -- 路径边界 ------------------------------------------------------------
    /// 把模型给的路径解析成绝对路径，并检查是否越界。
    ///
    /// ⚠️ 模型给的 path 是不可信输入：`..`、符号链接、绝对路径都要挡。
    /// 必须 weakly_canonical() 之后再比较（文件可能还不存在，所以不能用 canonical）。
    /// 判断"在 workdir 内"：C++ 没有 Path.relative_to，自己比较前缀 —— 注意
    /// `/work` 和 `/workspace` 这种前缀陷阱，要按路径分量比，不能按字符串比。
    /// TODO(Stage 3)
    std::pair<fs::path, Decision> resolve_path(std::string_view raw) const;

    // -- 命令检查 ------------------------------------------------------------
    /// 按 && || ; | 拆段。
    /// ⚠️ 这一条不做，前面所有权限设计都是装饰：`ls && rm -rf /` 会整条蒙混过关。
    static std::vector<std::string> split_command(std::string_view cmd);

    Decision check_command(std::string_view cmd) const;

    /// 通用工具判定。
    /// ⚠️ read_only 只说明"不改本地文件"，**不等于**"无副作用"。
    /// 一个只读但会出网的工具仍然要过闸；真正免闸的是 requires_permission=false，
    /// 那一步在 authorize() 里就返回了。（有一条测试专门锁这个语义。）
    Decision check(std::string_view rule_name, bool read_only, std::string_view subject) const;

    Decision confirm(std::string_view rule_name, std::string_view subject, Decision d);

    /// 用户选了"以后都允许"。
    /// 设计题：批准了 `npm test`，下次 `npm test -- --watch` 算不算？
    /// 太宽 = 形同虚设，太窄 = 烦死人。
    void remember_allow(std::string_view rule_name, std::string_view subject);

    void set_mode(PermissionMode m) { mode_ = m; }
    PermissionMode mode() const { return mode_; }

  private:
    const Config& cfg_;
    PermissionMode mode_;
    std::vector<Rule> allow_;
    std::vector<Rule> deny_;
    AskFn asker_;
};

/// 永远拒绝的命令模式。想想还该加什么。
/// rm -rf / sudo / mkfs / dd if= / fork bomb / chmod 777 / curl|sh / 裸设备 / push --force
struct DangerPattern {
    const char* regex;
    const char* why;
};
extern const std::vector<DangerPattern> kDangerous;   // TODO(Stage 3): sandbox.cpp

}  // namespace mini
