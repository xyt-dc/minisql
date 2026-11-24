/*
 * tests/test_db.c —— db 层单元测试
 *
 * db 层是「页 0 头页」的唯一解释者，也是整个项目所有崩溃路径的守门人。
 * 它对外只有 6 个接口，但 db_open 里有 4 个失败出口，每条都必须被压到测试里：
 *
 *   1. 魔数不对      -> 不是我们的文件 / 已被别的东西写坏
 *   2. version 不对  -> 老版本文件被新程序打开（或反之）
 *   3. page_count 与物理文件大小不符 -> 文件被截断 / 被外部追加
 *   4. root/freelist 页号越界 -> 头页被手工改坏
 *
 * 头页布局（src/db.c 内部宏，这里按契约硬编码，故意不共享宏）：
 *   偏移 0  8 字节 魔数 'M','I','N','I','S','Q','L','\0'
 *   偏移 8  4 字节 version          当前为 1
 *   偏移 12 4 字节 page_count
 *   偏移 16 4 字节 root_page        MINISQL_PGNO_NULL 表示空树
 *   偏移 20 4 字节 freelist_head    MINISQL_PGNO_NULL 表示空链
 *
 * 直接改磁盘字节的用例用 pwrite_raw()，绕开 pager 缓存 —— 模拟
 * 「进程没运行时文件被别的东西改坏了」这个场景，缓存里是看不到的。
 */
#include "minisql/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define DB_PATH "/tmp/minisql_test_db.tmp"

/* 头页字段偏移，与 src/db.c 的契约一致 */
#define OFF_MAGIC      0u
#define OFF_VERSION    8u
#define OFF_PAGE_COUNT 12u
#define OFF_ROOT       16u
#define OFF_FREELIST   20u

#define FORMAT_VERSION 1u

/* ------------------------------------------------------------------ */
/* 极简断言框架（与 test_pager.c / test_btree.c 保持一致）              */
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
/* 辅助                                                                */
/* ------------------------------------------------------------------ */

static void db_reset(void)
{
    remove(DB_PATH);
}

/* 直接读磁盘上的一个 u32 字段（不进 pager 缓存） */
static uint32_t read_raw_u32(uint32_t off)
{
    int fd = open(DB_PATH, O_RDONLY);
    if (fd < 0) return 0xDEADBEEFu;

    uint32_t v = 0xDEADBEEFu;
    if (pread(fd, &v, sizeof(v), (off_t)off) != (ssize_t)sizeof(v)) {
        v = 0xDEADBEEFu;
    }
    close(fd);
    return v;
}

/* 直接改磁盘上的一个 u32 字段（模拟外部破坏） */
static int write_raw_u32(uint32_t off, uint32_t v)
{
    int fd = open(DB_PATH, O_RDWR);
    if (fd < 0) return -1;

    ssize_t w = pwrite(fd, &v, sizeof(v), (off_t)off);
    close(fd);
    return (w == (ssize_t)sizeof(v)) ? 0 : -1;
}

/* 取磁盘上的魔数前 4 字节，用来判断"魔数还在不在" */
static uint32_t read_raw_magic4(void)
{
    return read_raw_u32(OFF_MAGIC);
}

static uint32_t file_page_count(void)
{
    struct stat st;
    if (stat(DB_PATH, &st) != 0) return 0;
    return (uint32_t)(st.st_size / MINISQL_PAGE_SIZE);
}

/* ================================================================== */
/* 用例 1：新建库——空白文件被初始化成合法头页                          */
/* ================================================================== */
static void test_open_new_db(void)
{
    puts("[1] 新建库：空白文件被初始化，头页写入魔数/版本/页数");

    db_reset();

    minisql_db *db = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
    if (db == NULL) return;

    /* 新库的语义：还没有 B+ 树 */
    CHECK_EQ(db_root_page(db), (long long)MINISQL_PGNO_NULL);
    CHECK_EQ(pager_freelist_head(db_pager(db)), (long long)MINISQL_PGNO_NULL);
    CHECK_EQ(pager_page_count(db_pager(db)), 1u);   /* 只有头页 */

    /* 必须真的把 pager 交出来，否则上层没法建树 */
    CHECK(db_pager(db) != NULL);

    CHECK_EQ(db_close(db), (long long)MINISQL_OK);

    /* 落盘之后磁盘上的头页必须已经写好，而不是只在内存里。
     * 前 4 字节是 'M','I','N','I'，按小端读成 u32 就是 0x494E494D。 */
    CHECK_EQ(read_raw_magic4(), 0x494E494Du);
    CHECK_EQ(read_raw_u32(OFF_VERSION),    (long long)FORMAT_VERSION);
    CHECK_EQ(read_raw_u32(OFF_PAGE_COUNT), 1u);
    CHECK_EQ(read_raw_u32(OFF_ROOT),      (long long)MINISQL_PGNO_NULL);
    CHECK_EQ(read_raw_u32(OFF_FREELIST),  (long long)MINISQL_PGNO_NULL);
}

