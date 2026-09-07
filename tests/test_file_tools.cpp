// write 工具，以及三个文件工具共用的参数解析加固。
//
// 最重要的一条是 subject_is_the_path：删掉 WriteTool::subject() 的 override
// 之后代码照样编译、照样跑，只是沙箱会拿**要写入的正文**去匹配权限规则 ——
// 静默失效，没有任何报错。那一条用例是唯一会红的东西。

#include "microtest.hpp"

#include "mini_agent/config.hpp"
#include "mini_agent/sandbox.hpp"
#include "mini_agent/session.hpp"
#include "mini_agent/tools/builtin.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>

using namespace mini;
namespace fs = std::filesystem;

namespace {

int g_seq = 0;

struct Fixture {
    /// workdir 的**上一级**。越界用例要往这里写，所以它得是这个 fixture 私有的：
    /// 早先版本断言的是固定路径 /tmp/escaped.txt，结果变异测试真写出一个之后，
    /// 正确代码也永远过不了那条 —— 断言依赖了全局文件系统状态。
    fs::path root;
    Config cfg;
    Session session;
    std::unique_ptr<Sandbox> sandbox;
    ToolContext ctx;
    ToolPtr write = make_write_tool();
    ToolPtr read = make_read_tool();

    Fixture() {
        root = fs::temp_directory_path() /
               ("file_tools_" + std::to_string(::getpid()) + "_" + std::to_string(++g_seq));
        fs::remove_all(root);
        fs::create_directories(root / "work");
        cfg.workdir = fs::canonical(root / "work");
        root = fs::canonical(root);
        cfg.permission_mode = PermissionMode::Yolo;
        cfg.normalize();
        sandbox = std::make_unique<Sandbox>(cfg);
        ctx.cfg = &cfg;
        ctx.sandbox = sandbox.get();
        ctx.session = &session;
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }

    ToolResult do_write(Json a) { return write->run(a, ctx); }
    ToolResult do_read(Json a) { return read->run(a, ctx); }
    std::string slurp(const std::string& rel) {
        std::ifstream in(cfg.workdir / rel, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), {});
    }
};

}  // namespace

// ── 唯一一条能抓住 subject() override 的用例 ────────────────────────────────
TEST(subject_is_the_path_not_the_content) {
    Fixture f;
    // nlohmann 按 key 字母序遍历，content < path。基类的默认实现会返回 content ——
    // 于是沙箱拿"要写入的正文"去匹配规则，Write(src/**) 永远命中不了。
    const Json args{{"content", "x = 1"}, {"path", "src/a.py"}};
    CHECK_MSG(f.write->subject(args) == "src/a.py",
              "必须 override subject() —— 否则沙箱审查的是正文不是路径，权限静默失效");
    CHECK_MSG(f.write->subject(args) != "x = 1", "默认实现返回的就是这个错误答案");
}

TEST(write_declares_itself_correctly) {
    Fixture f;
    CHECK(f.write->name() == "write");
    CHECK_MSG(!f.write->read_only(), "会改文件 → executor 不该并发跑它");
    CHECK_MSG(f.write->requires_permission(), "必须过权限闸");
    const auto req = f.write->input_schema().at("required");
    CHECK_MSG(req.size() == 2, "path 和 content 都是必填 —— 少一个就要多花一轮往返");
}

TEST(creates_a_new_file) {
    Fixture f;
    const auto r = f.do_write({{"path", "hello.txt"}, {"content", "hi\n"}});
    CHECK(!r.is_error);
    CHECK(f.slurp("hello.txt") == "hi\n");
    CHECK_MSG(r.metadata.at("created") == true, "新建和覆盖要能区分");
}

TEST(creates_missing_parent_directories) {
    Fixture f;
    const auto r = f.do_write({{"path", "a/b/c/deep.py"}, {"content", "x = 1\n"}});
    CHECK_MSG(!r.is_error, "父目录不存在时要自己建 —— 否则模型得先跑一条 bash mkdir");
    CHECK(f.slurp("a/b/c/deep.py") == "x = 1\n");
}

// ── 覆盖前必须先 read ───────────────────────────────────────────────────────
TEST(refuses_to_overwrite_a_file_never_read) {
    Fixture f;
    std::ofstream(f.cfg.workdir / "existing.py") << "重要代码\n";

    const auto r = f.do_write({{"path", "existing.py"}, {"content", "覆盖了"}});
    CHECK_MSG(r.is_error, "没读过就整个覆盖 = 凭想象删掉别人的代码");
    CHECK_MSG(f.slurp("existing.py") == "重要代码\n", "文件必须原封不动");
}

