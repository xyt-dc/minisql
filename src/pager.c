#include "minisql/pager.h"
#include <fcntl.h>
#include<unistd.h>
#include<sys/stat.h>
#include<stdlib.h>
#include<assert.h>
struct minisql_page {
    minisql_pgno pgno; /*我当前装的是磁盘第几页;MINISQL_PGNO_NULL 表示空页框*/ 
    int pin;
    int dirty; /*和磁盘是否一致*/
    minisql_page *hnext;
    minisql_page *lru_prev;
    minisql_page *lru_next;
    uint8_t data[MINISQL_PAGE_SIZE]; /*页内容 */
};
struct minisql_pager{
    int fd;
    minisql_page *frames;
    
    uint32_t cache_capacity;
    uint32_t cache_count;

    minisql_page *lru_head;
    minisql_page *lru_tail;
    

    minisql_page **buckets;
    uint32_t hash_mask;
    minisql_page *free_frames;

    uint32_t page_count;  
    uint32_t freelist_head;
    uint32_t root_page;
    
};
uint8_t *page_data(minisql_page* page){
    return page->data;
}
uint32_t pager_page_count(const minisql_pager *pager){
    return pager->page_count;
}

/*向上取整2的幂
*这里采用循环翻倍路线，后面可修改为位运算抹开路线
*/
static uint32_t round_up_pow2(uint32_t n){
    uint32_t result = 1;
    while (result < n){
        result = result << 1; /* 左移一位 = 乘 2 */
    }
    return result;
}
    


minisql_errno pager_open(const char* filename,uint32_t cache_pages,minisql_pager **pager_out){  
    minisql_errno rc; 
   
    /*打开或关联文件‌：获取数据库文件的文件描述符*/
    int fd = open(filename,O_RDWR|O_CREAT,0644);
    if (fd== -1){
        
        return MINISQL_ERR_IO;
        
    }

    /*获取文件大小（字节数）*/   
    struct stat st;
    
    /*fstat函数用于返回关于文件的信息到一个struct stat结构中,其中stat结构中的st_mode可以用来区分文件类型。*/
    int fst = fstat(fd,&st);
    if (fst == -1){
        close(fd);
        return MINISQL_ERR_IO;
    }
    /*校验文件大小，拒绝坏文件*/
    if (st.st_size !=0 && st.st_size % MINISQL_PAGE_SIZE !=0){
        close(fd);
        return MINISQL_ERR_CORRUPT;      
    }

    minisql_pager *pager = calloc(1,sizeof(minisql_pager)); /*分配一个动态内存,calloc失败时返回NULL*/
    if (!pager){
        rc = MINISQL_ERR_NOMEM;
        goto fail_pager;
    }
    pager->fd = fd;

    if (st.st_size == 0)
    {
        pager->page_count = 1;  
    }
    
    else
        pager->page_count = st.st_size / MINISQL_PAGE_SIZE;

/*初始化缓存结构‌：把页面缓存数组或哈希表置空，为后续按需加载页做准备*/
    pager->cache_capacity = round_up_pow2(cache_pages);
    assert(pager->cache_capacity >= cache_pages);          /* 不能比请求的小 */
    assert(pager->cache_capacity < 2 * cache_pages);       /* 也不能大太多 */

    pager->hash_mask = pager->cache_capacity - 1; /*？？hash_mask用来干什么的*/

    /*分配缓存页框数组和哈希桶数组‌：frames 数组存放缓存页框，buckets 数组存放哈希桶指针*/
    minisql_page *frames = calloc(pager->cache_capacity,sizeof(minisql_page)); /*返回首页框的地址*/
    if(!frames){
        
        rc= MINISQL_ERR_NOMEM;
        goto fail_frames;
    }
    pager->frames = frames;
    minisql_page **buckets = calloc(pager->cache_capacity,sizeof(minisql_page*));/*负责指向 frames 中的页面*/
    if(!buckets){
        
        rc = MINISQL_ERR_NOMEM;
        goto fail_buckets;
    }
    pager->buckets = buckets;
    /**/
    
    for (uint32_t i=0;i<pager->cache_capacity;i++){
        frames[i].lru_next = i==pager->cache_capacity-1 ? NULL : frames+i+1;                          
    }
    
    
    pager -> free_frames = frames;
    pager -> lru_head = NULL;
    pager -> lru_tail = NULL;
    *pager_out = pager;
    return MINISQL_OK;


fail_buckets:
    free(frames);  /*？？free局部变量frames还是pager里面的frames？？*/
fail_frames:
    free(pager);
fail_pager:
    close(fd);
    return rc;  

}