/* ================================================================== */
/* 用例 2：重开老库——root / freelist 原样读回                          */
/* ================================================================== */
static void test_reopen_reads_back(void)
{
    puts("[2] 重开老库：root_page 与 freelist_head 原样恢复");

    db_reset();

    minisql_pgno root = 0;
    minisql_pgno freed = 0;

    /* 第一段：建库、分配两页、把第二页 free 掉、设 root，然后关 */
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db == NULL) return;

        minisql_pager *p = db_pager(db);

        minisql_page *a = NULL, *b = NULL;
        CHECK_EQ(pager_alloc(p, &root, &a),  (long long)MINISQL_OK);
        CHECK_EQ(pager_alloc(p, &freed, &b), (long long)MINISQL_OK);
        CHECK(root  >= 1u);
        CHECK(freed >= 1u);
        CHECK(root != freed);

        CHECK_EQ(pager_unpin(p, a, 1), (long long)MINISQL_OK);
        CHECK_EQ(pager_unpin(p, b, 1), (long long)MINISQL_OK);

        /* 把后分配的那页还回 freelist */
        CHECK_EQ(pager_free(p, freed), (long long)MINISQL_OK);

        /* 让 root 指向第一页 */
        CHECK_EQ(db_set_root_page(db, root), (long long)MINISQL_OK);
        CHECK_EQ(db_root_page(db), (long long)root);

        CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    /* 第二段：重开，两个字段都必须回来 */
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db == NULL) return;

        CHECK_EQ(db_root_page(db), (long long)root);
        CHECK_EQ(pager_freelist_head(db_pager(db)), (long long)freed);

        /* freelist 是 LIFO，这里只有一个元素，alloc 必须复用它 */
        minisql_pgno reused = MINISQL_PGNO_NULL;
        minisql_page *pg = NULL;
        uint32_t before = pager_page_count(db_pager(db));
        CHECK_EQ(pager_alloc(db_pager(db), &reused, &pg), (long long)MINISQL_OK);
        CHECK_EQ(reused, (long long)freed);
        CHECK_EQ(pager_page_count(db_pager(db)), (long long)before);  /* 不该扩文件 */
        CHECK_EQ(pager_unpin(db_pager(db), pg, 1), (long long)MINISQL_OK);

        CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }
}

/* ================================================================== */
/* 用例 3：db_flush 的契约——只标脏，真正落盘要靠 pager_flush           */
/* ================================================================== */
static void test_flush_contract(void)
{
    puts("[3] db_flush 契约：先标脏，pager_flush 之后才真正落盘");

    db_reset();

    minisql_db *db = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
    if (db == NULL) return;

    minisql_page *pg = NULL;
    minisql_pgno newp = MINISQL_PGNO_NULL;
    CHECK_EQ(pager_alloc(db_pager(db), &newp, &pg), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(db_pager(db), pg, 1), (long long)MINISQL_OK);
    CHECK_EQ(db_set_root_page(db, newp), (long long)MINISQL_OK);

    /* db_flush 只把头页标脏、打包进缓存，磁盘此刻还是全零 ——
     * 新建库时头页也走同一条路径，从未被提前落盘。 */
    CHECK_EQ(db_flush(db), (long long)MINISQL_OK);
    CHECK_EQ(read_raw_u32(OFF_PAGE_COUNT), 0u);      /* 还没写 */
    CHECK_EQ(read_raw_u32(OFF_ROOT), 0u);            /* 还没写 */

    /* 必须再 pager_flush 才真正落盘 */
    CHECK_EQ(pager_flush(db_pager(db)), (long long)MINISQL_OK);
    CHECK_EQ(read_raw_u32(OFF_PAGE_COUNT), 2u);
    CHECK_EQ(read_raw_u32(OFF_ROOT), (long long)newp);
    CHECK_EQ(file_page_count(), 2u);

    /* 再分配一页，同样两步走，page_count 变成 3 */
    minisql_page *pg2 = NULL;
    minisql_pgno newp2 = MINISQL_PGNO_NULL;
    CHECK_EQ(pager_alloc(db_pager(db), &newp2, &pg2), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(db_pager(db), pg2, 1), (long long)MINISQL_OK);
    CHECK_EQ(db_flush(db), (long long)MINISQL_OK);
    CHECK_EQ(pager_flush(db_pager(db)), (long long)MINISQL_OK);
    CHECK_EQ(read_raw_u32(OFF_PAGE_COUNT), 3u);

    CHECK_EQ(db_close(db), (long long)MINISQL_OK);
}

