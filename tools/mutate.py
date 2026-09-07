#!/usr/bin/env python3
"""变异测试：验证测试真的能抓到它们声称在抓的 bug。

一条断言可以完全正确、却什么都证明不了 —— 只要它选的输入让**正确实现和错误
实现给出相同答案**。读代码看不出来（断言本身没写错），全绿也看不出来。唯一
可靠的办法是把 bug 真的种回去，看该红的那一条会不会红。

用法:
    python3 tools/mutate.py            # 跑全部
    python3 tools/mutate.py process    # 只跑名字或文件里含 "process" 的
    python3 tools/mutate.py --check    # 只校验变异点还对得上代码，不编译

每条变异都声明**期望红掉哪个用例**。只要求"有东西红了"是不够的：变异可能被
一条无关的用例误伤，而真正该守着它的那条依然是假的。

加一条变异：往 MUTANTS 里加一项，`edits` 里的 old 必须在文件中唯一出现
（--check 会验证），`expect` 写用例名的子串。
"""
import argparse
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
ANSI = re.compile(r"\x1b\[[0-9;]*m")

SUBJ_OLD = '\n'.join([
 '    /// 审查对象是搜索**起点**，不是模式 —— 权限规则约束的是"能看哪个目录"。',
 '    std::string subject(const Json& args) const override {',
 '        return str_arg(args, "path").value_or(std::string{"."});',
 '    }',
])
SUBJ_NEW = '\n'.join([
 '    std::string subject(const Json& args) const override {',
 '        return str_arg(args, "path").value_or(std::string{});',
 '    }',
])

S4_SPLIT_OLD = '    const std::size_t split = safe_split(keep_recent);'
S4_SPLIT_NEW = '    const std::size_t split = messages_.size() > keep_recent ? messages_.size() - keep_recent : 0;'
S4_RESP_OLD = '    if (!resp) return false;   // ⚠️ 失败就原样返回，绝不能把历史删了再发现总结没拿到'
S4_RESP_NEW = ('    if (!resp) {\n'
               '        messages_.erase(messages_.begin(), messages_.begin() + static_cast<long>(split));\n'
               '        ++compactions_;\n'
               '        save();\n'
               '        return false;\n'
               '    }')
S4_EMPTY_OLD = '    if (summary.empty()) return false;   // 空纪要比不压缩糟糕得多'
S4_EMPTY_NEW = '    // MUTANT'
S4_EST_OLD = '    return static_cast<int>(to_json(messages_).dump().size() / 4);'
S4_EST_NEW = '    return 0;'
S4_NOTE_OLD = '    Message note{Role::User,\n                 {TextBlock{"<system-reminder>\\n以下是此前对话的纪要（原文已省略）：\\n\\n" +\n                            summary + "\\n</system-reminder>"}}};\n'
S4_NOTE_NEW = '    Message note{Role::User, {TextBlock{summary}}};\n'
S4_WD_OLD = '    push_block(blocks, "Working directory: " + cfg.workdir.string());'
S4_WD_NEW = '    push_block(blocks, "Working directory: " + cfg.workdir.string() + "\\nNow: " +\n                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));'
S4_BREAK_OLD = '    blocks.back().cache_breakpoint = true;'
S4_BREAK_NEW = '    blocks.front().cache_breakpoint = true;'
S4_PUSH_OLD = '    if (!text.empty()) out.push_back(SystemBlock{std::move(text), false});'
S4_PUSH_NEW = '    out.push_back(SystemBlock{std::move(text), false});'
S4_CUT_OLD = '        if (cut) out += "\\n\\n[... 已截断]";'
S4_CUT_NEW = '        // MUTANT'
S4_REMIND_OLD = '    return "<system-reminder>\\n" + std::string(text) + "\\n</system-reminder>";'
S4_REMIND_NEW = '    return std::string(text);'
S4_RETURN_OLD = '        return out;\n    }\n    return {};'
S4_RETURN_NEW = '        acc += out;\n    }\n    return acc;'

M_SORT_OLD = '    std::ranges::sort(index_, {}, &MemoryItem::name);'
M_SORT_NEW = '    // MUTANT'
M_DESC_OLD = '    const auto d = fm->fields.find("description");\n    if (d == fm->fields.end() || d->second.empty()) return std::nullopt;\n    // ⚠️ 没有 description 就整条丢弃，不是给个空串。一条描述为空的记忆会进\n    //    索引、占位置，而模型永远不会去加载它 —— 静默失效比读不到更糟。\n    item.description = d->second;'
M_DESC_NEW = '    const auto d = fm->fields.find("description");\n    if (d != fm->fields.end()) item.description = d->second;'
M_DESCW_OLD = '            score += 4 * std::min(count_hits(desc_l, w), 2);'
M_DESCW_NEW = '            score += 1 * std::min(count_hits(desc_l, w), 2);'
M_BODYCAP_OLD = '            score += 1 * std::min(count_hits(body_l, w), 3);'
M_BODYCAP_NEW = '            score += 1 * count_hits(body_l, w);'
M_REIDX_OLD = '    rebuild_index();   // 下一轮的 system prompt 就该看得见它'
M_REIDX_NEW = '    // MUTANT'
M_IDXDESC_OLD = '        out += std::format("  {} [{}] — {}\\n", item.name, type_name(item.type), item.description);'
M_IDXDESC_NEW = '        out += std::format("  {} [{}] — {}\\n", item.name, type_name(item.type), item.body);'
F_CLOSED_OLD = '    if (!closed) return std::nullopt;   // 没有结束的 ---，整个文件当作没有 frontmatter'
F_CLOSED_NEW = '    (void)closed;'
F_COLON_OLD = "        const auto colon = raw.find(':');"
F_COLON_NEW = "        const auto colon = raw.rfind(':');"

