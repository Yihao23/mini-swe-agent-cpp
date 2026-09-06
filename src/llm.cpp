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
#include <map>

#include "mini_agent/config.hpp"

#if MINI_AGENT_HAVE_CURL
#include <curl/curl.h>
#endif

namespace mini {

/// @brief Accumulate one response's `usage` object into the running totals.
///
/// @param usage_json The `usage` object from a response. Silently ignored if it
///        is not an object — a streaming event without usage is not an error.
///
/// @note `requests` increments regardless of content: it counts requests sent,
///       not tokens seen.
///
/// @code
/// Usage u;
/// u.add(Json{{"input_tokens",100},{"output_tokens",50},{"cache_read_input_tokens",900}});
/// u.add(Json{{"input_tokens", 20},{"output_tokens",30},{"cache_read_input_tokens",1000}});
/// u.add(Json{});                       // no fields — still counts as a request
/// // u.requests == 3, input_tokens == 120, output_tokens == 80, cache_read == 1900
/// @endcode
void Usage::add(const Json& usage_json) {
    if (!usage_json.is_object()) return;   // 响应里没有 usage 字段不是错误
    input_tokens  += usage_json.value("input_tokens", 0L);
    output_tokens += usage_json.value("output_tokens", 0L);
    cache_read    += usage_json.value("cache_read_input_tokens", 0L);
    cache_write   += usage_json.value("cache_creation_input_tokens", 0L);
    ++requests;
}

/// @brief Total input tokens consumed so far, cached ones included.
///
/// @return `input_tokens + cache_read + cache_write`
///
/// @note Cached tokens are billed separately and far more cheaply, but they
///       occupy the same context window. Stage 4 reads this number to decide
///       when to compact; excluding them makes a nearly-full window look roomy.
///
/// @code
/// // after the two add() calls above:
/// u.total_input();     // 2020  ==  120 fresh + 1900 cached
/// @endcode
long Usage::total_input() const {
    // 缓存命中的部分 API 单独计费，但它们同样占用上下文窗口 —— 判断"离窗口上限
    // 还有多远"时必须算进来，否则会以为还很宽裕。
    return input_tokens + cache_read + cache_write;
}

/// @brief One-line statistics for the CLI's `/usage` command.
///
/// @return A human-readable line; the cache hit rate is `cache_read / total_input`.
///
/// @code
/// u.summary();
/// // "3 次请求 · 输入 2020 (缓存命中 1900 / 94%) · 输出 80"
/// @endcode
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

/// @brief Implementation details of AnthropicClient (pimpl).
///
/// Kept in the .cpp so `curl.h` — which drags in a large pile of macros — never
/// reaches a translation unit that merely includes llm.hpp. The price is that
/// the destructor must be defined here too (see ~AnthropicClient).
///
/// Doubles as the streaming state machine: the SSE callback feeds bytes in,
/// each event mutates these fields, and by the end of the request `blocks`,
/// `stop_reason`, `model` and `usage` hold the complete response.
///
/// @code
/// // How the state evolves across one streamed response:
/// //   message_start        → model="claude-opus-5", usage={input_tokens:100,...}
/// //   content_block_start  → partials[0] = Partial{type="thinking"}
/// //   content_block_delta  → partials[0].text += "Let me ", on_text not called
/// //   content_block_delta  → partials[0].signature += "sig_abc"
/// //   content_block_stop   → blocks += ThinkingBlock{...}, partials.erase(0)
/// //   content_block_start  → partials[1] = Partial{type="text"}
/// //   content_block_delta  → partials[1].text += "Hi", req->on_text("Hi")
/// //   content_block_stop   → blocks += TextBlock{"Hi"}
/// //   message_delta        → stop_reason="end_turn", usage.output_tokens=42
/// @endcode
struct AnthropicClient::Impl {
    /// @brief A content block still being assembled from streamed deltas.
    ///
    /// The server pushes increments per block index, and one response may carry
    /// thinking, text and several tool_use blocks at once — so each index needs
    /// its own accumulator. Which fields matter depends on `type`:
    ///
    ///   | type              | fields used                    |
    ///   |-------------------|--------------------------------|
    ///   | text              | text                           |
    ///   | thinking          | text, signature                |
    ///   | redacted_thinking | data                           |
    ///   | tool_use          | id, name, partial_json         |
    ///
    /// @code
    /// // After the three input_json_delta events of a tool call:
    /// // Partial{ type="tool_use", id="toolu_01", name="read",
    /// //          partial_json="{\"path\": \"hello.py\"}" }
    /// // finish_block() then parses partial_json and emits the ToolUseBlock.
    /// @endcode
    struct Partial {
        std::string type;          // text / thinking / redacted_thinking / tool_use
        std::string text;          // text_delta 或 thinking_delta 累积
        std::string signature;     // ⚠️ signature_delta 累积（不是覆盖），必须原样带回
        std::string id, name;      // tool_use
        std::string partial_json;  // ⚠️ tool_use 的 input 是 JSON **字符串分片**，拼完再 parse
        std::string data;          // redacted_thinking
    };