TEST(allows_overwrite_after_read) {
    Fixture f;
    std::ofstream(f.cfg.workdir / "existing.py") << "旧内容\n";

    CHECK(!f.do_read({{"path", "existing.py"}}).is_error);   // 先读
    const auto r = f.do_write({{"path", "existing.py"}, {"content", "新内容\n"}});
    CHECK_MSG(!r.is_error, "读过之后就该放行");
    CHECK(f.slurp("existing.py") == "新内容\n");
    CHECK(r.metadata.at("created") == false);
}

TEST(a_freshly_written_file_counts_as_read) {
    Fixture f;
    CHECK(!f.do_write({{"path", "new.py"}, {"content", "v1\n"}}).is_error);
    // 内容是我们刚写的，没有未知 —— 再写一次不该要求先 read
    const auto r = f.do_write({{"path", "new.py"}, {"content", "v2\n"}});
    CHECK_MSG(!r.is_error, "刚写完的文件应该算作读过");
    CHECK(f.slurp("new.py") == "v2\n");
}

// ── 沙箱接线 ────────────────────────────────────────────────────────────────
TEST(path_outside_the_workdir_is_refused) {
    Fixture f;
    const auto target = f.root / "escaped.txt";      // workdir 的兄弟，越界一步
    const auto r = f.do_write({{"path", "../escaped.txt"}, {"content", "x"}});
    CHECK_MSG(r.is_error, "越界路径必须挡住 —— 模型给的 path 是不可信输入");
    CHECK_MSG(!fs::exists(target), "拒绝了就不能真的把文件写出去");

    // 深一点的越界，以及一个绝对路径
    CHECK(f.do_write({{"path", "../../../etc/passwd"}, {"content", "x"}}).is_error);
    CHECK(f.do_write({{"path", "/etc/passwd"}, {"content", "x"}}).is_error);
}

TEST(writing_to_a_directory_is_refused) {
    Fixture f;
    fs::create_directories(f.cfg.workdir / "adir");
    CHECK(f.do_write({{"path", "adir"}, {"content", "x"}}).is_error);
}

// ── 参数类型加固（str_arg / int_arg）────────────────────────────────────────
TEST(wrong_argument_types_give_a_clean_error) {
    Fixture f;
    // ⚠️ args.value(key, default) 在 key 存在但类型不对时**抛异常**，不是退回默认值。
    //    schema 是给模型的提示，不是保证。
    CHECK(f.do_write({{"path", 42}, {"content", "x"}}).is_error);
    CHECK(f.do_write({{"path", "a.txt"}, {"content", 42}}).is_error);
    CHECK(f.do_write({{"path", "a.txt"}}).is_error);
    CHECK(f.do_write(Json::object()).is_error);
    CHECK(f.do_write(Json::array()).is_error);

    CHECK(f.do_read({{"path", 42}}).is_error);
    // offset/limit 类型不对时应退回默认值，而不是抛
    std::ofstream(f.cfg.workdir / "x.txt") << "a\nb\nc\n";
    CHECK(!f.do_read({{"path", "x.txt"}, {"offset", "第一行"}, {"limit", true}}).is_error);
}

TEST(empty_content_truncates_the_file) {
    Fixture f;
    CHECK(!f.do_write({{"path", "e.txt"}, {"content", "有内容\n"}}).is_error);
    CHECK_MSG(!f.do_write({{"path", "e.txt"}, {"content", ""}}).is_error,
              "清空文件是合法操作");
    CHECK(f.slurp("e.txt").empty());
}

// ── builtin_tools ───────────────────────────────────────────────────────────
TEST(builtin_tools_returns_the_stage2_set) {
    Config cfg;
    cfg.workdir = fs::temp_directory_path();
    cfg.normalize();
    ToolRegistry r;
    for (auto& t : builtin_tools(cfg)) r.add(std::move(t));

    CHECK_MSG(r.get("read") != nullptr, "read 要在");
    CHECK_MSG(r.get("write") != nullptr, "write 要在");
    CHECK_MSG(r.get("edit") != nullptr, "edit 要在");
    CHECK_MSG(r.get("bash") != nullptr, "bash 要在");
    CHECK_MSG(r.schemas().size() == r.size(), "每个工具都要出现在给 API 的表里");
}

TEST(builtin_tools_does_not_call_unimplemented_factories) {
    // enable_* 默认全 true。哪天有人手滑把 Stage 5 的工厂打开，
    // 那些还是 todo() 的函数一调就抛，默认配置下 agent 直接起不来。
    Config cfg;
    cfg.workdir = fs::temp_directory_path();
    cfg.enable_memory = cfg.enable_skills = cfg.enable_subagents = true;
    cfg.normalize();
    bool threw = false;
    try { (void)builtin_tools(cfg); } catch (...) { threw = true; }
    CHECK_MSG(!threw, "默认配置必须能拼出工具表");
}

int main() { return mt::run_all(); }
