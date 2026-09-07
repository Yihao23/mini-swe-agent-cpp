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
    """跑一个测试二进制，返回失败用例名的集合；卡死返回 None。"""
    exe = BUILD / binary
    try:
        p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    out = ANSI.sub("", p.stdout)
    return {m.group(1) for m in re.finditer(r"^✗ (\S+)", out, re.M)}


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

        red, hung = set(), []
        for b in m["binaries"]:
            f = failures(b)
            if f is None:
                hung.append(b)
            else:
                red |= f

        missed = [e for e in m["expect"] if not any(e in name for name in red)]
        if hung:
            # 死循环也算抓到了，但形式很差 —— CI 会挂而不是给出一条红
            return True, f"⚠ 抓到了，但表现是**卡死**（{', '.join(hung)}），不是干净的红"
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
    ap.add_argument("filter", nargs="?", default="", help="只跑名字或文件含此子串的变异")
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

    picked = [m for m in MUTANTS
              if args.filter in m["name"] or args.filter in m["file"]]
    if not picked:
        print(f"没有匹配 {args.filter!r} 的变异")
        return 1

    print("先确认基线是绿的 ...", end=" ", flush=True)
    ok, err = build()
    if not ok:
        print("编译不过\n" + err[-500:])
        return 1
    bins = sorted({b for m in picked for b in m["binaries"]})
    baseline = {b: failures(b) for b in bins}
    if any(v is None or v for v in baseline.values()):
        print("失败 —— 未变异的代码就有红的用例，先修那个")
        for b, v in baseline.items():
            if v:
                print(f"  {b}: {', '.join(sorted(v))}")
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
    if not ok or any(v is None or v for v in (after or {}).values()):
        print("失败 —— 还原没干净，或者某次变异污染了环境")
        return 1
    print("绿")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
