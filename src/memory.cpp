// 【Stage 5】长期记忆。契约写在 include/mini_agent/memory.hpp。

#include "mini_agent/memory.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

#include "frontmatter.hpp"

namespace mini {
namespace {

/// 类型名和枚举的双向映射。写在一个表里，两个方向不会漂。
constexpr struct { MemoryType type; std::string_view name; } kTypes[] = {
    {MemoryType::User, "user"},
    {MemoryType::Feedback, "feedback"},
    {MemoryType::Project, "project"},
    {MemoryType::Reference, "reference"},
};

std::string_view type_name(MemoryType t) {
    for (const auto& e : kTypes)
        if (e.type == t) return e.name;
    return "reference";
}

MemoryType type_from(std::string_view s) {
    for (const auto& e : kTypes)
        if (e.name == s) return e.type;
    return MemoryType::Reference;   // 未知类型退回最弱的那个，不是报错
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// 把查询切成词。分隔符取得很宽：模型给的 query 常常是一整句话。
std::vector<std::string> tokenize(std::string_view query) {
    std::vector<std::string> words;
    std::string cur;
    for (const unsigned char c : query) {
        // ⚠️ 只按 ASCII 标点切。中文没有词边界，整段会成为一个"词"，
        //    而子串匹配对中文照样有效 —— 所以不需要分词器。
        if (std::isspace(c) || std::ispunct(c)) {
            if (cur.size() >= 2) words.push_back(lower(cur));
            cur.clear();
        } else {
            cur += static_cast<char>(c);
        }
    }
    if (cur.size() >= 2) words.push_back(lower(cur));
    return words;
}

/// 一个词在文本里出现算几分。
int count_hits(const std::string& haystack_lower, const std::string& word) {
    int n = 0;
    for (std::size_t at = haystack_lower.find(word); at != std::string::npos;
         at = haystack_lower.find(word, at + word.size()))
        ++n;
    return n;
}

}  // namespace

std::string MemoryItem::render() const {
    // 类型要带上：模型看到 [feedback] 才知道这条是"该怎么做事"，
    // 而不是一条可以忽略的参考资料。
    return std::format("## {} [{}]\n{}\n\n{}", name, type_name(type), description, body);
}

Memory::Memory(fs::path root) : root_(std::move(root)) {
    // 目录不存在就建 —— 第一次 write 才发现建不了，那时错误信息离原因很远。
    std::error_code ec;
    fs::create_directories(root_, ec);
}

void Memory::rebuild_index() const {
    index_.clear();
    loaded_ = true;
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) return;

