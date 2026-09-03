# 用 C++ 建造一个 SWE Agent —— 分阶段指南

你写代码。这份文档给结构、给设计依据、给坑。

## 三条使用纪律

1. **测试是规格，不是作业。** 动手前先读 `tests/test_smoke.cpp` 里对应的用例 ——
   它把这一层的契约写死了（函数叫什么、返回什么、什么情况必须报错）。
2. **头文件顶部的注释是设计说明。** 每个 `.hpp` 开头都讲了"为什么这么设计"，
   那是这份指南的一部分，不是装饰。
3. **答案先别看。** 卡住先自己想 15 分钟。Python 参考实现在 `../mini-swe-agent/reference/`，
   它给的是**行为**答案；C++ 的结构问题它帮不上忙 —— 那部分看本文的"C++ 岔路口"。

```bash
cmake --build build && ./build/test_smoke     # 你的循环就是这一行
```

---

## 动手之前：三个心智模型

### 1. Agent 就是一个 while 循环

```
while (true) {
    问模型（历史 + 工具表）
    模型要调工具吗？
        要 → 执行，结果追加进历史，继续
        不要 → 这就是最终答案，返回
}
```

**这十行是不变的。** 你后面写的所有代码 —— 权限、并发、压缩、子 agent、MCP ——
都是围绕这十行的**流程控制**和**能力供给**。写复杂了就回来看这十行，问自己
"我现在写的东西是在服务谁"。

### 2. 一切能力都长在同一个接口上

```
工具 = JSON Schema（描述给模型看）+ run() 函数（你来执行）
```

文件操作、bash、子 agent、MCP 远程服务 —— 全是这一个形状。
`Tool` 抽象设计对一次，后面五个阶段都在复用；设计错了，五个阶段都别扭。

### 3. 上下文是唯一的稀缺资源

模型没有记忆，每一轮你都要把**整个历史**重新发过去。三个后果解释了这个项目里一半的设计：

- **要省** → 上下文压缩（Stage 4）、子 agent 隔离（Stage 6）、渐进式披露（Stage 5）
- **要稳** → system prompt 必须逐字节稳定，否则 prompt 缓存全废（Stage 4）
- **要准** → 塞进去的每一句都会影响行为，包括你以为"只是提示"的那句

---

## 阶段总览

| 阶段 | 主要文件 | C++ 特有的难点 | 结束时你拥有 |
|---|---|---|---|
| 0 | `config` `message` | variant 建模、成员初始化顺序 | 一套贯穿全局的类型 |
| 1 | `llm` `parser` `session` `loop` | SSE 流式解析、`std::expected` | **一个真能干活的 agent** |
| 2 | `tool` `executor` `process` `tools/builtin` | 进程管理、`launch::async`、所有权 | 完整工具层 + 并发执行 |
| 3 | `sandbox` | 路径规范化、glob 匹配 | 权限闸门，敢在真项目上跑 |
| 4 | `prompt` + `session::compact` | 字符串拼接的确定性 | 缓存友好 + 长任务不爆 |
| 5 | `memory` `skills` | 手写 frontmatter 解析 | 跨会话记忆 + 技能插件 |
| 6 | `subagent` `scheduler` `background` | 线程池、`jthread`、无 `wait_any` | 多 agent + 任务图 + 后台任务 |
| 7 | `mcp` `app` `cli` | 双向管道、成员顺序、pimpl | 接外部工具 + 能用的界面 |

**Stage 1 结束就已经是一个可用的 agent 了。** 后面每阶段都是独立可停的增量。

---

# Stage 0 — 契约

**目标**：把贯穿全项目的类型定下来。这一步对了，后面七个阶段都顺。

**交付**：`src/config.cpp`、`src/message.cpp`
**验收**：`Config` 能构造、`to_json(Message)` 往返正确

## C++ 的第一个岔路口：variant 还是虚函数？

判据很简单：

