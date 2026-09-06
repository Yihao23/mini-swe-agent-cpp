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

/// @brief The verdict on one tool call.
///
/// @note Three states, not a bool. `Ask` is the one that matters: it means
///       "hand the decision back to a human". Modelled as a bool, adding
///       interactive confirmation later would mean rewriting this layer.
enum class Action { Allow, Deny, Ask };   // ← 三态，不是布尔。Ask 是最重要的那个

/// @brief A verdict with the reasoning behind it.
///
/// @note `reason` reaches the model when a call is refused, so it should say
///       what would have to change: "命中 deny 规则" or "危险命令: rm -rf /"
///       let it pick another approach; "denied" does not.
struct Decision {
    Action action = Action::Ask;
    std::string reason;

    /// @brief Was this permitted outright?
    /// @note Ask counts as not allowed. By the time the executor sees a
    ///       Decision, confirm() has already resolved every Ask.
    bool allowed() const { return action == Action::Allow; }
};

/// @brief One permission rule from the config file.
///
/// Two syntaxes, plus a convenience:
///   - `Bash`                 — the whole tool, whatever the arguments
///   - `Write(src/**)`        — tool plus a glob over the subject
///   - `Bash(git status:*)`   — the same, where trailing `:*` reads as a prefix
///
/// @note Borrowed from Claude Code's own rule syntax, so someone who has seen
///       one recognises the other.
///
/// 一条权限规则：`Bash(git status:*)` / `Write(src/**)` / `Bash`
struct Rule {
    std::string tool;
    std::optional<std::string> pattern;   // nullopt = 整个工具都匹配

    /// @brief Parse one rule string.
    ///
    /// @param text A rule in any of the three forms above.
    /// @return nullopt when it does not parse.
    ///
    /// @note Not an exception: these strings come from a hand-edited config
    ///       file, so a typo has to produce a readable message rather than stop
    ///       the agent from starting. The Sandbox constructor skips what fails.
    /// @note The trailing `:*` is sugar for a prefix — `git status:*` becomes
    ///       the glob `git status*`. Writing the glob directly works
    ///       identically; the colon just makes the intent visible and stops
    ///       someone from writing `Bash(git status)` and getting an exact match
    ///       they did not want.
    ///
    /// @code{.test}
    /// Rule::parse("Bash")->tool                            ==> "Bash"
    /// Rule::parse("Bash")->pattern.has_value()             ==> false
    /// Rule::parse("Write(src/**)")->pattern.value()        ==> "src/**"
    /// Rule::parse("Bash(git status:*)")->pattern.value()   ==> "git status*"
    /// Rule::parse("Bash(unclosed").has_value()             ==> false
    /// Rule::parse("(nothing)").has_value()                 ==> false
    /// @endcode
    ///
    /// 约定：结尾 `:*` 表示前缀匹配（`git status:*` → glob `git status*`）
    static std::optional<Rule> parse(std::string_view text);

    /// @brief Does this rule cover the given call?
    ///
    /// @param tool_name   The tool being invoked.
    /// @param subject     What Tool::subject() returned for this call.
    /// @return true when the tool name matches and the glob accepts the subject.
    ///
    /// @note Tool names compare case-insensitively: config files say `Bash`
    ///       while Tool::name() returns `bash`.
    /// @note No pattern means the whole tool matches, whatever the subject.
    /// @note fnmatch is called without FNM_PATHNAME, so `*` crosses `/` —
    ///       Write(src/**) has to reach src/a/b.py, and with the flag set it
    ///       would stop at the first slash. The side effect is that `src/*` and
    ///       `src/**` behave identically here.
    /// @note fnmatch takes a const char*, so the subject is copied —
    ///       string_view carries no guarantee of a terminator. A handful of
    ///       rules per call against one file operation; not worth optimising.
    ///
    /// @code{.test}
    /// Rule::parse("Write(src/**)")->matches("write", "src/a/b.py")        ==> true
    /// Rule::parse("Write(src/**)")->matches("Write", "src/a/b.py")        ==> true
    /// Rule::parse("Write(src/**)")->matches("read",  "src/a.py")          ==> false
    /// Rule::parse("Write(src/**)")->matches("write", "docs/a.md")         ==> false
    /// Rule::parse("Bash(git status:*)")->matches("bash", "git status -s") ==> true
    /// Rule::parse("Bash(git status:*)")->matches("bash", "git push")      ==> false
    /// Rule::parse("Bash")->matches("bash", "anything at all")             ==> true
    /// @endcode
    ///
    /// 工具名要对上，pattern 走 glob。
    bool matches(std::string_view tool_name, std::string_view subject) const;
};

