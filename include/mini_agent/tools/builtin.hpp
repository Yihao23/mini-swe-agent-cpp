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
// ── 八个工具，三组 ──────────────────────────────────────────────────────────
//
//   记号见 executor.hpp 的图例：──▶ 同步 / ══▶ fork-join / ──▷ 异步
//
//                          read_only  过闸   subject()      备注
//   ── 文件 ──────────────────────────────────────────────────────────────
//     read      看          true      否     path           顺带记 mtime，
//                                                            edit/write 靠它判「读过没」
//     write     整个覆盖    false     是     path ⚠️        content < path，
//                                                            不 override 就交出正文
//     edit      改一段      false     是     path ⚠️        同上，还多两个 string
//     glob      按名字找    true      否     搜索起点        不打开文件
//     grep      按内容找    true      否     搜索起点        跳过二进制和大文件
//   ── 执行 ──────────────────────────────────────────────────────────────
//     bash      跑命令      false     是     整条命令行      sandbox 会拆段逐段查
//   ── 渐进式披露（Stage 5）───────────────────────────────────────────────
//     skill     加载手册    true      否     skill 名        正文按需取
//     memory    读写记忆    false     是     记忆名 ⚠️      action < name，
//                                                            不 override 就交出动作名
//   ── 子 agent（Stage 6）────────────────────────────────────────────────
//     task        派一个    false     是     agent_type
//     task_graph  派一张图  false     是     "task_graph"    内部 ══▶ Scheduler
//
//   ⚠️ 标了 ⚠️ 的三个**必须 override subject()**。默认实现取「按 key 字母序的
//      第一个字符串参数」，而那三个的字母序都排在正确答案前面 —— 沙箱会拿到
//      要写入的正文 / 动作名去匹配规则，**权限层静默失效，没有任何报错**。
//      每一个都配了一条测试和一条变异守着。
//
// ── 两个布尔标记的分工（最容易搞错的一对，见 tool.hpp）─────────────────────
//
//     read_only()            改不改本地文件？  → executor 的并发判据
//     requires_permission()  要不要问一句？    → sandbox 的免检开关
//
//   ⚠️ 免检不等于不受约束：deny 规则对 read/glob/grep/skill 照样生效。
//      这就是 sandbox.hpp 里 I3 说的事。
//
// ── builtin_tools(cfg) 按开关拼表 ───────────────────────────────────────────
//
//     总是有            read write edit glob grep bash
//     enable_memory     + memory
//     enable_skills     + skill
//     enable_subagents  + task task_graph
//
//   ⚠️ 一个工厂只有在**真的实现了**之后才能进这个列表。剩下的还是 todo()，
//      一调就抛，而开关默认全是 true —— 提前接上去，默认配置下 agent
//      直接起不来。有一条测试专门守着这件事。
//
// ── 谁调谁 ──────────────────────────────────────────────────────────────────
//
//     App 构造 ──▶ builtin_tools(cfg) ──▶ registry.add(...)
//                        │
//                        └─ 每个 make_*_tool() 返回 shared_ptr<Tool>
//                             ↑ 共享所有权：子 agent 的 subset 指向同一批实例
//
//     Executor ──▶ tool->run(args, ctx)
//                        │
//                        ├─ 文件类 ──▶ ctx.sandbox->resolve_path()
//                        ├─ bash   ──▶ run_shell()            （process.cpp）
//                        ├─ skill  ──▶ ctx.skills->get()
//                        ├─ memory ──▶ ctx.memory->search/get/write/remove
//                        ├─ task   ──▶ ctx.spawn()            （subagent.cpp）
//                        └─ task_graph ══▶ Scheduler::run()   （唯一开线程的）
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
/// @note `run_in_background: true` hands the command to BackgroundManager and
///       returns a task id at once. The fork sits **after** the sandbox, and
///       deliberately: nobody is watching a background command, so the gate
///       matters more there, not less.
///
/// @return The `bash` tool.
/// 参数：command，可选 timeout_sec、run_in_background。
ToolPtr make_bash_tool();

// --- Stage 4：agent 自己维护的计划清单，每轮通过 reminder 回灌 ---

