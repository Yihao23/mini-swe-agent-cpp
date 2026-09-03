// 【Stage 1】LLM 客户端。
//
// 建议的实现顺序：
//   1. FakeLlm            —— 最先写，后面全靠它测
//   2. build_body()       —— 纯函数，可以单测，不用发请求
//   3. 非流式 AnthropicClient::complete
//   4. 流式（SSE）
//
// build_body 要产出的形状（Claude Messages API）：
// {
//   "model": "claude-opus-5", "max_tokens": 16000,
//   "system": [ {"type":"text","text":"..."},
//               {"type":"text","text":"...","cache_control":{"type":"ephemeral"}} ],
//   "messages": [...],
//   "tools": [...],
//   "thinking": {"type":"adaptive","display":"summarized"},
//   "output_config": {"effort":"high"},
//   "cache_control": {"type":"ephemeral"}
// }
//
// HTTP 头：
//   x-api-key: $ANTHROPIC_API_KEY
//   anthropic-version: 2023-06-01
//   content-type: application/json

#include "mini_agent/llm.hpp"

#include "mini_agent/config.hpp"

namespace mini {

void Usage::add(const Json&) {
    todo("Stage 1: Usage::add —— input_tokens / output_tokens / "
         "cache_read_input_tokens / cache_creation_input_tokens");
}

long Usage::total_input() const {
    todo("Stage 1: Usage::total_input");
}

std::string Usage::summary() const {
    todo("Stage 1: Usage::summary");
}

// --- AnthropicClient -------------------------------------------------------
struct AnthropicClient::Impl {
    // pimpl：curl.h 关在这里，不污染头文件。
    // 至少要有：CURL* handle、一个 SSE 行缓冲、当前请求的回调指针。
};

AnthropicClient::AnthropicClient(const Config& cfg) : cfg_(cfg), impl_(nullptr) {
    todo("Stage 1: AnthropicClient 构造 —— curl_easy_init");
}

AnthropicClient::~AnthropicClient() = default;

Json AnthropicClient::build_body(const Config&, const LlmRequest&, bool) {
    todo("Stage 1: AnthropicClient::build_body（纯函数，先写这个并单测）");
}


// 1. 把 req 里的东西抄一份进 calls_（测试要看"agent 发了什么"）
// 2. 剧本用完了？返回一条 "done" + end_turn，别抛异常
// 3. 取当前这一轮，把每个 FakeBlock 翻译成 ContentBlock：
//      tool_name 空  → TextBlock{ .text = b.text }
//      tool_name 非空 → ToolUseBlock{ .id = "toolu_" + ++counter_, .name, .input }
// 4. 打包成 LlmResponse{ content, stop_reason }，返回
std::expected<LlmResponse, LlmError> AnthropicClient::complete(const LlmRequest&) {
    // 流式的坑：写回调是 C 函数指针，把 this 通过 CURLOPT_WRITEDATA 传进去；
    // 一次回调不保证是完整一行，必须自己缓冲、按 '\n' 切，再看 "data: " 前缀。
    todo("Stage 1: AnthropicClient::complete");



}

// --- FakeLlm ---------------------------------------------------------------
FakeBlock FakeBlock::text_block(std::string t) {
    FakeBlock b;
    b.text = std::move(t);
    return b;
}

FakeBlock FakeBlock::tool(std::string name, Json input) {
    FakeBlock b;
    b.tool_name = std::move(name);
    b.input = std::move(input);
    return b;
}

FakeLlm::FakeLlm(const Config& cfg, std::vector<Turn> script)
    : cfg_(cfg), script_(std::move(script)) {}

void FakeLlm::push_front(Turn) {
    todo("Stage 6: FakeLlm::push_front —— 子 agent 测试要往剧本中间插");
}

std::expected<LlmResponse, LlmError> FakeLlm::complete(const LlmRequest& req) {
    // 要点：
    //   * 记录本次调用（system/messages/tools）到 calls_，测试要断言
    //   * 从 script_ 弹一条，转成 ContentBlock；tool_use 要生成唯一 id
    //   * **只在 cfg_.stream 为真时**调 on_text，否则 loop 会重复输出
    //   * 剧本用完返回一条兜底文本，别抛异常

    // 三样都要记：system_prompt_is_byte_stable / has_no_timestamp 这类测试要断言它。
    Json sys = Json::array();
    if (req.system)
        for (const auto& b : *req.system)
            sys.push_back(Json{{"text", b.text}, {"cache_breakpoint", b.cache_breakpoint}});
    calls_.push_back(Json{
        {"system", std::move(sys)},
        {"messages", req.messages ? to_json(*req.messages) : Json::array()},
        {"tools", req.tools},
    });
    usage_.requests++;
    const std::string model = req.model.empty() ? cfg_.model : req.model;   // 子 agent 会覆盖 model
    if (script_.empty()) {
        LlmResponse fallback;
        fallback.content = {TextBlock{"done"}};
        fallback.stop_reason = "end_turn";
        fallback.model = model;
        return fallback;
    }
    Turn turn = std::move(script_.front());
    script_.erase(script_.begin());

    LlmResponse resp;
    resp.stop_reason = turn.stop_reason;
    resp.model = model;
    for (const auto& b : turn.blocks) {
            if (b.tool_name.empty()) {
              if (cfg_.stream && req.on_text) req.on_text(b.text);
                resp.content.push_back(TextBlock{.text = b.text});
            }
            else{
                resp.content.push_back(ToolUseBlock{
                    .id = "toolu_" + std::to_string(++counter_),
                    .name = b.tool_name,
                    .input = b.input,
                });
            }
    }

    return resp;
}

}  // namespace mini
