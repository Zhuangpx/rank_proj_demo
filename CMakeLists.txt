cmake_minimum_required(VERSION 3.16)
project(rank_proj_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

option(RANK_BUILD_TESTS "编译测试" ON)
option(RANK_BUILD_TOOLS "编译对账工具和崩溃注入程序" ON)
option(RANK_BUILD_BENCH "编译压测程序" ON)

if(RANK_BUILD_TESTS AND (NOT RANK_BUILD_TOOLS OR NOT RANK_BUILD_BENCH))
  message(FATAL_ERROR
    "测试要拉起 rank_crash_helper（崩溃用例）和 rank_bench（同种子复现用例），"
    "开着 RANK_BUILD_TESTS 就不能关掉 RANK_BUILD_TOOLS 或 RANK_BUILD_BENCH")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(MSVC)
  # /utf-8：源文件里有中文注释和中文字符串，不加的话 MSVC 会按本地代码页猜。
  add_compile_options(/utf-8 /W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

if(MINGW)
  # 必须静态链接运行时。机器上往往还装着 Git 自带的 mingw64，它的
  # libstdc++-6.dll / libwinpthread-1.dll 会先于编译器自己的被 PATH 找到；
  # 一旦混进来，exe 里内联的 win32 线程版 mutex/condition_variable/semaphore
  # 和 DLL 里 posix 线程版的对不上，表现就是信号量 release 了但 acquire 不醒。
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ---- 引擎本体 ----
add_library(rank STATIC
  src/util.cpp
  src/order_store.cpp
  src/staging.cpp
  src/op_log.cpp
  src/rank_store.cpp
  src/flusher.cpp
  src/recovery.cpp
  src/engine.cpp
)
target_include_directories(rank
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(rank PUBLIC Threads::Threads)

# ---- 压测入口 ----
if(RANK_BUILD_BENCH)
  add_executable(rank_bench bench/bench.cpp)
  target_link_libraries(rank_bench PRIVATE rank)
endif()

# ---- 工具 ----
if(RANK_BUILD_TOOLS)
  add_executable(rank_check tools/rank_check.cpp)
  target_include_directories(rank_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)

  add_executable(rank_crash_helper tools/crash_helper.cpp)
  target_link_libraries(rank_crash_helper PRIVATE rank)

  # 多用户并发买退的演示，按事件写日志，不参与测试
  add_executable(rank_demo tools/rank_demo.cpp)
  target_link_libraries(rank_demo PRIVATE rank)
endif()

# ---- 测试 ----
if(RANK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
