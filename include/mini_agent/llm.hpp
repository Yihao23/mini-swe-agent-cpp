#pragma once
/// @file
/// @brief The only place the agent talks to a model (Stage 1).
//
// 【Stage 1】LLM 客户端 —— agent 唯一和模型通信的地方。
//
// 这一层**不认识 agent 概念**：不知道什么是工具循环，只知道发请求收响应。
//
// ── C++ 的第三个设计决定：这里为什么用虚函数？────────────────────────────────
//
// LlmClient 是**开放集合**的一个特例：实现只有两个（真的 + 假的），
// 但"能不能在测试里换掉真实网络"决定了整个项目能不能自动化测试。
// 这是本项目里虚函数最值钱的一处 —— FakeLlm 让 21 个测试全部离线跑，0.5 秒跑完。
//
// ── 请求长什么样（Claude Messages API，照抄即可）─────────────────────────────
//   POST https://api.anthropic.com/v1/messages
//   headers: x-api-key, anthropic-version: 2023-06-01, content-type
//   body: {model, max_tokens, system[], messages[], tools[],
//          thinking:      {"type":"adaptive","display":"summarized"},
//          output_config: {"effort": "high"},
//          cache_control: {"type":"ephemeral"}}   ← 顶层自动缓存
//
// 流式是 SSE：`"stream": true`，响应体是一行行 `data: {...}`。
// 你要的是 content_block_delta 事件里的 text_delta / thinking_delta。
//
// ── 请求怎么拼，为什么是这个顺序 ────────────────────────────────────────────
//
//   build_body(cfg, req, stream)      纯函数，单独暴露就是为了能单测
//        │
//        ▼
//   ┌──────────────────────────────────────────────────────────────────┐
//   │ {                                              ← 缓存前缀从这里开始 │
//   │   "model", "max_tokens", ...     几乎不变                          │
//   │   "tools":    [...]              ⚠️ 按名字排序（ToolRegistry 负责）│
//   │   "system":   [{text, cache_control}]                             │
//   │                    └── 断点打在**最后一块**                        │
//   │                        ══════════════════════ ← 缓存到此为止       │
//   │   "messages": [...]              每轮追加，最易变                  │
//   │ }                                                                  │
//   └──────────────────────────────────────────────────────────────────┘
//
//   顺序不是随便排的：缓存**逐字节匹配前缀**，所以最稳定的放最前面。
//   tools 里少排一次序、system 里混进一个时间戳，它后面的整段对话
//   每轮都要重新计费 —— 而功能完全正常，什么都不会失败。
//
//   实测（一份 1442 token 的 system，同一个请求发两次）：
//       第 1 次   写缓存 1442   读缓存    0     约 1.25 倍价
//       第 2 次   写缓存    0   读缓存 1442     约 0.1  倍价
//
// ── 两条响应路径 ────────────────────────────────────────────────────────────
//
//   complete(req)
//        │
//        ├── req.on_text 为空 ──> 非流式
//        │       curl 一次拿全 → write_plain() 往 body 里 append
//        │       → nlohmann 解析 → LlmResponse
//        │
//        └── req.on_text 非空 ──> 流式（SSE）
//                curl 分块回调 → write_sse() → on_sse_data() → LlmResponse
//
//   ⚠️ libcurl 的写回调是 **C 函数指针**，拿不到 this。要靠
//      CURLOPT_WRITEDATA 把 this 传进去，回调里再 static_cast 回来。
//      所以那两个回调是 Impl 的 static 成员函数。
//
// ── SSE 的状态机 ────────────────────────────────────────────────────────────
//
//   ⚠️ 一次 curl 回调**不保证是完整的一行**。可能半行、可能三行半。
//      必须自己缓冲，按 '\n' 切，切不出来的留着等下一次。
//
//     write_sse(chunk)
//        buf += chunk
//        while (buf 里还有 '\n')  取出一行 → 以 "data: " 开头就交给 ↓
//
//     on_sse_data(payload)          按 event 类型分派：
//
//        content_block_start  ──> partials[index] = Partial{type, id, name}
//        content_block_delta  ──> 往 partials[index] 上追加
//                                     text_delta        → text +=
//                                     thinking_delta    → thinking +=
//                                     signature_delta   → signature +=   ⚠️
//                                     input_json_delta  → partial_json += ⚠️
//        content_block_stop   ──> finish_block(partials[index])
//        message_delta        ──> stop_reason / usage
//
//                    ┌──────────────────────────────────┐
//                    │  partials: map<int, Partial>     │  index → 正在建的块
//                    │  一次响应里可以有好几个块同时在建  │
//                    └──────────────┬───────────────────┘
//                                   ▼
//                            finish_block()
//                              tool_use 的 partial_json 到这里才 parse ——
//                              它是一片片来的，中途每一片都不是合法 JSON
//                                   │
//                                   ▼
//                            LlmResponse.content
//
//   ⚠️ signature_delta 也要拼。思考签名是分片来的，少拼一片，下一轮请求
//      会因为签名校验失败被拒 —— 而报错说的是「签名无效」，
//      离真正的原因（少收了一个 delta）很远。
//
// ── AnthropicClient 的三个字段 ──────────────────────────────────────────────
//
//     cfg_     协作者，引用。只用来取 model / max_tokens / effort / stream
//              → 没有不变量，只有生命周期这条前置条件（见文末）
//     usage_   状态                                              → I4
//     impl_    状态：curl handle + SSE 缓冲 + 半成品的块          → I5
//
// ── 谁保证什么 ──────────────────────────────────────────────────────────────
//
//   ┌── I1  失败是值，不是异常 ──────────────────────────────────────────┐
//   │ complete() 返回 expected<LlmResponse, LlmError>，任何情况下都不抛。 │
//   │ 429/529 要能重试，网络抖动不该炸掉 agent 循环。                     │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I2  content 里的块原样带回 ──────────────────────────────────────┐
//   │ 违反 → 思考签名丢失 → 下一轮 400。                                  │
//   │ 维护者：parse() 存 message 时不重建，只搬运。                       │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I3  流式时只有 on_text 吐文本 ───────────────────────────────────┐
//   │ 违反 → 整段响应打印两遍。                                           │
//   │ 维护者：Agent::run() 里那句 `if (!cfg_.stream && ...)`。            │
//   │        这一层只负责在 on_text 非空时调它，不管谁在听。              │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I4  usage_ 是**至今所有调用**的累加，不是最近一次的 ─────────────┐
//   │ 违反 → /usage 显示的是最后一轮的数字，缓存命中率无从判断。           │
//   │ 维护者：complete() 用 usage_.add()，两条路径（流式/非流式）各一次。 │
//   │ 注意返回的 LlmResponse.usage 是**累计值**的快照，不是本次增量 ——    │
//   │      因为调用方要的是"到目前为止花了多少"。                         │
//   └────────────────────────────────────────────────────────────────────┘
//   ┌── I5  impl_ 不跨调用携带状态 ──────────────────────────────────────┐
//   │ 违反 → 上一次响应的半成品块混进这一次。流式尤其危险：一个没收到     │
//   │        content_block_stop 的 partial 会一直留着，下一次响应的       │
//   │        同 index 事件往它上面追加 —— 拼出一个两次响应混合的块。      │
//   │ 维护者：complete() 开头的 impl_->reset(&req)，清 body / sse_buf /   │
//   │        partials / blocks / stop_reason / model / usage。            │
//   │ 唯一**不清**的是 handle —— 有意复用，省掉每次的连接和 TLS 握手。    │
//   └────────────────────────────────────────────────────────────────────┘
//
// FakeLlm 那边只有一条：script_ 每调用一次少一个，calls_ 每调用一次多一条，
// 所以 calls_.at(i) 就是"第 i 轮发出去的东西"。测试断言的是**请求**而不是响应
// 时靠的就是它 —— system 的字节稳定性、tools 有没有排序、压缩指令问没问对。
//
// ⚠️ 这一层**不认识 agent 概念** —— 不知道什么是工具循环、什么是步数上限，
//    只知道发请求收响应。这条边界是 FakeLlm 能存在的前提，而 FakeLlm 让
//    十五个测试二进制全部离线跑完，8 秒。
//
// ⚠️ 一条这一层保证不了的：cfg_ 是引用，必须活得比 client 长。构造函数
//    调用方的义务，类自己检查不了 —— 和 sandbox / loop 那两处同一类。
//
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "mini_agent/message.hpp"