/* ================================================================== */
/* 用例 4：魔数被破坏 -> 拒绝打开                                       */
/* ================================================================== */
static void test_bad_magic_rejected(void)
{
    puts("[4] 魔数损坏：db_open 返回 CORRUPT，不静默接管");

    db_reset();

    /* 先造一个合法库 */
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db != NULL) CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    /* 把魔数首字节改成 'X' */
    CHECK_EQ(write_raw_u32(OFF_MAGIC, 0x58585858u), 0);

    minisql_db *db2 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db2), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db2 == NULL);          /* 失败时不得写出半成品指针 */

    /* 头页仍在，但被拒绝了；文件不该被"顺手修好" */
    CHECK_EQ(read_raw_magic4(), 0x58585858u);
}

/* ================================================================== */
/* 用例 5：版本号不匹配 -> 拒绝打开                                     */
/* ================================================================== */
static void test_bad_version_rejected(void)
{
    puts("[5] 版本不符：拒绝打开，防止新程序误读老格式");

    db_reset();

    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db != NULL) CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    /* 魔数对，但版本号装成未来版本 99 */
    CHECK_EQ(write_raw_u32(OFF_VERSION, 99u), 0);

    minisql_db *db2 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db2), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db2 == NULL);

    /* 恢复版本号，应该又能打开 —— 说明拒绝的原因确实是版本而非别的东西 */
    CHECK_EQ(write_raw_u32(OFF_VERSION, FORMAT_VERSION), 0);
    minisql_db *db3 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db3), (long long)MINISQL_OK);
    if (db3 != NULL) CHECK_EQ(db_close(db3), (long long)MINISQL_OK);
}

/* ================================================================== */
/* 用例 6：头页 page_count 与物理文件大小不一致 -> 拒绝                 */
/* ================================================================== */
static void test_page_count_mismatch_rejected(void)
{
    puts("[6] page_count 与文件大小不符：以物理文件为权威，拒绝打开");

    db_reset();

    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db != NULL) CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    CHECK_EQ(file_page_count(), 1u);

    /* 情况 A：头页说 5 页，文件只有 1 页（文件被截断） */
    CHECK_EQ(write_raw_u32(OFF_PAGE_COUNT, 5u), 0);
    minisql_db *db = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db == NULL);

    /* 情况 B：头页说 1 页，文件却被外部追加成 2 页 */
    CHECK_EQ(write_raw_u32(OFF_PAGE_COUNT, 1u), 0);
    {
        int fd = open(DB_PATH, O_RDWR);
        CHECK(fd >= 0);
        if (fd >= 0) {
            static const uint8_t zero_page[MINISQL_PAGE_SIZE];
            CHECK_EQ(pwrite(fd, zero_page, MINISQL_PAGE_SIZE,
                            (off_t)MINISQL_PAGE_SIZE), MINISQL_PAGE_SIZE);
            close(fd);
        }
    }
    CHECK_EQ(file_page_count(), 2u);

    minisql_db *db2 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db2), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db2 == NULL);

    /* 把头页的 page_count 改成真实值 2，就该能打开了 */
    CHECK_EQ(write_raw_u32(OFF_PAGE_COUNT, 2u), 0);
    minisql_db *db3 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db3), (long long)MINISQL_OK);
    if (db3 != NULL) {
        CHECK_EQ(pager_page_count(db_pager(db3)), 2u);
        CHECK_EQ(db_close(db3), (long long)MINISQL_OK);
    }
}