    CURL* handle = nullptr;          // 复用同一个 handle：连接、TLS 握手都能复用
    std::string body;                // 完整响应体：非流式用来解析，流式用来看错误原文
    const LlmRequest* req = nullptr; // 流式回调要用到 on_text / on_thinking
    std::string sse_buf;             // ⚠️ 一次写回调不保证是完整一行，自己缓冲
    std::map<int, Partial> partials; // index → 正在构建的块
    std::vector<ContentBlock> blocks;
    std::string stop_reason;
    std::string model;
    Json usage = Json::object();

    /// @brief Non-streaming libcurl write callback: append the body verbatim.
    ///
    /// @param p    Start of this chunk.
    /// @param sz   Element size; always 1 for libcurl.
    /// @param n    Element count, i.e. the byte count.
    /// @param self The Impl* handed over through CURLOPT_WRITEDATA.
    /// @return Bytes consumed. Returning anything other than `sz * n` tells
    ///         libcurl the write failed and aborts the transfer.
    ///
    /// @code
    /// curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &Impl::write_plain);
    /// curl_easy_setopt(h, CURLOPT_WRITEDATA, impl_.get());   // becomes `self`
    /// // afterwards impl_->body holds the whole JSON response
    /// @endcode
    static std::size_t write_plain(char* p, std::size_t sz, std::size_t n, void* self) {
        static_cast<Impl*>(self)->body.append(p, sz * n);
        return sz * n;
    }

    /// @brief Streaming libcurl write callback: split SSE into lines, dispatch
    ///        each complete one to on_sse_data().
    ///
    /// @param p    Start of this chunk.
    /// @param sz   Element size; always 1 for libcurl.
    /// @param n    Element count, i.e. the byte count.
    /// @param self The Impl* handed over through CURLOPT_WRITEDATA.
    /// @return Bytes consumed.
    ///
    /// @warning Chunk boundaries are entirely outside our control — they follow
    ///          from when the server flushes, how TCP packetises, and what the
    ///          kernel buffer happened to hold. One call may deliver half a line
    ///          or three and a half; a single UTF-8 character can be split across
    ///          two calls (observed). Only complete lines are dispatched and the
    ///          remainder waits in `sse_buf` for the next call.
    ///
    /// @note The bytes also go into `body`: on failure the server sends plain
    ///       JSON rather than SSE, and that is where the error text is read from.
    ///
    /// @code
    /// // What libcurl actually delivered in one measured run, and what each
    /// // call produced. Note that calls 2 and 3 contain no newline at all —
    /// // they only grow the buffer.
    /// //
    /// //  call  bytes  content                                    result
    /// //  ----  -----  -----------------------------------------  ---------------------
    /// //   1     37    event: content_block_delta\ndata: {"ty      emits "event: ..." line,
    /// //                                                           leaves `data: {"ty`
    /// //   2     37    pe":"content_block_delta","index":0,"       no newline → buffered
    /// //   3     37    delta":{"type":"text_delta","text":"<hal    no newline → buffered
    /// //   4     37    f a char>"}}\n\ndata: {"type":"content_blo   emits the data: line
    /// //                                                           (assembled from calls
    /// //                                                            1-4), plus the blank
    /// //                                                            line; keeps the tail
    /// //
    /// // The `<half a char>` in call 3 is literal: a three-byte UTF-8 character
    /// // was split across calls 3 and 4. Harmless here — the scan only looks for
    /// // '\n', a single-byte ASCII value that never appears inside a multi-byte
    /// // UTF-8 sequence, so the halves rejoin in the buffer untouched.
    /// @endcode
    static std::size_t write_sse(char* p, std::size_t sz, std::size_t n, void* self) {
        auto* impl = static_cast<Impl*>(self);
        const std::size_t total = sz * n;
        impl->body.append(p, total);        // 出错时服务端发的是普通 JSON，留着看原文
        impl->sse_buf.append(p, total);

        std::size_t nl;
        while ((nl = impl->sse_buf.find('\n')) != std::string::npos) {
            std::string line = impl->sse_buf.substr(0, nl);
            impl->sse_buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();   // SSE 用 \r\n
            // event: 行不用管 —— data 的 JSON 里 "type" 字段有同样的信息
            if (line.starts_with("data: ")) impl->on_sse_data(line.substr(6));
        }
        return total;
    }

