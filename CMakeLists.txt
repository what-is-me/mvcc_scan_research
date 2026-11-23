cmake_minimum_required(VERSION 3.20)

project(mvcc_scan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 实验要跑性能，同时保留符号便于定位；显式给 -DCMAKE_BUILD_TYPE 可覆盖。
# 断言按 NDEBUG 区分，需要断言时用 Debug 构建。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

set(MVCC_SANITIZER "" CACHE STRING
    "启用 sanitizer：thread / address / undefined，留空关闭")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(mvcc_options INTERFACE)
target_compile_features(mvcc_options INTERFACE cxx_std_20)
target_compile_definitions(mvcc_options INTERFACE MVCC_BUILD_TYPE="$<CONFIG>")

if(MSVC)
  # /utf-8 不能省：源码里有中文注释，缺了它会按本地代码页解析。
  target_compile_options(mvcc_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(mvcc_options INTERFACE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
  # MinGW 运行时 DLL（libstdc++ 等）按 PATH 解析，PATH 上若有别的发行版
  # （如 Strawberry Perl）会遮蔽正确版本，启动时报 ENTRYPOINT_NOT_FOUND。
  # 全静态链接让 exe 自包含，不依赖 PATH；实验机不用配置任何环境。
  # 只对 MinGW 生效：Linux 上 -static 与 sanitizer 不兼容。
  if(MINGW)
    target_link_options(mvcc_options INTERFACE -static)
  endif()
endif()

# TSan 在 Windows 上没有可用运行时，并发测试要到 Linux/WSL 下用同一份脚本
# 构建：-DMVCC_SANITIZER=thread。MSVC 只有 address sanitizer。
if(MVCC_SANITIZER)
  # sanitizer 种类暴露成宏（MVCC_SANITIZER_THREAD 等），测试据此调整插桩
  # 敏感的计时窗口（TSan 有 15~20 倍减速，见 AGENTS.md §12）。
  string(TOUPPER "${MVCC_SANITIZER}" MVCC_SANITIZER_UPPER)
  target_compile_definitions(mvcc_options INTERFACE MVCC_SANITIZER_${MVCC_SANITIZER_UPPER})
  if(MSVC)
    if(NOT MVCC_SANITIZER STREQUAL "address")
      message(FATAL_ERROR "MSVC 只支持 address sanitizer；${MVCC_SANITIZER} 需要 GCC 或 Clang。")
    endif()
    target_compile_options(mvcc_options INTERFACE /fsanitize=address)
    target_link_options(mvcc_options INTERFACE /fsanitize=address)
  else()
    target_compile_options(mvcc_options INTERFACE
        -fsanitize=${MVCC_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(mvcc_options INTERFACE -fsanitize=${MVCC_SANITIZER})
  endif()
endif()

# CONFIGURE_DEPENDS 让新增或删除 .cpp 时自动重新配置，不用改本文件。
# main.cpp 是实验入口，不进核心库（否则 mvcc_tests 也会链进一个 main）。
file(GLOB_RECURSE MVCC_CORE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER MVCC_CORE_SOURCES EXCLUDE REGEX "/src/main/main\\.cpp$")
list(SORT MVCC_CORE_SOURCES)

if(MVCC_CORE_SOURCES)
  add_library(mvcc_core STATIC ${MVCC_CORE_SOURCES})
  target_include_directories(mvcc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(mvcc_core PUBLIC mvcc_options)
else()
  # 还没有实现文件时退化为头文件库，出现第一个 src 下的 .cpp 后自动变成静态库。
  add_library(mvcc_core INTERFACE)
  target_include_directories(mvcc_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(mvcc_core INTERFACE mvcc_options)
endif()

# 实验入口：读配置、装配 Env、跑一轮、写 JSON。
add_executable(mvcc_exp src/main/main.cpp)
target_link_libraries(mvcc_exp PRIVATE mvcc_core)

file(GLOB_RECURSE MVCC_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
if(NOT MVCC_TEST_SOURCES)
  message(FATAL_ERROR "tests/ 下没有 .cpp，缺少测试入口。")
endif()

find_package(GTest CONFIG REQUIRED)

add_executable(mvcc_tests ${MVCC_TEST_SOURCES})
target_compile_definitions(mvcc_tests PRIVATE MVCC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mvcc_tests PRIVATE mvcc_core GTest::gtest)

enable_testing()
include(GoogleTest)
gtest_discover_tests(mvcc_tests
    DISCOVERY_TIMEOUT 60
    PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

message(STATUS "构建目录: ${CMAKE_BINARY_DIR}")
message(STATUS "编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}，"
               "构建类型: ${CMAKE_BUILD_TYPE}，sanitizer: ${MVCC_SANITIZER}")
