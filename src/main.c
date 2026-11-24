/*
 * src/main.c —— minisql 基准测试（benchmark）
 *
 * 这个程序不是 SQL shell，也不解析任何查询。它只做一件事：
 * 用真实数据把 pager + db + B+ 树的性能量出来，让每个数字都可复现。
 *
 * 为什么不做 shell：
 *   项目没有词法/语法分析层，硬做 shell 只能演示"能跑"，
 *   而 benchmark 能回答面试官真正关心的三个问题：
 *     1. 树高是不是对数级？（10 万条 key，扇出 510，树高应该是 3）
 *     2. 缓存命中时点查有多快？（这是 pager + hash + LRU 的价值）
 *     3. 冷启动（进程重启、缓存全空）要付多少读盘代价？
 *
 * 用法：
 *   ./minisql                 # 默认 10 万条，1024 页缓存，跑完删文件
 *   ./minisql -n 200000       # 20 万条
 *   ./minisql -c 64           # 只给 64 页缓存，观察 LRU 驱逐的影响
 *   ./minisql -f /tmp/x.db -k # 指定文件并保留（方便用 ls -l 看大小）
 *
 * 注意：
 *   - 必须用 Release（-O2）跑，Debug 的数字要慢 5~10 倍，报出去就是错的。
 *   - /tmp 常常是 tmpfs（纯内存），那里的数字不代表真实磁盘。程序会打印
 *     检测到的文件系统类型，请诚实地按它解读结果。
 */
#include "minisql/btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/vfs.h>

/* tmpfs 的魔数，linux/magic.h 里有，但为了少一个头文件依赖直接写常量 */
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

/* ------------------------------------------------------------------ */
/* 命令行选项                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *file;
    uint32_t    cache_pages;
    uint32_t    n;
    int         keep_file;
} options;

/*
 * 防止优化器把整个 benchmark 循环删掉。
 * 如果 search 的返回值没人用，-O2 会认定循环无副作用直接抹掉，
 * 你会测出一个荒谬的 0.001ms。所有结果都累加进这里。
 */
static volatile uint64_t g_sink = 0;

/* 全表扫描命中的记录数。写在文件上方是因为 print_tree_size 要用它
 * 算空间放大率 —— 用"实际存活记录数"而不是"插入过多少次"做分母，
 * 才能把删除留下的空洞如实体现出来。 */
static int g_scan_count = 0;
static int g_hot_scan_count = 0;   /* 删除阶段之前的全扫记录数 */

/* ------------------------------------------------------------------ */
/* 计时                                                                */
/* ------------------------------------------------------------------ */

/* 只用 CLOCK_MONOTONIC：不受系统时间调整影响，纳秒精度。
 * 不用 clock()：它计的是 CPU 时间，IO 等待不计入，会低估落盘成本。 */
static double now_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 统一格式，方便横向对比。总宽控制在 70 列内，避免终端折行错位。
 * 只报 ns/op 不报 ops/s：ns/op 在数量级跨 1 万倍时更好读。 */
static void report(const char *label, uint64_t ops, double sec)
{
    if (sec <= 0.0) {
        printf("  %-22s %9" PRIu64 " ops   (计时异常)\n", label, ops);
        return;
    }
    printf("  %-22s %9" PRIu64 " ops  %8.2f ms  %8.1f ns/op\n",
           label, ops, sec * 1e3, sec * 1e9 / (double)ops);
}

/* 落盘只发生一次，"每秒多少次"没有意义，要报吞吐 MB/s */
static void report_io(const char *label, double sec, uint64_t bytes)
{
    if (sec <= 0.0) {
        printf("  %-22s %8.2f MB   (计时异常)\n", label, (double)bytes / 1048576.0);
        return;
    }
    printf("  %-22s %8.2f MB  %8.2f ms  %7.1f MB/s\n",
           label, (double)bytes / 1048576.0, sec * 1e3,
           (double)bytes / 1048576.0 / sec);
}

