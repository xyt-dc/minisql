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
# 可选的运行时检查（Sanitizer）
#   开启方式：cmake -B build-asan -DMINISQL_SANITIZE=ON
#   必须用 Debug 构建：ASan 与 -O2 同时开时，行号有时会偏，也更容易被内联干扰。
#   本项目踩过的最典型的坑就是「页内字段指针强转 -> 非对齐访问 UB」，
#   UBSan 能当场抓到，不用等到 ARM 上 SIGBUS 才发现。
# ---------------------------------------------------------------------------
option(MINISQL_SANITIZE "启用 ASan + UBSan 运行时检查" OFF)
if(MINISQL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# benchmark 需要优化才能出有意义的数字。
#   默认 Debug 下 main 会打印 "数字不可用于简历" 的警告；
#   想测真实性能：cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
# ---------------------------------------------------------------------------
option(MINISQL_BENCH_O2 "即使 Debug 构建也给 benchmark 加 -O2" OFF)

# ---------------------------------------------------------------------------
# 核心库：所有业务代码都在这里，main 和测试都链接它
# ---------------------------------------------------------------------------
add_library(minisql_core
    src/common.c
    src/pager.c
    src/db.c
    src/btree.c
)
target_include_directories(minisql_core PUBLIC include)

# ---------------------------------------------------------------------------
# 命令行入口
# ---------------------------------------------------------------------------
add_executable(minisql src/main.c)
target_link_libraries(minisql PRIVATE minisql_core)
if(MINISQL_BENCH_O2)
    target_compile_options(minisql PRIVATE -O2)
endif()

# ---------------------------------------------------------------------------
# 单元测试（后续每加一个模块就在这里挂一个测试）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_pager tests/test_pager.c)
target_link_libraries(test_pager PRIVATE minisql_core)
add_test(NAME pager COMMAND test_pager)

add_executable(test_btree tests/test_btree.c)
target_link_libraries(test_btree PRIVATE minisql_core)
add_test(NAME btree COMMAND test_btree)

add_executable(test_db tests/test_db.c)
target_link_libraries(test_db PRIVATE minisql_core)
add_test(NAME db COMMAND test_db)
