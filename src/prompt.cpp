// 【Stage 4】输入 prompt 组装。
//
// 契约写在 include/mini_agent/prompt.hpp。这里只讲实现上的取舍。

#include "mini_agent/prompt.hpp"

#include <fstream>

#include "mini_agent/background.hpp"
#include "mini_agent/memory.hpp"
#include "mini_agent/skills.hpp"

namespace mini {

const char* kIdentity =
    "You are a software engineering agent. You work in a real repository, with real\n"
    "consequences: what you write gets committed, what you delete is gone.\n"
    "\n"
    "Work in small, verifiable steps. After a change, run the thing that would fail\n"
    "if you got it wrong — the test, the build, the command. A change nobody ran is\n"
    "a guess.\n"
    "\n"
    "Say what you found, not what you hope. If a test fails, report the failure with\n"
    "its output. If you skipped part of the task, say which part and why. Finishing\n"
    "half the work and describing it as done is worse than not starting.\n"
    "\n"
    "Be brief. The person reading this is looking at a terminal, not an essay.";

const char* kToolGuide =
    "Tools:\n"
    "  read   — file contents with line numbers, or a directory listing\n"
    "  write  — create a file, or replace one whole\n"
    "  edit   — replace one span inside a file\n"
    "  glob   — find files by name, newest first\n"
    "  grep   — find files by content, as path:line: text\n"
    "  bash   — run one shell command\n"
    "\n"
    "  * Read a file before you edit it. edit refuses otherwise, and for good\n"
    "    reason: an edit written from memory silently deletes work.\n"
    "  * Prefer edit over write for a change. write replaces the whole file, which\n"
    "    means retyping every line you were not changing.\n"
    "  * Use glob and grep to find things; do not reach for bash ls/find/cat. The\n"
    "    dedicated tools check their boundaries and the shell does not.\n"
    "  * Issue independent read-only calls together rather than one per turn.\n"
    "  * A tool that fails returns a reason. Read it and adjust — do not retry the\n"
    "    identical call.";

/// @brief One system block, skipped when its body is empty.
///
/// @note Empty blocks are dropped rather than emitted: a stray "\n\n" in the
///       system prompt is a byte, and the prompt cache matches bytes.
static void push_block(std::vector<SystemBlock>& out, std::string text) {
    if (!text.empty()) out.push_back(SystemBlock{std::move(text), false});
}

std::vector<SystemBlock> build_system(const Config& cfg, const SkillRegistry* skills,
                                      const Memory* memory, std::string_view extra,
                                      std::string_view identity) {
    // ⚠️ 这个函数里**不许**出现任何随请求变化的东西：时间戳、uuid、当前 todo。
    //    渲染顺序是 tools → system → messages，缓存逐字节匹配前缀 —— system 里
    //    变一个字节，它后面的整段对话每轮都要重新计费。动态内容走 turn_context()。
    std::vector<SystemBlock> blocks;

    push_block(blocks, identity.empty() ? std::string(kIdentity) : std::string(identity));
    push_block(blocks, std::string(kToolGuide));
    push_block(blocks, "Working directory: " + cfg.workdir.string());

    // 项目约定（AGENTS.md / CLAUDE.md）。这是仓库的规矩，属于静态内容。
    push_block(blocks, project_doc(cfg.workdir));

    // Stage 5 的两个索引：只放"有什么、什么时候用"，正文按需加载。
    // 全文塞进 system 的话，十个 skill 就能把每轮的固定开销顶到几千 token。
    if (skills) push_block(blocks, skills->index_text());
    if (memory) push_block(blocks, memory->index_text());

    push_block(blocks, std::string(extra));

    // ⚠️ 缓存断点打在**最后一块**上，一次把 tools + 整个 system 都缓存住。
    //    打在中间的话后面几块每轮全价。blocks 不可能为空 —— kIdentity 是常量。
    blocks.back().cache_breakpoint = true;
    return blocks;
}

std::string project_doc(const fs::path& workdir, std::size_t limit) {
    // 靠前的优先，找到一个就停。两个都读会让同一段规矩出现两次。
    static const char* kNames[] = {"AGENTS.md", "CLAUDE.md", ".agent.md"};
    for (const char* name : kNames) {
        std::ifstream in(workdir / name, std::ios::binary);
        if (!in) continue;
        std::string body((std::istreambuf_iterator<char>(in)), {});
        if (body.empty()) continue;

        // 截断而不是跳过：一份 50KB 的 CLAUDE.md 也比没有强，
        // 而且开头几 KB 通常就是最要紧的规矩。
        bool cut = false;
        if (body.size() > limit) {
            body.resize(limit);
            cut = true;
        }
        std::string out = std::string("Project conventions (from ") + name + "):\n\n" + body;
        if (cut) out += "\n\n[... 已截断]";
        return out;
    }
    return {};
}

std::string reminder(std::string_view text) {
    if (text.empty()) return {};
    // ⚠️ 这个标签不是装饰。模型对 <system-reminder> 有先验：里面的内容是系统
    //    附加的上下文，不是用户的指令。不包的话，一句「后台任务完成了」会被
    //    当成用户在要求它去处理那个任务。
    return "<system-reminder>\n" + std::string(text) + "\n</system-reminder>";
}

std::string turn_context(BackgroundManager* background, const Json& todos) {
    // ⚠️ 这里是动态内容的唯一出口。放进 system 的话缓存每轮作废。
    std::string out;

    // ⚠️ notifications() 有副作用：它把任务标成"已通知"。所以这里**只能调一次**，
    //    而且调了就必须把结果用掉 —— 丢掉的话那条通知永远不会再出现，模型会
    //    一直等一个不会来的消息。
    if (background) {
        const auto news = background->notifications();
        if (!news.empty()) {
            out += "后台任务状态变化：\n";
            for (const auto& n : news) out += "  " + n + "\n";
            out += "\n";
        }
    }

    if (todos.is_array() && !todos.empty()) {
        out += "当前 todo：\n";
        for (const auto& t : todos) {
            const auto status = t.value("status", std::string{"pending"});
            const auto content = t.value("content", std::string{});
            if (!content.empty()) out += "  [" + status + "] " + content + "\n";
        }
    }
    return out;
}

}  // namespace mini
