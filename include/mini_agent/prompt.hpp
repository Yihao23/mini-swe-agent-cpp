#pragma once
/// @file
/// @brief Assembles what the model sees at the start of every turn (Stage 4).
//
// 【Stage 4】输入 prompt 组装 —— 决定"模型每一轮看到什么"。
//
// 一条铁律：**system prompt 必须逐字节稳定**。
//
// 渲染顺序是 tools → system → messages，缓存是前缀逐字节匹配。
// 只要 system 里出现一个时间戳、一个 uuid、一个"当前 todo 列表"，
// 它后面的所有内容（也就是整段对话）每轮都要重新计费。
//
// 所以划一条线：
//   静态（身份、工具用法、skill/memory 索引、工作区路径）→ system，最后一块打缓存断点
//   动态（当前时间、后台任务通知、todo 变化）→ user 轮的 <system-reminder> 块
//
// 这也解释了一个你可能见过但没想明白的现象：为什么各种 agent 的"提醒"
// 总是以 <system-reminder> 出现在用户消息里，而不是写在 system prompt 里。
//
// ── 一条线把内容分成两半 ────────────────────────────────────────────────────
//
//   请求渲染出来是这个顺序，缓存**逐字节匹配前缀**：
//
//     [tools]                      ToolRegistry::schemas()，按名字排序
//     [system]   ← build_system() 拼的
//        ├── kIdentity             身份、工作方式（子 agent 的 identity 覆盖它）
//        ├── kToolGuide            工具怎么用、有哪些坑
//        ├── Working directory     路径，一个会话里不变
//        ├── project_doc()         AGENTS.md / CLAUDE.md，改文件才变
//        ├── skills->index_text()  ┐ 只有名字 + 描述，正文按需 load
//        ├── memory->index_text()  ┘
//        └── extra                 子 agent 的补充说明
//                 ▲
//                 └── cache_breakpoint = true 打在**最后一块**
//     ═══════════════════════════════════ 缓存到此为止
//     [messages]
//        ...
//        {user, "<system-reminder>当前 todo: ...</system-reminder>"}  ← turn_context()
//        {user, "真正的用户输入"}
//
//   ⚠️ 分界线的判据只有一个：**这东西每轮都会变吗？**
//        不变 → system（进缓存，之后每轮省 90%）
//        会变 → 包成 <system-reminder>，追加成 messages 末尾一条 user 消息
//
//   ⚠️ 放错边的代价**不对称**：
//        动态内容误放 system    → 缓存每轮作废，账单十倍，**功能完全正常**
//        静态内容误放 reminder  → 只是多花一点点，没别的坏处
//      所以拿不准的时候往 reminder 放。
//
// ── 为什么是 <system-reminder> 而不是直接写 ─────────────────────────────────
//
//   模型对这个标签有先验：里面的是**系统附加的上下文**，不是用户的指令。
//
//     不包：  "后台任务 bg_1 完成了"
//             → 读成「用户让我去处理那个任务」
//     包上：  "<system-reminder>后台任务 bg_1 完成了</system-reminder>"
//             → 读成「有这么件事，跟我现在做的有关吗？」
//
// ── 四个函数各管一段 ────────────────────────────────────────────────────────
//
//   build_system(cfg, skills, memory, extra, identity)  ──▶ vector<SystemBlock>
//        │  ⚠️ 里面**一个会变的东西都不许有**。Agent::run() 在**循环外**
//        │     调它一次，整个 run 期间不再变
//        └──▶ project_doc(workdir)    AGENTS.md → CLAUDE.md → .agent.md，
//                                      第一个找到的赢；太大就截断并说明
//
//   turn_context(background, todos)  ──▶ string      每轮开头调
//        │  后台通知（Stage 6）+ 当前 todo；没内容返回**空串**
//        └──▶ reminder(text)         包成 <system-reminder>；空进空出
//
//   ⚠️ 空返回值是有意义的：调用方只在非空时才 append 一条消息，所以安静的
//      一轮**一个字节都不加**。返回占位符的话，每轮都往历史里塞一条废消息。
//
#include <string>
#include <vector>

#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"