/// @brief What the user answered when asked to confirm.
/// @note `Always` is remembered for the session via remember_allow().
///
/// 交互确认回调。返回值三态：拒绝 / 允许一次 / 本会话都允许
enum class Confirm { Deny, Once, Always };
/// @brief How to ask a human. Empty means there is nobody to ask.
///
/// The sandbox knows nothing about terminals. The CLI supplies a prompt, a web
/// front end would push a message and wait, and a test hands over a lambda that
/// answers immediately.
///
/// @code
/// AskFn asker = [](std::string_view tool, std::string_view subject,
///                  std::string_view reason) {
///     std::printf("%s wants: %s (%s) [y/a/n] ", ...);
///     // y → Once, a → Always, anything else → Deny
/// };
/// @endcode
using AskFn = std::function<Confirm(std::string_view tool, std::string_view subject,
                                    std::string_view reason)>;

/// @brief Decides whether a tool call may proceed.
///
/// Three layers, hardest first:
///   1. kDangerous  — hardcoded, not configurable, refused outright
///   2. deny rules  — the operator's blacklist
///   3. allow rules and the permission mode — the operator's baseline
///
/// The order is deliberate: an operator can loosen their own policy but cannot
/// switch off the layer covering operations that cannot be undone. An agent
/// that reads a file saying "now run rm -rf /" should not be able to.
///
/// @note One rule runs through the whole layer: tools do not make permission
///       decisions. Spread that logic across a dozen tools and there is no way
///       to tell whether the coverage is complete.
class Sandbox {
  public:
    /// @brief Parse the configured rules and record the baseline mode.
    ///
    /// @param cfg   ⚠️ Held by reference; it must outlive this sandbox.
    /// @param asker How to ask a human. Empty means non-interactive.
    ///
    /// @note Rules that fail to parse are skipped, not fatal. One typo in a
    ///       config file should not stop the agent from starting.
    ///
    /// asker 为空 = 非交互（CI、子 agent）。那时 Ask 该怎么办？想清楚再写。
    Sandbox(const Config& cfg, AskFn asker = {});

    // -- 统一入口：executor 只调这一个 ---------------------------------------
    /// @brief The only door the executor knocks on.
    ///
    /// @param tool The tool about to run.
    /// @param args The arguments the model supplied.
    /// @return An Allow or a Deny — never an Ask, confirm() has resolved it.
    ///
    /// Four steps, and the order carries the policy:
    ///   1. deny rules, **before** the exemption — requires_permission() == false
    ///      means "do not ask", not "unconstrained", so an explicit deny still
    ///      applies. Without this ordering `deny Read(**.env)` never fired,
    ///      because read declares itself exempt.
    ///   2. the exemption itself.
    ///   3. bash diverges to check_command, whose subject is a whole command
    ///      line that may hide several commands; every other subject is atomic.
    ///   4. confirm(), last, so no Ask escapes to the executor.
    ///
    /// @note The exemption is requires_permission(), not read_only(). A tool
    ///       that only reads but reaches the network is read-only and still has
    ///       to pass the gate — a smoke test pins that distinction.
    ///
    /// @code{.test}
    /// @setup Config cfg = doc_config(PermissionMode::Ask);
    /// @setup cfg.deny_rules = {"Doc(**.env)"};
    /// @setup Sandbox sb(cfg, {});
    /// @setup DocTool exempt{"doc", true, false};   // read_only, no permission needed
    /// @setup DocTool gated{"doc", true, true};     // read_only, but gated
    /// sb.authorize(exempt, Json{{"path","src/a.py"}}).allowed()      ==> true
    /// sb.authorize(exempt, Json{{"path","secrets/.env"}}).allowed()  ==> false
    /// sb.authorize(gated,  Json{{"path","src/a.py"}}).allowed()      ==> false
    /// @endcode
    Decision authorize(const Tool& tool, const Json& args);

