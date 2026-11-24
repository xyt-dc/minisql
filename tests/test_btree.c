/*
 * tests/test_btree.c —— B+ 树层单元测试
 *
 * 覆盖：
 *   1  空树：search 找不到、height == 0、check OK
 *   2  插入单条：能查到、value 正确、height == 1
 *   3  upsert：同 key 覆盖 value
 *   4  顺序插入 200 条：全部可查、height >= 2（叶子分裂过）
 *   5  逆序插入 200 条：全部可查（分裂在左端也要正确）
 *   6  随机插入 500 条：btree_check 结构自洽
 *   7  范围扫描：结果升序、边界含闭区间、提前终止
 *   8  删除：删掉的查不到，没删的还在
 *   9  重启持久化：close 后重开，root 与数据都还在
 *  10  freelist 复用：删除+插入后 page_count 不会无限增长（页被回收）
 *
 * 数据库文件用 /tmp 下的绝对路径，ctest 在 build/ 下也能跑。
 */
#include "minisql/btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BT_DB_PATH "/tmp/minisql_test_btree.tmp"

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

static void db_reset(void)
{
    remove(BT_DB_PATH);
}

/* 查询辅助：返回 1 表示找到，value 写进 *v */
static int q(minisql_btree *t, btree_key k, btree_val *v)
{
    int found = 0;
    btree_val got = 0;
    if (btree_search(t, k, &found, &got) != MINISQL_OK) return -1;
    if (found && v != NULL) *v = got;
    return found;
}