/* ------------------------------------------------------------------ */
/* 环境信息                                                            */
/* ------------------------------------------------------------------ */

/*
 * 探测文件系统类型。必须传「已存在的路径」：statfs 对不存在的路径会失败，
 * 而在 print_env 时数据文件还没创建 —— 所以这里退到父目录探测。
 * /tmp 若是 tmpfs，所有 IO 数字都是内存速度，不能当磁盘性能报出去。
 */
static const char *fs_type_name(const char *path)
{
    struct statfs sf;

    /* 先试目标文件；不存在（benchmark 开始前）就试它的父目录 */
    if (statfs(path, &sf) != 0) {
        char dir[512];
        const char *slash = strrchr(path, '/');
        if (slash == NULL || slash == path) {
            snprintf(dir, sizeof(dir), "/");
        } else {
            size_t len = (size_t)(slash - path);
            if (len >= sizeof(dir)) len = sizeof(dir) - 1;
            memcpy(dir, path, len);
            dir[len] = '\0';
        }
        if (statfs(dir, &sf) != 0) {
            return "unknown";
        }
    }

    if ((unsigned long)sf.f_type == (unsigned long)TMPFS_MAGIC) {
        return "tmpfs (纯内存! IO 数字不代表磁盘)";
    }
    if ((unsigned long)sf.f_type == 0xEF53ul) {
        return "ext2/3/4";
    }
    if ((unsigned long)sf.f_type == 0x58465342ul) {
        return "xfs";
    }
    if ((unsigned long)sf.f_type == 0x794C7630ul) {
        return "overlayfs (容器常用)";
    }
    if ((unsigned long)sf.f_type == 0x9123683Eul) {
        return "btrfs";
    }
    return "未知文件系统 (魔数见下方)";
}