S_SHADOW_OLD = '                skills_.emplace(s->name, std::move(*s));'
S_SHADOW_NEW = '                skills_.insert_or_assign(s->name, std::move(*s));'
S_DIRONLY_OLD = '            // 只是省掉一次无谓的开文件尝试 —— 「skill 必须是文件夹」这条规矩\n            // 其实是下面那句路径拼接保证的：对 loose.md 会去开\n            // loose.md/SKILL.md，不存在，照样跳过。\n            if (!e.is_directory(ec)) continue;\n            if (auto s = parse_skill_file(e.path() / "SKILL.md"))\n                skills_.emplace(s->name, std::move(*s));'
S_DIRONLY_NEW = '            const auto md = e.is_directory(ec) ? e.path() / "SKILL.md" : e.path();\n            if (auto s = parse_skill_file(md))\n                skills_.emplace(s->name, std::move(*s));'
S_DESC_OLD = '    const auto d = fm->fields.find("description");\n    if (d == fm->fields.end() || d->second.empty()) return std::nullopt;\n    s.description = d->second;'
S_DESC_NEW = '    const auto d = fm->fields.find("description");\n    if (d != fm->fields.end()) s.description = d->second;'
S_FOLDER_OLD = '    s.name = n != fm->fields.end() ? n->second : skill_md.parent_path().filename().string();'
S_FOLDER_NEW = '    s.name = n != fm->fields.end() ? n->second : skill_md.filename().stem().string();'
S_SIBS_OLD = '    std::ranges::sort(siblings);   // 顺序稳定：文件系统的遍历顺序不保证'
S_SIBS_NEW = '    // MUTANT'
S_SKIPSELF_OLD = '        if (fname == "SKILL.md") continue;'
S_SKIPSELF_NEW = '        // MUTANT'
S_NOSIBS_OLD = '    if (siblings.empty()) return out;'
S_NOSIBS_NEW = '    // MUTANT'
S_IDXDESC_OLD = '        out += std::format("  {} — {}\\n", name, s.description);'
S_IDXDESC_NEW = '        out += std::format("  {} — {}\\n", name, s.body);'

T5_SUBJ_OLD = '    std::string subject(const Json& args) const override {\n        if (const auto n = str_arg(args, "name")) return *n;\n        return str_arg(args, "action").value_or(std::string{});\n    }'
T5_SUBJ_NEW = '    std::string subject(const Json& args) const override {\n        return Tool::subject(args);\n    }'
T5_DESC_OLD = '            if (!desc || desc->empty())\n                return ToolResult::error("write 需要 description（写「什么时候该用它」）");'
T5_DESC_NEW = '            // MUTANT'
T5_ACTION_OLD = '        if (*action != "load" && *action != "write" && *action != "delete")\n            return ToolResult::error("未知 action: " + *action + "（search/load/write/delete）");\n'
T5_ACTION_NEW = ''
T5_NAMES_OLD = '            std::string names;\n            for (const Skill* k : ctx.skills->all()) names += (names.empty() ? "" : ", ") + k->name;\n            return ToolResult::error("没有名为 " + *want + " 的 skill。可用: " +\n                                     (names.empty() ? "（一个都没有）" : names));'
T5_NAMES_NEW = '            return ToolResult::error("没有名为 " + *want + " 的 skill");'
T5_NULLMEM_OLD = '        if (!ctx.memory) return ToolResult::error("memory 未启用");'
T5_NULLMEM_NEW = '        // MUTANT'
T5_DIRS_OLD = '    if (enable_memory) fs::create_directories(memory_dir());\n    if (enable_skills && !skills_dirs().empty()) fs::create_directories(skills_dirs().front());'
T5_DIRS_NEW = '    fs::create_directories(memory_dir());\n    if (!skills_dirs().empty()) fs::create_directories(skills_dirs().front());'

