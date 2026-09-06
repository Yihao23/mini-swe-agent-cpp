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

#include <cstdlib>
#include <format>

#include "mini_agent/config.hpp"

#if MINI_AGENT_HAVE_CURL
#include <curl/curl.h>
#endif

namespace mini {

void Usage::add(const Json& usage_json) {
    if (!usage_json.is_object()) return;   // 响应里没有 usage 字段不是错误
    input_tokens  += usage_json.value("input_tokens", 0L);
    output_tokens += usage_json.value("output_tokens", 0L);
    cache_read    += usage_json.value("cache_read_input_tokens", 0L);
    cache_write   += usage_json.value("cache_creation_input_tokens", 0L);
    ++requests;
}

long Usage::total_input() const {
    // 缓存命中的部分 API 单独计费，但它们同样占用上下文窗口 —— 判断"离窗口上限
    // 还有多远"时必须算进来，否则会以为还很宽裕。
    return input_tokens + cache_read + cache_write;
}

std::string Usage::summary() const {
    const long total = total_input();
    const double hit = total > 0 ? 100.0 * static_cast<double>(cache_read) /
                                       static_cast<double>(total)
                                 : 0.0;
    return std::format("{} 次请求 · 输入 {} (缓存命中 {} / {:.0f}%) · 输出 {}", requests, total,
                       cache_read, hit, output_tokens);
}

// --- AnthropicClient -------------------------------------------------------
#if MINI_AGENT_HAVE_CURL

struct AnthropicClient::Impl {
    CURL* handle = nullptr;          // 复用同一个 handle：连接、TLS 握手都能复用
    std::string body;                // 非流式：整个响应攒在这里
    const LlmRequest* req = nullptr; // 流式回调要用到 on_text / on_thinking
    std::string sse_buf;             // ⚠️ 一次写回调不保证是完整一行，自己缓冲
    std::string text;                // 流式累积的正文
    std::string thinking;            // 流式累积的思考
    std::string signature;           // ⚠️ 必须原样带回，API 会校验
    std::vector<ContentBlock> blocks;
    std::string stop_reason;
    std::string model;
    Json usage = Json::object();

    /// libcurl 的写回调是 C 函数指针，拿不到 this —— 通过 CURLOPT_WRITEDATA 传进来。
    /// 做成静态成员而不是自由函数：Impl 是私有类型，外面的函数看不见它。
    static std::size_t write_plain(char* p, std::size_t sz, std::size_t n, void* self) {
        static_cast<Impl*>(self)->body.append(p, sz * n);
        return sz * n;
    }