| | 用什么 | 为什么 |
|---|---|---|
| **封闭集合**（由外部定义，你不会扩展） | `std::variant` | 加一种时所有 `std::visit` 编译报错，逼你处理；值语义，无堆分配 |
| **开放集合**（用户随时会加） | 虚函数 | 你不可能穷举，也不该穷举 |

本项目里：
- `ContentBlock`（API 定的 5 种）→ variant
- `AgentEvent`（UI 要处理的 5 种）→ variant
- `Tool`（用户随时加自己的）→ 虚函数
- `LlmClient`（真的 + 假的）→ 虚函数（见 Stage 1）

写 `to_json` 时用 overloaded 惯用法，代码会很干净：

```cpp
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };

std::visit(overloaded{
    [](const TextBlock& b)    { return Json{{"type","text"},{"text",b.text}}; },
    [](const ToolUseBlock& b) { return Json{{"type","tool_use"},{"id",b.id},...}; },
    // 漏一个分支 → 编译报错，这正是你要的
}, block);
```

## 三个当场就要做对的决定

**① `ToolResult::is_error` 是布尔值，不是异常。**
工具失败必须变成"失败了，原因是 X"的结果**喂回模型**让它自己纠错。
抛异常会打断整个循环 —— 一个文件读不到就让整个任务崩掉，不合理。

**② 权限是三态（`Action::Allow/Deny/Ask`），不是 `bool`。**
`Ask` 最重要：它意味着"把决定权交还给人"。设计成 bool，后面加交互确认整层重写。

**③ `signature` 必须原样带回。**
`ThinkingBlock` 里那个 signature 字段不是装饰 —— API 会校验，改了就报错。

## 坑

- **`weakly_canonical` vs `canonical`**：`canonical` 要求路径存在，写文件时目标可能还不存在。用前者。
- **`fs::path` 的比较不能用字符串**：`/work` 和 `/workspace` 的字符串前缀是匹配的。按路径分量比。
- **`std::map<std::string, T>` 想用 `string_view` 查**：要写成 `std::map<std::string, T, std::less<>>`，否则每次查找都构造一个临时 string。

---

# Stage 1 — 最小闭环

**目标**：跑通 `问模型 → 调工具 → 回传结果 → 得到答案`。

**交付**：`src/llm.cpp`（先只写 `FakeLlm`）、`src/parser.cpp`、`src/session.cpp`、`src/loop.cpp`
**验收**：`loop_runs_tool_then_answers`、`all_tool_results_in_one_message`、`max_steps_guard`、`tool_error_becomes_result_not_crash`

## 实现顺序（很重要）

```
1. FakeLlm                ← 最先写！后面全靠它测，没有它你每改一行都要花钱
2. parser::parse
3. Session::append/save
4. Agent::run
5. ...（能跑通测试了，再回头写真的 AnthropicClient）
```

**`FakeLlm` 是整个项目 ROI 最高的 80 行。** 它让 15 个测试全部离线跑，不到一秒跑完。
先写它，别急着调通网络。

## 关键机制：tool_use 协议

一次带工具的往返（这是 API 的硬要求，不是你的选择）：

```
[user]      "看看 hello.py"
[assistant] tool_use(id="toolu_1", name="read", input={...})
[user]      tool_result(tool_use_id="toolu_1", content="...")
[assistant] text("这是一个打招呼函数")
```

三条不能违反的规则：

1. **每个 `tool_use` 必须有配对的 `tool_result`**，id 要对上。少一个，下一轮请求直接 400。
2. **一轮里的所有 `tool_result` 放进同一条 user 消息**。拆开会让模型逐渐学会不再并行调工具。
3. **assistant 那轮要原样回传**，包括 thinking 块和 signature。

## C++ 的第二个岔路口：`std::expected` 还是异常？

```cpp
std::expected<LlmResponse, LlmError> complete(const LlmRequest&);
```

判据：**调用方有没有可能"处理"这个失败？**

- 429 限流、529 过载、网络抖动 → 调用方要重试/降级 → **`expected`**
- 内存耗尽、逻辑断言失败 → 没法处理 → 异常（或直接 terminate）

这和 agent 的整体错误哲学是一致的：能被上游"理解并应对"的失败，都应该是值。

