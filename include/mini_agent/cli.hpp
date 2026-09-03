#pragma once
//
// 【Stage 7】终端界面 —— 事件渲染 + REPL + 斜杠命令 + 权限确认。
//
// 这一层**不含任何 agent 逻辑**，纯粹是把 loop 吐出的事件画到屏幕上。
// 做对了的标志：换成 Web 前端只要换掉这一个文件。
//
// 渲染用 std::visit 分派 AgentEvent —— 加一种事件时这里会编译报错，
// 这正是 variant 相对于"事件基类 + dynamic_cast"的价值。
//
#include <string>
#include <vector>

#include "mini_agent/parser.hpp"
#include "mini_agent/sandbox.hpp"

namespace mini {

class App;

/// 事件渲染器。有状态：要记住"当前是不是正处在流式文本中间"，
/// 否则工具调用会打断到半行文本里。
class Renderer {
  public:
    explicit Renderer(bool show_thinking = true);
    void operator()(const AgentEvent& event);

  private:
    void newline_if_needed();

    bool show_thinking_;
    bool in_text_ = false;
    bool in_thinking_ = false;
};

/// 权限确认弹窗：[y] 允许一次 [a] 本会话都允许 [n] 拒绝
Confirm ask_user(std::string_view tool, std::string_view subject, std::string_view reason);

/// 斜杠命令。返回 true 表示该退出。
/// 建议：/help /tools /mode /memory /skills /bg /compact /usage /session /clear /quit
bool handle_command(App& app, std::string_view line);

int repl(App& app);

/// 解析命令行 → 跑一次性任务或进 REPL。
/// TODO(Stage 7): C++ 没有 argparse，手写一个 30 行的解析循环就够了，
/// 别引第三方库 —— 这个项目的依赖只该有两个。
int cli_main(int argc, char** argv);

}  // namespace mini