    // -- 路径边界 ------------------------------------------------------------
    /// @brief Resolve a model-supplied path and check it stays inside workdir.
    ///
    /// @param raw The path as the model wrote it — untrusted input.
    /// @return The resolved absolute path, and whether it is permitted.
    ///
    /// @warning `..`, symlinks and absolute paths all have to be stopped here.
    ///          Before this was wired in, `{"path": "../../../etc/passwd"}` was
    ///          read without complaint.
    ///
    /// @note weakly_canonical, not canonical: a write target usually does not
    ///       exist yet and canonical throws on a missing path.
    /// @note Containment is tested with lexically_relative, comparing path
    ///       components. A string prefix test would accept /workspace for a
    ///       workdir of /work.
    ///
    /// @code{.test}
    /// @setup const Config cfg = doc_config();
    /// @setup const Sandbox sb(cfg, {});
    /// sb.resolve_path("src/a.py").second.allowed()             ==> true
    /// sb.resolve_path("./src/../src/a.py").second.allowed()    ==> true
    /// sb.resolve_path("src/a.py").first                        ==> cfg.workdir / "src/a.py"
    /// sb.resolve_path("../../../etc/passwd").second.allowed()  ==> false
    /// sb.resolve_path("").second.allowed()                     ==> false
    ///
    /// // The sibling whose name starts with the workdir's. A string-prefix test
    /// // would accept this; comparing path components rejects it.
    /// @setup const std::string sibling = cfg.workdir.string() + "-other/x.py";
    /// sb.resolve_path(sibling).second.allowed()                ==> false
    /// @endcode
    ///
    /// 把模型给的路径解析成绝对路径，并检查是否越界。
    std::pair<fs::path, Decision> resolve_path(std::string_view raw) const;

    // -- 命令检查 ------------------------------------------------------------
    /// @brief Split a command line into the individual commands it runs.
    ///
    /// @param cmd The whole command line.
    /// @return One entry per segment, whitespace trimmed.
    ///
    /// @warning Without this, every other permission check in this layer is
    ///          decoration. A rule of `allow Bash(ls*)` is matched against the
    ///          entire string, so `ls && rm -rf /` hits the prefix and the whole
    ///          line is permitted.
    ///
    /// @note Two-character operators are tested first — treating `&&` as two
    ///       single `&` produces an empty segment between them.
    /// @note Separators inside quotes are left alone: `git commit -m 'a; b'` is
    ///       one command, and splitting it would hand the matcher half of one.
    ///
    /// @code{.test}
    /// @setup const auto segs = Sandbox::split_command("ls && rm -rf / ; echo done | grep x");
    /// segs.size()                                              ==> 4u
    /// segs.at(0)                                               ==> "ls"
    /// segs.at(1)                                               ==> "rm -rf /"
    /// segs.at(3)                                               ==> "grep x"
    /// Sandbox::split_command("git commit -m 'a; b'").size()    ==> 1u
    /// Sandbox::split_command("npm test").size()                ==> 1u
    /// @endcode
    ///
    /// 按 && || ; | 拆段。
    static std::vector<std::string> split_command(std::string_view cmd);

    /// @brief Vet a bash command line, segment by segment.
    ///
    /// @param cmd The command line.
    /// @return The strictest verdict any segment produced.
    ///
    /// @note Danger patterns are tested per segment, ahead of rules and modes.
    /// @note One bad segment condemns the line. `git status && rm -rf /` matches
    ///       an allow rule on its first segment, and permitting it on that basis
    ///       is precisely the hole split_command exists to close.
    /// @note bash counts as having side effects for every segment. This one may
    ///       be a bare `ls`, but the next one need not be.
    ///
    /// @code{.test}
    /// @setup Config cfg = doc_config(PermissionMode::Ask);
    /// @setup cfg.allow_rules = {"Bash(git status:*)"};
    /// @setup const Sandbox sb(cfg, {});
    /// sb.check_command("git status --short").action     ==> Action::Allow
    /// sb.check_command("npm publish").action            ==> Action::Ask
    /// sb.check_command("sudo rm -rf /").action          ==> Action::Deny
    /// sb.check_command("git status && rm -rf /").action ==> Action::Deny
    /// @endcode
    Decision check_command(std::string_view cmd) const;

