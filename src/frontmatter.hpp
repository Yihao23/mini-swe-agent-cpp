#pragma once
//
// 【Stage 5】frontmatter 解析 —— memory 和 skills 共用。
//
// 这是**内部头文件**（在 src/ 下，不在 include/），所以不算公开 API：
// 外面看不见它，Doxyfile 也不扫它。放在这里而不是复制两份，是因为两个格式
// 完全一样，抄一份出去之后两边会各自漂移 —— 而漂移的表现会是「skill 加载
// 得了、memory 加载不了」，两处代码看起来都对，没人能一眼看出为什么。
//
// C++ 没有标准 YAML 库，为四个字段引一个会破坏这个项目"只有两个依赖"的规矩。
// 手写：找 `---\n ... \n---\n`，中间按第一个 `: ` 逐行切。
//
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mini {

/// 一份 Markdown 文件的两部分。
struct Frontmatter {
    /// 键值对。less<> 让 string_view 也能直接查，不用先构造 string。
    std::map<std::string, std::string, std::less<>> fields;
    std::string body;   ///< `---` 之后的正文，已去掉开头的空行。
};

/// 解析 frontmatter。
///
/// 格式：
///     ---
///     name: git-commit-style
///     description: 用户对提交信息格式的要求
///     metadata:
///       type: feedback
///     ---
///
///     正文……
///
/// ⚠️ 缩进行的键会被 trim 成顶层键 —— `  type: feedback` 存成 "type"。
///    这样 `metadata:` 那层嵌套不用特殊处理，代价是两层里的同名键会撞。
///    对这四个字段来说不会发生，真发生了也是文件写错了。
///
/// 没有 frontmatter（不以 `---` 开头，或找不到结束的 `---`）返回 nullopt。
/// 丢弃而不是猜：一个没有 name/description 的文件进了索引，模型会看到一条
/// 空描述，然后永远不会去加载它 —— 静默失效。
std::optional<Frontmatter> parse_frontmatter(std::string_view text);

/// 读文件再解析。文件不存在或解析失败都返回 nullopt。
std::optional<Frontmatter> parse_frontmatter_file(const std::string& path);

}  // namespace mini