static uint64_t file_size_bytes(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

static void print_env(const options *o, const char *build_type)
{
    printf("=== minisql benchmark ===\n");
    printf("  文件            : %s\n", o->file);
    printf("  文件系统        : %s\n", fs_type_name(o->file));
    printf("  构建类型        : %s\n", build_type);
    printf("  页大小          : %u B\n", (unsigned)MINISQL_PAGE_SIZE);
    printf("  缓存页数        : %u (%.1f MB)\n",
           o->cache_pages,
           (double)o->cache_pages * MINISQL_PAGE_SIZE / (1024.0 * 1024.0));
    printf("  目标记录数      : %u\n", o->n);
    printf("\n");
}

/* 打印树的物理规模 */
static void print_tree_size(const char *path, minisql_btree *t)
{
    uint64_t bytes = file_size_bytes(path);
    uint64_t pages = bytes / MINISQL_PAGE_SIZE;
    int height = btree_height(t);

    printf("  树高            : %d\n", height);
    printf("  页数 / 文件大小 : %" PRIu64 " 页 / %.2f MB\n",
           pages, (double)bytes / (1024.0 * 1024.0));

    /* 空间放大率 = 文件字节 / 有效数据字节。
     * 有效数据 = 全表扫描数出来的记录数 × 8B（key 4B + value 4B）。
     * 内部节点占的页和页内碎片都算作"放大" —— 这是诚实的口径，
     * 因为用户真正关心的就是"我存 8 字节想用多少磁盘"。
     * 实测约 3x 来自：内部节点 + 分裂后页半满 + 删除留下的空洞。 */
    if (g_scan_count > 0 && pages > 0) {
        double live = (double)g_scan_count * (double)BT_CELL_SIZE;
        printf("  存活记录        : %d 条 (全扫实测)\n", g_scan_count);
        printf("  空间放大率      : %.2f x  (有效 %.0f KB / 文件 %.0f KB)\n",
               (double)bytes / live,
               live / 1024.0, (double)bytes / 1024.0);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* 各阶段 benchmark                                                    */
/* ------------------------------------------------------------------ */

static int insert_seq(minisql_btree *t, uint32_t n)
{
    double t0 = now_sec();
    for (uint32_t i = 0; i < n; i++) {
        minisql_errno rc = btree_insert(t, (btree_key)i, (btree_val)(i * 3u));
        if (rc != MINISQL_OK) {
            printf("  插入失败于 i=%u: %s\n", i, minisql_strerror(rc));
            return -1;
        }
    }
    report("insert 顺序 (升序 key)", n, now_sec() - t0);
    return 0;
}

/*
 * 随机插入：用固定种子的 rand_r，保证每次运行序列完全一样，数字可复现。
 * 把 i 做一次位混淆再取模，避免相邻 key 落在同一个叶子（那是顺序插入的形态）。
 */
static int insert_rand(minisql_btree *t, uint32_t n)
{
    unsigned int seed = 20261122u;
    double t0 = now_sec();

    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = (uint32_t)rand_r(&seed);
        /* xorshift 混淆，打散低位。
         * 取值范围 [n, 3n)：完全避开顺序插入用掉的 [0, n)，
         * 否则撞上已有 key 会走 upsert 覆盖路径，测不到分裂。 */
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        btree_key k = (btree_key)(n + r % (n * 2u));

        minisql_errno rc = btree_insert(t, k, (btree_val)i);
        if (rc != MINISQL_OK) {
            printf("  插入失败于 i=%u: %s\n", i, minisql_strerror(rc));
            return -1;
        }
    }
    report("insert 随机 (混淆取模)", n, now_sec() - t0);
    return 0;
}

/* 点查：命中路径。key 用 0..n 步长 7，和顺序插入的数据对得上 */
static void point_lookup_hit(minisql_btree *t, uint32_t n)
{
    uint32_t ops = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < n; i += 7u) {
        btree_val v = 0;
        int found = 0;
        if (btree_search(t, (btree_key)i, &found, &v) == MINISQL_OK && found) {
            g_sink += v;            /* 必须有这一行，否则 -O2 抹掉整个循环 */
        }
        ops++;
    }
    report("search 命中", ops, now_sec() - t0);
}

/* 点查：全 miss 路径。miss 也要走完全部层才能确认不存在，成本一样 */
static void point_lookup_miss(minisql_btree *t, uint32_t n)
{
    uint32_t ops = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < n; i += 7u) {
        btree_val v = 0;
        int found = 0;
        /* 已知 key 空间是 [0, n) 顺序段 + [n, 3n) 随机段，
         * 所以 [4n, +∞) 一定全部 miss。 */
        btree_key k = (btree_key)(n * 4u + i);
        if (btree_search(t, k, &found, &v) == MINISQL_OK && found) {
            g_sink += 1;
        }
        ops++;
    }
    report("search 全部 miss", ops, now_sec() - t0);
}

/*
 * 范围扫描。全扫是 pager 顺序 IO + 叶子链最理想的场景，
 * 它和随机点查的数据一对比，就能说明"为什么索引扫描比回表快"。
 *
 * key 空间上界 = 4n：顺序段占 [0, n)，随机段占 [n, 3n)，
 * 所以 4n 是覆盖全表的发送上界（btree_scan 是闭区间）。
 */
#define KEY_SPACE_MAX(n) ((btree_key)((n) * 4u))
static int scan_counter(btree_key key, btree_val val, void *ctx)
{
    (void)key;
    (void)ctx;
    g_sink += (uint64_t)val;
    g_scan_count++;
    return 0;
}

static void range_scan_all(minisql_btree *t, uint32_t n)
{
    g_scan_count = 0;
    double t0 = now_sec();

    minisql_errno rc = btree_scan(t, 0, KEY_SPACE_MAX(n),
                                  scan_counter, NULL);
    double sec = now_sec() - t0;

    if (rc != MINISQL_OK) {
        printf("  全扫失败: %s\n", minisql_strerror(rc));
        return;
    }
    report("scan 全范围", (uint64_t)g_scan_count, sec);
    printf("  %-30s 命中 %d 条\n", "", g_scan_count);
    /* 记下来给后面的冷扫做对比基数 —— 中间隔着 delete 阶段，
     * 冷扫的记录数必然更少，不记住基数就没法解释这个差异。 */
    g_hot_scan_count = g_scan_count;}

