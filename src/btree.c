/*
 * src/btree.c —— B+ 树索引层
 *
 * ============================ 页布局 ============================
 *
 *   偏移 0 : uint8   is_leaf    1 = 叶子, 0 = 内部节点
 *   偏移 1 : uint8   reserved   恒 0
 *   偏移 2 : uint16  nkeys      cell 个数
 *   偏移 4 : pgno    link
 *              叶子 -> 下一个叶子页号（叶子链表）
 *              内部 -> 最右孩子页号
 *   偏移 8 : cell[]  每个 8 字节，按 key 升序连续存放
 *              叶子 cell = key(4) + value(4)
 *              内部 cell = key(4) + child_pgno(4)
 *
 * 内部节点搜索语义（全文件所有逻辑都依赖这一条）：
 *
 *   children[i] = cells[i].child              i ∈ [0, nkeys)
 *   children[nkeys] = header.link
 *
 *   children[i]        管 [ keys[i-1], keys[i] )
 *   children[nkeys]    管 [ keys[nkeys-1], +∞ )
 *
 *   选孩子：i = 第一个满足 key < keys[i] 的下标；没有则走 link。
 *
 * ============================ 不变式 ============================
 *
 *   1. 除根以外，每个节点 nkeys >= 1
 *   2. 内部节点有 nkeys + 1 个孩子
 *   3. 所有叶子在同一层，link 串成全局升序链
 *   4. 父节点 keys[i] 是 children[i+1] 的下界、children[i] 的上界（开区间）
 *   5. 节点内 keys 严格升序
 *
 * ============================ 特殊约定 ============================
 *
 *   - 根页号存在 db 头页的 root_page 字段里，btree 对象不留副本，
 *     避免「两份不同步」。空树 = MINISQL_PGNO_NULL。
 *   - 删除只做叶子摘除，不合并/借用（不 rebalance）。见 btree_delete。
 *   - 页内所有字段一律 memcpy 读写：page data 是 uint8_t[]，
 *     偏移 2/4 不保证自然对齐，直接解引用是 UB。
 */

#include "minisql/btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 树的最大层数（防死循环用）。511 扇出下 4 层就能装下 10^10 条。 */
#define BT_MAX_HEIGHT 16u

struct minisql_btree {
    minisql_db *db;   /* 树的全部数据都存在 db 的 pager 里 */
};

/* ------------------------------------------------------------------ *
 * 向下遍历时保存的路径（分裂需要自底向上改父节点）
 * ------------------------------------------------------------------ */
typedef struct {
    minisql_pgno  pgno;
    minisql_page *page;      /* 保持 pin 住，直到插入结束 */
    uint32_t      child_idx; /* 该节点内指向下一层的孩子下标 */
} bt_path_entry;

typedef struct {
    bt_path_entry e[BT_MAX_HEIGHT];
    uint32_t      depth;
} bt_path;

/* ================================================================== *
 * 节点字段读写（全部走 memcpy）
 * ================================================================== */

static uint8_t *bt_bytes(minisql_page *pg)
{
    return page_data(pg);
}

static const uint8_t *bt_cbytes(const minisql_page *pg)
{
    /* page_data 的签名是 minisql_page*，这里脱掉 const 只为复用同一个入口 */
    return page_data((minisql_page *)pg);
}

static void bt_read16(const uint8_t *d, uint32_t off, uint16_t *out)
{
    memcpy(out, d + off, sizeof(*out));
}

static void bt_write16(uint8_t *d, uint32_t off, uint16_t v)
{
    memcpy(d + off, &v, sizeof(v));
}

static void bt_read32(const uint8_t *d, uint32_t off, uint32_t *out)
{
    memcpy(out, d + off, sizeof(*out));
}

static void bt_write32(uint8_t *d, uint32_t off, uint32_t v)
{
    memcpy(d + off, &v, sizeof(v));
}

static int bt_node_is_leaf(const minisql_page *pg)
{
    return bt_cbytes(pg)[BT_OFF_IS_LEAF] != 0;
}

static uint32_t bt_node_nkeys(const minisql_page *pg)
{
    uint16_t n = 0;
    bt_read16(bt_cbytes(pg), BT_OFF_NKEYS, &n);
    return (uint32_t)n;
}

