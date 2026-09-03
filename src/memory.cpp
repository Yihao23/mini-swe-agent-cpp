// 【Stage 5】长期记忆。
#include "mini_agent/memory.hpp"

#include "mini_agent/json.hpp"

namespace mini {

std::string MemoryItem::render() const { todo("Stage 5: MemoryItem::render"); }

Memory::Memory(fs::path root) : root_(std::move(root)) { todo("Stage 5: Memory 构造"); }

std::vector<MemoryItem> Memory::items() const { todo("Stage 5: Memory::items"); }
std::optional<MemoryItem> Memory::get(std::string_view) const { todo("Stage 5: Memory::get"); }

std::vector<MemoryItem> Memory::search(std::string_view, std::size_t) const {
    todo("Stage 5: Memory::search —— 关键词打分，description 命中加权");
}

fs::path Memory::write(std::string_view, std::string_view, std::string_view, MemoryType) {
    todo("Stage 5: Memory::write —— 写文件 + 重建索引");
}

bool Memory::remove(std::string_view) { todo("Stage 5: Memory::remove"); }
void Memory::rebuild_index() const { todo("Stage 5: Memory::rebuild_index"); }

std::string Memory::index_text(std::size_t) const {
    todo("Stage 5: Memory::index_text —— 描述写给模型看：'什么时候该用它'");
}

std::optional<MemoryItem> parse_memory_file(const fs::path&) {
    todo("Stage 5: parse_memory_file —— frontmatter 手写解析，和 skill 共用");
}

}  // namespace mini