namespace mini {

class SkillRegistry;
class Memory;
class BackgroundManager;

/// @brief Who the agent is and how it should work.
///
/// @note Static by construction — it names no file, no time, no task. Anything
///       that varies per request belongs in turn_context() instead.
extern const char* kIdentity;

/// @brief What the tools are and when to reach for each.
///
/// @note Names the traps the model would otherwise fall into: read before
///       edit, prefer edit to write, use glob/grep rather than shelling out to
///       ls/find/cat. Each of those has a tool-side check behind it, and a
///       model that routes around the tool routes around the check too.
extern const char* kToolGuide;

/// @brief Assemble the system prompt.
///
/// @param cfg      Read for the workdir and the project conventions file.
/// @param skills   Contributes its index, when present (Stage 5).
/// @param memory   Contributes its index, when present (Stage 5).
/// @param extra    Appended as its own block; empty is skipped.
/// @param identity Replaces kIdentity rather than adding to it — a sub-agent
///                 is a different persona, not the main one plus a note.
/// @return At least one block; only the last carries the cache breakpoint.
///
/// @warning **Nothing here may vary between requests.** The rendering order is
///          tools → system → messages and the prompt cache matches a byte-exact
///          prefix, so one timestamp, uuid or "current todo list" in here means
///          the whole conversation after it is billed at full price every turn.
///          Nothing fails; it shows up on the bill. Dynamic content goes
///          through turn_context(), which lands in a user turn past the
///          breakpoint.
///
/// @note The breakpoint goes on the **last** block, which caches tools and the
///       entire system prompt in one go. On an earlier block, every block after
///       it is paid for again each turn.
/// @note Empty blocks are dropped rather than emitted. A stray "\n\n" is a
///       byte, and bytes are what the cache compares.
/// @note Skills and memory contribute an *index* — what exists and when to use
///       it — not their bodies. Ten skills inlined in full would put thousands
///       of tokens into every single turn's fixed cost.
///
/// @code{.test}
/// @setup const Config cfg = doc_config();
/// @setup const auto blocks = build_system(cfg);
/// (blocks.size() > 1)                        ==> true
/// blocks.back().cache_breakpoint             ==> true
/// blocks.front().cache_breakpoint            ==> false
/// // 逐字节稳定：同样的输入必须给出同样的字节
/// @setup std::string a; for (const auto& b : build_system(cfg)) a += b.text;
/// @setup std::string b; for (const auto& b2 : build_system(cfg)) b += b2.text;
/// (a == b)                                   ==> true
/// // identity 是覆盖，不是叠加
/// @setup std::string sub; for (const auto& b3 : build_system(cfg, nullptr, nullptr, {}, "我是子 agent")) sub += b3.text;
/// (sub.find("我是子 agent") != std::string::npos)   ==> true
/// @endcode
///
/// 组装 system。**不许放任何随请求变化的东西** —— 时间戳走 turn_context()。
std::vector<SystemBlock> build_system(const Config& cfg,
                                      const SkillRegistry* skills = nullptr,
                                      const Memory* memory = nullptr,
                                      std::string_view extra = {},
                                      std::string_view identity = {});

/// @brief The repository's own conventions: AGENTS.md, CLAUDE.md, .agent.md.
///
/// @param workdir Where to look.
/// @param limit   Keep at most this many bytes.
/// @return Empty when no such file exists.
///
/// @note The first name that exists wins; the rest are ignored. Reading two
///       would state the same rules twice, and a rule repeated in a system
///       prompt reads as emphasis the author did not intend.
/// @note Oversized files are truncated rather than skipped — the first few KB
///       are usually the rules that matter — and the truncation is stated, so
///       the model does not act as though it read the whole thing.
/// @note This is repository content, so it belongs in the cached system prompt
///       rather than in turn_context(). It changes when someone edits the file,
///       not per request.
std::string project_doc(const fs::path& workdir, std::size_t limit = 8000);

/// @brief Wrap dynamic context so the model reads it as context, not as an order.
///
/// @param text What to wrap; empty in, empty out.
/// @return The text inside a `<system-reminder>` element.
///
/// @warning The tag is not decoration. Models treat `<system-reminder>` as
///          system-supplied context rather than a user instruction. Unwrapped,
///          a line like "the background task finished" reads as the user asking
///          for that task to be dealt with.
///
/// @note Empty in gives empty out rather than an empty element — that would
///       still be bytes, and the cache compares bytes.
///
/// @code{.test}
/// (reminder("后台任务完成").find("<system-reminder>") != std::string::npos)  ==> true
/// (reminder("后台任务完成").find("后台任务完成") != std::string::npos)        ==> true
/// reminder("").empty()                                                       ==> true
/// @endcode
std::string reminder(std::string_view text);

/// @brief Everything that changes between turns: background output, todos.
///
/// @param background Source of completion notices (Stage 6); may be null.
/// @param todos      The current list; an empty array contributes nothing.
/// @return Empty when nothing changed, which means nothing gets injected.
///
/// @warning This is the **only** door dynamic content comes through. Put any of
///          it in build_system() instead and the prompt cache misses every
///          turn — silently, and for the whole conversation.
///
/// @note Returning empty rather than a placeholder matters: the caller appends
///       a message only when there is something to say, so a quiet turn adds
///       no bytes at all.
std::string turn_context(BackgroundManager* background, const Json& todos);

}  // namespace mini