    void reset(const LlmRequest* r) {
        body.clear(); sse_buf.clear(); text.clear(); thinking.clear(); signature.clear();
        blocks.clear(); stop_reason.clear(); model.clear();
        usage = Json::object();
        req = r;
    }
};

AnthropicClient::AnthropicClient(const Config& cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>()) {
    impl_->handle = curl_easy_init();
}

AnthropicClient::~AnthropicClient() {
    if (impl_ && impl_->handle) curl_easy_cleanup(impl_->handle);
}

#else   // 没装 libcurl：能编译、能构造，一调用就明确报错

struct AnthropicClient::Impl {};
AnthropicClient::AnthropicClient(const Config& cfg) : cfg_(cfg), impl_(nullptr) {}
AnthropicClient::~AnthropicClient() = default;

#endif

Json AnthropicClient::build_body(const Config& cfg, const LlmRequest& req, bool stream) {
    Json body{
        {"model", req.model.empty() ? cfg.model : req.model},
        {"max_tokens", req.max_tokens > 0 ? req.max_tokens : cfg.max_tokens},
        {"messages", req.messages ? to_json(*req.messages) : Json::array()},
    };

    // system 是块数组，最后一块打 cache_control —— 一个断点就把 tools + system
    // 整段缓存住（顺序是 system → tools → messages，前缀匹配）。
    if (req.system && !req.system->empty()) {
        Json blocks = Json::array();
        for (const auto& b : *req.system) {
            Json blk{{"type", "text"}, {"text", b.text}};
            if (b.cache_breakpoint) blk["cache_control"] = {{"type", "ephemeral"}};
            blocks.push_back(std::move(blk));
        }
        body["system"] = std::move(blocks);
    }

    // ⚠️ tools 空数组也别发 —— 有的 API 版本会因为空数组报错，而且它照样进
    //    缓存前缀，白占字节。
    if (!req.tools.empty()) body["tools"] = req.tools;

    if (cfg.thinking)
        body["thinking"] = {{"type", "adaptive"},
                            {"display", cfg.show_thinking ? "summarized" : "hidden"}};
    if (!cfg.effort.empty()) body["output_config"] = {{"effort", cfg.effort}};
    if (stream) body["stream"] = true;

    return body;
}


// 1. 把 req 里的东西抄一份进 calls_（测试要看"agent 发了什么"）
// 2. 剧本用完了？返回一条 "done" + end_turn，别抛异常
// 3. 取当前这一轮，把每个 FakeBlock 翻译成 ContentBlock：
//      tool_name 空  → TextBlock{ .text = b.text }
//      tool_name 非空 → ToolUseBlock{ .id = "toolu_" + ++counter_, .name, .input }
// 4. 打包成 LlmResponse{ content, stop_reason }，返回
#if MINI_AGENT_HAVE_CURL

std::expected<LlmResponse, LlmError> AnthropicClient::complete(const LlmRequest& req) {
    if (!impl_ || !impl_->handle)
        return std::unexpected(LlmError{0, "init_error", "curl 初始化失败"});

    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (!key || !*key)
        return std::unexpected(LlmError{0, "auth_error", "未设置 ANTHROPIC_API_KEY"});

    // TODO(Stage 1): 流式。req.on_text 非空且 cfg_.stream 时走 SSE。
    const bool stream = false;
    const std::string payload = build_body(cfg_, req, stream).dump();

    impl_->reset(&req);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, (std::string("x-api-key: ") + key).c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
    headers = curl_slist_append(headers, "content-type: application/json");

    CURL* h = impl_->handle;
    curl_easy_reset(h);
    curl_easy_setopt(h, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Impl::write_plain);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, impl_.get());
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 600L);

    const CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    // http_status = 0 表示网络层就没成功 —— 和「服务器返回了错误」是两回事
    if (rc != CURLE_OK)
        return std::unexpected(LlmError{0, "network_error", curl_easy_strerror(rc)});

    const Json j = Json::parse(impl_->body, nullptr, /*allow_exceptions=*/false);

    if (status < 200 || status >= 300) {
        // 429/529 要能被上层识别出来重试，所以 type 必须原样带出去
        std::string type = "http_error", msg = impl_->body.substr(0, 500);
        if (j.is_object() && j.contains("error")) {
            type = j["error"].value("type", type);
            msg = j["error"].value("message", msg);
        }
        return std::unexpected(LlmError{static_cast<int>(status), std::move(type), std::move(msg)});
    }

    if (!j.is_object())
        return std::unexpected(LlmError{static_cast<int>(status), "parse_error", "响应不是合法 JSON"});

    LlmResponse out;
    out.model = j.value("model", std::string{});
    out.stop_reason = j.value("stop_reason", std::string{});
    // ⚠️ 原样保留所有块 —— thinking 的 signature 也在里面，下一轮 API 会校验它
    for (const auto& b : j.value("content", Json::array()))
        if (auto blk = block_from_json(b)) out.content.push_back(std::move(*blk));

    usage_.add(j.value("usage", Json::object()));
    out.usage = usage_;
    return out;
}

#else

std::expected<LlmResponse, LlmError> AnthropicClient::complete(const LlmRequest&) {
    return std::unexpected(
        LlmError{0, "no_curl", "编译时没找到 libcurl，真实 API 调用不可用"});
}

#endif

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
