cmake_minimum_required(VERSION 3.16)
project(minisql C)

# C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 默认 Debug 构建，方便断点调试
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# 打开所有常用警告
add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# 核心库：所有业务代码都在这里，main 和测试都链接它
# ---------------------------------------------------------------------------
add_library(minisql_core
    src/common.c
    src/pager.c
    src/db.c
)
target_include_directories(minisql_core PUBLIC include)

# ---------------------------------------------------------------------------
# 命令行入口
# ---------------------------------------------------------------------------
add_executable(minisql src/main.c)
target_link_libraries(minisql PRIVATE minisql_core)

# ---------------------------------------------------------------------------
# 单元测试（后续每加一个模块就在这里挂一个测试）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_pager tests/test_pager.c)
target_link_libraries(test_pager PRIVATE minisql_core)
add_test(NAME pager COMMAND test_pager)
