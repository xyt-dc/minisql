#include "minisql/pager.h"
#include <fcntl.h>
#include<unistd.h>
#include<sys/stat.h>
#include<stdlib.h>
#include<assert.h>
#include<stdbool.h>
#include<errno.h>
struct minisql_page {
    minisql_pgno pgno; /*这个缓存帧当前存放的是数据库文件中的第几页;MINISQL_PGNO_NULL 表示空页框（这个缓存帧目前没有对应的数据库页面。）*/ 
    int pin;
    bool dirty; /*和磁盘是否一致,f表示干净页，true表示脏页*/
    minisql_page *hnext;
    minisql_page *lru_prev;
    minisql_page *lru_next;
    uint8_t data[MINISQL_PAGE_SIZE]; /*页内容 */
};
struct minisql_pager{
    int fd;
    minisql_page *frames; /*缓存帧*/
    
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

    static const uint8_t zero_page[MINISQL_PAGE_SIZE];   /* 全零，BSS   const 也有实际作用：程序运行期间这块内存只读，被放进只读段，误写会立刻段错误（而不是静默改坏）。pwrite 的签名是 const void *buf，兼容。*/
        if (pwrite(fd, zero_page, MINISQL_PAGE_SIZE, 0) != MINISQL_PAGE_SIZE) {
            rc = MINISQL_ERR_IO;
            goto fail_pager;   /* 注意此时 pager 还没分配，要调整 goto 目标 */
    }
        pager->page_count = 1;  
    }
    
    else
        pager->page_count = st.st_size / MINISQL_PAGE_SIZE;

/*初始化缓存结构‌：把页面缓存数组或哈希表置空，为后续按需加载页做准备*/
    pager->cache_capacity = round_up_pow2(cache_pages);
    assert(pager->cache_capacity >= cache_pages);          /* 不能比请求的小 */

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
    
    pager->freelist_head = MINISQL_PGNO_NULL;
    pager->root_page  = MINISQL_PGNO_NULL;
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

/*把内存中一个缓存页面的数据，完整地写回数据库文件中对应的页面位置。
*minisql_pager *pager：要写回的是哪个文件的缓存页
*minisql_page *page：要写回的缓存页
*/
static minisql_errno pager_write_page(minisql_pager *pager,minisql_page *page){
    if(pager == NULL || page == NULL){
        return MINISQL_ERR_IO; /*调用者传入的参数不合法。??这不属于正常的文件 I/O 错误，更不能返回成功。*/
    }
    if(page->pgno == MINISQL_PGNO_NULL || page->pgno >= pager->page_count){
        return MINISQL_ERR_CORRUPT; /*空页框或越界页不能写盘*/
    }
    off_t offset = (off_t)page->pgno*MINISQL_PAGE_SIZE; 
    size_t written = 0;
    while(written < MINISQL_PAGE_SIZE){

         /*ssize_t pwrite(int fd,const void *buf,size_t count,off_t offset);
        *fd：文件描述符。
        *buf：要写入的数据在内存中的地址。
        *count：写入多少字节。
        *offset：从文件的哪个字节位置开始写。
        *ssize_t:实际成功写入的字节数，失败返回 -1*/
        ssize_t n = pwrite(pager->fd,page->data + written,MINISQL_PAGE_SIZE - written,offset + (off_t)written);
        if(n == -1){
            if(errno == EINTR) continue;
            return minisql_errno_sys(errno);/*?这样写应该对吧*/
        }
        if(n == 0){
            return MINISQL_ERR_IO;
        }
        written += (size_t)n;
    }
    return MINISQL_OK;
}
/*pager_flush() 负责找出哪些页面需要写回，而 pager_write_page() 负责具体写回其中一个页面。*/
minisql_errno pager_flush(minisql_pager *pager){
    if(!pager){
        return MINISQL_ERR_IO; /*不能返回 MINISQL_OK，那是掩盖调用方的 bug。*/
    }
    minisql_errno rc = MINISQL_OK;
    bool any_dirty = false; /*这轮 flush 有没有产生过需要持久化的写"的累积标记*/
    for (uint32_t i = 0;i<pager->cache_capacity;i++){
        minisql_page *f = &pager->frames[i];
        if(f->pgno == MINISQL_PGNO_NULL || !f->dirty) continue;    
        any_dirty = true;
        minisql_errno w = pager_write_page(pager,f);
        if(w != MINISQL_OK){
            if(rc == MINISQL_OK) rc = w; /*只记录第一个错误原因*/    
            continue;     
        }
        f->dirty = false;
    }
    if(any_dirty){
        int fsy = fsync(pager->fd);
        if(fsy == -1 && rc == MINISQL_OK){
            rc = minisql_errno_sys(errno);
        }
    }
    return rc;
}

minisql_errno pager_close(minisql_pager *pager){
    if(pager == NULL){
        return MINISQL_OK;
    }
    minisql_errno rc = MINISQL_OK;
    minisql_errno plu = pager_flush(pager);
    if(plu != MINISQL_OK && rc == MINISQL_OK) rc = plu; /*flush失败继续释放内存，直接return会泄露内存*/
    free(pager->buckets);
    free(pager->frames);
    if(close(pager->fd) == -1){
        if(rc == MINISQL_OK) rc = minisql_errno_sys(errno);
    }
    free(pager);
    return rc;
}
