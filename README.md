# mini-swe-agent (C++23)

**English** · [简体中文](README.zh-CN.md)

## What this is

A small coding agent written in C++23. You give it a task in plain language; it
calls Claude, reads and edits files, runs commands, and keeps going until the
task is done.

At its core an agent is a loop: ask the model, run the tools it asks for, send
the results back, repeat. Everything else in this project — permissions,
context management, sub-agents, external tools — is built around that loop.

## What I have done

All seven stages are implemented:

| Stage | What it adds |
|---|---|
| 0–1 | The agent loop, a real Anthropic client with streaming, session history |
| 2 | Tools: `read` `write` `edit` `glob` `grep` `bash` `todo`, commands with timeouts |
| 3 | A permission gate: allow/deny rules, permission modes, dangerous-command checks |
| 4 | Cache-friendly prompts and automatic compaction of long conversations |
| 5 | Long-term memory and skills |
| 6 | Sub-agents, task graphs that run them in parallel, background commands |
| 7 | MCP client for external tools, a command-line interface, `--continue` |

Checked four ways:

- **Tests** — 22 test programs, all offline (a fake model stands in for the API).
- **Documented examples** — code examples in the headers are compiled and run as tests.
- **Mutation testing** — `tools/mutate.py` plants 102 known bugs and checks that
  the right test catches each one; 5 are recorded as not yet caught, with the reason.
- **ThreadSanitizer** — a separate build that checks the concurrent code for data races.

These checks found and fixed real bugs, including sub-agents corrupting each
other's responses through a shared client, a leaked file descriptor per
background task, and an "always allow" approval that silently allowed more than
the user approved.

## How to use it

### Build

```bash
sudo apt install libcurl4-openssl-dev      # needed for real API calls
cmake -S . -B build -G Ninja
cmake --build build
```

Tested on Ubuntu 24.04 with g++ 13, CMake 3.28 and Ninja.

### Run

```bash
export ANTHROPIC_API_KEY=...

./build/mini-agent                                   # interactive
./build/mini-agent "find where sessions are saved"   # one task, then exit
./build/mini-agent -c                                # continue the last session
```

| Option | Meaning |
|---|---|
| `-C, --dir <path>` | Working directory (default: current directory) |
| `--model <name>` | Model to use |
| `--mode <mode>` | `read-only` · `ask` · `auto` · `yolo` |
| `--no-stream` | Print the answer at the end instead of as it arrives |
| `-c, --continue` | Resume the most recently used session |

Inside interactive mode: `/help` `/tools` `/usage` `/session` `/mode` `/clear` `/exit`.

### Configure (optional)

Everything lives in `.mini-agent/` inside the working directory.

`.mini-agent/config.json`:

```json
{
  "model": "claude-opus-5",
  "permission_mode": "ask",
  "allow_rules": ["Bash(git status:*)", "Read"],
  "deny_rules": ["Bash(rm:*)"]
}
```

`.mini-agent/mcp.json` — external tool servers (any program that speaks MCP over stdio):

```json
{ "mcpServers": { "notes": { "command": "python3", "args": ["notes_server.py"] } } }
```

Its tools show up as `mcp__notes__<tool>`, so a rule like `Mcp__notes__*` covers all of them.

Environment variables override the file: `MINI_AGENT_MODEL`, `MINI_AGENT_MODE`,
`MINI_AGENT_EFFORT`, `MINI_AGENT_MAX_STEPS`.

### Test

```bash
ctest --test-dir build --output-on-failure    # all tests
python3 tools/mutate.py                       # mutation testing (slow)
cmake --build build --target docs             # check the documentation comments

cmake -S . -B build-tsan -DMINI_AGENT_SANITIZE=thread   # data-race checks
cmake --build build-tsan && ctest --test-dir build-tsan
```

## More

[BUILD-GUIDE.md](BUILD-GUIDE.md) walks through the stages ·
[ARCHITECTURE.md](ARCHITECTURE.md) explains the design ·
[docs/testing.md](docs/testing.md) explains the testing layers