static void bt_node_set_nkeys(minisql_page *pg, uint32_t n)
{
    bt_write16(bt_bytes(pg), BT_OFF_NKEYS, (uint16_t)n);
}

static minisql_pgno bt_node_link(const minisql_page *pg)
{
    uint32_t v = 0;
    bt_read32(bt_cbytes(pg), BT_OFF_LINK, &v);
    return (minisql_pgno)v;
}

static void bt_node_set_link(minisql_page *pg, minisql_pgno link)
{
    bt_write32(bt_bytes(pg), BT_OFF_LINK, (uint32_t)link);
}

/* 第 i 个 cell 的起始偏移 */
static uint32_t bt_cell_off(uint32_t i)
{
    return BT_OFF_CELLS + i * BT_CELL_SIZE;
}

static btree_key bt_cell_key(const minisql_page *pg, uint32_t i)
{
    uint32_t v = 0;
    bt_read32(bt_cbytes(pg), bt_cell_off(i), &v);
    return (btree_key)v;
}

static void bt_cell_set_key(minisql_page *pg, uint32_t i, btree_key k)
{
    bt_write32(bt_bytes(pg), bt_cell_off(i), (uint32_t)k);
}

/* 叶子里是 value，内部节点里是 child_pgno —— 同一个槽位 */
static uint32_t bt_cell_payload(const minisql_page *pg, uint32_t i)
{
    uint32_t v = 0;
    bt_read32(bt_cbytes(pg), bt_cell_off(i) + 4u, &v);
    return v;
}

static void bt_cell_set_payload(minisql_page *pg, uint32_t i, uint32_t v)
{
    bt_write32(bt_bytes(pg), bt_cell_off(i) + 4u, v);
}

static void bt_node_init(minisql_page *pg, int is_leaf)
{
    uint8_t *d = bt_bytes(pg);
    memset(d, 0, MINISQL_PAGE_SIZE);
    d[BT_OFF_IS_LEAF] = (uint8_t)(is_leaf ? 1u : 0u);
    bt_node_set_nkeys(pg, 0);
    bt_node_set_link(pg, MINISQL_PGNO_NULL);
}

/* ================================================================== *
 * 节点内二分查找
 * ================================================================== */

/* 第一个满足 keys[i] >= key 的下标（叶子用） */
static uint32_t bt_lower_bound(const minisql_page *pg, btree_key key)
{
    uint32_t lo = 0, hi = bt_node_nkeys(pg);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (bt_cell_key(pg, mid) < key) lo = mid + 1u;
        else                            hi = mid;
    }
    return lo;
}

/* 第一个满足 keys[i] > key 的下标（内部节点选孩子时用）
 * 返回值的语义：等于「keys 中 <= key 的个数」，也就是要走的 children 下标。 */
static uint32_t bt_upper_bound(const minisql_page *pg, btree_key key)
{
    uint32_t lo = 0, hi = bt_node_nkeys(pg);
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (bt_cell_key(pg, mid) <= key) lo = mid + 1u;
        else                             hi = mid;
    }
    return lo;
}

/* ================================================================== *
 * 路径栈
 * ================================================================== */

static void bt_path_release(minisql_pager *pager, bt_path *path)
{
    while (path->depth > 0) {
        path->depth--;
        pager_unpin(pager, path->e[path->depth].page, 0);
    }
}

/* ================================================================== *
 * 插入：在节点内插入一个 cell
 * ================================================================== */

/* 叶子：在 pos 处插入 (key, value)，后面的 cell 整体右移一格 */
static void bt_leaf_insert_at(minisql_page *pg, uint32_t pos,
                              btree_key key, btree_val val)
{
    uint32_t n = bt_node_nkeys(pg);
    uint8_t *d = bt_bytes(pg);

    if (pos < n) {
        memmove(d + bt_cell_off(pos + 1u),
                d + bt_cell_off(pos),
                (n - pos) * BT_CELL_SIZE);
    }
    bt_cell_set_key(pg, pos, key);
    bt_cell_set_payload(pg, pos, (uint32_t)val);
    bt_node_set_nkeys(pg, n + 1u);
}

