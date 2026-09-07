# Testing

*[中文版](testing.zh-CN.md)*

Three layers, each answering a different question.

| Layer | Where | Question | How to run |
|---|---|---|---|
| Unit tests | `tests/test_*.cpp` | Is the code correct? | `ctest` |
| Documented examples | `@code{.test}` in headers | Is the documentation still true? | same (builds `test_docs`) |
| Doc build | `Doxyfile` | Are the comments themselves wrong? | `cmake --build build --target docs` |
| Mutation testing | `tools/mutate.py` | **Are the tests worth anything?** | `python3 tools/mutate.py` |

The third layer is the one that gets skipped, and the one this project has got
the most out of.

---

## 1. Unit tests

Every `test_*.cpp` under `tests/` becomes its own executable. The GLOB in
`CMakeLists.txt` picks it up and `add_test` registers it, so a new file needs no
configuration change.

The framework is `tests/microtest.hpp` — fifty lines, readable in one sitting.
`TEST(name)` expands to a static `Registrar` whose constructor puts the case
into a registry before `main()` runs.

```cpp
TEST(refuses_to_overwrite_a_file_never_read) {
    CHECK_MSG(r.is_error, "没读过就整个覆盖 = 凭想象删掉别人的代码");
}
```

The second argument to `CHECK_MSG` says **why the case matters**, not what the
assertion compares. When a test goes red, the reader needs to know which
agreement was broken, not that a != b.

**`tests/test_smoke.cpp` is the specification.** It describes what the system
should do; do not edit it to accommodate an implementation.

## 2. Documented examples

`@code{.test}` blocks in the headers are lifted into real assertions by
`tools/gen_doc_tests.py`:

```cpp
/// @code{.test}
/// @setup const auto wd = doc_workdir();
/// run_shell("echo hello", wd, 5s).output    ==> "hello\n"
/// run_shell("exit 42", wd, 5s).exit_code    ==> 42
/// @endcode
```

becomes

```cpp
CHECK_MSG((run_shell("echo hello", wd, 5s).output) == ("hello\n"),
          "include/mini_agent/process.hpp:89 的示例失效了");
```

The failure names a line in the **header**, so what you go and fix is the
documentation itself (or the code the documentation was right about). The
generated `tests/test_docs.cpp` never needs opening.

### Rules

- Lines starting with `@setup` are emitted verbatim as statements; lines
  containing `==>` become assertions; **source order is preserved** — some
  examples exist precisely because something changes between two assertions.
- A plain `@code` block (no `{.test}`) is documentation only and is not compiled.
- Scaffolding beyond a line or two belongs in `tests/doc_prelude.hpp` (`DocTool`,
  `doc_config()`, `DocTools`). An example should read as documentation, not as a
  test.
- CMake regenerates on any header change. The generated file is checked in, so a
  build without Python still works.

### It is not there for coverage

Almost every assertion in `test_docs` is already covered by a hand-written test.
Its one job is this:

> Six months from now someone changes the behaviour and fixes the tests. Nobody
> goes back to update the comment.

Having a machine watch is the only thing that reliably works.

## 3. The doc build

```bash
cmake --build build --target docs      # output in docs/api/html/index.html
```

This layer does not ask whether the documentation is *true* — that is the layer
above. It asks whether the comments are **malformed**:

| Mistake | What it reports |
|---|---|
| `@param` naming a parameter that does not exist | `argument 'workdir' of command @param is not found in the argument list of run_shell(...)` |
| A parameter with no `@param` | `The following parameter ... is not documented: parameter 'timeout'` |
| `@ref` to a symbol that is not there | `unable to resolve reference to 'NoSuchSymbol'` |

`WARN_AS_ERROR = FAIL_ON_WARNINGS`, so a report is a failure. Without doxygen
installed the target simply does not exist; it is not a build dependency.

`INPUT` is `include` alone — not the READMEs, not this file. Doxygen's markdown
parser does not follow GitHub's rules (`#pragma` becomes a symbol reference even
inside a fence, a cross-language link becomes a `\ref`), and bending the markdown
to keep it quiet would mean making the GitHub rendering worse to satisfy a site
nobody reads the docs from. This target is worth having for the header comments.

### Every header needs `@file`

⚠️ **A free function is extracted only when the file it lives in is itself
documented.**

Adding this configuration is what revealed that none of the 21 headers had
`@file`, so the documentation on `run_shell`, `make_*_tool`, `load_config`,
`builtin_tools` and `kDangerous` reached no generated page at all — hundreds of
lines that did not exist as far as doxygen was concerned, and which the `@param`
checking therefore never touched.

Every header now opens with:

```cpp
#pragma once
/// @file
/// @brief Running a subprocess with a timeout — the most systems-level file here (Stage 2).
```

### Why `WARN_IF_UNDOCUMENTED` is NO

Turning it on reports 264 "not documented" against 2 real errors — 132:1, which
means nobody reads it. Documentation coverage is a gradual goal (`loop`, `app`,
`config`, `executor`, `llm`, `mcp`, `memory`, `skills`, `subagent` are still
undone) and does not belong behind the same switch as a mistake that can be
fixed on the spot. To see the coverage gap:

```bash
(cat Doxyfile; echo WARN_IF_UNDOCUMENTED=YES) | doxygen -
```

## 4. Mutation testing

### The problem

An assertion can be entirely correct and still prove nothing — as long as the
input it picks makes **a correct and a broken implementation agree**.

```cpp
// workdir = /work; is this path inside it?
CHECK(sb.resolve_path("src/a.py").second.allowed());              // both allow
CHECK(!sb.resolve_path("../../../etc/passwd").second.allowed());  // both deny
```

Swap "compare path components" for "compare string prefixes" — a real security
hole — and both lines stay green. Catching it needs an input the two
implementations answer **differently**:

```cpp
CHECK(!sb.resolve_path("/work-other/x.py").second.allowed());
//     string prefix: allow ✗    path components: deny ✓
```

That is a **counter-example**. An assertion without one is decoration.

### The seven vacuous tests found in this project

| What was wrong | The fix |
|---|---|
| `compactions == 0` | The value was already 0; any implementation passes |
| Byte stability | Held even with the implementation replaced by `std::rand()` |
| `resolve_path` containment | Four inputs both implementations agree on → added `/work-other/x.py` |
| `yes` never ends | A correct implementation times out too → `yes \| head -100000`, which exits |
| `dangerous_command_denied` | Read-only mode denies first; the danger layer is never consulted |
| `bash->subject()` | One string parameter, so the base-class default happens to be right → added a key sorting ahead of it |
| glob's mtime order | Files named `old.cpp` / `new.cpp` — alphabetical puts `new` first too → renamed to `a_newest` / `z_oldest` |

One sentence covers all seven: **the input failed to separate the two
implementations.** None of them is visible from reading the code, because every
assertion is written correctly.

### Running it

```bash
python3 tools/mutate.py            # everything
python3 tools/mutate.py process    # only mutants whose name or file matches
python3 tools/mutate.py --check    # verify the anchors still match; no build
```

### It really does edit your source files

Not a copy — it writes into `src/`, builds, runs the tests, and writes the
original back:

```python
backup = path.read_text()
try:
    apply(m)                    # the file on disk really changes
    build(); run_tests()
finally:
    path.write_text(backup)
```

`finally` survives exceptions, build failures and hung tests. It does not
survive `kill -9` or a power cut, which leave the mutant in the file; the way
back from that is `git checkout src/`. So the script refuses to start when any
file it mutates has uncommitted changes (`--dirty-ok` overrides, at the cost of
that rescue also discarding your own work).

Afterwards it **re-confirms the baseline**, which catches both a bad restore and
a polluted environment.

### Three outcomes

| | Meaning |
|---|---|
| `✓` | The named case went red — the test does its job |
| `○` | A known gap (`known_gap`), with the reason attached |
| `✗` | **Should have been caught and was not** — that test is vacuous |

Only `✗` counts as a failure. `known_gap` is there for code that is genuinely
hard to test: recording what is uncovered beats deleting the entry and
pretending otherwise. If someone later adds a covering test, the tool says the
`known_gap` can be removed.

One entry carries it today: the hard exit in `drain()`. It guards a race —
past the deadline `poll` still reports readable while data sits in the pipe —
and `yes` writes in bursts, so in the moment between our read draining the pipe
and `yes` refilling it, `poll` returns 0 and the loop exits anyway. Triggering
it reliably would need a producer that never leaves the pipe empty.

### Adding a mutant

```python
dict(
    name="删掉 WriteTool::subject() 的 override",
    file="src/tools/builtin.cpp",
    edits=[(correct_code, broken_code)],
    binaries=["test_file_tools", "test_docs"],
    expect=["subject_is_the_path", "builtin_hpp"],   # cases that must go red
    note="content < path，默认实现把要写入的正文当成审查对象 → 权限静默失效",
),
```

Two things matter.

**Each `old` must occur exactly once in the file.** `--check` verifies it, so a
refactor that moves the anchor is reported instead of silently skipping the
mutant.

**`expect` must name specific cases, not merely require that something went
red.** A mutation can be caught by an unrelated case while the one that is
supposed to guard it stays vacuous.

### Plant the right bug

The mutation itself can be wrong. Twice in this project:

- Inserting "stop at the limit" **after** the `read()` — that only skips the
  append, which is what correct code does anyway, so nothing could catch it.
- Using `return false` for "stop reading" — that took the timeout-and-kill path,
  which rescued the very bug being planted.

A mutation has to be **the bug you are actually worried about**, not something
shaped like it.

### Side effects

Mutation testing here has not only found vacuous tests. It found two real bugs:

- `drain()` had no hard termination guarantee — past the deadline `poll` still
  reports readable while data sits in the pipe, so the loop ending depended on
  the body consuming on every pass.
- A test assertion depended on the global path `/tmp/escaped.txt`. The mutation
  run duly created that file, and **correct code then failed that case forever
  after**.

---

## Everything at once

```bash
cmake --build build -j4 && ctest --test-dir build --output-on-failure
cmake --build build --target docs
python3 tools/mutate.py
```

The build must be warning-free under `-Wall -Wextra -Wpedantic`.