/// @brief `todo` — the agent's own plan, rewritten wholesale each time.
/// @return The `todo` tool.
/// @note Replace-the-whole-list rather than add/remove items: a model that has
///       to reason about which entry to mutate gets it wrong, and the list is
///       short enough that resending it costs nothing.
/// @note The current list is fed back each turn through turn_context(), not
///       kept in the system prompt — it changes, and the system prompt may not.
/// @note At most one entry may be in_progress; a submission with more is
///       refused. Allowing several leaves "what is being worked on right now"
///       without an answer, and that is the only question the list exists to
///       answer.
/// @note Validation is all-or-nothing. A submission that half-applies before
///       failing leaves the model holding a list that does not match the one
///       it will be shown next turn.
/// @note Keys other than content and status are dropped. The model likes to
///       add id, priority and notes; turn_context reads none of them, so
///       keeping them re-feeds dead weight every single turn.
/// @note Not read_only — it mutates ctx.todos, which a sub-agent spawn copies
///       wholesale. run_batch is serial today; the flag is what keeps this
///       correct if it stops being.
ToolPtr make_todo_tool();

// --- Stage 5：渐进式披露的两个入口 ---

/// @brief `skill` — load one manual in full, by name.
/// @return The `skill` tool.
/// @note The system prompt carries only the index — what exists and when to
///       use it. Ten skills inlined in full would put thousands of tokens into
///       every turn's fixed cost, most of them irrelevant to the task at hand.
/// @note read_only and exempt from the gate, like read: it opens files the
///       operator placed there deliberately, inside directories the config
///       names.
/// @warning A wrong name comes back listing what does exist. The model wrote
///          that name from the index, so telling it the alternatives lets it
///          correct itself; "no such skill" leaves it guessing again.
///
/// 参数：name。
ToolPtr make_skill_tool();

/// @brief `memory` — search, load, write and delete long-term notes.
/// @return The `memory` tool.
/// @note Same shape as skills: an index in the system prompt, bodies on
///       demand. What differs is who writes them — the agent writes memory,
///       a human writes skills.
/// @note One tool with an `action` rather than four tools. Four would occupy
///       four slots in every request's tool list — bytes in the cached prefix —
///       for operations used rarely and never confused with each other.
///
/// @warning Not read_only, and it does need authorisation: write and delete
///          change files. skill differs on both counts.
/// @warning `write` refuses without a description. A memory with an empty one
///          takes a slot in the index that the model will never load — written
///          and useless, while still costing context every turn.
///
/// @code{.test}
/// @setup DocTools t;
/// @setup Memory mem(t.cfg.memory_dir());
/// @setup t.ctx.memory = &mem;
/// @setup const auto memory = make_memory_tool();
///
/// // ⚠️ 审查对象是被操作的那条记忆，不是 action。字母序 action < name，
/// //    默认实现会把 "write" 交给沙箱去匹配规则。
/// memory->subject(Json{{"action","write"},{"name","user-prefs"}})   ==> "user-prefs"
///
/// @setup const Json write_args{{"action","write"},{"name","style"},
/// @setup                       {"description","提交信息的格式要求"},
/// @setup                       {"body","Conventional Commits"},{"type","feedback"}};
/// t.run(memory, write_args).is_error                                ==> false
/// @setup const auto loaded = t.run(memory, Json{{"action","load"},{"name","style"}});
/// (loaded.content.find("Conventional Commits") != std::string::npos) ==> true
///
/// // description 是必填的
/// t.run(memory, Json{{"action","write"},{"name","x"},{"body","b"}}).is_error  ==> true
/// // 未知 action 的报错要点出可用的那几个，而不是抱怨缺 name
/// @setup const auto bad = t.run(memory, Json{{"action","frobnicate"}});
/// (bad.content.find("search") != std::string::npos)                 ==> true
/// @endcode
///
/// 参数：action，以及 query / name / description / body / type。
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
/// @note No task_id lists every task. And "nothing new" comes back with that
///       listing attached — otherwise a mistyped id and a quiet task look
///       identical, and the model waits forever on the wrong one.
/// @note Read-only and ungated: the command already went through the sandbox
///       when bash started it.
ToolPtr make_bash_output_tool();

/// @brief `kill_task` — stop a background task, or all of them with "all".
/// @return The `kill_task` tool.
/// @note subject() is the task id, so `deny kill_task(bg_1)` is expressible.
/// @note An unknown id is not an error — that is BackgroundManager::kill's
///       contract, and reporting one would send the model off to fix nothing.
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
