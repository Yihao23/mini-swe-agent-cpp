// BashTool 的测试。
//
// 它自己几乎不做事 —— 主要工作是把 ProcessResult 的四种失败方式塌成
// ToolResult 的一个 is_error + 一段文字。塌错了模型就分不清
//「命令报错」「命令超时」「命令没跑起来」，而这三种下一步完全不同。
//
// 权限不在这里测：BashTool 不做任何权限判断，那是 sandbox 的事
// （见 include/mini_agent/sandbox.hpp 上 kDangerous 的可执行示例）。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <filesystem>
#include <memory>
#include <unistd.h>

using namespace mini;
namespace fs = std::filesystem;

namespace {

/// 一套能跑的最小上下文：config + sandbox，工具只用得到这两个。
struct Fixture {
    Config cfg;
    std::unique_ptr<Sandbox> sandbox;
    ToolContext ctx;
    ToolPtr bash = make_bash_tool();

    explicit Fixture(int timeout_sec = 10) {
        cfg.workdir = fs::temp_directory_path() / ("bash_tool_" + std::to_string(::getpid()));
        fs::create_directories(cfg.workdir);
        cfg.workdir = fs::canonical(cfg.workdir);
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.tool_timeout_sec = timeout_sec;
        cfg.normalize();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
    }

    ToolResult run(Json args) { return bash->run(args, ctx); }
};

}  // namespace

TEST(bash_declares_itself_correctly) {
    Fixture f;
    CHECK_MSG(f.bash->name() == "bash", "模型按这个名字调用");
    CHECK_MSG(!f.bash->read_only(), "会写文件 → executor 不该并发跑它");
    CHECK_MSG(f.bash->requires_permission(), "必须过权限闸");
    CHECK_MSG(f.bash->input_schema().at("required").at(0) == "command",
              "command 要在 required 里，否则模型可能漏给，白费一轮");
}

TEST(subject_is_the_command_itself) {
    Fixture f;
    // 沙箱拿这个字符串去拆段、匹配 kDangerous。给错了整层权限就失效。
    CHECK(f.bash->subject(Json{{"command", "git status"}}) == "git status");
    CHECK_MSG(f.bash->subject(Json{{"command", "ls"}, {"timeout_sec", 5}}) == "ls",
              "多一个参数也不能选错");

    // ⚠️ 上面两条基类的默认实现也能通过（"取字母序第一个字符串参数"，
    //    这里恰好就是 command），所以证明不了 override 有没有写。
    //    加一个字母序**排在 command 前面**的字符串参数才分得开：
    //    默认实现会返回 "闲聊"，override 返回真正要审查的命令。
    CHECK_MSG(f.bash->subject(Json{{"aaa_note", "闲聊"}, {"command", "rm -rf /"}}) == "rm -rf /",
              "必须 override —— 默认实现会把无关字段当成审查对象，沙箱静默失效");
}

TEST(successful_command_is_not_an_error) {
    Fixture f;
    const auto r = f.run({{"command", "echo hello"}});
    CHECK_MSG(!r.is_error, "退出码 0 = 成功");
    CHECK_MSG(r.content == "hello\n", "输出原样带回");
    CHECK(r.metadata.at("exit_code") == 0);
    CHECK_MSG(r.metadata.contains("duration_ms"), "耗时给 UI 用");
}

TEST(nonzero_exit_is_an_error_but_keeps_the_output) {
    Fixture f;
    const auto r = f.run({{"command", "echo 'compile failed'; exit 2"}});
    CHECK_MSG(r.is_error, "退出码非零 = 失败");
    CHECK_MSG(r.content.find("compile failed") != std::string::npos,
              "⚠️ 失败时输出更重要 —— 那正是模型下一步要读的东西");
    CHECK_MSG(r.content.find("退出码 2") != std::string::npos,
              "退出码要写进正文，模型看不到 metadata");
    CHECK(r.metadata.at("exit_code") == 2);
}

TEST(stderr_reaches_the_model) {
    Fixture f;
    const auto r = f.run({{"command", "echo boom >&2; exit 1"}});
    CHECK_MSG(r.content.find("boom") != std::string::npos, "报错基本都在 stderr 上");
}

TEST(timeout_is_an_error_and_says_so) {
    Fixture f;
    const auto r = f.run({{"command", "echo starting; sleep 30"}, {"timeout_sec", 1}});
    CHECK_MSG(r.is_error, "超时是失败");
    CHECK_MSG(r.content.find("starting") != std::string::npos, "超时前的输出要留下");
    CHECK_MSG(r.content.find("超时") != std::string::npos,
              "要能和普通的非零退出区分开 —— 两者下一步不同");
}

TEST(model_cannot_raise_the_timeout_above_the_config) {
    Fixture f(/*timeout_sec=*/1);
    // 模型说要 300 秒。给了的话它可以自己解除限制。
    const auto r = f.run({{"command", "sleep 30"}, {"timeout_sec", 300}});
    CHECK_MSG(r.is_error, "应该在 1 秒后被拦下，而不是等 300 秒");
    CHECK_MSG(r.metadata.at("duration_ms").get<long>() < 5000, "确实只等了 1 秒左右");
}

TEST(model_can_lower_the_timeout) {
    Fixture f(/*timeout_sec=*/60);
    const auto r = f.run({{"command", "sleep 20"}, {"timeout_sec", 1}});
    CHECK_MSG(r.metadata.at("duration_ms").get<long>() < 5000, "往下调应该生效");
}

TEST(missing_command_is_rejected_before_forking) {
    Fixture f;
    CHECK(f.run(Json::object()).is_error);
    CHECK(f.run({{"command", ""}}).is_error);
    CHECK_MSG(f.run({{"command", 42}}).is_error, "类型不对也不能崩 —— schema 是提示不是保证");
}

TEST(command_runs_in_the_workdir) {
    Fixture f;
    const auto r = f.run({{"command", "pwd"}});
    CHECK_MSG(r.content == f.cfg.workdir.string() + "\n",
              "跑错目录的话，agent 会去改别人的文件");
}

TEST(silent_command_gets_a_placeholder) {
    Fixture f;
    const auto r = f.run({{"command", "true"}});
    CHECK(!r.is_error);
    CHECK_MSG(!r.content.empty(), "空 content 在 API 那边是非法的，要给个占位符");
}

TEST(huge_output_is_truncated_not_fatal) {
    Fixture f;
    f.cfg.max_output_chars = 1024;
    const auto r = f.run({{"command", "echo HEAD; yes filler | head -100000"}});
    CHECK_MSG(!r.is_error, "命令自己正常结束了，截断不算失败");
    CHECK_MSG(r.content.size() < 8192, "不能把整轮上下文撑爆");
    CHECK_MSG(r.content.rfind("HEAD", 0) == 0, "保留开头");
}

int main() { return mt::run_all(); }