/* 扫 1%：模拟"只取一小段连续区间"，是 OLTP 里最常见的访问形态 */
static void range_scan_slice(minisql_btree *t, uint32_t n)
{
    g_scan_count = 0;
    double t0 = now_sec();

    btree_key lo = (btree_key)(n / 3u);
    btree_key hi = (btree_key)(lo + n / 100u);

    minisql_errno rc = btree_scan(t, lo, hi, scan_counter, NULL);
    double sec = now_sec() - t0;

    if (rc != MINISQL_OK) {
        printf("  区间扫描失败: %s\n", minisql_strerror(rc));
        return;
    }
    report("scan 区间 (约 1%)", (uint64_t)g_scan_count, sec);
}

/* 删除：当前实现只摘 key、不 rebalance，所以这测的是"最便宜路径"的成本，
 * 页数不会下降 —— 这个取舍要在 README 里明说。 */
static void delete_phase(minisql_btree *t, uint32_t n)
{
    uint32_t ops = 0;
    double t0 = now_sec();

    for (uint32_t i = 0; i < n; i += 3u) {
        if (btree_delete(t, (btree_key)i) == MINISQL_OK) {
            ops++;
        }
    }
    report("delete (每 3 个删 1 个)", ops, now_sec() - t0);
}

/* ------------------------------------------------------------------ */
/* 参数解析                                                            */
/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
    printf("用法: %s [-n 记录数] [-c 缓存页数] [-f 文件] [-k]\n", argv0);
    printf("  -n N   目标记录数，默认 100000\n");
    printf("  -c N   缓存页数，默认 1024 (=4 MB)\n");
    printf("  -f P   数据库文件路径，默认 /tmp/minisql_bench.db\n");
    printf("  -k     跑完保留文件（方便 ls -l 看大小）\n");
}

