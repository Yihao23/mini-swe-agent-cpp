cmake_minimum_required(VERSION 3.24)
project(mini_agent LANGUAGES CXX)

# C++23：这个项目刻意用了三个 23 的特性
#   std::expected  —— "失败是值不是异常"，正好对上 agent 的设计
#   std::format    —— 拼 prompt 和渲染时省掉一堆 ostringstream
#   std::jthread   —— 后台任务的输出泵，析构自动 join + stop_token
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # 给 clangd 用

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# 依赖：只有两个，而且都被隔离在很薄的一层后面
#   nlohmann/json —— 只在 json.hpp 里 typedef 一次，换掉只改一个文件
#   libcurl       —— 只有 src/llm.cpp 和 src/http.cpp 用到
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(CURL)
if(NOT CURL_FOUND)
  message(WARNING
    "没找到 libcurl 开发头文件 —— 真实 API 调用会被禁用（FakeLlm 和全部测试仍可用）。\n"
    "  装上：sudo apt install libcurl4-openssl-dev")
endif()

# ---------------------------------------------------------------------------
# 核心库。src/ 下的 .cpp 用 GLOB 收集：你每写一个新文件，重跑 cmake 就进来了。
# （GLOB 在生产项目里是反模式，这里图的是"边写边加文件"不用改构建脚本。）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MINI_AGENT_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM MINI_AGENT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(mini_agent_core ${MINI_AGENT_SOURCES})
target_include_directories(mini_agent_core PUBLIC include)
target_link_libraries(mini_agent_core PUBLIC nlohmann_json::nlohmann_json)

if(CURL_FOUND)
  target_link_libraries(mini_agent_core PRIVATE CURL::libcurl)
  target_compile_definitions(mini_agent_core PUBLIC MINI_AGENT_HAVE_CURL=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(mini_agent_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(mini-agent src/main.cpp)
target_link_libraries(mini-agent PRIVATE mini_agent_core)

# ---------------------------------------------------------------------------
# 文档测试：从头文件的 @code{.test} 块生成 tests/test_docs.cpp
#
# 生成物是**签入仓库**的，所以没装 Python 也能正常构建。装了的话，任何一个
# 头文件改动都会重新生成 —— 文档里的示例失效时，编译期或 ctest 会当场报出来，
# 而不是悄悄变成谎话。
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  file(GLOB_RECURSE DOC_TEST_INPUTS CONFIGURE_DEPENDS
       ${CMAKE_SOURCE_DIR}/include/*.hpp ${CMAKE_SOURCE_DIR}/src/*.cpp)
  add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/tests/test_docs.cpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_doc_tests.py
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_doc_tests.py ${DOC_TEST_INPUTS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "从 @code{.test} 块生成 tests/test_docs.cpp"
    VERBATIM)
else()
  message(STATUS "未找到 Python3 —— 使用签入的 tests/test_docs.cpp，不重新生成")
endif()

# ---------------------------------------------------------------------------
# 测试：tests/ 下每个 test_*.cpp 编成一个可执行文件，注册进 ctest
# ---------------------------------------------------------------------------
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE mini_agent_core)
  target_include_directories(${test_name} PRIVATE tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
