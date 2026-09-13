# mini-swe-agent (C++23)

[English](README.md) · **简体中文**

## 这是什么

一个用 C++23 写的小型编程 agent。用自然语言给它一个任务，它会调用 Claude、
读写文件、执行命令，一直做到任务完成。

agent 的核心就是一个循环：问模型 → 执行它要的工具 → 把结果送回去 → 再问。
这个项目里其余的一切 —— 权限控制、上下文管理、子 agent、外部工具 —— 都是围绕这个循环搭起来的。

## 我做了什么

七个阶段全部实现：

| 阶段 | 内容 |
|---|---|
| 0–1 | agent 主循环、支持流式输出的真实 Anthropic 客户端、会话历史 |
| 2 | 工具：`read` `write` `edit` `glob` `grep` `bash` `todo`，带超时的命令执行 |
| 3 | 权限闸门：allow / deny 规则、权限模式、危险命令检查 |
| 4 | 对提示缓存友好的 prompt，长对话自动压缩 |
| 5 | 长期记忆和 skills |
| 6 | 子 agent、并行执行子 agent 的任务图、后台命令 |
| 7 | 接入外部工具的 MCP 客户端、命令行界面、`--continue` |

用四种方式验证：

- **测试**：22 个测试程序，全部离线运行（用一个假模型代替 API）。
- **文档示例**：头文件里的代码示例会被编译成测试并实际运行。
- **变异测试**：`tools/mutate.py` 往代码里埋 102 个已知的 bug，检查每个都能被对应的测试抓到；其中 5 个明确登记为「暂时抓不到」，并写明了原因。
- **ThreadSanitizer**：单独的一套构建，检查并发代码有没有数据竞争。

这些检查找出并修掉了真实的 bug，比如：多个子 agent 共用一个客户端时互相破坏响应；
每个后台任务泄漏一个文件描述符；「总是允许」悄悄放行了比用户批准的更多的命令。

## 怎么用

### 构建

```bash
sudo apt install libcurl4-openssl-dev      # 调用真实 API 需要
cmake -S . -B build -G Ninja
cmake --build build
```

在 Ubuntu 24.04、g++ 13、CMake 3.28、Ninja 上验证过。

### 运行

```bash
export ANTHROPIC_API_KEY=...

./build/mini-agent                                 # 交互模式
./build/mini-agent "找出会话保存在哪里"             # 执行一个任务后退出
./build/mini-agent -c                              # 接着上次的会话
```

| 选项 | 含义 |
|---|---|
| `-C, --dir <路径>` | 工作目录（默认当前目录） |
| `--model <名字>` | 使用的模型 |
| `--mode <模式>` | `read-only` · `ask` · `auto` · `yolo` |
| `--no-stream` | 回答完再一次性打印，不边生成边输出 |
| `-c, --continue` | 恢复最近用过的会话 |

交互模式里可用：`/help` `/tools` `/usage` `/session` `/mode` `/clear` `/exit`。

### 配置（可选）

所有配置都放在工作目录下的 `.mini-agent/` 里。

`.mini-agent/config.json`：

```json
{
  "model": "claude-opus-5",
  "permission_mode": "ask",
  "allow_rules": ["Bash(git status:*)", "Read"],
  "deny_rules": ["Bash(rm:*)"]
}
```

`.mini-agent/mcp.json`：外部工具服务器（任何通过 stdio 说 MCP 协议的程序都行）

```json
{ "mcpServers": { "notes": { "command": "python3", "args": ["notes_server.py"] } } }
```

它的工具会显示为 `mcp__notes__<工具名>`，所以规则 `Mcp__notes__*` 能盖住它的全部工具。

环境变量优先于配置文件：`MINI_AGENT_MODEL`、`MINI_AGENT_MODE`、
`MINI_AGENT_EFFORT`、`MINI_AGENT_MAX_STEPS`。

### 测试

```bash
ctest --test-dir build --output-on-failure    # 全部测试
python3 tools/mutate.py                       # 变异测试（较慢）
cmake --build build --target docs             # 检查文档注释

cmake -S . -B build-tsan -DMINI_AGENT_SANITIZE=thread   # 数据竞争检查
cmake --build build-tsan && ctest --test-dir build-tsan
```

## 更多

[BUILD-GUIDE.md](BUILD-GUIDE.md) 按阶段讲怎么搭 ·
[ARCHITECTURE.md](ARCHITECTURE.md) 讲设计 ·
[docs/testing.md](docs/testing.md) 讲测试分层