## SSE 流式解析（写真客户端时）

libcurl 的写回调是 C 函数指针，标准做法是把 `this` 通过 userdata 传进去：

```cpp
static size_t on_write(char* p, size_t sz, size_t nm, void* self) {
    return static_cast<Impl*>(self)->consume(std::string_view{p, sz * nm});
}
curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &on_write);
curl_easy_setopt(h, CURLOPT_WRITEDATA, impl_.get());
```

**必踩的坑：一次回调不保证是完整一行。** 必须自己维护缓冲区，按 `'\n'` 切，
再看 `data: ` 前缀。直接把每次回调的内容当一行 JSON 解析，一定会在长响应上炸。

## 坑

- **`max_tokens` 是"思考 + 回答"的总额**。开了 adaptive thinking 后模型可能思考占掉大半，回答被截断。给足（非流式 16k，流式可到 64k）。
- **`Session` 存 `std::vector<Message>`，返回引用要小心**。`messages()` 返回非 const 引用很方便，但任何人都能改历史。想清楚要不要收窄。
- **信号处理**：Ctrl-C 的 handler 里只能改 `volatile std::sig_atomic_t`（不能 new、不能加锁、不能打日志）。循环在安全点检查这个标志位。

---

# Stage 2 — 工具层与执行器

**目标**：从"一个工具"扩到"一套工具"，并让只读工具并发跑。

**交付**：`src/tool.cpp`、`src/process.cpp`、`src/executor.cpp`、`src/tools/builtin.cpp`
**验收**：`edit_requires_read_first`、`edit_after_read_succeeds`、`tool_schemas_are_sorted`

## 为什么不是所有事都用 bash？

bash 宽度最大，但你的程序从它那儿拿到的**只有一个命令字符串**，对每次调用都长一样。
提升成独立工具，你就拿到了**带类型的参数**：

| 能做的事 | 有独立工具 | 只有 bash |
|---|---|---|
| 精确授权 | `Write(src/**)` 能匹配 | `bash -c "echo x > src/a"` 匹配不了 |
| 强制不变量 | `edit` 要求"先 read 且文件没变过" | `sed` 做不到 |
| 并发调度 | `read` 标记只读 → 敢并行 | 分不清 `grep` 和 `git push` |

**经验法则：先用 bash 铺宽度，需要 gate / 审计 / 并行 / 渲染时再提升成独立工具。**

## C++ 的第三个岔路口：谁拥有 Tool？

`ToolRegistry` 存 `shared_ptr<Tool>` 而不是 `unique_ptr`。**这不是偷懒**：
Stage 6 的子 agent 要拿主注册表的**子集**（explorer 只给只读工具），
两个注册表指向同一批实例 —— 这是真正的共享所有权。

`ToolContext` 则相反：里面全是**非拥有裸指针**，它是一个"借来的引用包"，
生命周期由 `App` 保证。这是 C++ 里很常见的一对搭配：
**拥有用智能指针，借用用裸指针/引用，两者在类型上就分得清。**

## `run_shell`：这个项目最"系统编程"的一块

Python 一行 `subprocess.run(timeout=)` 的东西，C++ 要你自己写。**值得亲手写一遍。**

不能用 `popen()`：拿不到 pid（超时没法 kill）、只能单向、退出码难拿。

正确做法：

```
pipe() → fork() → 子进程: dup2 stdout/stderr 到管道写端, setpgid(0,0), execl("/bin/sh","sh","-c",cmd)
       → 父进程: close 写端, poll() 读端 + 算剩余超时
       → 超时: kill(-pgid, SIGTERM) → 等一下 → SIGKILL
       → waitpid() 收尸, WIFEXITED/WEXITSTATUS
```

**三个必踩的坑**：

1. **不 close 父进程里的写端** → `read` 永远等不到 EOF，超时机制形同虚设
2. **不 `setpgid`** → 杀不掉整棵进程树（`sleep 100 &` 起的孙子进程会活下来）
3. **fork 之后、exec 之前只能调 async-signal-safe 的函数**（别在那儿 new、别打日志）

