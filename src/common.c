#include "minisql/common.h"
#include <errno.h>
const char* minisql_strerror(int rc){
switch(rc){
    case MINISQL_OK: return "OK";
    case MINISQL_ERR_IO: return "I/O error";
    case MINISQL_ERR_CORRUPT: return "File format corrupted";
    case MINISQL_ERR_NOMEM: return "Out of memory";
    case MINISQL_ERR_NOTIMPL: return "Not implemented";
    default: return "Unknown error code";
}
}
minisql_errno minisql_errno_sys(int e){
    switch (e)
    {
    case ENOMEM:  return MINISQL_ERR_NOMEM;
    case EIO:     return MINISQL_ERR_IO;
     
    default:
        return MINISQL_ERR_IO;
    }

}




