//
// 工具抽象与注册表 —— 锁住模型看到的那份工具清单。
//
//     cmake --build build && ./build/test_tool
//
// 这一层的错误都很安静：get() 比错字段只是「查不到工具」，schemas() 不排序只是
// 「缓存偶尔失效」，error() 忘了置 is_error 只是「模型以为成功了」。
// 没有一个会崩，所以只能靠断言守。
//
#include <algorithm>
#include <string>
#include <vector>

#include "microtest.hpp"
#include "mini_agent/tool.hpp"

using namespace mini;

// --- 夹具 -------------------------------------------------------------------
namespace {

/// 可配置的假工具。测试只关心元数据，run() 返回什么无所谓。
struct FakeTool : Tool {
    std::string n, d;
    Json schema_;
    bool ro = false, perm = true;

    FakeTool(std::string name_, std::string desc_, bool read_only_ = false, bool perm_ = true)
        : n(std::move(name_)), d(std::move(desc_)),
          schema_(Json{{"type", "object"}, {"properties", Json::object()}}),
          ro(read_only_), perm(perm_) {}

    std::string_view name() const override { return n; }
    std::string_view description() const override { return d; }
    Json input_schema() const override { return schema_; }
    bool read_only() const override { return ro; }
    bool requires_permission() const override { return perm; }
    ToolResult run(const Json&, ToolContext&) override { return ToolResult{.content = "ok"}; }
};

/// bash 那类工具会覆盖 subject()，固定看某个字段。
struct BashLike : FakeTool {
    BashLike() : FakeTool("bash", "跑命令") {}
    std::string subject(const Json& args) const override {
        return args.value("command", std::string{});
    }
};

ToolPtr make(std::string name, std::string desc, bool ro = false) {
    return std::make_shared<FakeTool>(std::move(name), std::move(desc), ro);
}

/// 故意让注册顺序 ≠ 字母序，否则测不出 schemas() 到底有没有排。
ToolRegistry three_tools() {
    ToolRegistry r;
    r.add(make("write", "写文件"));
    r.add(make("read", "读文件", /*read_only=*/true));
    r.add(make("bash", "跑命令"));
    return r;
}

std::vector<std::string> schema_names(const ToolRegistry& r) {
    std::vector<std::string> out;
    for (const auto& s : r.schemas()) out.push_back(s.value("name", std::string{}));
    return out;
}

}  // namespace

// ===========================================================================
// ToolResult
// ===========================================================================

TEST(error_result_is_flagged_as_error) {
    // 忘了置 is_error，模型会把错误信息当成正常输出接受下来，然后基于它继续推理。
    auto r = ToolResult::error("文件不存在");
    CHECK(r.content == "文件不存在");
    CHECK_MSG(r.is_error, "error() 造出来的结果必须 is_error = true");
}

TEST(normal_result_is_not_an_error) {
    ToolResult r{.content = "内容"};
    CHECK(!r.is_error);
    CHECK_MSG(r.metadata.is_object(), "metadata 的默认值是空对象，不是 null");
}

// ===========================================================================
// Tool::schema —— 发给 API 的工具定义
// ===========================================================================

TEST(schema_carries_exactly_the_three_api_fields) {
    FakeTool t{"read", "读取文件内容"};
    const Json s = t.schema();

    CHECK(s.value("name", std::string{}) == "read");
    CHECK(s.value("description", std::string{}) == "读取文件内容");
    CHECK_MSG(s.contains("input_schema"), "input_schema 是 API 必需字段");
    CHECK_MSG(s.size() == 3, "多余字段会白白占 prompt");
}

// ===========================================================================
// Tool::subject —— 沙箱拿去做规则匹配的那个字符串
// ===========================================================================

TEST(subject_takes_the_first_string_argument) {
    FakeTool t{"read", "读文件"};
    CHECK(t.subject(Json{{"path", "a.py"}}) == "a.py");
}

TEST(subject_skips_non_string_values) {
    FakeTool t{"read", "读文件"};
    CHECK(t.subject(Json{{"limit", 100}, {"path", "a.py"}}) == "a.py");
}

TEST(subject_on_non_object_returns_empty) {
    // 模型可能给出畸形参数。沙箱拿到空串会走「匹配不上任何规则」的默认路径，
    // 比崩溃或抛异常好。
    FakeTool t{"read", "读文件"};
    CHECK(t.subject(Json::array()).empty());
    CHECK(t.subject(Json{}).empty());          // null
    CHECK(t.subject(Json::object()).empty());  // 空对象
}

TEST(default_subject_picks_by_alphabetical_key_not_insertion_order) {
    // ⚠️ nlohmann 的 items() 按 **key 字母序** 遍历，不是插入顺序。
    //    {"path":..., "content":...} 里 content 排在 path 前面，
    //    所以默认实现返回的是文件内容，不是路径 —— 沙箱会审查错对象。
    //
    //    这就是 tool.hpp:76 说「文件类覆盖成 path」的原因：
    //    **凡是有多个字符串参数的工具，都必须自己覆盖 subject()。**
    FakeTool t{"write", "写文件"};
    CHECK_MSG(t.subject(Json{{"path", "a.py"}, {"content", "写入内容"}}) == "写入内容",
              "默认实现只对单字符串参数的工具可靠");
}