    /// @brief Handle one SSE `data:` line, advancing the parse state.
    ///
    /// @param payload Everything after `data: `, normally a JSON object.
    ///
    /// Five event types are recognised:
    ///   - `message_start`       — take `model` and the initial `usage`
    ///   - `content_block_start` — open a Partial at that index
    ///   - `content_block_delta` — accumulate one of four delta kinds
    ///   - `content_block_stop`  — hand the Partial to finish_block()
    ///   - `message_delta`       — take `stop_reason` and the final `usage`
    ///
    /// @note `event:` lines are ignored — the `type` field inside the JSON
    ///       carries the same information.
    /// @note A line that fails to parse is skipped: one bad line should not
    ///       destroy the whole stream.
    ///
    /// @warning `signature_delta` must be **accumulated**, not overwritten — the
    ///          signature arrives in fragments and has to be returned verbatim,
    ///          the API validates it on the next turn.
    /// @warning `input_json_delta` carries fragments of the **serialised JSON
    ///          string**. No individual fragment is valid JSON; only the
    ///          concatenation can be parsed (done in finish_block).
    ///
    /// @code
    /// // Three deltas for one tool_use block:
    /// //   {"type":"input_json_delta","partial_json":"{\"pa"}
    /// //   {"type":"input_json_delta","partial_json":"th\": \"a."}
    /// //   {"type":"input_json_delta","partial_json":"py\"}"}
    /// // partial_json accumulates to  {"path": "a.py"}
    ///
    /// // Two deltas for one signature:
    /// //   {"type":"signature_delta","signature":"sig_abc"}
    /// //   {"type":"signature_delta","signature":"123"}
    /// // signature accumulates to  "sig_abc123"   (not "123")
    /// @endcode
    void on_sse_data(std::string_view payload) {
        if (payload == "[DONE]") return;
        const Json e = Json::parse(payload, nullptr, /*allow_exceptions=*/false);
        if (!e.is_object()) return;   // 坏的一行跳过，不让整个流挂掉
        const auto type = e.value("type", std::string{});

        if (type == "message_start") {
            const auto& m = e.value("message", Json::object());
            model = m.value("model", std::string{});
            usage = m.value("usage", Json::object());

        } else if (type == "content_block_start") {
            const auto& cb = e.value("content_block", Json::object());
            Partial part;
            part.type = cb.value("type", std::string{});
            part.id = cb.value("id", std::string{});
            part.name = cb.value("name", std::string{});
            part.data = cb.value("data", std::string{});
            partials[e.value("index", 0)] = std::move(part);

        } else if (type == "content_block_delta") {
            Partial& part = partials[e.value("index", 0)];
            const auto& d = e.value("delta", Json::object());
            const auto dt = d.value("type", std::string{});
            if (dt == "text_delta") {
                const auto chunk = d.value("text", std::string{});
                part.text += chunk;
                if (req && req->on_text) req->on_text(chunk);          // 边收边吐给 UI
            } else if (dt == "thinking_delta") {
                const auto chunk = d.value("thinking", std::string{});
                part.text += chunk;
                if (req && req->on_thinking) req->on_thinking(chunk);
            } else if (dt == "signature_delta") {
                part.signature += d.value("signature", std::string{});
            } else if (dt == "input_json_delta") {
                part.partial_json += d.value("partial_json", std::string{});
            }

        } else if (type == "content_block_stop") {
            finish_block(e.value("index", 0));

        } else if (type == "message_delta") {
            stop_reason = e.value("delta", Json::object()).value("stop_reason", stop_reason);
            // message_delta 的 usage 是累计到此刻的值，覆盖而不是相加。
            // ⚠️ 必须先存进具名变量：range-for 只延长最外层表达式的寿命，
            //    e.value(...) 那个临时 Json 会在语句结束时销毁，.items() 的迭代器随之悬垂。
            const Json delta_usage = e.value("usage", Json::object());
            for (const auto& [k, v] : delta_usage.items()) usage[k] = v;
        }
    }

