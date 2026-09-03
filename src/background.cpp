// 【Stage 6】后台任务。
#include "mini_agent/background.hpp"

#include "mini_agent/json.hpp"

namespace mini {

struct BackgroundManager::Impl {
    // 至少要有：map<string, Task>、计数器、mutex
    // Task 里：pid、进程组 id、jthread、buffer、cursor、mutex、reported_exit
};

BackgroundManager::BackgroundManager() : impl_(nullptr) { todo("Stage 6: BackgroundManager 构造"); }
BackgroundManager::~BackgroundManager() = default;

std::string BackgroundManager::start(const std::string&, const fs::path&, std::string) {
    todo("Stage 6: BackgroundManager::start —— fork + 读取线程（用 jthread）");
}

std::string BackgroundManager::drain(std::string_view) {
    todo("Stage 6: BackgroundManager::drain —— 返回新增输出并推进游标（要加锁）");
}

std::string BackgroundManager::render_list() const { todo("Stage 6: render_list"); }
void BackgroundManager::kill(std::string_view) { todo("Stage 6: kill —— 杀进程组"); }
void BackgroundManager::kill_all() { todo("Stage 6: kill_all"); }

std::vector<std::string> BackgroundManager::notifications() {
    todo("Stage 6: notifications —— ⚠️ 每个任务只能通知一次");
}

}  // namespace mini
