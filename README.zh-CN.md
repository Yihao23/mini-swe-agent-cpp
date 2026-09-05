# mini-swe-agent (C++23)

[English](README.md) · **简体中文**

一个 SWE agent 的**骨架**。头文件、构建系统、测试都搭好了，函数体是你的活。

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

`test_smoke` 一开始全红。每条失败信息直接告诉你下一步该实现哪个函数：

```
✗ loop_runs_tool_then_answers
    TODO — Stage 7: App 构造 —— 把十几个模块接起来
```

## 这是什么

不是"怎么调 LLM API"的教程。真正值得学的是 C++ 逼你做的那些决定 —— 脚本语言全都帮你藏起来了：所有权、生命周期、并发、子进程管理。

整个设计建立在三个心智模型上：

1. **Agent 就是一个 while 循环。** 问模型、执行它要的工具、把结果喂回去、重复到它不再要工具为止。十行代码。其余的一切 —— 权限、并发、上下文压缩、子 agent —— 都是围绕这十行的流程控制。
2. **一切能力都长在同一个接口上。** 工具 = JSON Schema（描述给模型看）+ `run()` 函数（你来执行）。文件操作、bash、子 agent、远程 MCP 服务，全是这一个形状。
3. **上下文是唯一的稀缺资源。** 模型没有记忆，每一轮都要把整个历史重新发过去。这一个事实解释了这个项目里一半的设计：压缩、子 agent 隔离、渐进式披露，以及 system prompt 必须逐字节稳定。

## 仓库结构

```
mini-swe-agent-cpp/
├── BUILD-GUIDE.md          分阶段建造指南（先读这个）
├── ARCHITECTURE.md         带图的设计总览
├── include/mini_agent/     契约。每个头文件顶部讲清楚"为什么这么设计" ——
│                           那是文档的一部分，不是装饰
├── src/                    你的战场。未实现的函数体调用 todo("Stage N: ...")
└── tests/
    ├── microtest.hpp       50 行的测试框架，你能一口气读完
    ├── test_smoke.cpp      15 个用例，就是规格说明书
    ├── test_loop.cpp       一轮对话的形状，不经过 App 手工接线
    ├── test_config.cpp     优先级链与 to_string 往返一致性
    ├── test_tool.cpp       工具注册表与 schema 的不变量
    ├── test_parser.cpp     循环两端的 wire format 契约
    ├── test_session.cpp    持久化 round-trip
    └── mock_mcp_server.py  假 MCP server，Stage 7 验证握手用
```

## 阶段划分

每个阶段都是独立可停的增量，停在任何一个阶段都能得到能用的东西。

| 阶段 | 主要文件 | 结束时你拥有 |
|---|---|---|
| 0 | `config` `message` | 一套贯穿全局的类型 |
| 1 | `llm` `parser` `session` `loop` | **一个真能干活的 agent** |
| 2 | `tool` `executor` `process` | 完整工具层 + 并发执行 |
| 3 | `sandbox` | 权限闸门，敢在真项目上跑 |
| 4 | `prompt` `session::compact` | 缓存友好 + 长任务不爆上下文 |
| 5 | `memory` `skills` | 跨会话记忆 + 技能插件 |
| 6 | `subagent` `scheduler` `background` | 多 agent + 任务图 + 后台任务 |
| 7 | `mcp` `app` `cli` | 接外部工具 + 能用的界面 |

## 当前进度

```
Stage 0  ████████████████████  完成
Stage 1  ██████████████████░░  循环端到端跑通；AnthropicClient 待写
Stage 2  ████████████████░░░░  executor、工具注册表、read 和 edit
Stage 7  ████████░░░░░░░░░░░░  App 装配，够跑起来了
Stage 3  ████████████████████  完成 —— 闸门已接进 executor
Stage 4+ ░░░░░░░░░░░░░░░░░░░░
```

| 测试 | 结果 |
|---|---|
| `test_loop` | 16/16 |
| `test_config` | 19/19 |
| `test_tool` | 17/17 |
| `test_parser` | 14/14 |
| `test_session` | 12/12 |
| `test_smoke` | 12/15 —— 其余需要上下文压缩、调度器 |

`-Wall -Wextra -Wpedantic` 下零警告。

## 依赖只有两个

| 依赖 | 怎么来 | 用在哪 |
|---|---|---|
| nlohmann/json | CMake `FetchContent` 自动拉 | 只在 `json.hpp` 里 typedef 一次 |
| libcurl | `sudo apt install libcurl4-openssl-dev` | 只有 `src/llm.cpp` 用 |

**没装 libcurl 也能做完 Stage 0–6** —— 所有测试走 `FakeLlm`，不碰网络。CMake 检测不到 libcurl 时会打一条警告然后正常继续。

## 环境

已验证：g++ 13.3 / CMake 3.28 / Ninja 1.11 / Ubuntu 24.04。

用到三个 C++23 特性，各有明确理由：

| 特性 | 用在哪 | 为什么 |
|---|---|---|
| `std::expected` | `LlmClient::complete` | 失败是值不是异常 —— 429/529 要能重试 |
| `std::jthread` | 后台任务的输出泵 | 析构自动 join，还自带 stop_token |
| `std::format` | prompt 拼接、终端渲染 | 省掉一堆 ostringstream |

clangd 的配置在 `.clangd`（不加它会误报 `std::expected` 不存在）。

## 测试

```bash
cmake --build build && ctest --test-dir build --output-on-failure
./build/test_tool                       # 开发时看每个用例的逐条输出
```

新加测试只要往 `tests/` 里放一个 `test_*.cpp` —— CMake 会 glob 成独立可执行文件并注册进 ctest，不用改构建脚本。

绿灯只说明代码和测试的假设一致，不说明测试真的在看。想确认一条断言有效，就把它覆盖的东西改坏，看它是不是变红：

```bash
sed -i 's/a->name() == v/a->description() == v/' src/tool.cpp
cmake --build build --target test_tool && ./build/test_tool   # 应该变红
```

## 和 Python 参考实现的关系

`../mini-swe-agent/reference/` 是同一个设计的 Python 完整实现，3100 行、21 个测试全绿。用它来**对照行为**，不是用来抄结构 —— C++ 版有一整套 Python 里不存在的问题，那部分 `BUILD-GUIDE.md` 里单独讲。