    /// @brief Turn the accumulated Partial at `index` into a finished ContentBlock.
    ///
    /// @param index The block index the server assigned.
    ///
    /// @note A tool_use block's input is parsed here, once the fragments are
    ///       complete. If the result is not an object it degrades to an empty
    ///       one rather than throwing — letting the model see a tool error beats
    ///       killing the request.
    /// @warning A thinking block's signature is moved across untouched. Altering
    ///          a single byte makes the next request fail validation.
    ///
    /// @code
    /// // Partial{type="tool_use", id="toolu_01", name="read",
    /// //         partial_json="{\"path\": \"hello.py\"}"}
    /// //   becomes
    /// // ToolUseBlock{"toolu_01", "read", {"path":"hello.py"}}
    ///
    /// // Partial{type="thinking", text="Let me read the file", signature="sig_abc123"}
    /// //   becomes
    /// // ThinkingBlock{"Let me read the file", "sig_abc123"}
    /// @endcode
    void finish_block(int index) {
        const auto it = partials.find(index);
        if (it == partials.end()) return;
        Partial& part = it->second;

        if (part.type == "text") {
            blocks.push_back(TextBlock{std::move(part.text)});
        } else if (part.type == "thinking") {
            // ⚠️ signature 原样带回 —— 下一轮 API 会校验，改一个字节就 400
            blocks.push_back(ThinkingBlock{std::move(part.text), std::move(part.signature)});
        } else if (part.type == "redacted_thinking") {
            blocks.push_back(RedactedThinkingBlock{std::move(part.data)});
        } else if (part.type == "tool_use") {
            Json input = Json::parse(part.partial_json, nullptr, /*allow_exceptions=*/false);
            if (!input.is_object()) input = Json::object();   // 分片拼错了也别崩
            blocks.push_back(
                ToolUseBlock{std::move(part.id), std::move(part.name), std::move(input)});
        }
        partials.erase(it);
    }

