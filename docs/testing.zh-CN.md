# 测试

*[English](testing.md)*

三层，每层回答一个不同的问题。

| 层 | 在哪 | 回答什么 | 怎么跑 |
|---|---|---|---|
| 单元测试 | `tests/test_*.cpp` | 代码对不对？ | `ctest` |
| 文档示例 | 头文件的 `@code{.test}` | 文档还准不准？ | 同上（生成 `test_docs`） |
| 文档构建 | `Doxyfile` | 注释本身写错了吗？ | `cmake --build build --target docs` |
| 变异测试 | `tools/mutate.py` | **测试本身有没有用？** | `python3 tools/mutate.py` |

第三层是这个项目里最容易被跳过、也最值得做的一层。

---

## 一、单元测试

`tests/` 下每个 `test_*.cpp` 编成一个独立可执行文件，`CMakeLists.txt` 的 GLOB
自动收，`add_test` 自动登记。加一个新文件不用改任何配置。

框架是 `tests/microtest.hpp`，50 行，一眼读得完。`TEST(name)` 宏展开成一个静态
`Registrar` 对象，构造函数在 `main()` 之前把用例塞进注册表。

```cpp
TEST(refuses_to_overwrite_a_file_never_read) {
    CHECK_MSG(r.is_error, "没读过就整个覆盖 = 凭想象删掉别人的代码");
}
```

`CHECK_MSG` 的第二个参数写**为什么这条重要**，不是复述断言。测试红了的时候，
读的人需要知道的是「违反了什么约定」，不是「a 不等于 b」。

**`tests/test_smoke.cpp` 是规格文件**，描述整个系统该有的行为，不要改它去迁就实现。

---

## 二、文档示例

头文件里的 `@code{.test}` 块会被 `tools/gen_doc_tests.py` 抽成真正的断言：

```cpp
/// @code{.test}
/// @setup const auto wd = doc_workdir();
/// run_shell("echo hello", wd, 5s).output    ==> "hello\n"
/// run_shell("exit 42", wd, 5s).exit_code    ==> 42
/// @endcode
```

变成：

```cpp
CHECK_MSG((run_shell("echo hello", wd, 5s).output) == ("hello\n"),
          "include/mini_agent/process.hpp:89 的示例失效了");
```

报错指的是**头文件的行号**，所以红了之后去改的是文档本身（或者文档描述错了的那段代码），
生成文件 `tests/test_docs.cpp` 从头到尾不用打开。

### 规矩

- `@setup` 开头的行原样输出成语句；含 `==>` 的行变成断言；**保持源码顺序**
  （有些示例的意义就在于两次断言之间发生了变化）
- 普通 `@code`（不带 `{.test}`）只当文档渲染，不编译
- 脚手架超过一两行就搬进 `tests/doc_prelude.hpp`（`DocTool` / `doc_config()` /
  `DocTools`），示例应该读起来像文档，不像测试
- CMake 在任何头文件变动时自动重新生成。生成物签入仓库，所以没装 Python 也能构建

### 它不是为了增加覆盖率

`test_docs` 里的断言几乎都被手写测试覆盖过了。它存在的唯一理由是：

> 半年后有人改了行为、改了测试，头文件里的注释和示例**没人会主动去更新**。

让机器盯着，是唯一可靠的办法。

---

## 三、文档构建

```bash
cmake --build build --target docs      # 输出在 docs/api/html/index.html
```

它检查的不是「文档准不准」（那是上一层的事），而是**注释本身有没有写错**：

| 错误 | 报什么 |
|---|---|
| `@param` 拼错参数名 | `argument 'workdir' of command @param is not found in the argument list of run_shell(...)` |
| 漏了某个参数的 `@param` | `The following parameter ... is not documented: parameter 'timeout'` |
| `@ref` 指向不存在的符号 | `unable to resolve reference to 'NoSuchSymbol'` |