## C++ 的第四个岔路口：`std::async` 的默认策略

```cpp
auto f = std::async(std::launch::async, [&]{ return run_one(call); });
//                  ^^^^^^^^^^^^^^^^^ 不写这个就完了
```

默认策略是 `async | deferred`，允许实现选择"延迟到 `.get()` 时才在当前线程跑"。
那样你以为在并发，其实是顺序执行 —— **静默失去并行，没有任何报错**。
永远显式写 `std::launch::async`。

另外：先收集所有 future 再统一 `get()`，别边跑边 `push_back` —— 否则结果顺序会乱，
而 `tool_result` 的顺序必须和 `tool_use` 一致。

## 坑

- **路径必须解析后再用**。模型给的 `path` 是不可信输入，`../../../etc/passwd` 会来的。这一步别等到 Stage 3。
- **并发时的共享状态**：多个工具同时写 `session.read_files()` 会不会打架？（会。想想怎么办。）
- **工具表按名字排序**：现在就做，原因 Stage 4 揭晓。
- **`std::regex` 很慢**，但 grep 工具用它够了。真嫌慢再换 RE2 —— 别提前优化。

---

# Stage 3 — 沙箱

**目标**：敢把这个 agent 指向你的真实项目。

**交付**：`src/sandbox.cpp`
**验收**：`path_escape_blocked`、`dangerous_command_denied`、`command_is_split_before_checking`、`sandbox_rules_three_states`、`readonly_is_not_permissionless`

## 三道关卡，从粗到细

```
① 模式（全局基线）   readonly / ask / auto / yolo
② 规则（用户配置）   allow: ["Bash(git status:*)"]   deny: ["Bash(git push:*)"]
③ 硬检查（永不放行） 路径越界、rm -rf、sudo、curl|sh、git push --force
```

## 一条铁律：工具不做权限判断

工具只负责"怎么做"，`Executor` 在调用**前**统一问 `Sandbox::authorize()`。

为什么：权限判断散在每个工具里，你永远不知道覆盖全不全，加一个工具就多一个漏洞。
收敛成一层之后，"这个 agent 能干什么"有唯一的答案来源。

## 最容易漏的一点

```bash
ls && rm -rf /      # 整条匹配"以 ls 开头" → 放行 → 完蛋
```

按 `&&` `||` `;` `|` 拆开，**逐段**审。这一条不做，前面所有权限设计都是装饰。

## `readonly` ≠ 免授权

这是最容易设计错的语义，有一条测试专门锁它：

- `read_only()` = 不改本地文件 → 可以并发
- `requires_permission()` = 有副作用 → 要过闸

一个抓 URL 的工具两者**都是 true**。如果你在 `check()` 里写了
`if (read_only) return Allow;`，这个工具就永远不会被拦住。

## 你要自己决定的

- **规则语法**。本项目用 `Tool(pattern)`，glob 匹配，`:*` 表前缀。用户写规则时最常见的需求是什么？
- **"本会话都允许"怎么记**。批准了 `npm test`，下次 `npm test -- --watch` 算不算？太宽=形同虚设，太窄=烦死人。
- **非交互时 `Ask` 怎么办**。CI 里没人能回答 —— 默认拒绝还是允许？（几乎肯定是拒绝，但要想清楚为什么。）

## 坑

- **`fnmatch()`**（POSIX，`<fnmatch.h>`）能直接用；想练手就自己写 20 行递归匹配 `*` 和 `?`。
- **危险命令的正则要小心误伤**：`rm -rf` 拦，但 `git rm --cached` 呢？`npm run format` 里带 `rm` 呢？
- **别把沙箱写成"能拦住恶意攻击者"**。它拦的是**模型的失误**和**你自己的手滑**。真要防恶意代码需要容器或 VM。

---

# Stage 4 — 上下文工程

**目标**：跑得起长任务，同时不浪费钱。最没有存在感、但对成本影响最大的一层。