/*
 * 内部节点：把分隔键 sep 插到第 j 个孩子之后，新孩子是 new_child。
 *
 * 变形的本质（关键，容易写错）：
 *   插入前  cells[j] = (saved_key, old_child)
 *   插入后  cells[j]     = (sep,       old_child)   <- old_child 的右界变成 sep
 *           cells[j + 1] = (saved_key, new_child)   <- 新孩子接在后面
 *
 * 这样 children[j] = old（管 [.., sep)），children[j+1] = new（管 [sep, saved_key)），
 * 键序仍然严格递增（sep < saved_key）。
 *
 * j == nkeys 表示 old_child 是 header.link，此时直接把 sep 追加成最后一个 cell，
 * 并把 link 改成 new_child。
 */
static void bt_insert_sep_into_internal(minisql_page *pg, uint32_t j,
                                        btree_key sep,
                                        minisql_pgno old_child,
                                        minisql_pgno new_child)
{
    uint32_t n = bt_node_nkeys(pg);
    uint8_t *d = bt_bytes(pg);

    if (j < n) {
        btree_key saved_key = bt_cell_key(pg, j);

        memmove(d + bt_cell_off(j + 2u),
                d + bt_cell_off(j + 1u),
                (n - j - 1u) * BT_CELL_SIZE);

        bt_node_set_nkeys(pg, n + 1u);
        bt_cell_set_key(pg, j, sep);
        bt_cell_set_payload(pg, j, (uint32_t)old_child);
        bt_cell_set_key(pg, j + 1u, saved_key);
        bt_cell_set_payload(pg, j + 1u, (uint32_t)new_child);
    } else {
        bt_cell_set_key(pg, n, sep);
        bt_cell_set_payload(pg, n, (uint32_t)old_child);
        bt_node_set_nkeys(pg, n + 1u);
        bt_node_set_link(pg, new_child);
    }
}

/* ================================================================== *
 * 分裂
 * ================================================================== */

/*
 * 分裂叶子。进入时 pg 的 nkeys == BT_MAX_CELLS（溢出 1 格）。
 * 左半边留在 pg，右半边放进新分配的页。
 * 分隔键 = 右半边的第一个 key（叶子是「复制上去」，key 不移走）。
 */
static minisql_errno bt_split_leaf(minisql_pager *pager, minisql_page *pg,
                                   minisql_pgno *right_out,
                                   btree_key *sep_out)
{
    uint32_t n = bt_node_nkeys(pg);
    uint32_t left_n = n / 2u;

    minisql_pgno right_pgno = MINISQL_PGNO_NULL;
    minisql_page *right = NULL;
    minisql_errno rc = pager_alloc(pager, &right_pgno, &right);
    if (rc != MINISQL_OK) {
        return rc;
    }

    bt_node_init(right, 1);

    memcpy(bt_bytes(right) + bt_cell_off(0),
           bt_bytes(pg) + bt_cell_off(left_n),
           (n - left_n) * BT_CELL_SIZE);
    bt_node_set_nkeys(right, n - left_n);
    bt_node_set_link(right, bt_node_link(pg));   /* 右半继承原来的叶子链 */

    bt_node_set_nkeys(pg, left_n);
    bt_node_set_link(pg, right_pgno);            /* 左半指向新右半 */

    *sep_out = bt_cell_key(right, 0);
    *right_out = right_pgno;

    /* 新页已经写好了内容，必须标脏落盘 */
    return pager_unpin(pager, right, 1);
}

/*
 * 分裂内部节点。进入时 pg 的 nkeys == BT_MAX_CELLS。
 *
 * 内部节点是「把中间那个 key 移走往上推」，不是复制：
 *
 *   left  : cells[0 .. L-1]           link = cells[L].child
 *   up    : cells[L].key              作为父节点的新分隔键
 *   right : cells[L+1 .. n-1]         link = 原来的 link
 *
 * L = n / 2。
 */