    /// @brief Clear state left over from the previous request.
    ///
    /// @param r The request being started; the SSE callbacks read `on_text` and
    ///        `on_thinking` from it.
    ///
    /// @note One Impl serves every request of a session. Without this the blocks
    ///       of the previous turn would leak into the next one.
    void reset(const LlmRequest* r) {
        body.clear(); sse_buf.clear();
        partials.clear(); blocks.clear();
        stop_reason.clear(); model.clear();
        usage = Json::object();
        req = r;
    }
};

/// @brief Create the reusable curl handle.
///
/// @param cfg Configuration reference. ⚠️ It must outlive this client — only a
///        reference is stored, nothing is copied.
///
/// @note One handle serves the whole session so the connection and the TLS
///       handshake are reused. A fresh handle per request would renegotiate
///       TLS on every turn.
AnthropicClient::AnthropicClient(const Config& cfg)
    : cfg_(cfg), impl_(std::make_unique<Impl>()) {
    impl_->handle = curl_easy_init();
}

/// @brief Release the curl handle.
///
/// @note Has to be defined here rather than `= default` in the header:
///       destroying `unique_ptr<Impl>` needs Impl's complete definition, and the
///       header only forward-declares it. This is the standing cost of pimpl.
AnthropicClient::~AnthropicClient() {
    if (impl_ && impl_->handle) curl_easy_cleanup(impl_->handle);
}

#else   // 没装 libcurl：能编译、能构造，一调用就明确报错

/// @brief Empty stand-in when libcurl is absent, so the pimpl type has a definition.
struct AnthropicClient::Impl {};

/// @brief Constructible without libcurl — the failure is deferred to the call.
AnthropicClient::AnthropicClient(const Config& cfg) : cfg_(cfg), impl_(nullptr) {}

/// @brief No handle to release, but still defined in the .cpp: destroying
///        `unique_ptr<Impl>` needs Impl's complete definition.
AnthropicClient::~AnthropicClient() = default;

#endif

/// @brief Assemble the Claude Messages API request body.
///
/// @param cfg    Supplies the defaults: model, max_tokens, thinking, effort.
/// @param req    This request; non-empty fields override the matching default.
/// @param stream Whether to add `"stream": true`.
/// @return JSON ready to `dump()` and send.
///
/// A pure function: it reads its arguments and touches neither the network nor
/// any member. That is why it is exposed as a public static — the request shape
/// can be tested without an API key and without a connection.
///
/// @note `model` and `max_tokens` follow request-overrides-config, which is how
///       a sub-agent switches to a cheaper model.
/// @note An empty tools array is omitted rather than sent: some API versions
///       reject it, and it occupies bytes in the cache prefix either way.
/// @note `cache_control` goes on the final system block. One breakpoint covers
///       system and tools together, since the prefix is matched in the order
///       system → tools → messages.
///
/// @code
/// Config cfg;                                   // claude-opus-5, 16000 tokens
/// std::vector<SystemBlock> sys{{"You are an agent.", false},
///                              {"Workdir: /work",    true}};   // breakpoint here
/// std::vector<Message> msgs{{Role::User, {TextBlock{"look at a.py"}}}};
/// LlmRequest req; req.system = &sys; req.messages = &msgs;
/// req.tools = Json::array({{{"name","read"},{"description","..."},
///                           {"input_schema",Json::object()}}});
///
/// AnthropicClient::build_body(cfg, req, false);
/// // {
/// //   "model": "claude-opus-5",
/// //   "max_tokens": 16000,
/// //   "system": [ {"type":"text","text":"You are an agent."},
/// //               {"type":"text","text":"Workdir: /work",
/// //                "cache_control":{"type":"ephemeral"}} ],
/// //   "messages": [ {"role":"user","content":[{"type":"text","text":"look at a.py"}]} ],
/// //   "tools": [ {"name":"read", ...} ],
/// //   "thinking": {"type":"adaptive","display":"summarized"},
/// //   "output_config": {"effort":"high"}
/// // }
///
/// LlmRequest sub = req; sub.model = "claude-sonnet-5"; sub.max_tokens = 4000;
/// AnthropicClient::build_body(cfg, sub, true);
/// // same shape, but "model":"claude-sonnet-5", "max_tokens":4000, "stream":true
/// @endcode
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

/// @brief Send one request and block until the full response is in.
///
/// @param req System blocks, history, tool schemas and the optional streaming
///        callbacks.
/// @return The model's complete reply, or an LlmError.
///
/// Streaming is chosen by `cfg_.stream && (req.on_text || req.on_thinking)` —
/// with nobody to receive the increments there is nothing to gain from SSE, and
/// sub-agents and one-shot tasks pass no callbacks.
///
/// @note Failure is a **value**, not an exception: 429 and 529 have to be
///       recognisable so the caller can retry, and a network blip should not
///       tear down the agent loop.
/// @note `LlmError::http_status == 0` means the transport never succeeded (DNS,
///       TLS, timeout), which is a different thing from the server answering
///       with an error code — only the latter carries an `error.type`.
/// @note Content blocks are preserved **verbatim**, signatures included; the API
///       validates them on the following turn.
///
/// @code
/// LlmRequest req; req.system = &sys; req.messages = &history; req.tools = schemas;
/// req.on_text = [](std::string_view chunk) { std::fputs(...); };   // enables SSE
///
/// auto r = client.complete(req);
/// if (!r) {
///     // r.error() == LlmError{429, "rate_limit_error", "..."}   → retryable
///     // r.error() == LlmError{0, "network_error", "Timeout was reached"}
///     // r.error() == LlmError{0, "auth_error", "未设置 ANTHROPIC_API_KEY"}
/// } else {
///     // r->content     == { ThinkingBlock{"...", "sig_abc"},
///     //                     TextBlock{"Let me read that"},
///     //                     ToolUseBlock{"toolu_01","read",{"path":"a.py"}} }
///     // r->stop_reason == "tool_use"
///     // r->model       == "claude-opus-5"
/// }
/// @endcode
std::expected<LlmResponse, LlmError> AnthropicClient::complete(const LlmRequest& req) {
    if (!impl_ || !impl_->handle)
        return std::unexpected(LlmError{0, "init_error", "curl 初始化失败"});

    const char* key = std::getenv("ANTHROPIC_API_KEY");
    if (!key || !*key)
        return std::unexpected(LlmError{0, "auth_error", "未设置 ANTHROPIC_API_KEY"});

    // 没人接收增量就没必要流式 —— 子 agent 和一次性任务都不传回调
    const bool stream = cfg_.stream && (req.on_text || req.on_thinking);
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
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, stream ? &Impl::write_sse : &Impl::write_plain);
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

