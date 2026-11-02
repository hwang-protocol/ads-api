# Ads Campaign & Budget Service
# 两种构建:
#   主机 (macOS): 只有内存存储 → 单元测试 + 用 --memory 跑完整 HTTP 服务
#   Docker (Linux): -DADS_WITH_AWS=ON → 额外编译 DynamoDB (aws-sdk-cpp) 和 PostgreSQL (libpq) 实现
# 可以单独构建 (Dockerfile 里)，也可以作为上一级 CMake 项目的子目录构建
cmake_minimum_required(VERSION 3.24)
if(PROJECT_IS_TOP_LEVEL OR NOT DEFINED PROJECT_NAME)
    project(ads_campaign_budget CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # 给 VS Code / clangd: 每个 .cpp 的真实编译参数
    add_compile_options(-Wall -Wextra)
    enable_testing()
    find_package(Threads REQUIRED)
    if(NOT TARGET common)
        add_library(common INTERFACE)
        target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../common/include)
        target_link_libraries(common INTERFACE Threads::Threads)
    endif()
endif()

option(ADS_WITH_AWS "Build DynamoDB + PostgreSQL adapters (needs aws-sdk-cpp and libpq)" OFF)

# 第三方: 两个 header-only 库，固定版本 + 校验哈希 (供应链安全: 下载内容被篡改会直接构建失败)
include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.59.0.tar.gz
    URL_HASH SHA256=7c8cc7df044abb837d75f7c1e56333305acb40335c3788b8f5fbf5927e63e148
    SOURCE_SUBDIR no_cmake)   # 只要头文件，不执行它自己的 CMakeLists
FetchContent_MakeAvailable(nlohmann_json httplib)

add_library(ads_core STATIC src/model.cpp src/memory_store.cpp src/budget_service.cpp src/api.cpp)
target_include_directories(ads_core PUBLIC include ${httplib_SOURCE_DIR})
target_link_libraries(ads_core PUBLIC common nlohmann_json::nlohmann_json Threads::Threads)

set(ADS_MAIN_SOURCES src/main.cpp)
if(ADS_WITH_AWS)
    find_package(AWSSDK REQUIRED COMPONENTS dynamodb)
    find_package(PostgreSQL REQUIRED)
    add_library(ads_aws STATIC src/dynamo_store.cpp src/pg_ledger.cpp)
    target_link_libraries(ads_aws PUBLIC ads_core ${AWSSDK_LINK_LIBRARIES} PostgreSQL::PostgreSQL)
    target_compile_definitions(ads_aws PUBLIC ADS_WITH_AWS=1)
    set(ADS_LIBS ads_aws)
else()
    set(ADS_LIBS ads_core)
endif()

add_executable(ads_api src/main.cpp)
target_link_libraries(ads_api PRIVATE ${ADS_LIBS})

add_executable(budget_bench src/bench_main.cpp)
target_link_libraries(budget_bench PRIVATE ${ADS_LIBS})

add_executable(ads_tests tests/ads_tests.cpp)
target_link_libraries(ads_tests PRIVATE ads_core)
add_test(NAME ads_tests COMMAND ads_tests)