namespace mini {

struct Config;

/// @brief Running token totals for a session.
///
/// @note The cache figures are the ones worth watching. A healthy long run
///       shows cache_write once or twice — at the start, and after a
///       compaction — with cache_read large on every turn after. cache_write
///       climbing every turn while cache_read stays at zero means the prefix
///       is changing and the bill is roughly ten times what it should be,
///       with nothing failing to say so.
struct Usage {
    long input_tokens = 0;    ///< Tokens sent that did not come from cache.
    long output_tokens = 0;   ///< Tokens generated.
    long cache_read = 0;      ///< Served from cache, at about a tenth the price.
    long cache_write = 0;     ///< Written into cache, at about 1.25×.
    int requests = 0;         ///< How many calls were made.

    /// @brief Add one response's usage object.
    /// @param usage_json The `usage` field of a response.
    /// @note Tolerates missing fields — the API adds them over time, and an
    ///       unknown one must not break accounting.
    void add(const Json& usage_json);

    /// @brief Every input token, cached or not.
    /// @return input_tokens + cache_read + cache_write.
    long total_input() const;

    /// @brief One line of statistics, for `/usage`.
    /// @return A human-readable summary including the cache hit rate.
    std::string summary() const;
};

/// @brief One piece of the system prompt.
///
/// @warning Every block must be byte-stable across requests. The cache matches
///          a prefix byte for byte, so a timestamp in any of them costs the
///          whole conversation after it, every turn, silently.
struct SystemBlock {
    std::string text;                ///< The prose. Never empty — empty is still bytes.
    bool cache_breakpoint = false;   ///< 打在最后一块上，一次缓存 tools + system
};

/// @brief Why a request failed, as a value rather than an exception.
///
/// @note Kept apart from a successful response so the caller can decide: 429
///       and 529 are worth retrying, 401 never is, and a network blip should
///       not end the agent loop.
struct LlmError {
    int http_status = 0;     ///< 0 = 网络层失败
    std::string type;        ///< rate_limit_error / overloaded_error / ...
    std::string message;     ///< Human-readable; surfaced to the user.
};

/// @brief One successful response.
struct LlmResponse {
    /// @brief The blocks, verbatim.
    /// @warning Thinking signatures live here and the API validates them next
    ///          turn. They must reach the history unaltered.
    std::vector<ContentBlock> content;