    LlmResponse out;
    if (stream) {
        // 流式：响应体是一串 SSE 事件，内容已经在回调里攒进 impl_ 了
        if (impl_->blocks.empty() && impl_->stop_reason.empty())
            return std::unexpected(
                LlmError{static_cast<int>(status), "parse_error", "流式响应没有任何内容"});
        out.model = std::move(impl_->model);
        out.stop_reason = std::move(impl_->stop_reason);
        out.content = std::move(impl_->blocks);
        usage_.add(impl_->usage);
    } else {
        if (!j.is_object())
            return std::unexpected(
                LlmError{static_cast<int>(status), "parse_error", "响应不是合法 JSON"});
        out.model = j.value("model", std::string{});
        out.stop_reason = j.value("stop_reason", std::string{});
        // ⚠️ 原样保留所有块 —— thinking 的 signature 也在里面，下一轮 API 会校验它
        for (const auto& b : j.value("content", Json::array()))
            if (auto blk = block_from_json(b)) out.content.push_back(std::move(*blk));
        usage_.add(j.value("usage", Json::object()));
    }
    out.usage = usage_;
    return out;
}

#else

/// @brief Without libcurl, report a clear error instead of failing the build.
///
/// @return Always `LlmError{0, "no_curl", ...}`.
///
/// @note This branch is what gives CMake's warn-and-continue behaviour meaning:
///       FakeLlm and the whole test suite still work, only real API calls do not.
std::expected<LlmResponse, LlmError> AnthropicClient::complete(const LlmRequest&) {
    return std::unexpected(
        LlmError{0, "no_curl", "编译时没找到 libcurl，真实 API 调用不可用"});
}

#endif

