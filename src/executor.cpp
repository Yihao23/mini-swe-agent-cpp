// 【Stage 2】工具执行器。

#include "mini_agent/executor.hpp"

#include "mini_agent/sandbox.hpp"
#include <chrono>
namespace mini {

Executor::Executor(ToolRegistry& registry, ToolContext& ctx, EventSink on_event)
    : registry_(registry), ctx_(ctx), on_event_(std::move(on_event)) {}

ToolResultEvent Executor::run_one(const ToolCallEvent& call) {
    // 任何情况下都要返回结果，不能抛异常：
    //   工具不存在 / 权限拒绝 / run 抛异常 → 全部变成 is_error 的结果
      ToolResultEvent r{.id = call.id, .name = call.name, .output = {}};
const auto t0 = std::chrono::steady_clock::now();
if (on_event_) on_event_(call); 
      if (Tool* tool = registry_.get(call.name)) {
          // TODO(Stage 3): sandbox.authorize(*tool, call.input) —— 权限拒绝也走 error 结果
          try {
              const ToolResult res = tool->run(call.input, ctx_);
              r.output   = truncate_output(res.content, ctx_.cfg->max_output_chars);
              r.is_error = res.is_error;
          } catch (const std::exception& e) {
              r.is_error = true;
              r.output   = std::string("工具执行失败: ") + e.what();
          } catch (...) {                       // 非 std::exception 的东西也要接住
              r.is_error = true;
              r.output   = "工具抛出未知异常";
          }
      } else {
          // 列出可用工具名 —— 模型下一轮能自己纠正拼写
          std::string avail;
          for (const auto& n : registry_.names()) {
              if (!avail.empty()) avail += ", ";
              avail += n;
          }
          r.is_error = true;
          r.output   = "未知工具 \"" + call.name + "\"。可用: " + avail;
      }
      r.duration_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() -  t0).count();
      if (on_event_) on_event_(r);             // 通知 UI：结果出来了
      return r;

}

std::vector<ToolResultEvent> Executor::run_batch(const std::vector<ToolCallEvent>& calls) {
    // ⚠️ std::async 要显式写 std::launch::async，否则可能变成延迟执行（假并发）
    // ⚠️ 结果顺序要和入参一致
          std::vector<ToolResultEvent> out;
          out.reserve(calls.size());
      for (const auto& c : calls) out.push_back(run_one(c));
      return out;

}

std::string truncate_output(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) return std::string(text);   // size_t 是无符号的，不判就下溢
    // ⚠️ 退到 UTF-8 字符边界：切出半个汉字会让 to_json 抛 type_error.316，整个请求发不出去。
    //    续接字节的高两位固定是 10。
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return std::string(text.substr(0, cut)) +
           "\n... [输出共 " + std::to_string(text.size()) + " 字符，已截断]";
}

}  // namespace mini