# 每项: name, file, edits[(old, new)], binaries, expect[用例名子串], note
# 可选 known_gap: 已知抓不到，附上为什么。留在清单里是有意的 —— 把没覆盖的地方
# 记下来，比从清单里删掉假装不存在有用。
MUTANTS = [
    # ── process.cpp —— run_shell 的三个坑 ──────────────────────────────────
    dict(
        name="父进程漏关管道写端",
        file="src/process.cpp",
        edits=[("""    //    read() 等不到 EOF，命令早退出了也要卡到超时。
    ::close(fds[1]);""",
                """    //    read() 等不到 EOF，命令早退出了也要卡到超时。
    // ::close(fds[1]);""")],
        binaries=["test_process"],
        expect=["fast_command_returns_immediately"],
        note="管道永远有一个写者 → 读不到 EOF → 每条命令都卡满超时",
    ),
    dict(
        name="kill(pid) 而不是 kill(-pgid)",
        file="src/process.cpp",
        edits=[("::kill(-pid, SIGTERM)", "::kill(pid, SIGTERM)"),
               ("::kill(-pid, SIGKILL)", "::kill(pid, SIGKILL)")],
        binaries=["test_process"],
        expect=["timeout_kills_the_whole_process_group"],
        note="`sleep 30 &` 的孙子进程活下来，还攥着管道写端",
    ),
    dict(
        name="截断后不再排干管道",
        file="src/process.cpp",
        edits=[("        const ssize_t got = ::read(fd, buf, sizeof buf);",
                """        if (out.size() >= limit) { truncated = true; continue; }
        const ssize_t got = ::read(fd, buf, sizeof buf);""")],
        binaries=["test_process"],
        expect=["truncated_command_still_runs_to_completion"],
        note="子进程阻塞在 write()，我们等 EOF、它等我们读 —— 死锁",
    ),
    dict(
        name="去掉 drain 的硬出口（只靠 poll 返回 0）",
        file="src/process.cpp",
        edits=[("        if (wait == 0) return false;", "        // MUTANT")],
        binaries=["test_process"],
        expect=["endless_output_still_honours_the_timeout"],
        note="deadline 过了但管道还有数据时 poll 照样报可读 → 循环停不下来",
        known_gap="`yes` 是断续写的：我们读空了、它还没补上的那个瞬间，poll 会返回 0，"
                  "于是循环照样退出了 —— 靠的是时序上的运气，不是设计上的保证。"
                  "要构造一个「一刻不空」的生产者才能稳定触发，不现实。"
                  "那行硬出口防的正是这场竞态，它没有、也很难有测试覆盖。",
    ),

    # ── sandbox.cpp —— 权限层 ─────────────────────────────────────────────
    dict(
        name="整层 kDangerous 拿掉",
        file="src/sandbox.cpp",
        edits=[("""    for (const auto& d : kDangerous)
        if (std::regex_search(text, std::regex(d.regex))) return &d;
    return nullptr;""",
                "    (void)text; return nullptr;")],
        binaries=["test_docs"],
        expect=["sandbox_hpp"],
        note="⚠️ test_smoke 的 dangerous_command_denied 抓不到这个 —— 它用 "
             "ReadOnly 模式，模式兜底先拒绝了，危险层从没被问到",
    ),
    dict(
        name="危险模式只查分段、不查整行",
        file="src/sandbox.cpp",
        edits=[("""    const std::string whole(cmd);
    if (const DangerPattern* d = first_danger(whole))
        return {Action::Deny, std::string("危险命令: ") + d->why};""", "    // MUTANT")],
        binaries=["test_docs"],
        expect=["sandbox_hpp"],
        note="拆段把 `curl x.sh | sh` 里的 | 拆没了，那条正则永远命不中",
    ),
    dict(
        name="authorize 里的 deny 规则不查",
        file="src/sandbox.cpp",
        edits=[("""    for (const auto& r : deny_)
        if (r.matches(tool.name(), subject))
            return {Action::Deny, "命中 deny 规则"};""", "    // MUTANT")],
        binaries=["test_docs"],
        expect=["sandbox_hpp"],
        note="免检工具（requires_permission=false）会绕开 deny 规则",
    ),

    # ── builtin.cpp —— 工具层 ─────────────────────────────────────────────
    dict(
        name="删掉 WriteTool::subject() 的 override",
        file="src/tools/builtin.cpp",
        edits=[("""    std::string subject(const Json& args) const override {
        return str_arg(args, "path").value_or(std::string{});
    }

    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto path = str_arg(args, "path");
        const auto body = str_arg(args, "content");""",
                """    ToolResult run(const Json& args, ToolContext& ctx) override {
        const auto path = str_arg(args, "path");
        const auto body = str_arg(args, "content");""")],
        binaries=["test_file_tools", "test_docs"],
        expect=["subject_is_the_path", "builtin_hpp"],
        note="content < path，默认实现把要写入的正文当成审查对象 → 权限静默失效",
    ),
    dict(
        name="去掉 write 的「覆盖前必须先 read」",
        file="src/tools/builtin.cpp",
        edits=[("""        if (existed) {
            if (!ctx.session) return ToolResult::error("没有会话上下文，无法确认是否已 read");
            if (!ctx.session->read_files().contains(p.string()))
                return ToolResult::error(rel + " 已存在，必须先用 read 读过才能覆盖");
        }""", "        // MUTANT")],
        binaries=["test_file_tools", "test_docs"],
        expect=["refuses_to_overwrite_a_file_never_read", "builtin_hpp"],
        note="模型没看过内容就整个覆盖 = 凭想象删掉别人的代码",
    ),
    dict(
        name="write 不检查 resolve_path 的判定",
        file="src/tools/builtin.cpp",
        edits=[("""        const auto [p, decision] = ctx.sandbox->resolve_path(rel);
        if (!decision.allowed()) return ToolResult::error(decision.reason + ": " + rel);

        std::error_code ec;
        const bool existed = fs::exists(p, ec);""",
                """        const auto [p, decision] = ctx.sandbox->resolve_path(rel);

        std::error_code ec;
        const bool existed = fs::exists(p, ec);""")],
        binaries=["test_file_tools"],
        expect=["path_outside_the_workdir_is_refused"],
        note="`../../../etc/passwd` 一路写出去",
    ),
    dict(
        name="write 不建父目录",
        file="src/tools/builtin.cpp",
        edits=[("""        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);""",
                """        if (false) {
            fs::create_directories(p.parent_path(), ec);""")],
        binaries=["test_file_tools"],
        expect=["creates_missing_parent_directories"],
        note="模型得先跑一条 bash mkdir，白费一轮",
    ),
    dict(
        name="退回 args.value（类型不对会抛）",
        file="src/tools/builtin.cpp",
        edits=[('const auto body = str_arg(args, "content");',
                'const auto body = std::optional<std::string>{args.value("content", std::string{})};')],
        binaries=["test_file_tools"],
        expect=["wrong_argument_types_give_a_clean_error"],
        note="{\"content\": 42} 抛 json::type_error，模型收到一句没头没尾的 what()",
    ),
    dict(
        name="去掉 edit 的唯一性检查",
        file="src/tools/builtin.cpp",
        edits=[("""        if (content.find(old_s, first + old_s.size()) != std::string::npos)
            return ToolResult::error("old_string 在 " + rel +
                                     " 中出现多次，请多带几行上下文使其唯一");""", "        // MUTANT")],
        binaries=["test_docs"],
        expect=["builtin_hpp"],
        note="只替换第一处，模型以为全改了",
    ),
    dict(
        name="glob 按字母序而不是修改时间倒序",
        file="src/tools/builtin.cpp",
        edits=[("        std::ranges::sort(hits, std::ranges::greater{}, &decltype(hits)::value_type::first);",
                "        std::ranges::sort(hits, {}, &decltype(hits)::value_type::second);")],
        binaries=["test_search_tools"],
        expect=["glob_sorts_newest_first"],
        note="模型问「有哪些 cpp」时要的几乎总是最近动过的，字母序把 app.cpp 排前面",
    ),
    # ── Stage 5：长期记忆与 frontmatter ──────────────────────────────────
    dict(
        name='memory 索引不排序',
        file='src/memory.cpp',
        edits=[(M_SORT_OLD, M_SORT_NEW)],
        binaries=['test_memory'],
        expect=['the_index_order_is_stable'],
        note='index_text 进 system prompt，目录遍历顺序不保证 → 缓存每轮作废，账单十倍而功能正常',
    ),
    dict(
        name='没有 description 的记忆也进索引',
        file='src/memory.cpp',
        edits=[(M_DESC_OLD, M_DESC_NEW)],
        binaries=['test_memory'],
        expect=['a_memory_without_a_description_is_discarded'],
        note='空描述的记忆占着索引位置，模型永远不会去加载它 —— 静默失效。'
             '注意变异形状：光把那个 return 去掉会解引用 end() 迭代器（UB），'
             '测的不是这条守卫。这里直接写出「空描述也放行」这个真正的 bug。',
    ),
    dict(
        name='description 命中不加权',
        file='src/memory.cpp',
        edits=[(M_DESCW_OLD, M_DESCW_NEW)],
        binaries=['test_memory'],
        expect=['search_weights_the_description_above_the_body'],
        note='真正相关的那条掉出 limit，排序退化成按名字',
    ),
    dict(
        name='正文命中不封顶',
        file='src/memory.cpp',
        edits=[(M_BODYCAP_OLD, M_BODYCAP_NEW)],
        binaries=['test_memory'],
        expect=['a_long_body_cannot_win_by_length_alone'],
        note='堆砌关键词的长文永远排第一',
    ),
    dict(
        name='write 之后不重建索引',
        file='src/memory.cpp',
        edits=[(M_REIDX_OLD, M_REIDX_NEW)],
        binaries=['test_memory'],
        expect=['write_is_visible_immediately'],
        note='这一轮写的记忆下一轮 system 里看不到',
    ),
    dict(
        name='索引里放正文而不是描述',
        file='src/memory.cpp',
        edits=[(M_IDXDESC_OLD, M_IDXDESC_NEW)],
        binaries=['test_memory'],
        expect=['the_index_carries_the_description_not_the_body'],
        note='渐进式披露整个失效，每轮固定开销几千 token',
    ),
    dict(
        name='frontmatter 不要求结束的 ---',
        file='src/frontmatter.cpp',
        edits=[(F_CLOSED_OLD, F_CLOSED_NEW)],
        binaries=['test_memory'],
        expect=['frontmatter_requires_both_delimiters'],
        note='整个文件被当成 frontmatter 吞掉，正文全丢',
    ),
    dict(
        name='frontmatter 按最后一个冒号切',
        file='src/frontmatter.cpp',
        edits=[(F_COLON_OLD, F_COLON_NEW)],
        binaries=['test_memory'],
        expect=['frontmatter_splits_on_the_first_colon_not_the_last'],
        note='值里有冒号时键被切坏',
    ),
    # ── Stage 5：两个工具与接线 ──────────────────────────────────────────
    dict(
        name='memory 工具的 subject 用默认实现',
        file='src/tools/builtin.cpp',
        edits=[(T5_SUBJ_OLD, T5_SUBJ_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['memory_subject_is_the_item_not_the_action'],
        note='字母序 action < name，沙箱会拿动作名去匹配规则，而不是被操作的那条记忆',
    ),
    dict(
        name='memory write 不要求 description',
        file='src/tools/builtin.cpp',
        edits=[(T5_DESC_OLD, T5_DESC_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['memory_write_demands_a_description'],
        note='空描述的记忆进了索引占位置，模型永远不会去加载它 —— 写了等于没写还白占上下文',
    ),
    dict(
        name='未知 action 先抱怨缺 name',
        file='src/tools/builtin.cpp',
        edits=[(T5_ACTION_OLD, T5_ACTION_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['memory_rejects_a_bad_action_and_names_the_valid_ones'],
        note='模型收到「frobnicate 需要 name」会照着补一个 name 再试，而真正错的是动作名',
    ),
    dict(
        name='skill 名字错时不列出可用的',
        file='src/tools/builtin.cpp',
        edits=[(T5_NAMES_OLD, T5_NAMES_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['skill_lists_the_alternatives'],
        note='模型是照着索引写的名字，不给候选它只能再猜一次',
    ),
    dict(
        name='memory 未启用时不检查空指针',
        file='src/tools/builtin.cpp',
        edits=[(T5_NULLMEM_OLD, T5_NULLMEM_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['the_tools_report_rather_than_crash_when_disabled'],
        note='关掉 memory 的配置下，模型调一次这个工具就是空指针解引用。'
             '表现是段错误 —— 这是最强的信号，但一条 ✗ 都打不出来，'
             '所以工具要把"崩溃"和"没抓到"分开，否则看起来一模一样。',
    ),
    dict(
        name='ensure_dirs 不看开关',
        file='src/config.cpp',
        edits=[(T5_DIRS_OLD, T5_DIRS_NEW)],
        binaries=['test_stage5_tools', 'test_docs'],
        expect=['app_leaves_no_memory_directory_when_the_switch_is_off'],
        note='在别人的工作区里留下一个永远空着的 .mini-agent/memory',
    ),
    # ── Stage 5：Skill 插件 ──────────────────────────────────────────────
    dict(
        name='skill 同名时靠后的目录覆盖靠前的',
        file='src/skills.cpp',
        edits=[(S_SHADOW_OLD, S_SHADOW_NEW)],
        binaries=['test_skills'],
        expect=['an_earlier_directory_shadows_a_later_one'],
        note='仓库里的约定会被别人机器上的全局配置覆盖',
    ),
    dict(
        name='顺手也接受散落的 .md 当 skill',
        file='src/skills.cpp',
        edits=[(S_DIRONLY_OLD, S_DIRONLY_NEW)],
        binaries=['test_skills'],
        expect=['a_bare_markdown_file_is_not_a_skill'],
        note='散落的 .md 会被当成 skill —— 那是 memory 的形态，而 skill 是文件夹，'
             '它的附属脚本和模板要靠 render() 列出来。'
             '这条变异必须同时改两行：is_directory 和路径拼接互为冗余，'
             '单独删掉任何一行，另一行都会把 loose.md 兜住 —— 谁都抓不到。',
    ),
    dict(
        name='没有 description 的 skill 也进索引',
        file='src/skills.cpp',
        edits=[(S_DESC_OLD, S_DESC_NEW)],
        binaries=['test_skills'],
        expect=['a_skill_without_a_description_is_discarded'],
        note='空描述的 skill 占着索引位置，模型永远不会去加载它',
    ),
    dict(
        name='name 缺失时用文件名而不是文件夹名兜底',
        file='src/skills.cpp',
        edits=[(S_FOLDER_OLD, S_FOLDER_NEW)],
        binaries=['test_skills'],
        expect=['a_missing_name_falls_back_to_the_folder_name'],
        note='每个 SKILL.md 都叫这个名字 —— 所有匿名 skill 会撞成一个',
    ),
    dict(
        name='附属文件清单不排序',
        file='src/skills.cpp',
        edits=[(S_SIBS_OLD, S_SIBS_NEW)],
        binaries=['test_skills'],
        expect=['sibling_listing_is_stable'],
        note='render 的字节不稳定；这段会进上下文',
    ),
    dict(
        name='附属文件清单里也列出 SKILL.md',
        file='src/skills.cpp',
        edits=[(S_SKIPSELF_OLD, S_SKIPSELF_NEW)],
        binaries=['test_skills'],
        expect=['render_lists_the_sibling_files'],
        note='正文就是它，列出来是噪音',
    ),
    dict(
        name='没有附属文件也加一个空标题',
        file='src/skills.cpp',
        edits=[(S_NOSIBS_OLD, S_NOSIBS_NEW)],
        binaries=['test_skills'],
        expect=['render_omits_the_listing_when_there_is_nothing_else'],
        note='一个说「同目录下还有」然后什么都没有的标题',
    ),
    dict(
        name='skill 索引里放正文而不是描述',
        file='src/skills.cpp',
        edits=[(S_IDXDESC_OLD, S_IDXDESC_NEW)],
        binaries=['test_skills'],
        expect=['the_index_carries_the_description_not_the_body'],
        note='渐进式披露整个失效，十个 skill 全文每轮都发',
    ),
    # ── Stage 4：上下文压缩与 prompt 组装 ──────────────────────────────
    dict(
        name='compact 用朴素切分而不是 safe_split',
        file='src/session.cpp',
        edits=[(S4_SPLIT_OLD, S4_SPLIT_NEW)],
        binaries=['test_compact'],
        expect=['compaction_never_orphans_a_tool_pair'],
        note='切在 tool_result 上 → 它的 tool_use 进了纪要 → 下一轮 400，而历史已落盘，--continue 回来照样 400',
    ),
    dict(
        name='compact 请求失败也照删历史',
        file='src/session.cpp',
        edits=[(S4_RESP_OLD, S4_RESP_NEW)],
        binaries=['test_compact'],
        expect=['a_failed_request_leaves_the_history_untouched'],
        note='先删历史再发现总结没拿到 = 不可逆的数据丢失。'
             '注意变异的形状：单纯删掉那个 return 会解引用出错的 expected（UB），'
             '空字符串又被下一道 summary.empty() 拦住 —— 那测的不是这条守卫。'
             '这里直接把「失败时删历史」写出来，才是真正要防的 bug。',
    ),
    dict(
        name='compact 接受空纪要',
        file='src/session.cpp',
        edits=[(S4_EMPTY_OLD, S4_EMPTY_NEW)],
        binaries=['test_compact'],
        expect=['an_empty_summary_leaves_the_history_untouched'],
        note='把整段历史换成一句空话',
    ),
    dict(
        name='estimated_tokens 永远返回 0',
        file='src/session.cpp',
        edits=[(S4_EST_OLD, S4_EST_NEW)],
        binaries=['test_compact'],
        expect=['estimated_tokens_grows_with_the_history'],
        note='永远不会触发压缩，超 token 直接挂掉',
    ),
    dict(
        name='纪要不带 system-reminder 标记',
        file='src/session.cpp',
        edits=[(S4_NOTE_OLD, S4_NOTE_NEW)],
        binaries=['test_compact'],
        expect=['the_summary_is_marked_as_a_summary'],
        note='模型会把机器生成的纪要当成用户的新指令去执行',
    ),
    dict(
        name='system prompt 里混进时间戳',
        file='src/prompt.cpp',
        edits=[(S4_WD_OLD, S4_WD_NEW)],
        binaries=['test_compact'],
        expect=['system_is_byte_stable_across_calls'],
        note='缓存逐字节匹配前缀 —— 变一个字节整段对话每轮全价，而功能完全正常',
    ),
    dict(
        name='缓存断点打在第一块而不是最后一块',
        file='src/prompt.cpp',
        edits=[(S4_BREAK_OLD, S4_BREAK_NEW)],
        binaries=['test_compact'],
        expect=['only_the_last_system_block_carries_the_cache_breakpoint'],
        note='断点之后的几块每轮全价',
    ),
    dict(
        name='build_system 保留空块',
        file='src/prompt.cpp',
        edits=[(S4_PUSH_OLD, S4_PUSH_NEW)],
        binaries=['test_compact'],
        expect=['no_empty_system_blocks'],
        note='空块也是字节，缓存按字节匹配',
    ),
    dict(
        name='project_doc 截断了不说',
        file='src/prompt.cpp',
        edits=[(S4_CUT_OLD, S4_CUT_NEW)],
        binaries=['test_compact'],
        expect=['project_doc_truncates_instead_of_giving_up'],
        note='模型以为读全了，照着半份规矩干活',
    ),
    dict(
        name='reminder 不包 system-reminder 标签',
        file='src/prompt.cpp',
        edits=[(S4_REMIND_OLD, S4_REMIND_NEW)],
        binaries=['test_compact'],
        expect=['reminder_wraps_but_not_when_empty'],
        note='「后台任务完成了」会被当成用户要求它去处理',
    ),
    dict(
        name="glob 的 **/ 不能匹配零个目录",
        file="src/tools/builtin.cpp",
        edits=[("""    if (::fnmatch(pattern.c_str(), rel.c_str(), 0) == 0) return true;
    // 每次去掉一个 `**/` 再试；有多个就逐个递归，一定会收敛。
    const auto at = pattern.find("**/");
    if (at == std::string::npos) return false;
    return glob_match(std::string(pattern).erase(at, 3), rel);""",
                "    return ::fnmatch(pattern.c_str(), rel.c_str(), 0) == 0;")],
        binaries=["test_search_tools"],
        expect=["globstar_matches_zero_directories"],
        note="src/**/*.cpp 会漏掉 src/ 正下方的所有文件 —— agent 自己跑起来后发现的",
    ),
    dict(
        name="遍历不跳过 build/ .git/ 等目录",
        file="src/tools/builtin.cpp",
        edits=[("            if (is_skipped_dir(p.filename().string())) it.disable_recursion_pending();",
                "            // MUTANT")],
        binaries=["test_search_tools"],
        expect=["glob_skips_build_and_vcs_directories", "grep_skips_build_and_vcs_directories"],
        note="build/ 几千个中间文件 + .git/ 成千上万个 object 会把真正的结果淹掉",
    ),
    dict(
        name="grep 不跳过二进制文件",
        file="src/tools/builtin.cpp",
        edits=[("            if (looks_binary(in)) return;", "            // MUTANT")],
        binaries=["test_search_tools"],
        expect=["grep_skips_binary_files"],
        note="一个 .o 就能吐出几千行乱码，撑爆整轮上下文，而且模型也用不上",
    ),
    dict(
        name="grep 不截断超长行",
        file="src/tools/builtin.cpp",
        edits=[("""                if (line.size() > kMaxLineChars)
                    line = line.substr(0, kMaxLineChars) + " …(行过长已截断)";""",
                "                // MUTANT")],
        binaries=["test_search_tools"],
        expect=["grep_clips_very_long_lines"],
        note="一行 minified js 就是一兆",
    ),
    dict(
        name="grep 坏正则直接抛而不是返回错误",
        file="src/tools/builtin.cpp",
        edits=[("""        } catch (const std::regex_error& e) {
            return ToolResult::error("正则有语法错误: " + *pattern + " —— " + e.what());
        }""",
                """        } catch (const std::regex_error&) {
            throw;
        }""")],
        binaries=["test_search_tools"],
        expect=["grep_reports_a_bad_regex_instead_of_throwing"],
        note="正则是模型写的，抛出去它只看到一句 what()，改不了",
    ),
    dict(
        name="glob/grep 的 subject 默认返回空串",
        file="src/tools/builtin.cpp",
        edits=[(SUBJ_OLD, SUBJ_NEW)],
        binaries=["test_search_tools"],
        expect=["subject_is_the_search_root"],
        note="空串在沙箱里是「路径为空」，规则匹配不到任何东西",
    ),
    dict(
        name="bash 超时不标记 is_error",
        file="src/tools/builtin.cpp",
        edits=[("""        if (r.timed_out)
            return ToolResult{.content = body, .is_error = true, .metadata = std::move(meta)};""",
                """        if (r.timed_out)
            return ToolResult{.content = body, .is_error = false, .metadata = std::move(meta)};""")],
        binaries=["test_bash"],
        expect=["timeout_is_an_error_and_says_so"],
        note="模型会把半截输出当成完整结果继续推理",
    ),
    dict(
        name="bash 非零退出码不标记 is_error",
        file="src/tools/builtin.cpp",
        edits=[(".is_error = r.exit_code != 0,", ".is_error = false,")],
        binaries=["test_bash"],
        expect=["nonzero_exit_is_an_error"],
        note="编译失败看起来像成功",
    ),
    dict(
        name="bash 不夹住模型给的超时",
        file="src/tools/builtin.cpp",
        edits=[("std::clamp(want, 1, cfg_timeout)", "want")],
        binaries=["test_bash"],
        expect=["model_cannot_raise_the_timeout"],
        note="模型可以自己解除超时限制",
    ),
]


def failures(binary, timeout=120):
    """跑一个测试二进制。

    返回 (失败用例名集合, 状态)，状态是 "ok" / "crash" / "hang"。

    ⚠️ 崩溃必须和"没抓到"分开。一个让二进制段错误的变异是**被抓到了**，而且
       是最强的形式 —— 但崩溃时一条 ✗ 都打不出来，只看 stdout 的话会算成
       "一个都没红"，和真正的漏网看起来一模一样。踩过一次：把 memory 的空指针
       检查删掉，测试段错误，工具报"没抓到"。
    """
    exe = BUILD / binary
    try:
        p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return set(), "hang"
    out = ANSI.sub("", p.stdout)
    red = {m.group(1) for m in re.finditer(r"^✗ (\S+)", out, re.M)}
    # 负的退出码 = 被信号杀死（Python 的约定），139 那种是 shell 的算法
    crashed = p.returncode < 0 or p.returncode >= 128
    return red, ("crash" if crashed and not red else "ok")


def build():
    p = subprocess.run(["cmake", "--build", str(BUILD), "-j4"],
                       capture_output=True, text=True, timeout=600)
    return p.returncode == 0, p.stderr


def dirty_sources():
    """哪些会被变异的文件当前有未提交改动。

    还原靠的是 finally 里把内存里的备份写回去 —— kill -9 或断电就还原不了。
    那种情况下唯一的救命稻草是 `git checkout src/`，所以跑之前这些文件必须干净，
    否则你自己没提交的改动会跟变异版本混在一起，分不开。
    """
    files = sorted({m["file"] for m in MUTANTS})
    p = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--"] + files,
                       capture_output=True, text=True)
    return [l[3:] for l in p.stdout.splitlines() if l.strip()]


def check_anchors():
    """每个 old 必须在文件里唯一出现。代码改了但变异点没跟上时会在这里报。"""
    bad = 0
    for m in MUTANTS:
        text = (ROOT / m["file"]).read_text()
        for old, _ in m["edits"]:
            n = text.count(old)
            if n != 1:
                print(f"✗ {m['name']}: 锚点在 {m['file']} 中出现 {n} 次（应为 1）")
                print(f"    {old.splitlines()[0][:70]}")
                bad += 1
    return bad


def apply(m):
    path = ROOT / m["file"]
    text = path.read_text()
    for old, new in m["edits"]:
        assert text.count(old) == 1
        text = text.replace(old, new)
    path.write_text(text)


def run_one(m):
    """返回 (ok, 说明)。"""
    path = ROOT / m["file"]
    backup = path.read_text()
    try:
        apply(m)
        ok, err = build()
        if not ok:
            return False, "变异后编译不过:\n" + err[-500:]

        red, hung, crashed = set(), [], []
        for b in m["binaries"]:
            f, status = failures(b)
            red |= f
            if status == "hang":
                hung.append(b)
            elif status == "crash":
                crashed.append(b)

        missed = [e for e in m["expect"] if not any(e in name for name in red)]
        if hung:
            # 死循环也算抓到了，但形式很差 —— CI 会挂而不是给出一条红
            return True, f"⚠ 抓到了，但表现是**卡死**（{', '.join(hung)}），不是干净的红"
        if crashed:
            # 段错误是最强的信号，但一条 ✗ 都打不出来 —— 不能算成漏网
            return True, f"⚠ 抓到了，但表现是**崩溃**（{', '.join(crashed)}），不是干净的红"
        if missed and m.get("known_gap"):
            return None, "已知未覆盖: " + m["known_gap"]
        if missed:
            return False, ("测试没抓到。期望红掉含 " + ", ".join(repr(x) for x in missed) +
                           " 的用例；实际红掉: " + (", ".join(sorted(red)) or "（一个都没有）"))
        if m.get("known_gap"):
            return True, ("抓到了 —— known_gap 可以删掉了: " + ", ".join(sorted(red)))
        extra = len(red) - len(m["expect"])
        tail = f"（另有 {extra} 个用例受牵连）" if extra > 0 else ""
        return True, "抓到: " + ", ".join(sorted(red)) + tail
    finally:
        path.write_text(backup)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("filter", nargs="?", default="",
                    help="只跑 name / file / binaries 里含此子串的变异")
    ap.add_argument("--check", action="store_true", help="只校验锚点，不编译")
    ap.add_argument("--dirty-ok", action="store_true",
                    help="源码有未提交改动时也跑（还原失败就救不回来了）")
    args = ap.parse_args()

    if not shutil.which("cmake") or not BUILD.exists():
        print(f"需要一个已配置好的构建目录: {BUILD}")
        return 2

    if not args.check and not args.dirty_ok:
        dirty = dirty_sources()
        if dirty:
            print("以下会被变异的文件有未提交改动，先提交或 stash：")
            for f in dirty:
                print(f"  {f}")
            print("\n还原只在进程正常结束时发生。真崩了要靠 `git checkout src/` 救，"
                  "\n而那会连你自己没提交的改动一起冲掉。确实要跑就加 --dirty-ok。")
            return 1

    bad = check_anchors()
    if args.check:
        print("锚点全部对得上。" if not bad else f"{bad} 个锚点失配。")
        return 1 if bad else 0
    if bad:
        print("先修好锚点再跑。")
        return 1

    # ⚠️ 三个字段都要匹配。只匹配 name 的话，`mutate.py glob` 会静默漏掉
    #    「遍历不跳过 build/ .git/」（名字里没有 glob），`mutate.py test_compact`
    #    会一条都匹配不上 —— 而"跑了 0 条"和"全都通过"在输出上很难区分。
    def matches(m):
        return (args.filter in m["name"] or args.filter in m["file"]
                or any(args.filter in b for b in m["binaries"]))

    picked = [m for m in MUTANTS if matches(m)]
    if not picked:
        print(f"没有匹配 {args.filter!r} 的变异。可用的过滤词：")
        print("  文件: " + ", ".join(sorted({m["file"] for m in MUTANTS})))
        print("  测试: " + ", ".join(sorted({b for m in MUTANTS for b in m["binaries"]})))
        return 1
    print(f"选中 {len(picked)}/{len(MUTANTS)} 条变异")

    print("先确认基线是绿的 ...", end=" ", flush=True)
    ok, err = build()
    if not ok:
        print("编译不过\n" + err[-500:])
        return 1
    bins = sorted({b for m in picked for b in m["binaries"]})
    baseline = {b: failures(b) for b in bins}
    if any(status != "ok" or red for red, status in baseline.values()):
        print("失败 —— 未变异的代码就有红的用例（或崩溃/卡死），先修那个")
        for b, (red, status) in baseline.items():
            if red or status != "ok":
                print(f"  {b}: {status} {', '.join(sorted(red))}")
        return 1
    print("绿")

    passed, gaps, failed = 0, 0, 0
    for i, m in enumerate(picked, 1):
        print(f"\n[{i}/{len(picked)}] {m['name']}")
        print(f"        {m['note']}")
        good, msg = run_one(m)
        mark = {True: "✓", False: "✗", None: "○"}[good]
        if good is None:
            gaps += 1
        elif good:
            passed += 1
        else:
            failed += 1
        print(f"    {mark} {msg}")

    print(f"\n{passed}/{len(picked)} 条变异被测试抓到"
          + (f"，{gaps} 条已知未覆盖" if gaps else "")
          + (f"，{failed} 条**该抓没抓到**" if failed else ""))

    print("\n还原后重新确认基线 ...", end=" ", flush=True)
    ok, _ = build()
    after = {b: failures(b) for b in bins} if ok else None
    if not ok or any(status != "ok" or red for red, status in (after or {}).values()):
        print("失败 —— 还原没干净，或者某次变异污染了环境")
        return 1
    print("绿")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
