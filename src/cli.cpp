// 【Stage 7】终端界面。
//
// 这一层不含任何 agent 逻辑，纯粹把 loop 吐出的事件画到屏幕上。
// 做对了的标志：换成 Web 前端只要换掉这一个文件。

#include "mini_agent/cli.hpp"

#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "mini_agent/app.hpp"
#include "mini_agent/config.hpp"
#include "mini_agent/llm.hpp"
#include "mini_agent/loop.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tool.hpp"

namespace mini {

namespace {

constexpr const char* kDim = "\033[2m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kReset = "\033[0m";

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    return std::string(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
}

/// 工具参数压成一行，太长就截断 —— 屏幕上一行就够看清在干什么
std::string one_line(const Json& args, std::size_t limit = 60) {
    std::string s = args.is_object() && args.empty() ? "" : args.dump();
    for (auto& c : s)
        if (c == '\n') c = ' ';
    if (s.size() > limit) s = s.substr(0, limit) + "…";
    return s;
}

}  // namespace

Renderer::Renderer(bool show_thinking) : show_thinking_(show_thinking) {}

void Renderer::newline_if_needed() {
    // 流式文本是一段段吐出来的，中途插别的东西前得先换行，
    // 否则工具调用会打断到半行文本里。
    if (in_text_ || in_thinking_) {
        std::fputs("\n", stdout);
        in_text_ = in_thinking_ = false;
    }
}

void Renderer::operator()(const AgentEvent& event) {
    std::visit(
        [this](const auto& e) {
            using T = std::decay_t<decltype(e)>;

            if constexpr (std::is_same_v<T, TextEvent>) {
                if (in_thinking_) newline_if_needed();
                std::fputs(e.text.c_str(), stdout);
                in_text_ = true;

            } else if constexpr (std::is_same_v<T, ThinkingEvent>) {
                if (!show_thinking_) return;
                if (in_text_) newline_if_needed();
                std::printf("%s%s%s", kDim, e.text.c_str(), kReset);
                in_thinking_ = true;

            } else if constexpr (std::is_same_v<T, ToolCallEvent>) {
                newline_if_needed();
                std::printf("%s⚙ %s%s(%s)\n", kYellow, e.name.c_str(), kReset,
                            one_line(e.input).c_str());

            } else if constexpr (std::is_same_v<T, ToolResultEvent>) {
                const char* mark = e.is_error ? "✗" : "✓";
                const char* color = e.is_error ? kRed : kGreen;
                std::printf("  %s%s%s %s%.1fs%s", color, mark, kReset, kDim, e.duration_sec,
                            kReset);
                if (e.is_error) std::printf("  %s", one_line(Json(e.output), 80).c_str());
                std::fputs("\n", stdout);

            } else {   // StopEvent
                newline_if_needed();
                if (e.reason != "end_turn" && !e.reason.empty())
                    std::printf("%s[%s%s%s]%s\n", kDim, e.reason.c_str(),
                                e.detail.empty() ? "" : ": ", e.detail.c_str(), kReset);
            }
            std::fflush(stdout);
        },
        event);
}

Confirm ask_user(std::string_view tool, std::string_view subject, std::string_view reason) {
    std::printf("\n%s⚠ %.*s%s 想执行: %.*s\n", kYellow, static_cast<int>(tool.size()),
                tool.data(), kReset, static_cast<int>(subject.size()), subject.data());
    if (!reason.empty())
        std::printf("  %s%.*s%s\n", kDim, static_cast<int>(reason.size()), reason.data(), kReset);
    std::printf("  [y] 允许一次  [a] 本会话都允许  [n] 拒绝: ");
    std::fflush(stdout);

    std::string line;
    if (!std::getline(std::cin, line)) return Confirm::Deny;   // EOF（管道输入）→ 拒绝
    const auto ans = trim(line);
    if (ans == "a" || ans == "A") return Confirm::Always;
    if (ans == "y" || ans == "Y") return Confirm::Once;
    return Confirm::Deny;
}

bool handle_command(App& app, std::string_view line) {
    const auto cmd = trim(line);

    if (cmd == "/quit" || cmd == "/exit") return true;

    if (cmd == "/help") {
        std::puts("  /help     这些命令\n"
                  "  /tools    列出可用工具\n"
                  "  /usage    token 统计\n"
                  "  /session  会话文件位置和消息数\n"
                  "  /mode     当前权限模式\n"
                  "  /clear    清空历史\n"
                  "  /quit     退出");
    } else if (cmd == "/tools") {
        for (const Tool* t : app.registry().all())
            std::printf("  %-10s %s\n", std::string(t->name()).c_str(),
                        std::string(t->description()).substr(0, 60).c_str());
    } else if (cmd == "/usage") {
        std::printf("  %s\n", app.llm().usage().summary().c_str());
    } else if (cmd == "/session") {
        std::printf("  %s\n  %zu 条消息\n", app.session().path().string().c_str(),
                    app.session().messages().size());
    } else if (cmd == "/mode") {
        std::printf("  %s\n", std::string(to_string(app.sandbox().mode())).c_str());
    } else if (cmd == "/clear") {
        app.session().messages().clear();
        std::puts("  历史已清空");
    } else {
        std::printf("  未知命令 %s，试试 /help\n", cmd.c_str());
    }
    return false;
}

int repl(App& app) {
    std::printf("%smini-agent%s  %s  %s\n输入 /help 看命令，Ctrl-D 退出\n\n", kBold, kReset,
                app.cfg().model.c_str(), app.cfg().workdir.string().c_str());
    for (const auto& w : app.warnings()) std::printf("%s⚠ %s%s\n", kYellow, w.c_str(), kReset);

    std::string line;
    for (;;) {
        std::printf("%s>%s ", kBold, kReset);
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) { std::puts(""); break; }

        const auto input = trim(line);
        if (input.empty()) continue;
        if (input.starts_with("/")) {
            if (handle_command(app, input)) break;
            continue;
        }

        g_interrupt = 0;                    // 每轮重置，上一轮的 Ctrl-C 不该影响这一轮
        app.agent().run(input);
        std::puts("");
    }
    return 0;
}

int cli_main(int argc, char** argv) {
    // 手写参数解析 —— 这个项目的依赖只该有两个，不为一个 argparse 再加一个。
    Config cfg;
    std::string task;
    bool continue_session = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if (a == "--help" || a == "-h") {
            std::puts("用法: mini-agent [选项] [任务]\n"
                      "  -C, --dir <路径>    工作目录（默认当前目录）\n"
                      "      --model <名字>  覆盖模型\n"
                      "      --mode <模式>   read-only | ask | auto | yolo\n"
                      "      --no-stream     关闭流式输出\n"
                      "  -c, --continue      接着上次的会话\n"
                      "  -h, --help          这些说明\n\n"
                      "给了任务就跑一次退出，不给就进交互模式。");
            return 0;
        } else if (a == "-C" || a == "--dir") {
            cfg.workdir = next();
        } else if (a == "--model") {
            cfg.model = next();
        } else if (a == "--mode") {
            const auto v = next();
            if (auto m = permission_mode_from_string(v)) cfg.permission_mode = *m;
            else { std::fprintf(stderr, "未知模式: %s\n", v.c_str()); return 2; }
        } else if (a == "--no-stream") {
            cfg.stream = false;
        } else if (a == "-c" || a == "--continue") {
            continue_session = true;
        } else if (a.starts_with("-")) {
            std::fprintf(stderr, "未知选项: %.*s\n", static_cast<int>(a.size()), a.data());
            return 2;
        } else {
            task = task.empty() ? std::string(a) : task + " " + std::string(a);
        }
    }