**交付**：`src/prompt.cpp` + `Session::compact`
**验收**：`compaction_splits_on_user_boundary`，以及你自己补的两个 prompt 稳定性测试

## 一、Prompt 缓存：不是加个参数，是一条纪律

渲染顺序是 `tools → system → messages`，缓存是**前缀逐字节匹配**。

**后果：system prompt 里出现一个时间戳，它后面的整段对话每轮都要重新计费。**

| 内容 | 放哪 |
|---|---|
| 身份、工具用法、工作区路径、skill/memory 索引 | `system`，最后一块打 `cache_breakpoint` |
| 当前时间、后台任务通知、todo 变化 | user 轮里的 `<system-reminder>` 块 |

这解释了一个你可能见过但没想明白的现象：为什么各种 agent 的"提醒"总是以
`<system-reminder>` 出现在用户消息里。

**顺便**：现在你知道为什么 Stage 2 要求工具表按名字排序了 —— 工具渲染在最前面，
顺序一变，后面全废。

**C++ 特有的注意点**：拼字符串时任何"不确定性"都是缓存杀手。
遍历 `std::unordered_map` 输出 skill 索引 → 顺序不保证 → 每次跑都可能不一样。
用 `std::map` 或显式排序。这个 bug 不会有任何报错，只会让你的账单翻倍。

## 二、上下文压缩：长任务的生死线

**唯一的技术难点是切分点**：不能把 `tool_use` 和它的 `tool_result` 拆散
（少一个配对，下一轮直接 400）。

安全的切法：往前找最近一条**真正的用户输入**（`Role::User` 且 `!has_tool_result`），在那里切。

## 你要自己决定的

- **摘要 prompt 怎么写**。留：目标、已做的决定、改过的文件、验证过的事实、没做完的事。丢：寒暄、被否决方案的细节、过时的中间状态。这个 prompt 的质量直接决定长任务能不能接着干。
- **什么时候触发**。按估算 token？按消息条数？（提示：序列化字节数 / 4 就够用。）
- **压缩后 `read_files` 怎么办**？模型不记得读过了，但你的陈旧检查还记得 —— 会导致奇怪的"必须先 read"错误。

---

# Stage 5 — 记忆与技能

**目标**：跨会话变聪明 + 按需加载专项知识。

**交付**：`src/memory.cpp`、`src/skills.cpp`、`tools/builtin.cpp` 里的两个工具
**验收**：你自己补 `memory_roundtrip` 和 `skills_progressive_disclosure`

## 一招吃两处：渐进式披露

memory 和 skills 结构**完全一样**，值得放一起写：

```
常驻上下文：   - code-review: 代码评审清单，改动 PR 前用     ← 一行摘要
模型判断相关： skill(name="code-review")                     ← 主动加载
加载后：       完整手册进上下文                               ← 只在需要时占位置
```

一百个 skill 也只占几百 token 的常驻预算。区别只有：**memory 跨会话存活，且 agent 能自己写入。**

## C++ 特有：手写 frontmatter 解析

没有现成的 YAML 库值得为几个字段引入。手写：找 `---\n...\n---\n`，中间按 `key: value` 逐行切。
**memory 和 skill 用的是同一套逻辑 —— 抽出来复用，别写两遍。**

## 你要自己决定的

- **检索怎么做**。关键词打分够用吗？（提示：上百条之前都够用，别提前上 embedding。）
- **该记什么**。写进工具描述里，否则 agent 会把整个对话都存下来。（该记：用户偏好、代码里看不出来的约束和决策。不该记：代码结构、git 历史里有的。）
- **索引里的描述是给模型看的**，写"什么时候该用它"，不是"它是什么"。

---

# Stage 6 — 规模与时间

**目标**：突破"单线程、一次一件事"。

**交付**：`src/subagent.cpp`、`src/scheduler.cpp`、`src/background.cpp`
**验收**：`scheduler_respects_dependencies`、`scheduler_detects_cycle`，加你自己补的后台/子 agent 测试

## 子 agent：价值是上下文隔离，不是并行

