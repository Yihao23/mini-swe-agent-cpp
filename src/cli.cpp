// 【Stage 7】终端界面。

#include "mini_agent/cli.hpp"

#include "mini_agent/app.hpp"
#include "mini_agent/sandbox.hpp"

namespace mini {

Renderer::Renderer(bool show_thinking) : show_thinking_(show_thinking) {}

void Renderer::newline_if_needed() { todo("Stage 7: Renderer::newline_if_needed"); }

void Renderer::operator()(const AgentEvent&) {
    todo("Stage 7: Renderer::operator() —— std::visit 分派五种事件");
}

Confirm ask_user(std::string_view, std::string_view, std::string_view) {
    todo("Stage 7: ask_user —— [y] 一次 [a] 本会话 [n] 拒绝");
}

bool handle_command(App&, std::string_view) { todo("Stage 7: handle_command"); }

int repl(App&) { todo("Stage 7: repl"); }

int cli_main(int, char**) { todo("Stage 7: cli_main —— 手写参数解析，别引第三方库"); }

}  // namespace mini