/* ================================================================== */
/* 用例 7：root / freelist 页号越界 -> 拒绝                            */
/* ================================================================== */
static void test_bad_root_freelist_rejected(void)
{
    puts("[7] root/freelist 越界：头页被改坏时拒绝打开");

    /* --- root 越界 --- */
    db_reset();
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db != NULL) CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    CHECK_EQ(write_raw_u32(OFF_ROOT, 9999u), 0);        /* 文件只有 1 页 */
    minisql_db *db = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db == NULL);

    /* root = 0 也不合法：0 是头页，不能当树根 */
    CHECK_EQ(write_raw_u32(OFF_ROOT, 0u), 0);
    minisql_db *db0 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db0), (long long)MINISQL_ERR_CORRUPT);
    CHECK(db0 == NULL);

    /* 改回 NULL（空树），应该能打开 */
    CHECK_EQ(write_raw_u32(OFF_ROOT, MINISQL_PGNO_NULL), 0);
    minisql_db *dbok = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &dbok), (long long)MINISQL_OK);
    if (dbok != NULL) {
        CHECK_EQ(db_root_page(dbok), (long long)MINISQL_PGNO_NULL);
        CHECK_EQ(db_close(dbok), (long long)MINISQL_OK);
    }

    /* --- freelist 越界 --- */
    db_reset();
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
        if (db != NULL) CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    CHECK_EQ(write_raw_u32(OFF_FREELIST, 9999u), 0);
    minisql_db *dbf = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &dbf), (long long)MINISQL_ERR_CORRUPT);
    CHECK(dbf == NULL);

    /* freelist = 0 同样不合法 */
    CHECK_EQ(write_raw_u32(OFF_FREELIST, 0u), 0);
    minisql_db *dbf0 = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &dbf0), (long long)MINISQL_ERR_CORRUPT);
    CHECK(dbf0 == NULL);
}

/* ================================================================== */
/* 用例 8：db_set_root_page 的范围检查                                 */
/* ================================================================== */
static void test_set_root_bounds(void)
{
    puts("[8] db_set_root_page：只接受 NULL 或 [1, page_count)");

    db_reset();

    minisql_db *db = NULL;
    CHECK_EQ(db_open(DB_PATH, 8, &db), (long long)MINISQL_OK);
    if (db == NULL) return;

    /* 此时只有头页，page_count == 1，任何非 NULL 页号都越界 */
    CHECK_EQ(db_set_root_page(db, 1u), (long long)MINISQL_ERR_CORRUPT);
    CHECK_EQ(db_set_root_page(db, 9999u), (long long)MINISQL_ERR_CORRUPT);
    CHECK_EQ(db_set_root_page(db, 0u), (long long)MINISQL_ERR_CORRUPT);

    /* 失败不能改状态 */
    CHECK_EQ(db_root_page(db), (long long)MINISQL_PGNO_NULL);

    /* NULL 永远合法（表示清空树） */
    CHECK_EQ(db_set_root_page(db, MINISQL_PGNO_NULL), (long long)MINISQL_OK);
    CHECK_EQ(db_root_page(db), (long long)MINISQL_PGNO_NULL);

    /* 分配一页后，页号 1 就合法了 */
    minisql_page *pg = NULL;
    minisql_pgno p1 = MINISQL_PGNO_NULL;
    CHECK_EQ(pager_alloc(db_pager(db), &p1, &pg), (long long)MINISQL_OK);
    CHECK_EQ(pager_unpin(db_pager(db), pg, 1), (long long)MINISQL_OK);
    CHECK_EQ(p1, 1u);

    CHECK_EQ(db_set_root_page(db, 1u), (long long)MINISQL_OK);
    CHECK_EQ(db_root_page(db), 1u);
    /* page_count 变成了 2，页号 2 仍越界 */
    CHECK_EQ(db_set_root_page(db, 2u), (long long)MINISQL_ERR_CORRUPT);

    CHECK_EQ(db_close(db), (long long)MINISQL_OK);
}

