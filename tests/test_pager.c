/*
 * tests/test_pager.c —— pager 层单元测试
 *
 * 覆盖范围（只测已实现的接口）：
 *   pager_open / pager_close / pager_get / pager_unpin / pager_flush
 *   page_data / pager_page_count
 *
 * 未实现，故不测：pager_alloc / pager_free
 * 需要多页文件时，用 make_db_with_pages() 直接在底下铺 N 页全零，
 * 绕开尚未实现的 pager_alloc。
 *
 * 数据库文件用 /tmp 下的绝对路径，这样 ctest 在 build/ 目录下运行也能找到。
 */
#include "minisql/pager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define DB_PATH "/tmp/minisql_test_pager.tmp"

/* ------------------------------------------------------------------ */
/* 极简断言框架：不中断执行，累计通过/失败，最后统一报告                */
/* ------------------------------------------------------------------ */
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                 \
    } while (0)

#define CHECK_EQ(actual, expected)                                        \
    do {                                                                  \
        long long _a = (long long)(actual);                               \
        long long _e = (long long)(expected);                             \
        if (_a == _e) {                                                   \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            printf("  [FAIL] %s:%d  %s == %s   (got %lld, want %lld)\n",  \
                   __FILE__, __LINE__, #actual, #expected, _a, _e);       \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* 测试辅助                                                            */
/* ------------------------------------------------------------------ */

/* 删掉测试用数据库文件 */
static void db_reset(void)
{
    remove(DB_PATH);
}

/* 直接在文件系统上铺 n 页全零，绕过尚未实现的 pager_alloc */
static int make_db_with_pages(uint32_t n)
{
    db_reset();

    int fd = open(DB_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("make_db_with_pages: open");
        return -1;
    }

    static const uint8_t zero_page[MINISQL_PAGE_SIZE];

    for (uint32_t i = 0; i < n; i++) {
        ssize_t w = pwrite(fd, zero_page, MINISQL_PAGE_SIZE,
                           (off_t)i * MINISQL_PAGE_SIZE);
        if (w != MINISQL_PAGE_SIZE) {
            perror("make_db_with_pages: pwrite");
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

/* 把整页填成同一个字节 */
static void fill_page(minisql_page *pg, uint8_t byte)
{
    memset(page_data(pg), byte, MINISQL_PAGE_SIZE);
}

/* 检查整页是否都是 byte */
static int page_is(minisql_page *pg, uint8_t byte)
{
    const uint8_t *d = page_data(pg);
    for (uint32_t i = 0; i < MINISQL_PAGE_SIZE; i++) {
        if (d[i] != byte) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 用例 1：新建库                                                      */
/* ------------------------------------------------------------------ */
static void test_open_new_db(void)
{
    puts("[1] pager_open 新建库：至少一个头页，文件恰好 4096 字节");

    db_reset();

    minisql_pager *p = NULL;
    CHECK_EQ(pager_open(DB_PATH, 8, &p), (long long)MINISQL_OK);
    CHECK(p != NULL);
    if (p == NULL) return;

    /* 页 0 是文件头页，新建库至少要有它 */
    CHECK_EQ(pager_page_count(p), 1u);

    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);

    struct stat st;
    CHECK_EQ(stat(DB_PATH, &st), 0);
    CHECK_EQ((long long)st.st_size, (long long)MINISQL_PAGE_SIZE);
}

/* ------------------------------------------------------------------ */
/* 用例 2：写 -> 标脏 -> flush -> close -> 重开 -> 校验内容             */
/* ------------------------------------------------------------------ */
static void test_write_read_roundtrip(void)
{
    puts("[2] 写入标脏 -> flush -> close -> 重开后内容仍在");

    db_reset();

    minisql_pager *p = NULL;
    minisql_page *pg = NULL;

    CHECK_EQ(pager_open(DB_PATH, 8, &p), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, MINISQL_PGNO_HEADER, &pg), (long long)MINISQL_OK);
    if (pg == NULL) return;

    fill_page(pg, 0xAB);
    CHECK_EQ(pager_unpin(p, pg, 1), (long long)MINISQL_OK);  /* dirty = 1 */
    CHECK_EQ(pager_flush(p), (long long)MINISQL_OK);
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);

    /* 重新打开，从磁盘读回来 */
    p = NULL;
    pg = NULL;
    CHECK_EQ(pager_open(DB_PATH, 8, &p), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, MINISQL_PGNO_HEADER, &pg), (long long)MINISQL_OK);
    if (pg == NULL) return;

    CHECK(page_is(pg, 0xAB));
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);  /* dirty = 0 */
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 3：缓存命中返回同一个页框                                       */
/* ------------------------------------------------------------------ */
static void test_cache_hit_same_frame(void)
{
    puts("[3] 同一页 get 两次，必须返回同一个页框指针");

    if (make_db_with_pages(2) != 0) { CHECK(0); return; }

    minisql_pager *p = NULL;
    minisql_page *a = NULL;
    minisql_page *b = NULL;

    CHECK_EQ(pager_open(DB_PATH, 8, &p), (long long)MINISQL_OK);

    CHECK_EQ(pager_get(p, 1, &a), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 1, &b), (long long)MINISQL_OK);

    CHECK(a != NULL);
    CHECK(a == b);

    CHECK_EQ(pager_unpin(p, a, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(p, b, 0), (long long)MINISQL_OK);  /* pin 计数到 0 */
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 4：参数边界                                                     */
/* ------------------------------------------------------------------ */
static void test_bounds(void)
{
    puts("[4] 参数边界：NULL 页号 / 越界页号 / NULL 指针");

    if (make_db_with_pages(4) != 0) { CHECK(0); return; }

    minisql_pager *p = NULL;
    minisql_page *pg = NULL;

    CHECK_EQ(pager_open(DB_PATH, 8, &p), (long long)MINISQL_OK);
    CHECK_EQ(pager_page_count(p), 4u);

    /* MINISQL_PGNO_NULL 是"没有页"的哨兵，不是合法页号 */
    CHECK_EQ(pager_get(p, MINISQL_PGNO_NULL, &pg), (long long)MINISQL_ERR_INVALID);

    /* 合法页号是 [0, page_count)，page_count 本身已越界 */
    CHECK_EQ(pager_get(p, 4, &pg), (long long)MINISQL_ERR_CORRUPT);
    CHECK_EQ(pager_get(p, 999, &pg), (long long)MINISQL_ERR_CORRUPT);

    /* unpin 传 NULL 必须被拦住，不能解引用 */
    CHECK_EQ(pager_unpin(p, NULL, 0), (long long)MINISQL_ERR_INVALID);

    /* 合法的边界页号 0 和 3 都应该能拿到 */
    CHECK_EQ(pager_get(p, MINISQL_PGNO_HEADER, &pg), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 3, &pg), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);

    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 5：坏文件被拒绝                                                 */
/* ------------------------------------------------------------------ */
static void test_bad_file_rejected(void)
{
    puts("[5] 文件大小不是页的整数倍 -> ERR_CORRUPT");

    db_reset();

    int fd = open(DB_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;

    CHECK_EQ(pwrite(fd, "junk", 4, 0), 4);
    close(fd);

    /* 哨兵值：验证失败时不会写 *pager_out */
    minisql_pager *p = (minisql_pager *)0x1;

    CHECK_EQ(pager_open(DB_PATH, 4, &p), (long long)MINISQL_ERR_CORRUPT);
    CHECK(p == (minisql_pager *)0x1);
}

/* ------------------------------------------------------------------ */
/* 用例 6：NULL 入参                                                    */
/* ------------------------------------------------------------------ */
static void test_null_args(void)
{
    puts("[6] pager_close(NULL) / pager_flush(NULL) 不崩");

    CHECK_EQ(pager_close(NULL), (long long)MINISQL_OK);
    CHECK_EQ(pager_flush(NULL), (long long)MINISQL_ERR_IO);
}

/* ------------------------------------------------------------------ */
/* 用例 7：pin 计数（需要驱逐路径，页框数 == 1）                        */
/* ------------------------------------------------------------------ */
static void test_pin_count(void)
{
    puts("[7] pin 计数：get 两次后必须 unpin 两次才能被驱逐");

    /* 需要页 0/1/2 都存在，所以要 3 页 */
    if (make_db_with_pages(3) != 0) { CHECK(0); return; }

    minisql_pager *p = NULL;
    minisql_page *a = NULL;
    minisql_page *b = NULL;
    minisql_page *c = NULL;

    /* 只给 1 个页框：取第 2 页时必然要走驱逐路径 */
    CHECK_EQ(pager_open(DB_PATH, 1, &p), (long long)MINISQL_OK);

    CHECK_EQ(pager_get(p, 1, &a), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 1, &b), (long long)MINISQL_OK);   /* pin == 2 */
    CHECK(a == b);

    CHECK_EQ(pager_unpin(p, a, 0), (long long)MINISQL_OK);  /* pin == 1：仍不可驱逐 */
    CHECK_EQ(pager_get(p, 2, &c), (long long)MINISQL_ERR_CACHE_FULL);

    CHECK_EQ(pager_unpin(p, b, 0), (long long)MINISQL_OK);  /* pin == 0：可驱逐了 */
    CHECK_EQ(pager_get(p, 2, &c), (long long)MINISQL_OK);   /* 这次应成功 */

    CHECK_EQ(pager_unpin(p, c, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 8：页框全被 pin 住 -> CACHE_FULL，且状态不变                    */
/* ------------------------------------------------------------------ */
static void test_cache_full(void)
{
    puts("[8] 页框全被 pin 住 -> ERR_CACHE_FULL，且不破坏已有页");

    if (make_db_with_pages(4) != 0) { CHECK(0); return; }

    minisql_pager *p = NULL;
    minisql_page *a = NULL;
    minisql_page *b = NULL;
    minisql_page *c = NULL;

    CHECK_EQ(pager_open(DB_PATH, 2, &p), (long long)MINISQL_OK);  /* 2 个页框 */

    CHECK_EQ(pager_get(p, 1, &a), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 2, &b), (long long)MINISQL_OK);

    /* 两个页框都被 pin 住，无法驱逐 */
    CHECK_EQ(pager_get(p, 3, &c), (long long)MINISQL_ERR_CACHE_FULL);
    CHECK(c == NULL);   /* 失败时不应写出垃圾指针 */

    /* 失败不改状态：a / b 依然有效，数据没被破坏 */
    fill_page(a, 'X');
    fill_page(b, 'Y');
    CHECK(page_is(a, 'X'));
    CHECK(page_is(b, 'Y'));

    CHECK_EQ(pager_unpin(p, a, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(p, b, 0), (long long)MINISQL_OK);

    /* 有可驱逐页框后，同一请求应当成功 */
    CHECK_EQ(pager_get(p, 3, &c), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(p, c, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 9：LRU 驱逐 + 脏页写回                                          */
/* ------------------------------------------------------------------ */
static void test_lru_evict_and_writeback(void)
{
    puts("[9] LRU 驱逐：脏页必须先写回磁盘，之后重读内容不丢");

    if (make_db_with_pages(4) != 0) { CHECK(0); return; }

    minisql_pager *p = NULL;
    minisql_page *pg = NULL;

    /* 1 个页框：每次换页都会驱逐上一个 */
    CHECK_EQ(pager_open(DB_PATH, 1, &p), (long long)MINISQL_OK);

    CHECK_EQ(pager_get(p, 1, &pg), (long long)MINISQL_OK);
    fill_page(pg, 'A');
    CHECK_EQ(pager_unpin(p, pg, 1), (long long)MINISQL_OK);   /* 脏页 */

    /* 取第 2 页 -> 必须把脏页 1 写回磁盘后才能复用页框 */
    CHECK_EQ(pager_get(p, 2, &pg), (long long)MINISQL_OK);
    fill_page(pg, 'B');
    CHECK_EQ(pager_unpin(p, pg, 1), (long long)MINISQL_OK);

    /* 再取第 1 页 -> 磁盘上应该是 'A'，说明驱逐时确实写回了 */
    CHECK_EQ(pager_get(p, 1, &pg), (long long)MINISQL_OK);
    CHECK(page_is(pg, 'A'));
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);

    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);

    /* 换一个 pager 再确认一次，排除"只是留在内存里"的可能 */
    p = NULL;
    pg = NULL;
    CHECK_EQ(pager_open(DB_PATH, 1, &p), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 1, &pg), (long long)MINISQL_OK);
    CHECK(page_is(pg, 'A'));
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_get(p, 2, &pg), (long long)MINISQL_OK);
    CHECK(page_is(pg, 'B'));
    CHECK_EQ(pager_unpin(p, pg, 0), (long long)MINISQL_OK);
    CHECK_EQ(pager_close(p), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    /* 不碰驱逐路径的先跑，这样即使后面崩了也能看到前面结果 */
    test_open_new_db();             /* 1 */
    test_write_read_roundtrip();    /* 2 */
    test_cache_hit_same_frame();    /* 3 */
    test_bounds();                  /* 4 */
    test_bad_file_rejected();       /* 5 */
    test_null_args();               /* 6 */

    /* 以下用例会走 frame_acquire 的驱逐分支 */
    test_pin_count();               /* 7 */
    test_cache_full();              /* 8 */
    test_lru_evict_and_writeback(); /* 9 */

    db_reset();

    printf("\n===== %d passed, %d failed =====\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf(">>> 有用例失败 <<<\n");
    }

    return g_fail != 0 ? 1 : 0;
}