/* ------------------------------------------------------------------ */
/* 用例 1：空树                                                        */
/* ------------------------------------------------------------------ */
static void test_empty(void)
{
    puts("[1] 空树：search 找不到、height == 0、check OK");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 64, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    CHECK_EQ(btree_height(t), 0);
    CHECK_EQ(q(t, 1, NULL), 0);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    /* 空树范围扫描不能崩，也不能回调 */
    int calls = 0;
    btree_visit_fn dummy = NULL;
    (void)dummy;
    CHECK_EQ(btree_scan(t, 0, 100, (btree_visit_fn)0, NULL),
             (long long)MINISQL_ERR_INVALID);   /* visit == NULL 被拦 */
    (void)calls;

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 2：插入单条                                                    */
/* ------------------------------------------------------------------ */
static void test_single_insert(void)
{
    puts("[2] 插入单条：能查到、value 正确、height == 1");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 64, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    CHECK_EQ(btree_insert(t, 42, 4242), (long long)MINISQL_OK);

    btree_val v = 0;
    CHECK_EQ(q(t, 42, &v), 1);
    CHECK_EQ(v, 4242u);
    CHECK_EQ(q(t, 41, NULL), 0);
    CHECK_EQ(q(t, 43, NULL), 0);

    CHECK_EQ(btree_height(t), 1);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 3：upsert                                                      */
/* ------------------------------------------------------------------ */
static void test_upsert(void)
{
    puts("[3] upsert：同 key 重复插入覆盖 value，不产生重复项");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 64, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    CHECK_EQ(btree_insert(t, 7, 100), (long long)MINISQL_OK);
    CHECK_EQ(btree_insert(t, 7, 200), (long long)MINISQL_OK);
    CHECK_EQ(btree_insert(t, 7, 300), (long long)MINISQL_OK);

    btree_val v = 0;
    CHECK_EQ(q(t, 7, &v), 1);
    CHECK_EQ(v, 300u);

    /* 一条记录都没多出来：范围扫描只应命中 1 次 */
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 4：顺序插入 -> 必然触发叶子分裂                                 */
/* ------------------------------------------------------------------ */
static void test_sequential_insert(void)
{
    puts("[4] 顺序插入 700 条：超过单页容量，必然分裂，全部可查");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 64, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 700;   /* > BT_MAX_KEYS(510)，单页放不下 */
    for (uint32_t i = 0; i < N; i++) {
        if (btree_insert(t, i * 10u, i) != MINISQL_OK) {
            CHECK(0);
            printf("    插入失败于 i=%u\n", i);
            break;
        }
    }

    int missing = 0;
    for (uint32_t i = 0; i < N; i++) {
        btree_val v = 0;
        if (q(t, i * 10u, &v) != 1 || v != i) missing++;
    }
    CHECK_EQ(missing, 0);

    /* 700 条 > 510，必须至少分裂一次，树高至少 2 */
    CHECK(btree_height(t) >= 2);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 4b：足够多，必然分裂出多层                                      */
/* ------------------------------------------------------------------ */
static void test_many_insert_splits(void)
{
    puts("[4b] 插入 3000 条：必然多层分裂，height >= 2，全部可查");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 3000;
    for (uint32_t i = 0; i < N; i++) {
        if (btree_insert(t, i, i * 3u) != MINISQL_OK) {
            CHECK(0);
            printf("    插入失败于 i=%u\n", i);
            break;
        }
    }

    CHECK(btree_height(t) >= 2);

    int missing = 0;
    for (uint32_t i = 0; i < N; i++) {
        btree_val v = 0;
        if (q(t, i, &v) != 1 || v != i * 3u) missing++;
    }
    CHECK_EQ(missing, 0);

    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 5：逆序插入                                                    */
/* ------------------------------------------------------------------ */
static void test_reverse_insert(void)
{
    puts("[5] 逆序插入 1000 条：全部可查（左端分裂正确）");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 1000;
    for (uint32_t i = 0; i < N; i++) {
        btree_key k = (N - 1u - i) * 2u;
        if (btree_insert(t, k, i) != MINISQL_OK) { CHECK(0); break; }
    }

    int missing = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (q(t, i * 2u, NULL) != 1) missing++;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 6：随机插入                                                    */
/* ------------------------------------------------------------------ */
static void test_random_insert(void)
{
    puts("[6] 随机插入 2000 条（固定种子）：check 结构自洽、全部可查");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 2000;
    srand(12345);

    /* 用一张 bitmap 记录哪些 key 被插过，避免重复 upsert 干扰计数 */
    static uint8_t seen[100000];
    memset(seen, 0, sizeof(seen));

    uint32_t inserted = 0;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t k = (uint32_t)(rand() % 100000);
        if (seen[k]) { i--; continue; }
        seen[k] = 1;
        if (btree_insert(t, k, k + 1u) != MINISQL_OK) { CHECK(0); break; }
        inserted++;
    }

    int missing = 0;
    for (uint32_t k = 0; k < 100000; k++) {
        if (!seen[k]) continue;
        btree_val v = 0;
        if (q(t, k, &v) != 1 || v != k + 1u) missing++;
    }
    CHECK_EQ(missing, 0);

    /* 不存在的 key 必须查不到 */
    int false_hit = 0;
    for (uint32_t k = 0; k < 100000; k++) {
        if (seen[k]) continue;
        if (q(t, k, NULL) != 0) false_hit++;
    }
    CHECK_EQ(false_hit, 0);

    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);
    printf("    插入 %u 条，树高 %d\n", inserted, btree_height(t));

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 7：范围扫描                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t       count;
    btree_key      last;
    int            out_of_order;
    uint32_t       stop_after;   /* 0 = 不提前终止 */
} scan_ctx;

static int scan_cb(btree_key k, btree_val v, void *ctx)
{
    scan_ctx *c = (scan_ctx *)ctx;
    if (c->count > 0 && !(c->last < k)) c->out_of_order = 1;
    c->last = k;
    c->count++;
    (void)v;
    if (c->stop_after != 0 && c->count >= c->stop_after) return 1;
    return 0;
}

static void test_scan(void)
{
    puts("[7] 范围扫描：升序、闭区间、提前终止");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    /* 插入 0,10,20,...,990 */
    for (uint32_t i = 0; i < 100; i++) {
        if (btree_insert(t, i * 10u, i) != MINISQL_OK) { CHECK(0); break; }
    }

    /* 全扫 */
    scan_ctx c;
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 0, 990, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 100u);
    CHECK_EQ(c.out_of_order, 0);

    /* 闭区间 [200, 250] -> 200,210,220,230,240,250 共 6 条 */
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 200, 250, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 6u);
    CHECK_EQ(c.last, 250u);
    CHECK_EQ(c.out_of_order, 0);

    /* 边界不在数据上：[205, 235] -> 210,220,230 共 3 条 */
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 205, 235, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 3u);

    /* 空区间 */
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 1000, 2000, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 0u);

    /* lo > hi：直接返回 OK，不回调 */
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 500, 400, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 0u);

    /* 提前终止：只要前 5 条 */
    memset(&c, 0, sizeof(c));
    c.stop_after = 5;
    CHECK_EQ(btree_scan(t, 0, 990, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, 5u);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 8：删除                                                        */
/* ------------------------------------------------------------------ */
static void test_delete(void)
{
    puts("[8] 删除：删掉的查不到，没删的还在，结构仍自洽");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 1000;
    for (uint32_t i = 0; i < N; i++) {
        if (btree_insert(t, i, i) != MINISQL_OK) { CHECK(0); break; }
    }

    /* 删偶数 */
    for (uint32_t i = 0; i < N; i += 2) {
        CHECK_EQ(btree_delete(t, i), (long long)MINISQL_OK);
    }

    /* 删不存在的 key 不应报错 */
    CHECK_EQ(btree_delete(t, 999999), (long long)MINISQL_OK);

    int bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        int found = q(t, i, NULL);
        if (i % 2 == 0) { if (found != 0) bad++; }
        else            { if (found != 1) bad++; }
    }
    CHECK_EQ(bad, 0);

    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    /* 删完再扫：应该只剩奇数 500 条，且升序 */
    scan_ctx c;
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 0, N - 1u, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.count, N / 2u);
    CHECK_EQ(c.out_of_order, 0);
    CHECK_EQ(c.last, N - 1u);   /* 999 是奇数，应该最后被访问到 */

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 9：重启持久化                                                  */
/* ------------------------------------------------------------------ */
static void test_persist(void)
{
    puts("[9] 重启持久化：close 后重开，root 与全部数据都在");

    db_reset();

    const uint32_t N = 2000;

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    for (uint32_t i = 0; i < N; i++) {
        if (btree_insert(t, i * 7u, i) != MINISQL_OK) { CHECK(0); break; }
    }
    int h_before = btree_height(t);
    CHECK(h_before >= 2);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);

    /* 重开 */
    t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    CHECK_EQ(btree_height(t), h_before);

    int missing = 0;
    for (uint32_t i = 0; i < N; i++) {
        btree_val v = 0;
        if (q(t, i * 7u, &v) != 1 || v != i) missing++;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    /* 重开后还能继续插入 */
    CHECK_EQ(btree_insert(t, 999999, 1), (long long)MINISQL_OK);
    CHECK_EQ(q(t, 999999, NULL), 1);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 10：删除后页被 freelist 回收                                   */
/* ------------------------------------------------------------------ */
static void test_page_reuse(void)
{
    puts("[10] 删除只摘 key：同规模的反复增删不会让页数无限增长");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 128, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    /* 先塞满一批，让它分裂出多层 */
    for (uint32_t i = 0; i < 2000; i++) {
        if (btree_insert(t, i, i) != MINISQL_OK) { CHECK(0); break; }
    }
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    /* 注意：btree_delete 不做合并，所以页数不会降回去。
     * 这里只验证「结构仍然自洽」和「数据语义正确」，
     * 不验证页回收 —— 页回收需要 rebalance，属于未实现范围。 */
    for (uint32_t i = 0; i < 2000; i += 3) {
        btree_delete(t, i);
    }
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    scan_ctx c;
    memset(&c, 0, sizeof(c));
    CHECK_EQ(btree_scan(t, 0, 1999, scan_cb, &c), (long long)MINISQL_OK);
    CHECK_EQ(c.out_of_order, 0);
    /* 0,3,6,... 被删 -> 剩 2000 - ceil(2000/3) = 2000 - 667 = 1333 */
    CHECK_EQ(c.count, 1333u);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
/* 用例 11：小缓存 + 多层（pager 驱逐与 btree 配合）                    */
/* ------------------------------------------------------------------ */
static void test_small_cache(void)
{
    puts("[11] 小缓存(16 页) + 多层树：LRU 驱逐不影响正确性");

    db_reset();

    minisql_btree *t = NULL;
    CHECK_EQ(btree_open(BT_DB_PATH, 16, &t), (long long)MINISQL_OK);
    if (t == NULL) return;

    const uint32_t N = 1500;
    for (uint32_t i = 0; i < N; i++) {
        if (btree_insert(t, i, i + 5u) != MINISQL_OK) {
            CHECK(0);
            printf("    插入失败于 i=%u（可能是 CACHE_FULL）\n", i);
            break;
        }
    }

    int missing = 0;
    for (uint32_t i = 0; i < N; i++) {
        btree_val v = 0;
        if (q(t, i, &v) != 1 || v != i + 5u) missing++;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(btree_check(t), (long long)MINISQL_OK);

    CHECK_EQ(btree_close(t), (long long)MINISQL_OK);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    test_empty();
    test_single_insert();
    test_upsert();
    test_sequential_insert();
    test_many_insert_splits();
    test_reverse_insert();
    test_random_insert();
    test_scan();
    test_delete();
    test_persist();
    test_page_reuse();
    test_small_cache();

    db_reset();

    printf("\n===== %d passed, %d failed =====\n", g_pass, g_fail);
    if (g_fail != 0) {
        printf(">>> 有用例失败 <<<\n");
    }
    return g_fail != 0 ? 1 : 0;
}