/* ================================================================== */
/* 用例 9：NULL 参数与 NULL 句柄                                       */
/* ================================================================== */
static void test_null_args(void)
{
    puts("[9] NULL 参数：一律用 INVALID 拦截，不解引用");

    minisql_db *db = NULL;

    CHECK_EQ(db_open(NULL, 8, &db), (long long)MINISQL_ERR_INVALID);
    CHECK(db == NULL);

    CHECK_EQ(db_open(DB_PATH, 8, NULL), (long long)MINISQL_ERR_INVALID);

    /* db_close(NULL) 是合法调用，方便上层统一清理 */
    CHECK_EQ(db_close(NULL), (long long)MINISQL_OK);

    CHECK_EQ(db_flush(NULL), (long long)MINISQL_ERR_INVALID);
    CHECK_EQ(db_set_root_page(NULL, 1u), (long long)MINISQL_ERR_INVALID);
    CHECK_EQ(db_root_page(NULL), (long long)MINISQL_PGNO_NULL);
    CHECK(db_pager(NULL) == NULL);
}

/* ================================================================== */
/* 用例 10：freelist 跨重启复用同一批页（db 与 pager 的交接闭环）        */
/* ================================================================== */
static void test_freelist_survives_restart(void)
{
    puts("[10] freelist 跨重启：free 掉的页号能持久化并被复用");

    db_reset();

    minisql_pgno kept[3]  = { 0, 0, 0 };
    minisql_pgno freed    = MINISQL_PGNO_NULL;
    uint32_t pages_before = 0;

    /* 第一段：分配 4 页，只留 3 页，free 掉最后一页 */
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 16, &db), (long long)MINISQL_OK);
        if (db == NULL) return;

        minisql_pager *p = db_pager(db);
        for (int i = 0; i < 3; i++) {
            minisql_page *pg = NULL;
            CHECK_EQ(pager_alloc(p, &kept[i], &pg), (long long)MINISQL_OK);
            CHECK_EQ(pager_unpin(p, pg, 1), (long long)MINISQL_OK);
        }
        minisql_page *pg4 = NULL;
        CHECK_EQ(pager_alloc(p, &freed, &pg4), (long long)MINISQL_OK);
        CHECK_EQ(pager_unpin(p, pg4, 1), (long long)MINISQL_OK);
        CHECK_EQ(pager_free(p, freed), (long long)MINISQL_OK);

        CHECK_EQ(db_set_root_page(db, kept[0]), (long long)MINISQL_OK);
        CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    CHECK_EQ(read_raw_u32(OFF_FREELIST), (long long)freed);
    pages_before = file_page_count();

    /* 第二段：重启后 alloc 必须拿回被 free 的那一页，文件不增长 */
    {
        minisql_db *db = NULL;
        CHECK_EQ(db_open(DB_PATH, 16, &db), (long long)MINISQL_OK);
        if (db == NULL) return;

        CHECK_EQ(pager_freelist_head(db_pager(db)), (long long)freed);
        CHECK_EQ(db_root_page(db), (long long)kept[0]);

        minisql_pgno reused = MINISQL_PGNO_NULL;
        minisql_page *pg = NULL;
        CHECK_EQ(pager_alloc(db_pager(db), &reused, &pg), (long long)MINISQL_OK);
        CHECK_EQ(reused, (long long)freed);
        CHECK_EQ(pager_unpin(db_pager(db), pg, 1), (long long)MINISQL_OK);

        /* 消费掉 freelist 之后头也应该是 NULL 了 */
        CHECK_EQ(pager_freelist_head(db_pager(db)), (long long)MINISQL_PGNO_NULL);
        CHECK_EQ(pager_page_count(db_pager(db)), (long long)pages_before);

        CHECK_EQ(db_close(db), (long long)MINISQL_OK);
    }

    CHECK_EQ(file_page_count(), pages_before);
    CHECK_EQ(read_raw_u32(OFF_FREELIST), (long long)MINISQL_PGNO_NULL);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int main(void)
{
    /* 正常路径 */
    test_open_new_db();                 /*  1 */
    test_reopen_reads_back();           /*  2 */
    test_flush_contract();              /*  3 */

    /* 崩溃路径：每一类损坏文件都必须在 db_open 被拦住 */
    test_bad_magic_rejected();          /*  4 */
    test_bad_version_rejected();        /*  5 */
    test_page_count_mismatch_rejected();/*  6 */
    test_bad_root_freelist_rejected();  /*  7 */

    /* 接口边界 */
    test_set_root_bounds();             /*  8 */
    test_null_args();                   /*  9 */

    /* 与 pager 的交接闭环 */
    test_freelist_survives_restart();   /* 10 */

    db_reset();

    printf("\n===== %d passed, %d failed =====\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf(">>> 有用例失败 <<<\n");
    }

    return g_fail != 0 ? 1 : 0;
}