static int parse_args(int argc, char **argv, options *o)
{
    int opt;
    while ((opt = getopt(argc, argv, "n:c:f:kh")) != -1) {
        switch (opt) {
        case 'n': o->n = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'c': o->cache_pages = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'f': o->file = optarg; break;
        case 'k': o->keep_file = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 0;
        }
    }
    if (o->n == 0u) {
        printf("错误: -n 必须大于 0\n");
        return 0;
    }
    if (o->cache_pages == 0u) {
        printf("错误: -c 必须大于 0\n");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* 构建类型在编译期确定，写死成宏而不是运行时猜 */
#if defined(__OPTIMIZE__)
#define MINISQL_BUILD_TYPE "Release/带 -O 优化 (数字可信)"
#else
#define MINISQL_BUILD_TYPE "Debug (未开优化 —— 数字不可用于简历!)"
#endif

int main(int argc, char **argv)
{
    options o = { "/tmp/minisql_bench.db", 1024u, 100000u, 0 };

    if (!parse_args(argc, argv, &o)) {
        return 1;
    }

    /* 每次都从干净的文件开始，否则上一轮的数据会让数字不可复现 */
    remove(o.file);

    print_env(&o, MINISQL_BUILD_TYPE);

    minisql_btree *t = NULL;
    minisql_errno rc = btree_open(o.file, o.cache_pages, &t);
    if (rc != MINISQL_OK) {
        printf("btree_open 失败: %s\n", minisql_strerror(rc));
        return 1;
    }

    /* ---- 写入阶段：两轮，对比"总在最右叶子分裂"与"分裂分散到各处" ---- */
    printf("[写入] 顺序插入 vs 随机插入（分裂位置完全不同）\n");
    if (insert_seq(t, o.n) != 0) {
        btree_close(t);
        return 1;
    }

    /* 随机插入单独用一个规模减半的 key 空间，避免和上面顺序插入的 key 冲突过多
     * （冲突会走 upsert 覆盖路径，测不到分裂）。这里把 key 偏移到 [n, 3n) 区间。 */
    if (insert_rand(t, o.n) != 0) {
        btree_close(t);
        return 1;
    }
    printf("\n");

    /* ---- 读阶段 ---- */
    printf("[读取] (顺序插入的数据已全部在缓存里)\n");
    point_lookup_hit(t, o.n);
    point_lookup_miss(t, o.n);
    range_scan_slice(t, o.n);
    range_scan_all(t, o.n);
    printf("\n");

    printf("[树的物理规模]\n");
    print_tree_size(o.file, t);

    /* ---- 删除阶段 ---- */
    printf("[删除] 当前实现只摘 key、不 rebalance，页数不会下降\n");
    delete_phase(t, o.n);
    printf("  树高            : %d  (删除后)\n", btree_height(t));
    printf("\n");

    /* ---- 落盘：单独计时，这是最容易被隐藏的数字 ---- */
    printf("[落盘] 全量脏页 + fsync，这是最容易被隐藏的成本\n");
    uint64_t before = file_size_bytes(o.file);
    double t0 = now_sec();
    rc = btree_flush(t);
    double flush_sec = now_sec() - t0;
    if (rc != MINISQL_OK) {
        printf("  flush 失败: %s\n", minisql_strerror(rc));
    } else {
        /* 按"刷了多少字节"报吞吐，而不是"每秒刷几次" —— 后者恒为 1 次，无意义 */
        report_io("flush", flush_sec, before);
    }
    printf("  文件大小        : %.2f MB\n",
           (double)file_size_bytes(o.file) / (1024.0 * 1024.0));
    printf("\n");

    rc = btree_close(t);
    if (rc != MINISQL_OK) {
        printf("btree_close 失败: %s\n", minisql_strerror(rc));
    }

    /* ---- 冷启动：缓存全空，每次点查都要真的 pread ---- */
    printf("[冷启动] 重开文件，进程内缓存全空\n");
    printf("  注意：这只确保 minisql 自己的页缓存是空的；\n");
    printf("        OS page cache 仍可能命中，真实磁盘要 drop_caches 才能测。\n");
    {
        minisql_btree *t2 = NULL;
        double t1 = now_sec();
        rc = btree_open(o.file, o.cache_pages, &t2);
        double open_sec = now_sec() - t1;
        if (rc != MINISQL_OK) {
            printf("  重开失败: %s\n", minisql_strerror(rc));
        } else {
            /* open 只发生一次，报绝对耗时比报 ns/op 有意义 */
            printf("  %-22s %8.2f ms  (读元数据 + 校验头页)\n",
                   "open 冷启动", open_sec * 1e3);
            printf("  树高            : %d  (重启后应与之前一致)\n",
                   btree_height(t2));

            /* 冷缓存下第一次全扫：所有叶子页都要从盘上读进来。
             * 注意此时已经过 delete 阶段，记录数比上面的热扫少，
             * 所以两者的 ns/op 不能直接比 —— 看的是「有没有变慢一个量级」。 */
            g_scan_count = 0;
            double t2s = now_sec();
            btree_scan(t2, 0, KEY_SPACE_MAX(o.n), scan_counter, NULL);
            report("scan 全范围 (冷缓存)", (uint64_t)g_scan_count,
                   now_sec() - t2s);
            printf("  %-22s 冷扫 %d 条 vs 热扫 %d 条(删除前)\n", "",
                   g_scan_count, g_hot_scan_count);

            btree_close(t2);
        }
    }
    printf("\n");

    if (!o.keep_file) {
        remove(o.file);
        printf("(已删除测试文件；加 -k 可保留)\n");
    }

    printf("(校验和 %" PRIu64 "，防止优化器消除循环)\n", g_sink);
    return 0;
}