新手直觉：子 agent = 并行 = 快。
**真正的价值**：子 agent 读了两万行代码，主 agent 只收到 300 字结论。
主循环的上下文没被撑爆，缓存前缀也保住了。并行只是附赠。

代价你必须接受：**子 agent 看不到主 agent 的历史**，派活时任务描述必须自包含。
这不是缺陷，是隔离的定义。

实现就三件事：全新的 `Session`（隔离）、收窄的工具集、可能更便宜的模型。

## 调度器：一个不认识 LLM 的调度器

`Scheduler` 只知道 `runner(task, upstream) -> string`。
测试时塞一个 lambda，0.5 秒验证完拓扑序、并发、环检测 —— **不烧一分钱 token**。

**任何时候你能把某一层的 LLM 依赖切掉，都值得这么做。**
这是整个项目里最容易被忽略、但对开发速度影响最大的一条。

**C++ 特有的麻烦**：标准库**没有 `wait_any`**。最简单的做法是轮询
`future.wait_for(0ms) == std::future_status::ready` 配一个很短的 sleep；
想做漂亮就用 `condition_variable` + 完成队列。（`std::when_any` 在 concurrency TS 里，
GCC 13 没有。）

## 后台任务：让事件自己找上门

agent 循环是同步的，但世界不是：
读取线程收输出 → 主循环每轮开头抽取状态变化 → 作为 `<system-reminder>` 注入下一轮。
于是 agent 不需要轮询。

**C++ 并发要点**：
- 用 `std::jthread`：析构自动 join，还自带 stop_token。裸 `std::thread` 在 manager 析构时线程还活着 → `std::terminate`
- buffer 和 cursor 是一对不变量，必须在同一个锁里改
- buffer 要有上界。超了丢最老的一半，**记得同步修正 cursor**，否则会重复输出或跳过内容

## 坑

- **每个后台任务只能通知一次**。发两遍模型会以为跑了两次。
- **嵌套深度限制**：子 agent 能不能再派子 agent？（建议不能 —— 否则一个失控任务能派出指数级的 agent。）
- **并发的子 agent 共享沙箱**，会不会同时改同一个文件？（Python 参考实现也没解决这个，你可以想想。）

---

# Stage 7 — 外部世界与界面

**目标**：接入别人写的工具 + 做一个真能用的终端界面。

**交付**：`src/mcp.cpp`、`src/app.cpp`、`src/cli.cpp`
**验收**：自己写 `tests/test_mcp.cpp`，用 `tests/mock_mcp_server.py` 验证握手

## MCP 其实很简单

去掉所有术语：**JSON-RPC 2.0，一行一条消息，走子进程的 stdin/stdout**。

```
→ initialize                  我是谁、协议版本
← 服务端能力
→ notifications/initialized   通知（无回复）
→ tools/list
→ tools/call
```

接进来后包成普通的 `Tool` —— executor 和 sandbox 完全不知道它是远程的。
**这就是 Stage 2 把 `Tool` 抽象设计对的回报。**

`tests/mock_mcp_server.py` 会**故意在握手中间插一条通知**，测你会不会被带偏：
必须循环读到 id 对上的那条才返回。

**C++ 特有**：双向管道要 `pipe()` 两次 + `fork` + `dup2`。
父进程那两个 fd 用 `fdopen` 包成 `FILE*` 会比裸 `read/write` 好写很多（能用 `getline`）。

## `App` 的成员声明顺序 = 构造顺序

这是 C++ 版最容易炸的地方。`ToolContext` 里全是指向其他成员的指针，
`Agent` 又持有 `ToolContext` 的引用。成员**按声明顺序**构造：

```cpp
struct App::Impl {
    Config cfg;                 // 1. 被所有人引用
    std::unique_ptr<LlmClient> llm;
    Sandbox sandbox;
    BackgroundManager background;
    std::optional<Memory> memory;
    std::optional<SkillRegistry> skills;
    ToolRegistry registry;
    Session session;
    ToolContext ctx;            // 指向上面所有东西
    Agent agent;                // 最后！它引用 ctx
};
```