`Doxyfile` 里 `WARN_AS_ERROR = FAIL_ON_WARNINGS`，报了就是失败。没装 doxygen
的话 target 不存在，构建和测试照跑 —— 它不是构建依赖。

`INPUT` 只有 `include`，不含 README 和这份文档。doxygen 的 markdown 解析器和
GitHub 的规则不一样（`#pragma` 会被当成符号引用，跨语言链接会被当成 `\ref`），
为了让它不报错去改 markdown 的写法，等于让 GitHub 上的渲染迁就一个没人从那儿
读文档的地方。这个 target 的价值在校验头文件注释，不在生成网站。

### 每个头文件都要有 `@file`

⚠️ **自由函数（不在类里的）只有在文件本身被文档化时才会被 doxygen 提取。**

加这份配置的时候才发现，21 个头文件一个都没有 `@file`，于是
`run_shell`、`make_*_tool`、`load_config`、`builtin_tools`、`kDangerous`
这些的文档**一条都没进生成的页面** —— 写了几百行，全在文档里不存在，
`@param` 校验也就完全没跑到它们身上。

所以每个头文件现在都以这三行开头：

```cpp
#pragma once
/// @file
/// @brief Running a subprocess with a timeout — the most systems-level file here (Stage 2).
```

### 文档覆盖率现在是强制的

`WARN_IF_UNDOCUMENTED = YES`：`include/` 下每一个公开符号都必须有文档，
新加一个没写的，`--target docs` 当场失败。

这个开关一开始是 NO —— 那时有 264 条「还没写文档」会把 2 条真错误淹掉，
信噪比 132:1，没人会看。补完之后才打开，从此**不可回退**。

具体要求比想象的严：

| | 够不够 |
|---|---|
| 只写 `@brief` | ✗ 有返回值就要 `@return` |
| `@param` 少一个 | ✗ 每个参数都要 |
| 结构体成员没注释 | ✗ 用 `///<` 写在行尾 |

补完这 315 处的过程本身也验证了一件事：**很多契约我以为写清楚了，其实只写了
一半。** 比如 `Config` 的 21 个字段，之前只有分组注释（`// --- 模型 ---`），
没有一个字段说清「改了它会怎样」。

## 四、变异测试

### 问题

一条断言可以完全正确、却什么都证明不了 —— 只要它选的输入让**正确实现和错误实现
给出相同答案**。

```cpp
// workdir = /work，判断路径在不在里面
CHECK(sb.resolve_path("src/a.py").second.allowed());          // 两种实现都放行
CHECK(!sb.resolve_path("../../../etc/passwd").second.allowed());  // 两种实现都拒绝
```

把「按路径分量比较」换成「按字符串比前缀」（一个真实的安全漏洞），上面两条照样全绿。
要抓到它，需要一个两种实现**答案不同**的输入：

```cpp
CHECK(!sb.resolve_path("/work-other/x.py").second.allowed());
//     字符串前缀: 放行 ✗    路径分量: 拒绝 ✓
```

这个叫**反例**。没有反例的断言是装饰品。

### 这个项目里抓到过的七个假测试

| 假在哪 | 怎么修的 |
|---|---|
| `compactions == 0` | 那个值本来就是 0，怎么写都过 |
| 字节稳定性 | 把实现换成 `std::rand()` 也照样通过 |
| `resolve_path` 越界 | 四个输入两种实现答案一致 → 补 `/work-other/x.py` |
| `yes` 永不结束 | 正确实现也会超时 → 换成 `yes \| head -100000`（会自己退出） |
| `dangerous_command_denied` | ReadOnly 模式先拒绝了，危险层从没被问到 |
| `bash->subject()` | 只有一个字符串参数，默认实现碰巧也对 → 加一个字母序更靠前的参数 |
| glob 按时间排序 | 文件名叫 `old.cpp` / `new.cpp`，字母序也把 new 排前面 → 换成 `a_newest` / `z_oldest` |