static minisql_errno bt_split_internal(minisql_pager *pager, minisql_page *pg,
                                       minisql_pgno *right_out,
                                       btree_key *sep_out)
{
    uint32_t n = bt_node_nkeys(pg);
    uint32_t left_n = n / 2u;
    btree_key up_key = bt_cell_key(pg, left_n);
    minisql_pgno left_link = (minisql_pgno)bt_cell_payload(pg, left_n);
    uint32_t right_n = n - left_n - 1u;

    minisql_pgno right_pgno = MINISQL_PGNO_NULL;
    minisql_page *right = NULL;
    minisql_errno rc = pager_alloc(pager, &right_pgno, &right);
    if (rc != MINISQL_OK) {
        return rc;
    }

    bt_node_init(right, 0);

    memcpy(bt_bytes(right) + bt_cell_off(0),
           bt_bytes(pg) + bt_cell_off(left_n + 1u),
           right_n * BT_CELL_SIZE);
    bt_node_set_nkeys(right, right_n);
    bt_node_set_link(right, bt_node_link(pg));

    bt_node_set_nkeys(pg, left_n);
    bt_node_set_link(pg, left_link);

    *sep_out = up_key;
    *right_out = right_pgno;

    return pager_unpin(pager, right, 1);
}

/* ================================================================== *
 * 公共接口：open / close / flush
 * ================================================================== */

minisql_errno btree_open(const char *filename, uint32_t cache_pages,
                         minisql_btree **out)
{
    if (filename == NULL || out == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_db *db = NULL;
    minisql_errno rc = db_open(filename, cache_pages, &db);
    if (rc != MINISQL_OK) {
        return rc;
    }

    minisql_btree *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        db_close(db);
        return MINISQL_ERR_NOMEM;
    }
    t->db = db;

    *out = t;
    return MINISQL_OK;
}

minisql_errno btree_close(minisql_btree *t)
{
    if (t == NULL) {
        return MINISQL_OK;
    }
    minisql_errno rc = db_close(t->db);
    free(t);
    return rc;
}

minisql_errno btree_flush(minisql_btree *t)
{
    if (t == NULL || t->db == NULL) {
        return MINISQL_ERR_INVALID;
    }

    /* 先把 root_page 写回头页，再把所有脏页刷盘 */
    minisql_errno rc = db_flush(t->db);
    minisql_errno f = pager_flush(db_pager(t->db));
    if (f != MINISQL_OK && rc == MINISQL_OK) {
        rc = f;
    }
    return rc;
}

/* ================================================================== *
 * 点查
 * ================================================================== */

minisql_errno btree_search(minisql_btree *t, btree_key key,
                           int *found, btree_val *val_out)
{
    if (t == NULL || t->db == NULL || found == NULL) {
        return MINISQL_ERR_INVALID;
    }
    *found = 0;

    minisql_pager *pager = db_pager(t->db);
    minisql_pgno pgno = db_root_page(t->db);

    while (pgno != MINISQL_PGNO_NULL) {
        minisql_page *pg = NULL;
        minisql_errno rc = pager_get(pager, pgno, &pg);
        if (rc != MINISQL_OK) {
            return rc;
        }

        uint32_t n = bt_node_nkeys(pg);

        if (bt_node_is_leaf(pg)) {
            uint32_t i = bt_lower_bound(pg, key);
            if (i < n && bt_cell_key(pg, i) == key) {
                *found = 1;
                if (val_out != NULL) {
                    *val_out = (btree_val)bt_cell_payload(pg, i);
                }
            }
            pager_unpin(pager, pg, 0);
            return MINISQL_OK;
        }

        uint32_t i = bt_upper_bound(pg, key);
        minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(pg, i)
                                    : bt_node_link(pg);
        pager_unpin(pager, pg, 0);

        if (child == MINISQL_PGNO_NULL || child >= pager_page_count(pager)) {
            return MINISQL_ERR_CORRUPT;
        }
        pgno = child;
    }

    return MINISQL_OK;   /* 空树 */
}

/* ================================================================== *
 * 插入
 * ================================================================== */