顺序写错，构造 `Agent` 时拿到的是还没初始化的对象 ——
**这类 bug 只在 release 构建下偶尔炸，非常难查。** 写完回来数一遍。

还有：`App` 里成员互指，所以必须 `delete` 掉拷贝和移动（移动会让 `ToolContext`
里的指针指向旧对象）。

## 界面层

`Renderer` 用 `std::visit` 分派 `AgentEvent` —— 加一种事件时这里会编译报错，
这正是 variant 相对于"事件基类 + `dynamic_cast`"的价值。

**做对了的标志：换成 Web 前端只要换掉 `cli.cpp` 一个文件。**

C++ 没有 argparse，手写一个 30 行的解析循环就够了 —— 这个项目的依赖只该有两个。

---

# C++ 的 8 个岔路口（速查）

| # | 岔路口 | 判据 |
|---|---|---|
| 1 | variant 还是虚函数？ | 封闭集合（API 定的）→ variant；开放集合（用户扩展）→ 虚函数 |
| 2 | `expected` 还是异常？ | 调用方有可能处理吗？能 → `expected`；不能 → 异常 |
| 3 | `unique_ptr` 还是 `shared_ptr`？ | 真的有多个所有者吗？（Tool 有：主表 + 子 agent 子集） |
| 4 | 引用/裸指针 还是智能指针？ | 借用关系用裸的（`ToolContext`），拥有关系用智能的 |
| 5 | 头文件放什么？ | 只放调用方需要的。curl.h / 具体 Tool 类 → pimpl 或工厂函数关进 .cpp |
| 6 | `std::async` 还是线程池？ | 一次性 fan-out 用 async（记得 `launch::async`）；长期存在用 jthread |
| 7 | 成员声明顺序 | 被引用的在前，引用别人的在后。`Agent` 永远最后 |
| 8 | 什么时候该有确定性顺序？ | 任何进 prompt 的东西：`map` 不是 `unordered_map`，排序不是遍历 |

---

# 坑速查表

| 症状 | 原因 |
|---|---|
| API 报 "tool_use ids ... no tool_result" | 有 `tool_use` 没配对结果。中断时尤其容易漏 |
| 模型逐渐不并行调工具了 | 你把多个 `tool_result` 拆进了不同消息 |
| 缓存命中率是 0 | system prompt 里有时间戳，或工具表/skill 索引顺序不稳定 |
| 以为在并发，实际是顺序 | `std::async` 没写 `std::launch::async` |
| 子进程超时了杀不掉 | 没 `setpgid`，杀的是 sh 不是它的孩子 |
| `run_shell` 永远不返回 | 父进程没 close 管道写端，读不到 EOF |
| 压缩之后请求 400 | 切分点把 `tool_use` / `tool_result` 拆散了 |
| 只读工具没过权限闸 | 在 `check()` 里把 `read_only` 当成了 `requires_permission` |
| `ls && rm -rf /` 被放行 | bash 命令没有拆段审查 |
| release 下偶发崩溃 | `App` 成员声明顺序错了，`Agent` 引用了未初始化的成员 |
| `std::terminate` 在析构时 | 用了裸 `std::thread` 没 join；换 `jthread` |
| clangd 报 `std::expected` 不存在 | `.clangd` 没配 `-std=c++23` |

---

# 建议的节奏

- **Stage 0-1 一口气写完**（一到两天）。中间停下你手里没有能跑的东西，容易失去感觉。
- **Stage 1 结束后先用一阵子。** 装上 libcurl，接真 API 干几件真事。你会自己撞上
  "权限好危险"、"上下文爆了"、"这个调查把我上下文塞满了" ——
  **带着痛点去写 Stage 3/4/6，比照着文档写理解深十倍。**
- **每个 Stage 结束跑一次全量测试**，别让回归攒着。
- **补测试**：`test_smoke.cpp` 末尾列了 8 个我没写的用例。写它们本身就是理解设计的过程。

写完之后的下一批练习：Docker 执行后端、分层压缩、embedding 检索、
PreToolUse/PostToolUse 钩子、MCP resources、主循环并发。
