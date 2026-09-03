#pragma once
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
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "mini_agent/message.hpp"

namespace mini {

struct Config;

struct Usage {
    long input_tokens = 0;
    long output_tokens = 0;
    long cache_read = 0;
    long cache_write = 0;
    int requests = 0;

    void add(const Json& usage_json);
    long total_input() const;
    std::string summary() const;   // 一行统计，给 /usage 用
};

struct SystemBlock {
    std::string text;
    bool cache_breakpoint = false;   // 打在最后一块上，一次缓存 tools + system
};

struct LlmError {
    int http_status = 0;     // 0 = 网络层失败
    std::string type;        // rate_limit_error / overloaded_error / ...
    std::string message;
};

struct LlmResponse {
    std::vector<ContentBlock> content;
    std::string stop_reason;
    std::string model;
    Usage usage;
};

struct LlmRequest {
    const std::vector<SystemBlock>* system = nullptr;
    const std::vector<Message>* messages = nullptr;
    Json tools = Json::array();
    std::string model;          // 空 = 用 cfg.model
    int max_tokens = 0;         // 0 = 用 cfg.max_tokens
    std::function<void(std::string_view)> on_text;
    std::function<void(std::string_view)> on_thinking;
};

class LlmClient {
  public:
    virtual ~LlmClient() = default;
    /// 失败是**值**不是异常 —— 429/529 要能重试，网络抖动不该炸掉 agent 循环
    virtual std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) = 0;
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
    explicit AnthropicClient(const Config& cfg);
    ~AnthropicClient() override;

    std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) override;
    const Usage& usage() const override { return usage_; }

    /// 纯函数，单独暴露出来是为了能单测（不用发网络请求）
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
struct FakeBlock {
    std::string text;      // 二选一
    std::string tool_name;
    Json input = Json::object();

    static FakeBlock text_block(std::string t);
    static FakeBlock tool(std::string name, Json input);
};

class FakeLlm final : public LlmClient {
  public:
    struct Turn {
        std::vector<FakeBlock> blocks;
        std::string stop_reason;
    };

    FakeLlm(const Config& cfg, std::vector<Turn> script);

    std::expected<LlmResponse, LlmError> complete(const LlmRequest& req) override;
    const Usage& usage() const override { return usage_; }

    /// 测试要断言"第几轮发了什么"
    const std::vector<Json>& calls() const { return calls_; }
    void push_front(Turn turn);   // 子 agent 测试要往剧本中间插

  private:
    const Config& cfg_;
    std::vector<Turn> script_;
    std::vector<Json> calls_;
    Usage usage_;
    int counter_ = 0;             // 生成唯一的 tool_use id
};

}  // namespace mini