    /// @brief tool_use | end_turn | max_tokens | refusal.
    /// @note Only end_turn means the model finished on its own.
    std::string stop_reason;

    std::string model;   ///< Which model answered; may differ from what was asked.
    Usage usage;         ///< This call's token counts.
};

/// @brief One request to the model.
///
/// @warning `system` and `messages` are **non-owning pointers**. Whatever they
///          point at has to outlive the call — which is why Agent::run hoists
///          both out of the loop rather than building them per turn.
///
/// @note The rendering order is tools → system → messages, and the cache
///       matches a byte-exact prefix. Most stable first, most volatile last.
struct LlmRequest {
    const std::vector<SystemBlock>* system = nullptr;   ///< ⚠️ Non-owning; must outlive the call.
    const std::vector<Message>* messages = nullptr;     ///< ⚠️ Non-owning; must outlive the call.

    /// @brief The tool definitions, already sorted by name.
    /// @warning Sorted, because this sits at the very front of the request and
    ///          an unstable order invalidates the whole cache.
    Json tools = Json::array();

    std::string model;          ///< 空 = 用 cfg.model
    int max_tokens = 0;         ///< 0 = 用 cfg.max_tokens

    /// @brief Called per text chunk while streaming.
    /// @note Empty means not streaming. Agent::run then emits one TextEvent
    ///       itself — doing both prints the response twice.
    std::function<void(std::string_view)> on_text;

    /// @brief Called per thinking chunk while streaming.
    std::function<void(std::string_view)> on_thinking;
};

/// @brief The interface between the agent and a model.
///
/// @note Two implementations, and the virtual is worth it for exactly one
///       reason: FakeLlm lets every test run offline in under a second. This
///       is the highest-value abstraction in the project.
/// @note Knows nothing about agents — no tool loop, no steps. It sends a
///       request and returns a response.
/// @warning The destructor is virtual because AnthropicClient is deleted
///          through this pointer. Without it, the pimpl and the curl handle
///          leak on every session.
class LlmClient {
  public:
    virtual ~LlmClient() = default;

    /// @brief Send one request.
    ///
    /// @param req What to send.
    /// @return The response, or why it failed.
    ///
    /// @warning **Failure is a value, not an exception.** 429 and 529 are
    ///          retryable and a network blip is routine; throwing would end
    ///          the agent loop over something the caller could have handled.
    virtual std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) = 0;

    /// @brief Running totals for this client.
    /// @return Token counts accumulated across every call.
    virtual const Usage& usage() const = 0;
};