    for (const auto& e : fs::directory_iterator(root_, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".md") continue;
        if (auto item = parse_memory_file(e.path())) index_.push_back(std::move(*item));
    }
    // 顺序稳定：index_text() 进 system prompt，缓存逐字节匹配前缀。
    // 目录遍历顺序不保证，不排的话每次启动 system 的字节都可能不同。
    std::ranges::sort(index_, {}, &MemoryItem::name);
}

std::vector<MemoryItem> Memory::items() const {
    if (!loaded_) rebuild_index();
    return index_;
}

std::optional<MemoryItem> Memory::get(std::string_view name) const {
    if (!loaded_) rebuild_index();
    const auto it = std::ranges::find(index_, name, &MemoryItem::name);
    if (it == index_.end()) return std::nullopt;
    return *it;
}

std::vector<MemoryItem> Memory::search(std::string_view query, std::size_t limit) const {
    if (!loaded_) rebuild_index();
    const auto words = tokenize(query);
    if (words.empty() || index_.empty()) return {};

    struct Scored { int score; const MemoryItem* item; };
    std::vector<Scored> scored;
    for (const auto& item : index_) {
        const auto name_l = lower(item.name);
        const auto desc_l = lower(item.description);
        const auto body_l = lower(item.body);

        int score = 0;
        for (const auto& w : words) {
            // 权重依次递减：名字 > 描述 > 正文。
            //
            // ⚠️ 三处都对**出现次数**封顶，而且上限依次放宽。理由是重复在三个
            //    地方的含义完全不同：
            //      名字   是个「有或没有」的信号 —— 文件名里重复一个词毫无意义，
            //             所以只算一次。它是最刻意的标注：有人给这条记忆起了这个名。
            //      描述   一行字里重复一个词，相关性并没有翻倍，封到 2。
            //      正文   封到 3，否则堆砌关键词的长文永远排第一。
            //
            //    不封顶的话，一条描述里重复两次的记忆会压过名字直接命中的那条 ——
            //    实测过，而这显然是错的。
            score += 10 * std::min(count_hits(name_l, w), 1);
            score += 4 * std::min(count_hits(desc_l, w), 2);
            score += 1 * std::min(count_hits(body_l, w), 3);
        }
        if (score > 0) scored.push_back({score, &item});
    }

    // 分数降序；同分按名字，保证结果稳定可复现
    std::ranges::sort(scored, [](const Scored& a, const Scored& b) {
        return a.score != b.score ? a.score > b.score : a.item->name < b.item->name;
    });

    std::vector<MemoryItem> out;
    for (const auto& s : scored) {
        if (out.size() >= limit) break;
        out.push_back(*s.item);
    }
    return out;
}

fs::path Memory::write(std::string_view name, std::string_view description,
                       std::string_view body, MemoryType type) {
    std::error_code ec;
    fs::create_directories(root_, ec);
    const fs::path path = root_ / (std::string(name) + ".md");

    // description 里的换行会毁掉 frontmatter 的行结构 —— 压成一行。
    std::string desc(description);
    std::ranges::replace(desc, '\n', ' ');

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << "---\n"
            << "name: " << name << "\n"
            << "description: " << desc << "\n"
            << "metadata:\n"
            << "  type: " << type_name(type) << "\n"
            << "---\n\n"
            << body;
        if (!body.empty() && body.back() != '\n') out << "\n";
    }
    out.close();

    rebuild_index();   // 下一轮的 system prompt 就该看得见它
    return path;
}

bool Memory::remove(std::string_view name) {
    std::error_code ec;
    const bool gone = fs::remove(root_ / (std::string(name) + ".md"), ec);
    if (gone) rebuild_index();
    // 删除和写入一样重要：一条后来发现是错的记忆，不删掉就会一直被召回。
    return gone && !ec;
}

std::string Memory::index_text(std::size_t max_items) const {
    if (!loaded_) rebuild_index();
    if (index_.empty()) return {};

    std::string out = "Long-term memory (load one by name when it looks relevant):\n";
    std::size_t n = 0;
    for (const auto& item : index_) {
        if (n++ >= max_items) {
            out += std::format("  ... 还有 {} 条，用 memory search 查\n", index_.size() - max_items);
            break;
        }
        out += std::format("  {} [{}] — {}\n", item.name, type_name(item.type), item.description);
    }
    return out;
}

std::optional<MemoryItem> parse_memory_file(const fs::path& path) {
    const auto fm = parse_frontmatter_file(path.string());
    if (!fm) return std::nullopt;

    MemoryItem item;
    item.path = path;
    // name 缺失时用文件名兜底 —— 手写的文件常常忘了写它，而文件名总是对的。
    const auto it = fm->fields.find("name");
    item.name = it != fm->fields.end() ? it->second : path.stem().string();

    const auto d = fm->fields.find("description");
    if (d == fm->fields.end() || d->second.empty()) return std::nullopt;
    // ⚠️ 没有 description 就整条丢弃，不是给个空串。一条描述为空的记忆会进
    //    索引、占位置，而模型永远不会去加载它 —— 静默失效比读不到更糟。
    item.description = d->second;

    const auto t = fm->fields.find("type");
    item.type = t != fm->fields.end() ? type_from(t->second) : MemoryType::Reference;
    item.body = fm->body;
    return item;
}

}  // namespace mini
