#include "minisql/common.h"
#include <errno.h>
const char* minisql_strerror(int rc){
switch(rc){
    case MINISQL_OK: return "OK";
    case MINISQL_ERR_IO: return "I/O error";
    case MINISQL_ERR_CORRUPT: return "File format corrupted";
    case MINISQL_ERR_NOMEM: return "Out of memory";
    case MINISQL_ERR_NOTIMPL: return "Not implemented";
    case MINISQL_ERR_NOSPC: return "No space left on device";
    case MINISQL_ERR_CACHE_FULL: return "Page cache full (all frames pinned)";
    default: return "Unknown error code";
}
}

/* minisql_errno_sys(errno) 只能出现在这三种地方：
*pwrite/pread 返回 -1 之后、
*fsync 返回 -1 之后、
*open/fstat/calloc 失败之后。*/
minisql_errno minisql_errno_sys(int e){
    switch (e)
    {
    case ENOMEM:  return MINISQL_ERR_NOMEM;
    case EIO:     return MINISQL_ERR_IO;
    case ENOSPC: /*与下面的EDQUOT共享一个标签*/
    case EDQUOT:  return MINISQL_ERR_NOSPC;   /* EDQUOT = 超出磁盘配额，语义相同 */
    case EFBIG:   return MINISQL_ERR_NOSPC;   /* 超过文件大小上限 */ 
    case EBADF:
    case EINVAL:  return MINISQL_ERR_IO;
    default:
        return MINISQL_ERR_IO;
    }

}