TEST(subject_can_be_overridden) {
    BashLike b;
    CHECK(b.subject(Json{{"command", "rm -rf /"}}) == "rm -rf /");
    CHECK_MSG(b.subject(Json{{"timeout", 5}}).empty(), "字段缺失时返回空，不抛");
}

// ===========================================================================
// ToolRegistry —— 查找
// ===========================================================================

TEST(get_matches_name_not_description) {
    // 拿 description 去比会让每次工具查找都失败 —— 而 executor 只会报
    // 「工具不存在」，看不出真正的原因。
    auto r = three_tools();
    CHECK_MSG(r.get("read") != nullptr, "按名字必须能查到");
    CHECK(r.get("read")->description() == "读文件");
    CHECK_MSG(r.get("读文件") == nullptr, "描述文本不该被当成名字");
}

TEST(get_returns_nullptr_for_unknown_name) {
    // executor 靠这个 nullptr 走「工具不存在」分支，不能抛异常。
    auto r = three_tools();
    CHECK(r.get("nonexistent") == nullptr);
    CHECK(r.get("") == nullptr);
}

TEST(names_and_size_and_all_agree) {
    auto r = three_tools();
    CHECK(r.size() == 3);
    CHECK(r.names().size() == 3);
    CHECK(r.all().size() == 3);

    auto ns = r.names();
    CHECK(std::ranges::find(ns, "write") != ns.end());
    CHECK(std::ranges::find(ns, "read") != ns.end());
    CHECK(std::ranges::find(ns, "bash") != ns.end());
}

// ===========================================================================
// ToolRegistry::schemas —— 发给 API 的 tools 数组
// ===========================================================================

TEST(schemas_are_sorted_by_name) {
    // ⚠️ 工具定义渲染在 prompt 最前面。顺序一变，缓存前缀就失效，
    //    每一轮都要重新为整个 system + tools 付全价。
    //    注册顺序是 write/read/bash，输出必须是 bash/read/write。
    auto r = three_tools();
    const auto got = schema_names(r);
    CHECK_MSG(got == (std::vector<std::string>{"bash", "read", "write"}),
              "schemas() 必须按名字升序，不能跟着注册顺序走");
}

TEST(schemas_are_stable_across_calls) {
    // 缓存前缀要求「同样的工具集 → 逐字节相同的 JSON」。
    auto r = three_tools();
    CHECK(r.schemas().dump() == r.schemas().dump());
}

TEST(schemas_is_an_array_of_complete_entries) {
    // 曾经写成 Json::object() —— 对 object 调 push_back 直接抛 type_error.308。
    auto r = three_tools();
    const Json s = r.schemas();

    CHECK_MSG(s.is_array(), "API 的 tools 字段是数组");
    CHECK(s.size() == 3);
    for (const auto& e : s) {
        CHECK_MSG(e.contains("name"), "每一项都要有 name");
        CHECK_MSG(e.contains("description"), "每一项都要有 description");
        CHECK_MSG(e.contains("input_schema"), "只塞 input_schema 是不够的");
    }
}

TEST(empty_registry_yields_empty_array) {
    ToolRegistry r;
    CHECK(r.size() == 0);
    CHECK(r.schemas().is_array());
    CHECK(r.schemas().empty());
    CHECK(r.names().empty());
}

// ===========================================================================
// 两个布尔标记
// ===========================================================================

TEST(read_only_and_requires_permission_are_independent) {
    // tool.hpp:60 —— 一个抓 URL 的工具是 read_only（executor 敢并发）
    // 但 requires_permission（出网要过闸）。合成一个布尔就表达不了。
    FakeTool fetch{"fetch", "抓网页", /*read_only=*/true, /*perm=*/true};
    CHECK(fetch.read_only());
    CHECK(fetch.requires_permission());

    FakeTool w{"write", "写文件", /*read_only=*/false, /*perm=*/true};
    CHECK(!w.read_only());
}

TEST(defaults_are_the_safe_side) {
    // 默认「有副作用 + 要过闸」—— 新工具忘了覆盖时，错在保守一边。
    struct Bare : Tool {
        std::string_view name() const override { return "bare"; }
        std::string_view description() const override { return "d"; }
        Json input_schema() const override { return Json::object(); }
        ToolResult run(const Json&, ToolContext&) override { return {}; }
    } b;

    CHECK_MSG(!b.read_only(), "默认不是只读 → executor 不会贸然并发");
    CHECK_MSG(b.requires_permission(), "默认要过权限闸");
}

int main() { return mt::run_all(); }
