# mini-swe-agent (C++23)

**English** · [简体中文](README.zh-CN.md)

A skeleton of a SWE agent. Headers, build system and tests are in place — the
function bodies are the exercise.

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`test_smoke` starts out entirely red. Each failure names the next function to
write:

```
✗ loop_runs_tool_then_answers
    TODO — Stage 7: App 构造 —— 把十几个模块接起来
```

## What this is

Not a tutorial on calling an LLM API. The interesting part is everything C++
forces you to decide that a scripting language hides: ownership, lifetimes,
concurrency, subprocess management.

Three ideas carry the whole design:

1. **An agent is a while loop.** Ask the model, run the tools it asks for, feed
   the results back, repeat until it stops asking. Ten lines. Everything else —
   permissions, concurrency, compaction, sub-agents — is flow control around
   those ten lines.
2. **Every capability is the same shape.** A tool is a JSON Schema (for the
   model) plus a `run()` function (for you). File I/O, bash, sub-agents and
   remote MCP servers all fit that one interface.
3. **Context is the scarce resource.** The model has no memory; every turn
   resends the whole history. That single fact explains compaction, sub-agent
   isolation, progressive disclosure and the byte-stability requirement on the
   system prompt.

## Layout

```
mini-swe-agent-cpp/
├── BUILD-GUIDE.md          Stage-by-stage construction guide (read this first)
├── ARCHITECTURE.md         Design overview with diagrams
├── include/mini_agent/     The contracts. Every header opens with *why* it is
│                           shaped that way — that is documentation, not decoration
├── src/                    The work. Unimplemented bodies call todo("Stage N: ...")
└── tests/
    ├── microtest.hpp       A 50-line test framework you can read in one sitting
    ├── test_smoke.cpp      15 cases that serve as the specification
    ├── test_loop.cpp       Shape of a turn, wired up without App
    ├── test_config.cpp     Priority chain and the to_string round trip
    ├── test_tool.cpp       Tool registry and schema invariants
    ├── test_parser.cpp     Wire-format contracts on both ends of the loop
    ├── test_session.cpp    Persistence round-trip
    └── mock_mcp_server.py  A fake MCP server for the Stage 7 handshake
```

## Stages

Each stage is a self-contained increment; stopping after any of them leaves
something usable.

| Stage | Files | What you end up with |
|---|---|---|
| 0 | `config` `message` | The types that run through everything |
| 1 | `llm` `parser` `session` `loop` | **An agent that actually works** |
| 2 | `tool` `executor` `process` | Full tool layer with concurrent execution |
| 3 | `sandbox` | A permission gate you'd trust on a real repo |
| 4 | `prompt` `session::compact` | Cache-friendly prompts, long tasks that don't overflow |
| 5 | `memory` `skills` | Cross-session memory and skill plugins |
| 6 | `subagent` `scheduler` `background` | Multi-agent, task graphs, background jobs |
| 7 | `mcp` `app` `cli` | External tools and a usable interface |

## Current status

```
Stage 0  ████████████████████  done
Stage 1  ██████████████████░░  loop runs end to end; AnthropicClient still open
Stage 2  ██████████████░░░░░░  executor, tool registry, read tool
Stage 7  ████████░░░░░░░░░░░░  App wiring, enough to run
Stage 3  ██████████░░░░░░░░░░  rules, command splitting, mode fallbacks
Stage 4+ ░░░░░░░░░░░░░░░░░░░░
```

| Suite | Result |
|---|---|
| `test_loop` | 16/16 |
| `test_config` | 19/19 |
| `test_tool` | 17/17 |
| `test_parser` | 14/14 |
| `test_session` | 12/12 |
| `test_smoke` | 9/15 — the rest need the rest of Stage 3, edit, compaction, scheduler |

Builds clean under `-Wall -Wextra -Wpedantic`.

## Dependencies

| Dependency | How it arrives | Used by |
|---|---|---|
| nlohmann/json | CMake `FetchContent`, automatic | Aliased once in `json.hpp` |
| libcurl | `sudo apt install libcurl4-openssl-dev` | `src/llm.cpp` only |

**Stages 0–6 need no libcurl.** Every test runs against `FakeLlm` and never
touches the network. CMake warns and carries on when libcurl is missing.

## Requirements

Verified on g++ 13.3, CMake 3.28, Ninja 1.11, Ubuntu 24.04.

Three C++23 features are used, each for a stated reason:

| Feature | Where | Why |
|---|---|---|
| `std::expected` | `LlmClient::complete` | Failure is a value, not an exception — 429/529 must be retryable |
| `std::jthread` | Background task output pump | Joins on destruction, carries a stop_token |
| `std::format` | Prompt assembly, terminal rendering | Replaces a pile of ostringstream |

clangd configuration lives in `.clangd` (without it you get spurious
`std::expected` errors).

## Testing

```bash
cmake --build build && ctest --test-dir build --output-on-failure
./build/test_tool                       # per-case output while developing
```

Adding `tests/test_*.cpp` is enough — CMake globs them into individual
executables and registers each with ctest.

Green tests only prove code and tests agree. To check that a test is actually
watching, break the thing it covers and confirm it turns red:

```bash
sed -i 's/a->name() == v/a->description() == v/' src/tool.cpp
cmake --build build --target test_tool && ./build/test_tool   # expect a failure
```

## Relationship to the Python reference

`../mini-swe-agent/reference/` is the same design in Python — 3100 lines, 21
tests green. Use it to compare *behaviour*, not structure. The C++ version has a
whole category of problems Python never raises, and `BUILD-GUIDE.md` covers
those separately.