minisql_errno btree_insert(minisql_btree *t, btree_key key, btree_val val)
{
    if (t == NULL || t->db == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_pager *pager = db_pager(t->db);
    minisql_errno rc;

    /* ---------- 空树：建第一个叶子，它就是根 ---------- */
    minisql_pgno root = db_root_page(t->db);
    if (root == MINISQL_PGNO_NULL) {
        minisql_pgno np = MINISQL_PGNO_NULL;
        minisql_page *pg = NULL;
        rc = pager_alloc(pager, &np, &pg);
        if (rc != MINISQL_OK) {
            return rc;
        }
        bt_node_init(pg, 1);
        bt_cell_set_key(pg, 0, key);
        bt_cell_set_payload(pg, 0, (uint32_t)val);
        bt_node_set_nkeys(pg, 1);
        rc = pager_unpin(pager, pg, 1);
        if (rc != MINISQL_OK) {
            return rc;
        }
        return db_set_root_page(t->db, np);
    }

    /* ---------- 一路向下，沿途全部 pin 住 ---------- */
    bt_path path;
    path.depth = 0;

    minisql_pgno cur_pgno = root;
    minisql_page *cur = NULL;
    rc = pager_get(pager, cur_pgno, &cur);
    if (rc != MINISQL_OK) {
        return rc;
    }

    while (!bt_node_is_leaf(cur)) {
        if (path.depth >= BT_MAX_HEIGHT) {
            pager_unpin(pager, cur, 0);
            bt_path_release(pager, &path);
            return MINISQL_ERR_CORRUPT;
        }

        uint32_t n = bt_node_nkeys(cur);
        uint32_t i = bt_upper_bound(cur, key);
        minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(cur, i)
                                    : bt_node_link(cur);

        if (child == MINISQL_PGNO_NULL || child >= pager_page_count(pager)) {
            pager_unpin(pager, cur, 0);
            bt_path_release(pager, &path);
            return MINISQL_ERR_CORRUPT;
        }

        path.e[path.depth].pgno = cur_pgno;
        path.e[path.depth].page = cur;
        path.e[path.depth].child_idx = i;
        path.depth++;

        minisql_errno g = pager_get(pager, child, &cur);
        if (g != MINISQL_OK) {
            bt_path_release(pager, &path);
            return g;
        }
        cur_pgno = child;
    }

    /* ---------- 叶子内插入 ---------- */
    uint32_t n = bt_node_nkeys(cur);
    uint32_t pos = bt_lower_bound(cur, key);

    if (pos < n && bt_cell_key(cur, pos) == key) {
        /* upsert：覆盖旧 value */
        bt_cell_set_payload(cur, pos, (uint32_t)val);
        pager_unpin(pager, cur, 1);
        bt_path_release(pager, &path);
        return MINISQL_OK;
    }

    bt_leaf_insert_at(cur, pos, key, val);

    if (bt_node_nkeys(cur) <= BT_MAX_KEYS) {
        pager_unpin(pager, cur, 1);
        bt_path_release(pager, &path);
        return MINISQL_OK;
    }

    /* ---------- 叶子溢出 -> 分裂，然后自底向上推 ---------- */
    btree_key  up_sep = 0;
    minisql_pgno up_right = MINISQL_PGNO_NULL;
    rc = bt_split_leaf(pager, cur, &up_right, &up_sep);
    if (rc != MINISQL_OK) {
        pager_unpin(pager, cur, 1);
        bt_path_release(pager, &path);
        return rc;
    }
    minisql_pgno up_left = cur_pgno;
    pager_unpin(pager, cur, 1);

    while (path.depth > 0) {
        bt_path_entry *pe = &path.e[path.depth - 1u];
        minisql_page *parent = pe->page;
        uint32_t j = pe->child_idx;

        bt_insert_sep_into_internal(parent, j, up_sep, up_left, up_right);
        path.depth--;

        if (bt_node_nkeys(parent) <= BT_MAX_KEYS) {
            pager_unpin(pager, parent, 1);
            bt_path_release(pager, &path);
            return MINISQL_OK;
        }

        /* 父节点也溢出：继续分裂 */
        btree_key  sep2 = 0;
        minisql_pgno right2 = MINISQL_PGNO_NULL;
        rc = bt_split_internal(pager, parent, &right2, &sep2);
        if (rc != MINISQL_OK) {
            pager_unpin(pager, parent, 1);
            bt_path_release(pager, &path);
            return rc;
        }

        up_left  = pe->pgno;
        up_sep   = sep2;
        up_right = right2;
        pager_unpin(pager, parent, 1);
    }

    /* ---------- 根也分裂了 -> 造新根，树高 +1 ---------- */
    minisql_pgno nr = MINISQL_PGNO_NULL;
    minisql_page *npg = NULL;
    rc = pager_alloc(pager, &nr, &npg);
    if (rc != MINISQL_OK) {
        return rc;
    }

    bt_node_init(npg, 0);
    bt_cell_set_key(npg, 0, up_sep);
    bt_cell_set_payload(npg, 0, (uint32_t)up_left);
    bt_node_set_nkeys(npg, 1);
    bt_node_set_link(npg, up_right);

    rc = pager_unpin(pager, npg, 1);
    if (rc != MINISQL_OK) {
        return rc;
    }
    return db_set_root_page(t->db, nr);
}

/* ================================================================== *
 * 删除（只摘 key，不合并）
 * ================================================================== */

minisql_errno btree_delete(minisql_btree *t, btree_key key)
{
    if (t == NULL || t->db == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_pager *pager = db_pager(t->db);
    minisql_pgno pgno = db_root_page(t->db);

    while (pgno != MINISQL_PGNO_NULL) {
        minisql_page *pg = NULL;
        minisql_errno rc = pager_get(pager, pgno, &pg);
        if (rc != MINISQL_OK) {
            return rc;
        }

        uint32_t n = bt_node_nkeys(pg);

        if (!bt_node_is_leaf(pg)) {
            uint32_t i = bt_upper_bound(pg, key);
            minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(pg, i)
                                         : bt_node_link(pg);
            pager_unpin(pager, pg, 0);
            if (child == MINISQL_PGNO_NULL || child >= pager_page_count(pager)) {
                return MINISQL_ERR_CORRUPT;
            }
            pgno = child;
            continue;
        }

        uint32_t pos = bt_lower_bound(pg, key);
        if (pos < n && bt_cell_key(pg, pos) == key) {
            if (pos + 1u < n) {
                memmove(bt_bytes(pg) + bt_cell_off(pos),
                        bt_bytes(pg) + bt_cell_off(pos + 1u),
                        (n - pos - 1u) * BT_CELL_SIZE);
            }
            bt_node_set_nkeys(pg, n - 1u);
            pager_unpin(pager, pg, 1);
        } else {
            pager_unpin(pager, pg, 0);   /* 不存在：什么都不改 */
        }
        return MINISQL_OK;
    }

    return MINISQL_OK;   /* 空树 */
}

/* ================================================================== *
 * 范围扫描
 * ================================================================== */

minisql_errno btree_scan(minisql_btree *t, btree_key lo, btree_key hi,
                         btree_visit_fn visit, void *ctx)
{
    if (t == NULL || t->db == NULL || visit == NULL) {
        return MINISQL_ERR_INVALID;
    }
    if (hi < lo) {
        return MINISQL_OK;
    }

    minisql_pager *pager = db_pager(t->db);
    minisql_pgno pgno = db_root_page(t->db);
    if (pgno == MINISQL_PGNO_NULL) {
        return MINISQL_OK;
    }

    /* ---------- 找到 lo 可能所在的最左叶子 ---------- */
    minisql_page *cur = NULL;
    minisql_errno rc;

    for (;;) {
        rc = pager_get(pager, pgno, &cur);
        if (rc != MINISQL_OK) {
            return rc;
        }
        if (bt_node_is_leaf(cur)) {
            break;
        }
        uint32_t n = bt_node_nkeys(cur);
        uint32_t i = bt_upper_bound(cur, lo);
        minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(cur, i)
                                    : bt_node_link(cur);
        pager_unpin(pager, cur, 0);
        cur = NULL;
        if (child == MINISQL_PGNO_NULL || child >= pager_page_count(pager)) {
            return MINISQL_ERR_CORRUPT;
        }
        pgno = child;
    }

    /* ---------- 沿叶子链走 ---------- */
    while (cur != NULL) {
        uint32_t n = bt_node_nkeys(cur);
        uint32_t i = bt_lower_bound(cur, lo);

        for (; i < n; i++) {
            btree_key k = bt_cell_key(cur, i);
            if (k > hi) {
                pager_unpin(pager, cur, 0);
                return MINISQL_OK;
            }
            if (visit(k, (btree_val)bt_cell_payload(cur, i), ctx) != 0) {
                pager_unpin(pager, cur, 0);
                return MINISQL_OK;   /* 调用方要求提前结束 */
            }
        }

        minisql_pgno next = bt_node_link(cur);
        minisql_page *nxt = NULL;

        if (next != MINISQL_PGNO_NULL) {
            if (next >= pager_page_count(pager)) {
                pager_unpin(pager, cur, 0);
                return MINISQL_ERR_CORRUPT;
            }
            rc = pager_get(pager, next, &nxt);
            if (rc != MINISQL_OK) {
                pager_unpin(pager, cur, 0);
                return rc;
            }
        }

        pager_unpin(pager, cur, 0);
        cur = nxt;
    }

    return MINISQL_OK;
}

/* ================================================================== *
 * 调试 / 校验
 * ================================================================== */

int btree_height(minisql_btree *t)
{
    if (t == NULL || t->db == NULL) {
        return 0;
    }

    minisql_pager *pager = db_pager(t->db);
    minisql_pgno pgno = db_root_page(t->db);
    int h = 0;

    /* 沿最右孩子一路走到底，层数就是树高 */
    while (pgno != MINISQL_PGNO_NULL) {
        minisql_page *pg = NULL;
        if (pager_get(pager, pgno, &pg) != MINISQL_OK) {
            return h;
        }
        h++;
        if (bt_node_is_leaf(pg)) {
            pager_unpin(pager, pg, 0);
            return h;
        }
        pgno = bt_node_link(pg);
        pager_unpin(pager, pg, 0);
    }

    return h;
}

#define BT_CHECK_FAIL() do { pager_unpin(pager, pg, 0); return MINISQL_ERR_CORRUPT; } while (0)

static minisql_errno bt_check_rec(minisql_pager *pager, minisql_pgno pgno,
                                  int is_root,
                                  int has_low, btree_key low,
                                  int has_high, btree_key high,
                                  uint32_t depth, uint32_t *leaf_level)
{
    if (pgno < 1 || pgno >= pager_page_count(pager)) {
        return MINISQL_ERR_CORRUPT;
    }

    minisql_page *pg = NULL;
    minisql_errno rc = pager_get(pager, pgno, &pg);
    if (rc != MINISQL_OK) {
        return rc;
    }

    uint32_t n = bt_node_nkeys(pg);
    if (n > BT_MAX_KEYS) BT_CHECK_FAIL();

    /* 不变式 5：节点内 key 严格升序 */
    for (uint32_t i = 1; i < n; i++) {
        if (!(bt_cell_key(pg, i - 1u) < bt_cell_key(pg, i))) BT_CHECK_FAIL();
    }

    if (bt_node_is_leaf(pg)) {
        /* 叶子 key 必须落在父节点给的开区间内 */
        for (uint32_t i = 0; i < n; i++) {
            btree_key k = bt_cell_key(pg, i);
            if (has_low  && k <  low)  BT_CHECK_FAIL();
            if (has_high && !(k < high)) BT_CHECK_FAIL();
        }
        uint32_t level = depth + 1u;
        if (*leaf_level == 0u) {
            *leaf_level = level;
        } else if (*leaf_level != level) {
            BT_CHECK_FAIL();   /* 不变式 3：所有叶子同层 */
        }
        pager_unpin(pager, pg, 0);
        return MINISQL_OK;
    }

    /* 不变式 1/2：内部节点至少 1 个 key，共 n+1 个孩子 */
    if (n == 0u) BT_CHECK_FAIL();

    for (uint32_t i = 0; i <= n; i++) {
        minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(pg, i)
                                     : bt_node_link(pg);
        if (child == MINISQL_PGNO_NULL || child >= pager_page_count(pager)) {
            BT_CHECK_FAIL();
        }

        int cl = (i == 0u) ? has_low  : 1;
        btree_key clo = (i == 0u) ? low : bt_cell_key(pg, i - 1u);
        int ch = (i == n)  ? has_high : 1;
        btree_key chi = (i == n)  ? high : bt_cell_key(pg, i);

        rc = bt_check_rec(pager, child, 0, cl, clo, ch, chi, depth + 1u, leaf_level);
        if (rc != MINISQL_OK) {
            pager_unpin(pager, pg, 0);
            return rc;
        }
    }

    pager_unpin(pager, pg, 0);
    (void)is_root;
    return MINISQL_OK;
}

#undef BT_CHECK_FAIL

minisql_errno btree_check(minisql_btree *t)
{
    if (t == NULL || t->db == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_pager *pager = db_pager(t->db);
    minisql_pgno root = db_root_page(t->db);
    if (root == MINISQL_PGNO_NULL) {
        return MINISQL_OK;   /* 空树，结构上没问题 */
    }

    uint32_t leaf_level = 0;
    minisql_errno rc = bt_check_rec(pager, root, 1, 0, 0, 0, 0, 0, &leaf_level);
    if (rc != MINISQL_OK) {
        return rc;
    }

    /* 再走一遍叶子链：全局升序、无环、终点为 NULL */
    minisql_pgno pgno = root;
    for (int guard = 0; pgno != MINISQL_PGNO_NULL; guard++) {
        if (guard > (int)pager_page_count(pager) + 1) {
            return MINISQL_ERR_CORRUPT;   /* 成环 */
        }
        minisql_page *pg = NULL;
        rc = pager_get(pager, pgno, &pg);
        if (rc != MINISQL_OK) {
            return rc;
        }
        if (bt_node_is_leaf(pg)) {
            pager_unpin(pager, pg, 0);
            break;
        }
        pgno = (minisql_pgno)bt_cell_payload(pg, 0);   /* 最左孩子 */
        pager_unpin(pager, pg, 0);
    }

    int have_prev = 0;
    btree_key prev = 0;
    int guard2 = 0;

    while (pgno != MINISQL_PGNO_NULL) {
        if (++guard2 > (int)pager_page_count(pager) + 1) {
            return MINISQL_ERR_CORRUPT;
        }
        minisql_page *pg = NULL;
        rc = pager_get(pager, pgno, &pg);
        if (rc != MINISQL_OK) {
            return rc;
        }
        uint32_t n = bt_node_nkeys(pg);
        for (uint32_t i = 0; i < n; i++) {
            btree_key k = bt_cell_key(pg, i);
            if (have_prev && !(prev < k)) {
                pager_unpin(pager, pg, 0);
                return MINISQL_ERR_CORRUPT;
            }
            prev = k;
            have_prev = 1;
        }
        pgno = bt_node_link(pg);
        pager_unpin(pager, pg, 0);
    }

    return MINISQL_OK;
}

static void bt_dump_rec(minisql_btree *t, minisql_pgno pgno, int level,
                        int is_root)
{
    minisql_pager *pager = db_pager(t->db);
    if (pgno == MINISQL_PGNO_NULL) {
        printf("%*s(NULL)\n", level * 2, "");
        return;
    }

    minisql_page *pg = NULL;
    if (pager_get(pager, pgno, &pg) != MINISQL_OK) {
        printf("%*s[pager_get(%u) 失败]\n", level * 2, "", (unsigned)pgno);
        return;
    }

    uint32_t n = bt_node_nkeys(pg);
    int leaf = bt_node_is_leaf(pg);

    printf("%*s%s pgno=%u%s nkeys=%u  keys=[",
           level * 2, "",
           leaf ? "LEAF" : "INT ",
           (unsigned)pgno,
           is_root ? " (root)" : "",
           (unsigned)n);

    uint32_t show = n > 8u ? 8u : n;
    for (uint32_t i = 0; i < show; i++) {
        printf("%u%s", (unsigned)bt_cell_key(pg, i), (i + 1u < show) ? " " : "");
    }
    if (show < n) printf(" ...");
    printf("]");

    if (leaf) {
        printf("  link=%d\n", (int)(int32_t)bt_node_link(pg));
    } else {
        printf("  link=%u\n", (unsigned)bt_node_link(pg));
    }

    if (!leaf) {
        for (uint32_t i = 0; i <= n; i++) {
            minisql_pgno child = (i < n) ? (minisql_pgno)bt_cell_payload(pg, i)
                                         : bt_node_link(pg);
            bt_dump_rec(t, child, level + 1, 0);
        }
    }

    pager_unpin(pager, pg, 0);
}

void btree_dump(minisql_btree *t)
{
    if (t == NULL || t->db == NULL) {
        printf("btree_dump: btree 为空\n");
        return;
    }

    minisql_pgno root = db_root_page(t->db);
    printf("---- btree dump (height=%d, root=%d) ----\n",
           btree_height(t), (int)(int32_t)root);
    if (root == MINISQL_PGNO_NULL) {
        printf("(empty tree)\n");
        return;
    }
    bt_dump_rec(t, root, 0, 1);
    printf("---- end ----\n");
}