// --- FakeLlm ---------------------------------------------------------------
/// @brief Build a script entry for a text block.
///
/// @param t What the model "says".
///
/// @note A factory rather than aggregate initialisation at the call site:
///       `text` and `tool_name` are mutually exclusive, and the factory keeps
///       that invariant at the single point of entry.
///
/// @code
/// FakeLlm llm(cfg, {
///     {{FakeBlock::text_block("It is a greeting function.")}, "end_turn"},
/// });
/// // complete() returns LlmResponse{ content={TextBlock{"It is a greeting function."}},
/// //                                 stop_reason="end_turn" }
/// @endcode
FakeBlock FakeBlock::text_block(std::string t) {
    FakeBlock b;
    b.text = std::move(t);
    return b;
}

/// @brief Build a script entry for a tool call.
///
/// @param name  Tool name; must match one in the registry.
/// @param input Tool arguments.
///
/// @note The id is not supplied here — FakeLlm::complete generates it, which
///       keeps ids unique even among several tools in one turn.
///
/// @code
/// FakeLlm llm(cfg, {
///     {{FakeBlock::tool("read", {{"path","a.py"}}),
///       FakeBlock::tool("grep", {{"pattern","foo"}})}, "tool_use"},
/// });
/// // complete() returns content = { ToolUseBlock{"toolu_1","read",{"path":"a.py"}},
/// //                                ToolUseBlock{"toolu_2","grep",{"pattern":"foo"}} }
/// @endcode
FakeBlock FakeBlock::tool(std::string name, Json input) {
    FakeBlock b;
    b.tool_name = std::move(name);
    b.input = std::move(input);
    return b;
}

/// @brief A fake client that replays a script.
///
/// @param cfg    ⚠️ Stored by reference — it must outlive this object. Worth
///        watching in tests where cfg is a local.
/// @param script One Turn per response. Once exhausted, every further call
///        returns "done" with `end_turn`.
///
/// @code
/// Config cfg; cfg.stream = false;
/// FakeLlm llm(cfg, {
///     {{FakeBlock::tool("read", {{"path","hello.py"}})}, "tool_use"},
///     {{FakeBlock::text_block("It is a greeting function.")}, "end_turn"},
/// });
/// App app(cfg, std::move(llm_ptr));
/// app.agent().run("look at hello.py");
/// // Two turns are consumed; the session ends with four messages:
/// //   user / assistant(tool_use) / user(tool_result) / assistant(text)
/// @endcode
FakeLlm::FakeLlm(const Config& cfg, std::vector<Turn> script)
    : cfg_(cfg), script_(std::move(script)) {}

/// @brief Insert a turn at the front so the next complete() returns it.
///
/// @param turn The response to insert.
///
/// @note Stage 6's sub-agent tests need to change later responses mid-run.
///
/// @code
/// FakeLlm llm(cfg, {turn_a, turn_b});
/// llm.complete(req);                    // → turn_a
/// llm.push_front(turn_c);
/// llm.complete(req);                    // → turn_c, not turn_b
/// llm.complete(req);                    // → turn_b
/// @endcode
void FakeLlm::push_front(Turn) {
    todo("Stage 6: FakeLlm::push_front —— 子 agent 测试要往剧本中间插");
}

/// @brief Pop the next turn from the script and translate it into an LlmResponse.
///
/// @param req This request; its system, messages and tools are recorded into
///        calls() for tests to assert on.
/// @return Always succeeds — an exhausted script yields a fallback line rather
///         than an error.
///
/// @note `on_text` fires only when `cfg_.stream` is set. On the non-streaming
///       path Agent::run emits the TextEvent itself, and doing both would print
///       the answer twice.
/// @note tool_use ids come from a counter, so several tools in one turn do not
///       collide.
///
/// @code
/// FakeLlm llm(cfg, {
///     {{FakeBlock::tool("read", {{"path","a.py"}})}, "tool_use"},
///     {{FakeBlock::text_block("Done.")},             "end_turn"},
/// });
/// llm.complete(req);   // → content={ToolUseBlock{"toolu_1","read",...}}, "tool_use"
/// llm.complete(req);   // → content={TextBlock{"Done."}},                 "end_turn"
/// llm.complete(req);   // → content={TextBlock{"done"}},  "end_turn"   (script empty)
/// llm.calls().size();  // 3 — every call is recorded, including the fallback
/// @endcode
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