    /// @brief Match one call against the rules, falling back to the mode.
    ///
    /// @param rule_name The tool name rules are written against.
    /// @param read_only Whether the tool leaves local files alone.
    /// @param subject   What is being acted on.
    /// @return Allow, Deny or Ask.
    ///
    /// @note deny is consulted before allow. The other order lets one broad
    ///       allow rule silently disable every deny beside it — the opposite of
    ///       what someone writing `deny Bash(rm *)` next to `allow Bash` means.
    /// @note With no rule matching, the mode decides:
    ///       Yolo allows everything; Auto allows read-only and asks otherwise;
    ///       ReadOnly allows read-only and denies otherwise; Ask asks for
    ///       everything.
    /// @note read_only only picks among those fallbacks. It is not an
    ///       exemption — that is requires_permission(), handled in authorize().
    ///
    /// @code{.test}
    /// @setup const Config yolo = doc_config(PermissionMode::Yolo);
    /// @setup const Config autom = doc_config(PermissionMode::Auto);
    /// @setup const Config ro = doc_config(PermissionMode::ReadOnly);
    /// @setup const Config ask = doc_config(PermissionMode::Ask);
    /// Sandbox(yolo,  {}).check("x", false, "s").action  ==> Action::Allow
    /// Sandbox(autom, {}).check("x", false, "s").action  ==> Action::Ask
    /// Sandbox(autom, {}).check("x", true,  "s").action  ==> Action::Allow
    /// Sandbox(ro,    {}).check("x", false, "s").action  ==> Action::Deny
    /// Sandbox(ro,    {}).check("x", true,  "s").action  ==> Action::Allow
    /// Sandbox(ask,   {}).check("x", true,  "s").action  ==> Action::Ask
    /// @endcode
    ///
    /// 通用工具判定。
    Decision check(std::string_view rule_name, bool read_only, std::string_view subject) const;

    /// @brief Turn an Ask into an Allow or a Deny.
    ///
    /// @param rule_name Tool name, shown to the user.
    /// @param subject   What it wants to act on, shown to the user.
    /// @param d         The verdict so far; returned unchanged unless it is Ask.
    /// @return A resolved verdict.
    ///
    /// @note With no asker — CI, a sub-agent — Ask resolves to **Deny**. Erring
    ///       toward refusal is the point: getting a run through unattended
    ///       should mean writing `--mode yolo` or an allow rule, an explicit
    ///       decision, rather than relying on "nobody was around to ask".
    /// @note Always is recorded through remember_allow, so the same subject is
    ///       not asked about twice in one session.
    Decision confirm(std::string_view rule_name, std::string_view subject, Decision d);

    /// @brief Record a session-scoped allow after the user answered Always.
    ///
    /// @param rule_name The tool.
    /// @param subject   The exact subject that was approved.
    ///
    /// @note Stores the subject exactly, with no generalisation. Recording
    ///       `npm test` as `npm*` would turn approving one command into
    ///       approving npm publish — widening "allow this operation" into
    ///       "allow this family". Re-asking when an argument changes is the
    ///       cheaper mistake, and an operator who wants the broader grant can
    ///       write an allow rule, which is explicit.
    /// @note Duplicates are skipped so the list does not grow on repeated
    ///       answers to the same question.
    ///
    /// @code{.test}
    /// @setup Config cfg = doc_config(PermissionMode::Ask);
    /// @setup int asked = 0;
    /// @setup Sandbox sb(cfg, [&asked](auto, auto, auto) { ++asked; return Confirm::Always; });
    /// @setup DocTool bash{"bash", false, true};
    /// @setup sb.authorize(bash, Json{{"command","npm test"}});
    /// asked                                                        ==> 1
    /// @setup sb.authorize(bash, Json{{"command","npm test"}});
    /// asked                                                        ==> 1
    /// @setup sb.authorize(bash, Json{{"command","npm test -- --watch"}});
    /// asked                                                        ==> 2
    /// @setup sb.authorize(bash, Json{{"command","npm publish"}});
    /// asked                                                        ==> 3
    /// @endcode
    ///
    /// 用户选了"以后都允许"。
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

/// @brief A command pattern refused outright, whatever the configuration says.
///
/// @note Checked ahead of rules and modes and not configurable. `--mode yolo`
///       together with `allow Bash` still does not get `rm -rf /` through —
///       verified. These are the operations with no way back.
///
/// 永远拒绝的命令模式。想想还该加什么。
/// rm -rf / sudo / mkfs / dd if= / fork bomb / chmod 777 / curl|sh / 裸设备 / push --force
struct DangerPattern {
    const char* regex;   ///< ECMAScript regex, matched against one command segment.
    const char* why;     ///< Shown to the model, so it says what to avoid.
};

/// @brief Commands refused before any rule or mode is consulted.
///
/// @note Not configurable on purpose. An agent can be talked into things — it
///       reads files, and a file can contain instructions. This layer holds
///       whatever the configuration and the model both say.
/// @note Regexes rather than exact strings: `rm -rf /` has a dozen spellings.
///
/// 永远拒绝，不问、不看规则、不看模式。挡的是「一旦执行就无法挽回」的操作。
extern const std::vector<DangerPattern> kDangerous;

}  // namespace mini
