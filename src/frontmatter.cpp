// 【Stage 5】frontmatter 解析。契约见 src/frontmatter.hpp。

#include "frontmatter.hpp"

#include <fstream>
#include <iterator>

namespace mini {
namespace {

constexpr std::string_view kSpace = " \t\r";

std::string_view trim(std::string_view s) {
    const auto b = s.find_first_not_of(kSpace);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(kSpace) - b + 1);
}

/// 去掉成对的首尾引号。YAML 里 `description: "带: 冒号的值"` 很常见。
std::string_view unquote(std::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        return s.substr(1, s.size() - 2);
    return s;
}

/// 取下一行（不含换行符），并把 rest 推进到下一行开头。
std::string_view next_line(std::string_view& rest) {
    const auto nl = rest.find('\n');
    if (nl == std::string_view::npos) {
        const auto line = rest;
        rest = {};
        return line;
    }
    const auto line = rest.substr(0, nl);
    rest.remove_prefix(nl + 1);
    return line;
}

}  // namespace

std::optional<Frontmatter> parse_frontmatter(std::string_view text) {
    std::string_view rest = text;
    if (trim(next_line(rest)) != "---") return std::nullopt;   // 必须以 --- 开头

    Frontmatter fm;
    bool closed = false;
    while (!rest.empty()) {
        const auto raw = next_line(rest);
        if (trim(raw) == "---") { closed = true; break; }

        // ⚠️ 按第一个冒号切，不是最后一个。值里可能还有冒号
        //    （`description: 用法: 见下`），按最后一个切会把键切坏。
        const auto colon = raw.find(':');
        if (colon == std::string_view::npos) continue;   // 不是键值对，跳过

        // 缩进被 trim 掉 —— `  type: feedback` 存成 "type"，
        // 于是 metadata: 那层嵌套不用特殊处理。
        const auto key = trim(raw.substr(0, colon));
        const auto value = unquote(trim(raw.substr(colon + 1)));
        if (key.empty()) continue;
        if (value.empty()) continue;   // `metadata:` 这种容器行没有值，跳过
        fm.fields.emplace(std::string(key), std::string(value));
    }
    if (!closed) return std::nullopt;   // 没有结束的 ---，整个文件当作没有 frontmatter

    // 正文前的空行去掉，否则每份 render() 都以两个换行开头
    while (!rest.empty() && (rest.front() == '\n' || rest.front() == '\r'))
        rest.remove_prefix(1);
    fm.body = std::string(rest);
    return fm;
}

std::optional<Frontmatter> parse_frontmatter_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    return parse_frontmatter(text);
}

}  // namespace mini
