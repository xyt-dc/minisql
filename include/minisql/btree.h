#ifndef MINISQL_BTREE_H
#define MINISQL_BTREE_H

#include "minisql/db.h"

/* ==================================================================
 * B+ 树节点页布局（全树统一，叶子和内部节点共用一套 header）
 *
 *   偏移 0 : uint8   is_leaf      1 = 叶子, 0 = 内部节点
 *   偏移 1 : uint8   reserved     保留，恒为 0
 *   偏移 2 : uint16  nkeys        当前 cell 个数
 *   偏移 4 : pgno    link
 *              叶子  -> 下一个叶子的页号（叶子链表）
 *              内部  -> 最右孩子页号（因为 n 个 key 有 n+1 个孩子）
 *   偏移 8 : cell[] 每个 8 字节，连续存放，按 key 升序
 *              叶子 cell = key(4B) + value(4B)
 *              内部 cell = key(4B) + child_pgno(4B)
 *
 * 内部节点搜索语义（所有代码都依赖这一条，务必记牢）：
 *
 *   children[i] = cells[i].child     i ∈ [0, nkeys)
 *   children[nkeys] = header.link
 *
 *   children[i] 子树里的 key 落在 [ keys[i-1], keys[i] )
 *   children[nkeys] 子树里的 key 落在 [ keys[nkeys-1], +∞ )
 *
 *   要找 key 走哪个孩子：取第一个满足 key < keys[i] 的 i；没有则 i = nkeys。
 * ================================================================== */
#define BT_NODE_HDR_SIZE 8u
#define BT_CELL_SIZE     8u
/* 物理容量：一页能塞下的 cell 数 */
#define BT_MAX_CELLS     ((MINISQL_PAGE_SIZE - BT_NODE_HDR_SIZE) / BT_CELL_SIZE)
/* 逻辑上限：留一个 cell 的余量给"先插入再判断溢出" */
#define BT_MAX_KEYS      (BT_MAX_CELLS - 1u)

#define BT_OFF_IS_LEAF   0u
#define BT_OFF_NKEYS     2u
#define BT_OFF_LINK      4u
#define BT_OFF_CELLS     8u

/* 定长 key / 定长 value。
 * 想支持字符串 key 时只需把 btree_key 换成定长结构体，
 * 页布局和分裂逻辑一行都不用改（扇出会下降）。 */
typedef uint32_t btree_key;
typedef uint32_t btree_val;

typedef struct minisql_btree minisql_btree;

/* ------------------------------------------------------------------ *
 * 生命周期
 * ------------------------------------------------------------------ *
 * btree_open 内部调用 db_open，所以树的数据和头页在同一个文件里；
 * 根页号存在 db 头页的 root_page 字段，不在 btree 对象里留副本。
 */
minisql_errno btree_open(const char *filename, uint32_t cache_pages,
                         minisql_btree **out);
minisql_errno btree_close(minisql_btree *t);

/* 把头页（含 root_page）和所有脏页刷到磁盘 */
minisql_errno btree_flush(minisql_btree *t);

/* ------------------------------------------------------------------ *
 * 点查
 * ------------------------------------------------------------------ *
 * found 输出 1 表示命中、0 表示没找到；val_out 仅在命中时被写。
 */
minisql_errno btree_search(minisql_btree *t, btree_key key,
                           int *found, btree_val *val_out);

/* ------------------------------------------------------------------ *
 * 插入（upsert：key 已存在则覆盖 value）
 * ------------------------------------------------------------------ */
minisql_errno btree_insert(minisql_btree *t, btree_key key, btree_val val);

/* ------------------------------------------------------------------ *
 * 删除
 * ------------------------------------------------------------------ *
 * 只把 key 从叶子里摘掉，不做节点合并/借用（不 rebalance）。
 * 因此可能留下空叶子和冗余分隔键：搜索与范围扫描的语义仍然正确，
 * 只是空间利用率会随删除量下降。这是有意的简化。
 */
minisql_errno btree_delete(minisql_btree *t, btree_key key);

/* ------------------------------------------------------------------ *
 * 范围扫描：遍历 [lo, hi] 闭区间内的记录，按 key 升序
 * ------------------------------------------------------------------ *
 * visit 返回非 0 时提前终止（整体仍返回 MINISQL_OK）。
 */
typedef int (*btree_visit_fn)(btree_key key, btree_val val, void *ctx);
minisql_errno btree_scan(minisql_btree *t, btree_key lo, btree_key hi,
                         btree_visit_fn visit, void *ctx);

/* ------------------------------------------------------------------ *
 * 调试
 * ------------------------------------------------------------------ *
 * btree_height 返回层数（空树为 0，只有一个叶子根为 1）。
 * btree_check 遍历整棵树校验不变量，返回 MINISQL_OK 表示结构自洽。
 * btree_dump  把树结构打印到 stdout，分裂调试时非常有用。
 */
int  btree_height(minisql_btree *t);
minisql_errno btree_check(minisql_btree *t);
void btree_dump(minisql_btree *t);

#endif /* MINISQL_BTREE_H */