// ---------------------------------------------------------------------------
/// 真实客户端。src/llm.cpp + src/http.cpp
///
/// TODO(Stage 1):
///   - build_body(req) -> Json         （纯函数，好测；先写这个）
///   - 非流式：POST → 解析 JSON → LlmResponse
///   - 流式：SSE 回调里逐行解析，最后拼出完整响应
///
/// libcurl 的坑：写回调是 C 函数指针，要把 this 通过 userdata 传进去：
///   static size_t on_write(char* p, size_t n, size_t m, void* self)
///       { return static_cast<Impl*>(self)->consume({p, n*m}); }
/// SSE 的坑：一次回调不保证是完整一行，必须自己缓冲、按 '\n' 切。
// ---------------------------------------------------------------------------
class AnthropicClient final : public LlmClient {
  public:
    /// @brief Build a client.
    /// @param cfg ⚠️ Held by reference; must outlive the client.
    /// @note The API key comes from ANTHROPIC_API_KEY, read at request time
    ///       rather than stored — one fewer copy of a secret in memory.
    explicit AnthropicClient(const Config& cfg);
    ~AnthropicClient() override;

    std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) override;

    /// @brief Running totals for this client.
    /// @return Token counts accumulated across every call.
    const Usage& usage() const override { return usage_; }

    /// @brief Render a request into the JSON body.
    ///
    /// @param cfg    Supplies the defaults a request left unset.
    /// @param req    What to send.
    /// @param stream Whether to ask for SSE.
    /// @return The complete request body.
    ///
    /// @note Static and pure, so the request shape can be tested without a
    ///       network call — which is where the ordering that keeps the cache
    ///       alive gets verified.
    static Json build_body(const Config& cfg, const LlmRequest& req, bool stream);

  private:
    const Config& cfg_;
    Usage usage_;
    struct Impl;                 // pimpl：把 curl.h 关在 .cpp 里，别污染头文件
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
/// 【Stage 1 最高 ROI 的 80 行】按剧本回放的假模型。
///
/// 先写这个，再写真的。有了它：没有 API key 也能跑通整条链路，测试全靠它。
///
///   FakeLlm llm(cfg, {
///       {{ FakeBlock::tool("read", {{"path","hello.py"}}) }, "tool_use"},
///       {{ FakeBlock::text("这是一个打招呼函数") },          "end_turn"},
///   });
// ---------------------------------------------------------------------------
/// @brief One block in a scripted response.
/// @note Either text or a tool call, never both — `tool_name` being empty is
///       what distinguishes them.
struct FakeBlock {
    std::string text;                 ///< Set for a text block.
    std::string tool_name;            ///< Set for a tool call; empty means text.
    Json input = Json::object();      ///< Arguments, for a tool call.

    /// @brief Build a text block.
    /// @param t The text.
    /// @return The block.
    static FakeBlock text_block(std::string t);

    /// @brief Build a tool call.
    /// @param name  Which tool.
    /// @param input Its arguments.
    /// @return The block. The id is generated by FakeLlm::complete, which keeps
    ///         ids unique even among several tools in one turn.
    static FakeBlock tool(std::string name, Json input);
};

/// @brief A model that replays a script.
///
/// @warning When the script runs out it returns a fallback text response
///          rather than failing. A test that needs a failed request has to
///          supply its own client — this one cannot model that path, which is
///          the one where a mistake destroys data.
///
/// @note Records every call, so a test can assert on what was *sent*: the
///       byte-stability of the system prompt, the tool ordering, whether
///       compaction asked for the right things.
class FakeLlm final : public LlmClient {
  public:
    /// @brief One scripted response.
    struct Turn {
        std::vector<FakeBlock> blocks;   ///< What the response contains.
        std::string stop_reason;         ///< tool_use | end_turn | ...
    };

    /// @brief Build a scripted client.
    /// @param cfg    ⚠️ Held by reference; must outlive the client.
    /// @param script Responses to hand back, in order.
    FakeLlm(const Config& cfg, std::vector<Turn> script);

    std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) override;

    /// @brief Running totals; requests are counted, tokens are not simulated.
    /// @return The accumulated usage.
    const Usage& usage() const override { return usage_; }

    /// @brief Everything that was sent, one entry per call.
    /// @return system, messages and tools as they went out.
    /// @note This is what lets a test assert on the request rather than the
    ///       response — the byte-stable system prompt, the sorted tools, the
    ///       compaction instruction.
    const std::vector<Json>& calls() const { return calls_; }

    /// @brief Insert a response at the front of the remaining script.
    /// @param turn What to return next.
    /// @note Sub-agent tests need this: the parent's script is already
    ///       running when the sub-agent's response has to be arranged.
    void push_front(Turn turn);

  private:
    const Config& cfg_;
    std::vector<Turn> script_;
    std::vector<Json> calls_;
    Usage usage_;
    int counter_ = 0;             // 生成唯一的 tool_use id
};

}  // namespace mini
