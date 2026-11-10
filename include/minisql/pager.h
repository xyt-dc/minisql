#ifndef MINISQL_PAGER_H
#define MINISQL_PAGER_H
#include "minisql/common.h"
typedef struct minisql_page  minisql_page; /*磁盘上的页是"内容"，minisql_page 是"一个能装下内容、并附带管理信息哪些页我装着、被引用几次...)的箱子"。*/
typedef struct minisql_pager minisql_pager; /* 一个"页框管理器"（管着整个文件 + 所有页框）。*/

/* 打开一个数据库文件，返回一个 pager 对象
 * filename: 数据库文件名
 * cache_pages: 缓存页数
 * pager_out: 输出参数，返回 pager 对象
 * 返回值: minisql_errno 错误码
 * */
minisql_errno pager_open(const char* filename,uint32_t cache_pages,minisql_pager** pager_out); /*往地址里面写，相当于返回了*/

/*返回的页是 pin 住的，调用方必须调用 pager_unpin 释放。
*返回已有的页
*/
minisql_errno pager_get(minisql_pager* pager,minisql_pgno pgno,minisql_page **page_out);

minisql_errno pager_unpin(minisql_pager* pager,minisql_page* pg,int dirty);

minisql_errno pager_free(minisql_pager* pager,minisql_pgno pgno);

uint8_t *page_data(minisql_page* page);

/*新建一个页的入口*/
minisql_errno pager_alloc(minisql_pager *pager,minisql_pgno *pgno_out, minisql_page **page_out);

minisql_errno pager_flush(minisql_pager *pager);

uint32_t pager_page_count(const minisql_pager *pager);

minisql_errno pager_close(minisql_pager *pager);

#endif