    // 命令行覆盖配置文件：先 load_config 拿到文件+环境变量的值，再把命令行的盖上去
    const auto workdir = cfg.workdir.empty() ? fs::current_path() : cfg.workdir;
    Config loaded = load_config(workdir);
    if (!cfg.model.empty() && cfg.model != Config{}.model) loaded.model = cfg.model;
    if (cfg.permission_mode != Config{}.permission_mode) loaded.permission_mode = cfg.permission_mode;
    if (!cfg.stream) loaded.stream = false;
    loaded.workdir = workdir;
    loaded.normalize();

    // Ctrl-C 只置一个标志位；循环在安全点检查它，把"被打断"写进历史而不是杀进程。
    std::signal(SIGINT, [](int) { g_interrupt = 1; });

    std::optional<Session> resumed;
    if (continue_session) {
        // TODO(Stage 7): 找 sessions_dir 里最新的那个会话文件
        std::fprintf(stderr, "--continue 还没实现\n");
    }

    Renderer render(loaded.show_thinking);
    try {
        App app(loaded, nullptr, &ask_user, [&render](const AgentEvent& e) { render(e); },
                std::move(resumed));
        if (!task.empty()) {
            app.agent().run(task);
            std::puts("");
            return 0;
        }
        return repl(app);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s启动失败: %s%s\n", kRed, e.what(), kReset);
        return 1;
    }
}

}  // namespace mini
