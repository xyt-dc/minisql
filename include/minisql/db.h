#ifndef MINISQL_DB_H
#define MINISQL_DB_H

#include "minisql/pager.h"

/*
 * db 层 —— 负责「页 0 头页」的语义。
 *
 * 分层约定：
 *   pager.c  只知道「页号 <-> 内存页」，不知道页里那些字节是什么意思。
 *   db.c     才知道头页里放的是什么（魔数/版本/page_count/root_page/freelist_head）。
 *
 * freelist_head 的唯一权威在 pager 内部（pager_alloc/pager_free 直接改它）；
 * db 层只在 db_open 时把它「装载」进 pager、在 db_flush 时「取出」写回头页。
 * root_page 的权威在 db 内部（B+ 树换了根，由上层调 db_set_root_page 通知 db）。
 */
typedef struct minisql_db minisql_db;

/* 打开（或新建）数据库。
 * 新库会自动初始化头页；老库会校验魔数与版本。
 * 成功返回 MINISQL_OK 并写出 *db_out；失败时 *db_out 不被修改。 */
minisql_errno db_open(const char *filename, uint32_t cache_pages, minisql_db **db_out);

/* 写回头页（含 freelist_head 快照）并关闭 pager。
 * db 传 NULL 时直接返回 OK，方便上层统一清理。 */
minisql_errno db_close(minisql_db *db);

/* 把头页的四个字段写回磁盘。不调用 pager_flush 刷整个缓存，
 * 但会把页 0 标脏；想要真正落盘请随后调用 pager_flush 或 db_close。 */
minisql_errno db_flush(minisql_db *db);

/* root_page 读写。新库是 MINISQL_PGNO_NULL，表示 B+ 树还没建起来。 */
minisql_pgno db_root_page(const minisql_db *db);
minisql_errno db_set_root_page(minisql_db *db, minisql_pgno root);

/* 需要直接用 pager（比如 B+ 树层要 alloc/get 页）时拿 pager。 */
minisql_pager *db_pager(minisql_db *db);

#endif /* MINISQL_DB_H */