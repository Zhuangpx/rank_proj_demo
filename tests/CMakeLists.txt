# GoogleTest 按三级顺序找，用哪一级会在配置阶段打印出来：
#   1. 系统里已装好的 GTest
#   2. -DRANK_GTEST_FETCH=ON 时用 FetchContent 去下载（需要联网）
#   3. 都不行就用 tests/gtest_lite 里自带的那份替身
# 测试源码只用标准 GoogleTest 的写法，三级之间换来换去不需要改一行测试代码。
option(RANK_GTEST_FETCH "联网下载 googletest" OFF)

set(RANK_GTEST_TARGET "")

find_package(GTest QUIET)
if(GTest_FOUND)
  set(RANK_GTEST_TARGET GTest::gtest)
  message(STATUS "GoogleTest: 用系统已安装的版本")
endif()

if(NOT RANK_GTEST_TARGET AND RANK_GTEST_FETCH)
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set(RANK_GTEST_TARGET gtest)
  message(STATUS "GoogleTest: 用 FetchContent 下载的版本")
endif()

if(NOT RANK_GTEST_TARGET)
  add_library(gtest_lite INTERFACE)
  target_include_directories(gtest_lite INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/gtest_lite)
  set(RANK_GTEST_TARGET gtest_lite)
  message(STATUS "GoogleTest: 用自带的 tests/gtest_lite（没装也没下载到）")
endif()

add_executable(rank_tests
  test_main.cpp
  test_a_submit.cpp
  test_b_query.cpp
  test_c_flush.cpp
  test_d_persist.cpp
  test_e_concurrency.cpp
  test_f_config.cpp
)
target_link_libraries(rank_tests PRIVATE rank ${RANK_GTEST_TARGET} Threads::Threads)
# tools/ 里的 ops_scan.h 是对账用的独立实现：它只按字节读文件，不碰引擎内部，
# 所以用它来比对才算两条独立的路径。
target_include_directories(rank_tests PRIVATE ${CMAKE_SOURCE_DIR}/tools)
# 两个外部程序的真实路径在编译期塞进去，测试不用去猜构建目录长什么样。
target_compile_definitions(rank_tests PRIVATE
  RANK_CRASH_HELPER_EXE="$<TARGET_FILE:rank_crash_helper>"
  RANK_BENCH_EXE="$<TARGET_FILE:rank_bench>")
add_dependencies(rank_tests rank_crash_helper rank_bench)

if(MSVC)
  target_compile_options(rank_tests PRIVATE /utf-8)
endif()

# 整个套件一条 ctest；按模块过滤直接用 rank_tests --gtest_filter=D_Persist.*
add_test(NAME rank_tests COMMAND rank_tests)
set_tests_properties(rank_tests PROPERTIES TIMEOUT 900)
