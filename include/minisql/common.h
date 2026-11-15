
#ifndef MINISQL_COMMON_H
#define MINISQL_COMMON_H
#include <stdint.h>
#define MINISQL_PAGE_SIZE 4096     /* 页大小  单位为字节 */
#define MINISQL_HEADER_SIZE 100  /* 文件头大小  单位为字节 */
typedef uint32_t minisql_pgno;  /*页号*/
#define MINISQL_PGNO_HEADER  0u  /*页0放文件头*/
#define MINISQL_PGNO_NULL    0xFFFFFFFFu

/* 统一错误码 */
typedef enum {
    MINISQL_OK          = 0,    /* 成功 */
    MINISQL_ERR_IO      = -1,   /* 底层 read/write/pread/pwrite 失败 */
    MINISQL_ERR_CORRUPT = -2,   /* 文件格式损坏（魔数、页号越界等） */
    MINISQL_ERR_NOMEM   = -3,   /* 内存分配失败 */
    MINISQL_ERR_NOTIMPL = -4,   /* 功能尚未实现（当前骨架阶段） */
    MINISQL_ERR_NOSPC   = -5,   /* 磁盘空间/配额不足，清理后可重试 */
    MINISQL_ERR_INVALID = -6,   /*参数无效*/
}minisql_errno;
/*页大小必须是2的幂*/
_Static_assert((MINISQL_PAGE_SIZE & (MINISQL_PAGE_SIZE - 1)) == 0, "Page size must be a power of 2");
/*文件头不能超过一页*/
_Static_assert(MINISQL_HEADER_SIZE <= MINISQL_PAGE_SIZE, "Header size exceeds one page");
const char* minisql_strerror(int rc);
minisql_errno minisql_errno_sys(int e);
#endif 