共同点都是一句话：**输入没能让两种实现分开。** 读代码一个都看不出来，因为断言
本身都写得对。

### 怎么跑

```bash
python3 tools/mutate.py            # 全部
python3 tools/mutate.py process    # 只跑 process 相关的
python3 tools/mutate.py --check    # 只校验变异点还对得上代码，不编译
```

### 它真的会改你的源文件

不是拷贝一份改，是**直接写进 `src/`**，编译、跑测试、再写回去：

```python
backup = path.read_text()
try:
    apply(m)                    # 磁盘上真的变了
    build(); run_tests()
finally:
    path.write_text(backup)     # 还原
```

`finally` 挡得住异常、编译失败、测试卡死。挡不住 `kill -9` 和断电 —— 那种情况下
文件里会留着变异版本，得靠 `git checkout src/` 救。所以脚本在跑之前会检查这些
文件有没有未提交改动，有就拒绝跑（`--dirty-ok` 可以强行跑，但那意味着救命的
`git checkout` 会连你自己的改动一起冲掉）。

跑完还会**重新确认一遍基线**，同时抓「没还原干净」和「环境被污染」。

### 三种结果

| | 含义 |
|---|---|
| `✓` | 说好的那条用例红了 —— 测试有效 |
| `○` | 已知未覆盖（`known_gap`），附了原因 |
| `✗` | **该抓没抓到** —— 那条测试是假的，要补反例 |

只有 `✗` 算失败。`known_gap` 是给那些确实很难测的代码留的位置 —— 把没覆盖的地方
记下来，比从清单里删掉假装不存在有用。哪天有人补上了测试，工具会提示
「known_gap 可以删掉了」。

现在清单里有一条 `known_gap`：`drain()` 的硬出口。它防的是一场竞态（deadline
过了但管道还有数据时 `poll` 照样报可读），而 `yes` 是断续写的 —— 读空和补上之间
那个瞬间 `poll` 会返回 0，循环照样退出。要稳定触发得构造一个「一刻不空」的
生产者，不现实。

### 加一条变异

往 `MUTANTS` 里加一项：

```python
dict(
    name="删掉 WriteTool::subject() 的 override",
    file="src/tools/builtin.cpp",
    edits=[(正确的代码, 错误的代码)],
    binaries=["test_file_tools", "test_docs"],
    expect=["subject_is_the_path", "builtin_hpp"],   # 期望红掉的用例名子串
    note="content < path，默认实现把要写入的正文当成审查对象 → 权限静默失效",
),
```

两个要点：

**`edits` 的 old 必须在文件中唯一出现。** `--check` 会验证 —— 代码重构之后变异点
对不上了，这一步会当场报出来，而不是悄悄跳过。

**`expect` 必须点名具体用例，不能只要求「有东西红了」。** 变异可能被一条无关的
用例误伤，而真正该守着它的那条依然是假的。

### 挖坑要挖对形状

变异本身也会写错。这个项目里踩过两次：

- 把「截断后不读」插在 `read()` **之后** —— 等于只是跳过 append，行为和正确代码
  一模一样，自然抓不到
- 用 `return false` 表示「不读了」—— 触发了超时的 kill 分支，反而"救"了这个 bug

变异必须**真的是你担心的那个 bug**，不是一个长得像它的东西。

### 副产品

变异测试在这个项目里不只发现了假测试，还直接找出过两个真 bug：

- `drain()` 的循环没有硬的终止保证 —— deadline 过了但管道还有数据时 `poll` 照样
  报可读，能结束全靠「每轮都真的消费掉数据」这个隐含前提
- 一条测试断言依赖全局路径 `/tmp/escaped.txt`，变异跑真的把那个文件写出来之后，
  **正确代码也永远过不了那条**

---

## 全部跑一遍

```bash
cmake --build build -j4 && ctest --test-dir build --output-on-failure
cmake --build build --target docs
python3 tools/mutate.py
```

编译必须零 warning（`-Wall -Wextra -Wpedantic`）。
