#pragma once
//
// 一个 50 行的测试框架。为什么不用 Catch2 / GoogleTest？
//
//   * 这个项目的依赖只该有两个（json + curl），测试框架不值得再加一个
//   * 你能一眼读完它，出问题时不用去查框架文档
//   * 真需要参数化/mock 时再换 —— 那时你已经知道自己要什么了
//
// 用法：
//     TEST(loop_runs_tool_then_answers) {
//         CHECK(x == 1);
//         CHECK_MSG(y > 0, "y 应该是正数");
//     }
//     int main() { return mt::run_all(); }
//
// 只跑一部分：MT_FILTER=子串 ./build/test_mcp —— 只跑名字里含这个子串的用例。
// 一个都没匹配上时返回 2 并说明，而不是安安静静地"0/0 通过"：拼错的名字和
// 全部通过在输出上不能长得一样。tools/mutate.py 的 TSan 模式靠它只跑一个用例。
//
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace mt {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline int run_all() {
    const char* filter = std::getenv("MT_FILTER");
    const bool filtering = filter && *filter;
    int failed = 0;
    std::size_t ran = 0;
    for (const auto& c : registry()) {
        if (filtering && c.name.find(filter) == std::string::npos) continue;
        ++ran;
        try {
            c.fn();
            std::cout << "\033[32m✓\033[0m " << c.name << "\n";
        } catch (const std::exception& e) {
            ++failed;
            std::cout << "\033[31m✗\033[0m " << c.name << "\n    " << e.what() << "\n";
        }
    }
    if (filtering && ran == 0) {
        std::cout << "MT_FILTER=" << filter << " 没有匹配任何用例\n";
        return 2;
    }
    std::cout << "\n" << (ran - static_cast<std::size_t>(failed)) << "/" << ran << " 通过\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace mt

#define TEST(name)                                             \
    static void name();                                        \
    static ::mt::Registrar mt_reg_##name(#name, name);         \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond))                                                                   \
            throw ::mt::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) \
                                + "  CHECK 失败: " #cond);                             \
    } while (0)

#define CHECK_MSG(cond, msg)                                                           \
    do {                                                                               \
        if (!(cond))                                                                   \
            throw ::mt::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) \
                                + "  " + (msg));                                       \
    } while (0)

#define CHECK_THROWS(expr)                                          \
    do {                                                            \
        bool threw = false;                                         \
        try {                                                       \
            (void)(expr);                                           \
        } catch (...) {                                             \
            threw = true;                                           \
        }                                                           \
        CHECK_MSG(threw, "期望抛异常但没有: " #expr);               \
    } while (0)
