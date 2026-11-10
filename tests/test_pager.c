#include"minisql/pager.h"
#include <assert.h>
#include<stdio.h>
int main() {
    minisql_pager *pager = NULL;
    minisql_errno lic = pager_open("tests/test_db.c",100,&pager);
    assert(lic == MINISQL_OK);
    printf("pager_open rc = %d\n",lic); 
    return 0;
}
