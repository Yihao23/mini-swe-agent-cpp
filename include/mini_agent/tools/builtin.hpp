#pragma once
/// @file
/// @brief Factory functions for the builtin tools (Stage 2/5/6).
//
// 【Stage 2/5/6】内置工具的工厂函数。
//
// ── 注意这里没有一个具体的 class 声明 ───────────────────────────────────────
//
// ReadTool / BashTool 这些类型全部关在各自的 .cpp 里，外面只看得见 `ToolPtr`。
// 好处：
//   * 改一个工具的私有成员，不会引起全项目重编译
//   * 注册表只依赖 Tool 接口，编译期依赖是一条线而不是一张网
//   * 逼你只通过接口使用工具 —— 想在别处 dynamic_cast 回 BashTool？做不到，这是对的
//
// 这是 C++ 里"接口/实现分离"最省事的一种做法，比 pimpl 轻，比暴露类干净。
//
#include <vector>

#include "mini_agent/tool.hpp"

namespace mini {

struct Config;

// --- Stage 2 ---

/// @brief `read` — file contents with line numbers, or a directory listing.
///
/// @note read_only, and the only builtin that declares requires_permission()
///       false. Path containment still applies: an exempt tool skips the
///       question, not the boundary.
/// @note Records what it read on the session, which is what lets edit and
///       write refuse to change a file the model has not seen.
///
///
/// @return The `read` tool.
/// 参数：path，可选 offset / limit。path 是目录时列出条目。
ToolPtr make_read_tool();

/// @brief `write` — create a file, or replace one whole.
///
/// @warning Overwriting a file the model has not read is refused. write
///          replaces everything, so doing it unseen deletes work nobody
///          looked at. A file that does not exist yet needs no prior read.
///
/// @note Prefer edit for a small change. write makes the model reproduce the
///       whole file, which is slow, expensive, and silently destructive when
///       it mistypes a line it was not even changing.
/// @note Creates missing parent directories, so `src/new/mod.py` works without
///       a round trip through bash mkdir.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup const auto write = make_write_tool();
///
/// // ⚠️ subject() 交给沙箱的必须是 path。默认实现按 key 字母序取第一个字符串，
/// //    content < path —— 那样沙箱审查的就是要写入的正文，规则永远命不中。
/// write->subject(Json{{"content","x = 1"},{"path","src/a.py"}})   ==> "src/a.py"
///
/// // 新建：不需要先 read，父目录自动建
/// t.run(write, Json{{"path","src/a.py"},{"content","x = 1\n"}}).is_error   ==> false
/// t.slurp("src/a.py")                                                      ==> "x = 1\n"
///
/// // 已存在但没读过：拒绝，文件原封不动
/// @setup t.seed("old.py", "重要代码\n");
/// t.run(write, Json{{"path","old.py"},{"content","没了"}}).is_error         ==> true
/// t.slurp("old.py")                                                        ==> "重要代码\n"
///
/// // 读过之后就放行
/// @setup t.run(make_read_tool(), Json{{"path","old.py"}});
/// t.run(write, Json{{"path","old.py"},{"content","新的\n"}}).is_error       ==> false
/// t.slurp("old.py")                                                        ==> "新的\n"
/// @endcode
///
///
/// @return The `write` tool.
/// 参数：path、content，都必填。
ToolPtr make_write_tool();

/// @brief `edit` — replace one span inside a file.
///
/// @warning old_string must occur exactly once. Replacing the first of several
///          matches without saying so leaves the model believing it changed
///          them all.
///
/// @note Requires a prior read, and refuses when the file changed after that
///       read — that edit would silently overwrite whatever the other writer
///       did. write has no such check: replacing everything is its job.
/// @note An empty new_string deletes the span.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup const auto write = make_write_tool();
/// @setup const auto edit  = make_edit_tool();
/// @setup t.run(write, Json{{"path","a.py"},{"content","x = 1\ny = 2\nz = 2\n"}});
///
/// // 改一行用 edit，不必把整个文件重发一遍
/// t.run(edit, Json{{"path","a.py"},{"old_string","x = 1"},{"new_string","x = 9"}}).is_error ==> false
/// t.slurp("a.py")                                                            ==> "x = 9\ny = 2\nz = 2\n"
///
/// // old_string 出现两次（"= 2" 在 y 和 z 两行里）→ 拒绝，不猜是哪一个
/// t.run(edit, Json{{"path","a.py"},{"old_string","= 2"},{"new_string","= 8"}}).is_error     ==> true
/// t.slurp("a.py")                                                            ==> "x = 9\ny = 2\nz = 2\n"
///
/// // 没读过的文件不能改
/// @setup t.seed("other.py", "q = 1\n");
/// t.run(edit, Json{{"path","other.py"},{"old_string","q = 1"},{"new_string","q = 2"}}).is_error ==> true
/// @endcode
///
///
/// @return The `edit` tool.
/// 参数：path、old_string、new_string。
ToolPtr make_edit_tool();

/// @brief `glob` — find files by name, newest first.
///
/// @note Sorted by modification time descending, not alphabetically. A model
///       asking "which .cpp files are there" almost always wants the ones
///       someone touched recently; alphabetical order leads with app.cpp for no
///       reason anyone cares about.
/// @note `*` crosses `/`, so `*.cpp` and `**/*.cpp` behave the same. A model
///       writing the former means "every .cpp", not "the top-level ones" — the
///       same trade-off Rule::matches makes.
/// @note Never opens a file. That is why it answers in milliseconds where grep
///       has to read everything.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup const auto glob = make_glob_tool();
/// @setup t.seed("src/a.cpp", "x\n");
/// @setup t.seed("build/gen.cpp", "x\n");
/// @setup const auto found = t.run(glob, Json{{"pattern","*.cpp"}}).content;
/// (found.find("src/a.cpp") != std::string::npos)     ==> true
/// (found.find("build/") != std::string::npos)        ==> false
/// // 审查对象是搜索起点，不是模式 —— 规则约束的是「能看哪个目录」
/// glob->subject(Json{{"pattern","*.cpp"},{"path","src"}})  ==> "src"
/// @endcode
///
///
/// @return The `glob` tool.
/// 参数：pattern，可选 path（从哪个子目录开始）。
ToolPtr make_glob_tool();

/// @brief `grep` — find files by content, as `path:line: text`.
///
/// @warning Binary files are skipped, detected by a NUL byte in the first 8 KB.
///          Without that one `.o` under build/ emits thousands of lines of
///          mojibake and swallows the turn's context — and the model could not
///          use the result anyway.
///
/// @note `build/`, `.git/`, `node_modules/` and friends are pruned during the
///       walk, not filtered afterwards; descending into `.git` first would
///       already cost tens of thousands of entries.
/// @note The line number is the point of the output format: it lets the model
///       go straight to read with an offset instead of pulling a whole file.
/// @note Long lines are clipped and huge files skipped. One minified `.js` line
///       is a megabyte.
/// @note A malformed regex comes back as an error naming the pattern. It is the
///       model that wrote it, and a bare what() gives it nothing to fix.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup const auto grep = make_grep_tool();
/// @setup t.seed("a.py", "import os\nx = compute()\n");
/// @setup t.seed("blob.o", std::string("compute\0\0garbage", 15));
/// @setup const auto hits = t.run(grep, Json{{"pattern","compute"}});
/// (hits.content.find("a.py:2:") != std::string::npos)      ==> true
/// (hits.content.find("blob.o") != std::string::npos)       ==> false
/// hits.metadata.at("matches")                              ==> 1
/// // 坏正则给一句能改的话，不抛
/// t.run(grep, Json{{"pattern","[unclosed"}}).is_error      ==> true
/// @endcode
///
///
/// @return The `grep` tool.
/// 参数：pattern，可选 path / glob / ignore_case。
ToolPtr make_grep_tool();

/// @brief `bash` — run one shell command in the workdir.
///
/// @warning Makes no permission decision of its own. Sandbox::authorize has
///          already split the line on `&& || ; |` and checked every segment
///          before run() is reached; a second check here would be a second
///          place to keep the policy correct.
///
/// @note Not read_only, so the executor never runs it beside another tool.
/// @note A model-supplied timeout_sec is clamped to Config::tool_timeout_sec.
///       Honoured as given, the model could lift its own limit.
/// @note A non-zero exit is an error **with the output kept** — a compiler
///       error or a failing test is exactly what the model needs to read next.
///
///
/// @return The `bash` tool.
/// 参数：command，可选 timeout_sec。
ToolPtr make_bash_tool();

// --- Stage 4：agent 自己维护的计划清单，每轮通过 reminder 回灌 ---

/// @brief `todo` — the agent's own plan, rewritten wholesale each time.
/// @return The `todo` tool.
/// @note Replace-the-whole-list rather than add/remove items: a model that has
///       to reason about which entry to mutate gets it wrong, and the list is
///       short enough that resending it costs nothing.
/// @note The current list is fed back each turn through turn_context(), not
///       kept in the system prompt — it changes, and the system prompt may not.
ToolPtr make_todo_tool();

// --- Stage 5：渐进式披露的两个入口 ---

/// @brief `skill` — load one manual in full, by name.
/// @return The `skill` tool.
/// @note The system prompt carries only the index — what exists and when to
///       use it. Ten skills inlined in full would put thousands of tokens into
///       every turn's fixed cost, most of them irrelevant to the task at hand.
ToolPtr make_skill_tool();

/// @brief `memory` — search, load, write and delete long-term notes.
/// @return The `memory` tool.
/// @note Same shape as skills: an index in the system prompt, bodies on
///       demand. What differs is who writes them — the agent writes memory,
///       a human writes skills.
ToolPtr make_memory_tool();

// --- Stage 6 ---

/// @brief `task` — hand one job to a sub-agent and receive its conclusion.
/// @return The `task` tool.
/// @note The saving is context, not time: twenty turns of investigation come
///       back as one paragraph instead of twenty turns of transcript.
ToolPtr make_task_tool();

/// @brief `task_graph` — hand over a dependency graph of jobs at once.
/// @return The `task_graph` tool.
/// @note Runs on Scheduler, which knows nothing about LLMs — which is what
///       lets its topological ordering and concurrency be tested in half a
///       second without spending a token.
ToolPtr make_task_graph_tool();

/// @brief `bash_output` — read what a background task has produced since last time.
/// @return The `bash_output` tool.
/// @note Returns only new output. Returning everything each turn would refill
///       the context with lines the model has already read.
ToolPtr make_bash_output_tool();

/// @brief `kill_task` — stop a background task.
/// @return The `kill_task` tool.
ToolPtr make_kill_task_tool();

/// @brief The builtin set for one agent, assembled from the config switches.
///
/// @param cfg Read for its enable_* flags.
/// @return read, write, edit, glob, grep and bash today; the Stage 4/5/6 tools
///         join as they land.
///
/// @warning A factory that is still todo() throws the moment it is called, and
///          the enable_* flags default to true — so a tool only goes in here
///          once its factory actually returns something. Getting that wrong
///          stops the agent from starting under the default config.
///
/// @note Order does not matter: ToolRegistry::schemas() sorts by name, because
///       the prompt cache matches a byte-exact prefix.
/// @note This is the single place tools are enumerated. App used to list them
///       by hand, which meant two places to keep in step.
std::vector<ToolPtr> builtin_tools(const Config& cfg);

}  // namespace mini
