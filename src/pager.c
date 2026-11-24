#include "minisql/pager.h"
#include <fcntl.h>
#include<unistd.h>
#include<sys/stat.h>
#include<stdlib.h>
#include<assert.h>
#include<stdbool.h>
#include<errno.h>
#include<string.h>
struct minisql_page {
    minisql_pgno pgno; /*这个缓存帧当前存放的是数据库文件中的第几页;MINISQL_PGNO_NULL 表示空页框（这个缓存帧目前没有对应的数据库页面。）*/ 
    int pin;
    bool dirty; /*和磁盘是否一致,f表示干净页，true表示脏页*/
    minisql_page *hnext; // 哈希链表的下一个节点*/
    minisql_page *lru_prev;
    minisql_page *lru_next;
    uint8_t data[MINISQL_PAGE_SIZE]; /*页内容 */
};
struct minisql_pager{
    int fd;
    minisql_page *frames; /*缓存帧  LRU 链表中的页面不是重新分配出来的，它们仍然是 frames 数组里的那些对象，只是通过 lru_prev 和 lru_next 连接起来。*/    
    
    uint32_t cache_capacity;
    uint32_t cache_count; /*当前已经占用的缓存帧数量*/

    minisql_page *lru_head; /*链表头 lru_head：最近使用过的页面。   可被复用*/
    minisql_page *lru_tail;/*链表尾 lru_tail：最久没有使用的页面。 可被复用 */
    

    minisql_page **buckets;
    uint32_t hash_mask;
    minisql_page *free_frames; /*链表的头指针*/

    uint32_t page_count;  
    minisql_pgno freelist_head; /*磁盘空闲页面链表头页号   */
    uint32_t root_page;
    
};
static void lru_remove(minisql_pager *pager, minisql_page *page);
static void lru_push_front(minisql_pager *pager, minisql_page *page);
static void lru_touch(minisql_pager *pager, minisql_page *page);
static minisql_errno pager_write_page(minisql_pager *pager, minisql_page *page);
static void frame_release(minisql_pager *pager, minisql_page *frame);
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
   
    /*打开或关联文件：获取数据库文件的文件描述符*/
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
            goto fail_frames;   /* 注意此时 pager 已经分配 */
    }
        pager->page_count = 1;  
    }
    
    else
        pager->page_count = st.st_size / MINISQL_PAGE_SIZE;

/*初始化缓存结构：把页面缓存数组或哈希表置空，为后续按需加载页做准备*/
    pager->cache_capacity = round_up_pow2(cache_pages);
    assert(pager->cache_capacity >= cache_pages);          /* 不能比请求的小 */

    /* hash_mask = capacity - 1，配合 capacity 已是 2 的幂：
 * 桶下标 = pgno & hash_mask，用一条位与指令代替取模（除法要几十个周期）。
 * 这就是 round_up_pow2 存在的唯一理由。 */
    pager->hash_mask = pager->cache_capacity - 1;

    /*分配缓存页框数组和哈希桶数组：frames 数组存放缓存页框，buckets 数组存放哈希桶指针*/
    minisql_page *frames = calloc(pager->cache_capacity,sizeof(minisql_page)); /*返回首页框的地址。数组的元素是 minisql_page 结构体本身。*/
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
    /* 这里的 frames 和 pager->frames 是同一块内存。
     * 用局部变量更安全：走到这个标签时 pager 马上就要被 free 掉，
     * 再经由 pager->frames 取值会依赖 pager 尚未释放这一隐含前提。 */
    free(frames);
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
        return MINISQL_ERR_INVALID; /*调用者传入的参数不合法。这不属于正常的文件 I/O 错误，更不能返回成功。*/
    }
    if(page->pgno == MINISQL_PGNO_NULL || page->pgno >= pager->page_count){
        return MINISQL_ERR_CORRUPT; /*空页框或越界页不能写盘*/
    }
    off_t offset = (off_t)page->pgno*MINISQL_PAGE_SIZE; 
    size_t written = 0;
    while(written < MINISQL_PAGE_SIZE){
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
/*@brief 解除缓存页面的一次固定引用，并记录页面修改状态。
 *
 * 当上层使用完页面后，将 pin 引用计数减 1。
 * 如果 dirty 非 0，则将页面标记为脏页，等待后续写回磁盘。
 * 如果 dirty 为 0，则保留页面原有的脏页状态。
 *
 * 当 pin 降为 0 时，页面可以参与 LRU 淘汰候选。
 * 本函数不释放页面内存，也不直接写回磁盘。
 *
 * @param pager 页面缓存管理器。
 * @param pg    需要解除固定引用的缓存页面。
 * @param dirty 是否在本次使用中修改了页面（0：未修改；非 0：已修改）。
*/
minisql_errno pager_unpin(minisql_pager *pager,minisql_page *page,int dirty){
    if (pager == NULL || page == NULL){
        return MINISQL_ERR_INVALID; 
    }
    if(page->pgno == MINISQL_PGNO_NULL){
        return MINISQL_ERR_INVALID;
    }
    if(dirty) page->dirty = true;
    if(page->pin <= 0){      /* 先防风控：能不能减 */
        assert(page->pin > 0);
        return MINISQL_ERR_INVALID;
    }  
   
    page->pin--;
    if(page->pin == 0) lru_touch(pager,page); /* 减完才判断是否归零 */
    return MINISQL_OK;
}

/*将一个链表中的page摘下来*/
static void lru_remove(minisql_pager *pager,minisql_page *page){
    assert(pager != NULL && page != NULL);
    if(page->lru_prev != NULL){
        page->lru_prev->lru_next = page->lru_next; 
    }       
    else pager->lru_head = page->lru_next; /* 没有左邻居 ⟹ page 是 head */
    if(page->lru_next != NULL){
        page->lru_next->lru_prev = page->lru_prev;
    }
    else pager->lru_tail = page->lru_prev; /* 没有右邻居 ⟹ page 是 tail */

    /*从链上摘下 → 清空自身指针*/
    page->lru_prev = NULL;
    page->lru_next = NULL;
}

/*将一个页面节点插入 LRU 双向链表的头部，让它成为最近使用的页面。
*将一个最近新的使用的页面（page未在链表中）加入LRU链表中
*lru_push_front()：要求传入的 page 尚未加入 LRU 链表。
*lru_touch()：要求传入的 page 已经在 LRU 链表中。
*lru_remove()：要求传入的 page 已经在 LRU 链表中。
*/
static void lru_push_front(minisql_pager *pager,minisql_page *page){
    assert(page->lru_prev == NULL && page->lru_next == NULL); /*一次性挡住“重复插入 / free_frames 残留 / 忘 remove”*/
    assert(pager != NULL && page != NULL);
    page->lru_prev = NULL; /*入链前显式置 NULL*/
 //    
//     情况一：链表原本为空
//    if(pager->lru_head == NULL ){  /*head 为 NULL 时 tail 必然是 NULL*/
//         pager->lru_head = pager->lru_tail = page;
//    }
//    *情况二：链表原本有节点
//    else 
//    {
//     page->lru_next = pager->lru_head;
//     pager->lru_head->lru_prev = page; /*原来的head不要忘记回指*/
//     pager->lru_head = page;
//    }
/*合并两种情况， “旧 head 是否为 NULL”一个判断同时决定了 tail 要不要更新*/ 
    page->lru_next = pager->lru_head;
   if(pager->lru_head) {
    pager->lru_head->lru_prev = page;  
   }                
   else pager->lru_tail = page;
   pager->lru_head = page; /*把 head 的更新移出分支*/
}

// 1. 如果 page 已经是头节点，直接返回。
// 2. 调用 lru_remove()，从原位置摘除。
// 3. 调用 lru_push_front()，插入链表头部。
/*将链表中的page移到head*/
static void lru_touch(minisql_pager *pager,minisql_page *page){
    assert(pager != NULL && page != NULL);
    if(page == pager->lru_head) return;              
    else{
     lru_remove(pager,page);
     lru_push_front (pager,page);   
    }

}

/*Hash 负责快速找到页面，LRU 负责管理页面的使用顺序。*/
/*根据磁盘页号 pgno，判断这个页面是否已经存在于 Pager 缓存中。
*找到了：返回对应的 minisql_page *。 没找到：返回 NULL。

*思路：用 pgno & hash_mask 定位桶 → 沿 hnext 单链表遍历 → 比对 pgno → 命中返回页框，否则返回 NULL
*/
static minisql_page* hash_find(minisql_pager *pager,minisql_pgno pgno){
    assert(pager != NULL);
    assert(pager->buckets != NULL);
    uint32_t index = pgno & pager->hash_mask;
    for(minisql_page *cur = pager->buckets[index];cur != NULL;cur = cur->hnext){
        if(cur->pgno == pgno) return cur;
        //else continue;
    }
    return NULL;
}
/*把一个已经存在的缓存页面 page 插入 Pager 的哈希表中 (使用单链表头插法)*/
/*Pager 的哈希表就是一个缓存目录，通过磁盘页号 pgno，快速判断该页面是否已经在内存缓存中，并找到它对应的缓存帧。 */
/*1.frames：存放真正的缓存页面，每个页面有自己的 pgno。
2.buckets：存放哈希链表的头指针，帮助快速定位缓存页面。
3.查找时，根据目标 pgno 计算桶下标，只遍历对应桶的链表，而不是遍历整个 frames 数组。*/
static void hash_insert(minisql_pager *pager,minisql_page *page){
    assert(pager != NULL);
    assert(page != NULL);
    assert(pager->buckets != NULL);
    assert(page->hnext == NULL);/*页框的 hnext 残留上一个主人的值，导致两条链被串到一起，之后 hash_remove 会截断别人的链*/
    
    uint32_t index = page->pgno & pager->hash_mask; 
    page->hnext = pager->buckets[index]; /*←新节点的 next 指向原链头*/
    pager->buckets[index] = page;      /*桶头改成新节点*/

}

/*page：被删除的对象*/
static void hash_remove(minisql_pager *pager,minisql_page *page){
    assert(pager != NULL);
    assert(page != NULL);
    assert(pager->buckets != NULL);

    uint32_t index = page->pgno & pager->hash_mask;
    minisql_page *prev = NULL;
    minisql_page *cur = pager->buckets[index];
    while(cur != page && cur != NULL){
        prev = cur;
        cur = cur->hnext;
    }
    assert(cur == page);
    if(prev == NULL){
        pager->buckets[index] = page->hnext;   
    }
    else{
        prev->hnext = page->hnext;   
    }
    page->hnext = NULL;
}

/*frame_acquire() 负责为即将加载的磁盘页面，找到一个可以使用的内存缓存帧。*/
static minisql_errno frame_acquire(minisql_pager *pager,minisql_page **frame_out){
    assert(pager != NULL);
    assert(frame_out != NULL);
    /*先判断有没有空闲缓存帧*/
    /*这比O(n)扫数组快*/
    minisql_page *frame = pager->free_frames;
    
    if(frame != NULL){
        assert(frame->lru_prev == NULL);
        assert(frame->hnext == NULL);
        pager->free_frames = frame->lru_next;
        frame->lru_next = NULL;
        *frame_out = frame; 
        pager->cache_count++;
        assert(pager->cache_count <= pager->cache_capacity);
        return MINISQL_OK;
    } 
    /*没有空闲缓存帧的话，从LRU尾部一次寻找pin=0的缓存页，*/
    minisql_page *cur = pager->lru_tail;
    minisql_errno rc = MINISQL_OK;
    while(cur != NULL){
        if(cur->pin == 0){
            if(cur->dirty) rc = pager_write_page(pager,cur);/*判断该页面是不是已修改过，需要写回*/
            if(rc != MINISQL_OK) return rc;
            hash_remove(pager,cur); /*page->hnext = NULL*/
            lru_remove(pager,cur); /*将page lru_prev  lru_next 重置为NULL*/
            cur->dirty = false;
            cur->pgno = MINISQL_PGNO_NULL;
            cur->pin = 0;
            *frame_out = cur;
            return MINISQL_OK;
        }
        cur = cur->lru_prev;
    }
    
    *frame_out = NULL;  /*找一圈没找到pin= 0 缓存帧*/
    return MINISQL_ERR_CACHE_FULL;
}
/*
 * frame_release - 释放缓存帧，将其放回空闲链表。
 *
 * 重置页框状态，将其标记为空闲。
 * 使用 lru_next 串联空闲链表，并插入链表头部。
 * 最后减少已使用缓存帧数量。
 *
 * 注意：调用前必须确保该页框已从 Hash 和 LRU 链表中移除。
 */
static void frame_release(minisql_pager *pager,minisql_page *frame){
    assert(pager != NULL && frame != NULL);
    assert(pager->cache_count > 0);      /* 不能减成负数 */
    frame->pin = 0;
    frame->dirty = false;
    frame->pgno = MINISQL_PGNO_NULL;
    /*「LRU 顺序」只在页框处于 LRU 链上时成立。 一旦它回到空闲链，这个字段就不再承载时间顺序语义，而是被借用来串空闲链。*/
    frame->lru_prev = NULL;
    frame->lru_next = pager->free_frames;
    frame->hnext = NULL;
    pager->free_frames = frame;
    pager->cache_count--;

}
/*
 * pager_get - 获取一个已经存在的磁盘页对应的缓存帧。
 *
 * 1. 缓存命中：增加 pin 计数，更新 LRU，返回缓存帧。
 * 2. 缓存未命中：获取可用页框，从磁盘读取完整页面，
 *    初始化页框状态，并插入 Hash 和 LRU 链表。
 * 3. 读取失败：释放已获取的页框，返回错误码。
 *
 * 参数：
 * pager    - Pager 管理器。
 * pgno     - 目标磁盘页号。
 * page_out - 输出参数，用于返回缓存帧地址。
 *
 * 注意：成功获取页面后，调用者必须调用 pager_unpin()。
 */
minisql_errno pager_get(minisql_pager *pager,minisql_pgno pgno,minisql_page **page_out){
    assert(pager != NULL);
    assert(page_out != NULL); /*传一个已知的地址，地址指向的指针变量用来保存需要返回的页框地址*/
    if (pgno == MINISQL_PGNO_NULL) return MINISQL_ERR_INVALID;
    if (pgno >= pager->page_count) return MINISQL_ERR_CORRUPT; /* *不能用assert校验调用方输入   不能等于，因为包括头页了*/
    /*情况一：经hash查找页面已存在在缓存页面中*/
    minisql_page *frame_ptr = NULL;
    frame_ptr = hash_find(pager,pgno);
    if(frame_ptr != NULL){ 
        frame_ptr->pin ++; /* 将页面的 pin 计数加 1*/
        lru_touch(pager,frame_ptr); /*将页面移动到 LRU 链表头部。 */
        *page_out = frame_ptr; /*返回该页面的缓存帧地址。*/
        return MINISQL_OK;
    }

    minisql_errno rc = MINISQL_OK;
    rc = frame_acquire(pager,&frame_ptr); /*先找一个可用的缓存帧，用于存放缓存该页面*/
    if(rc != MINISQL_OK) return rc;
    /*pread() 只负责把磁盘中的数据读取到内存，并不会自动设置缓存帧的管理信息
    *minisql_page 不仅保存页面数据，还保存了 Pager 管理缓存所需要的状态。
    */
    ssize_t got = 0;
    while(got < MINISQL_PAGE_SIZE){
    ssize_t n = pread(pager->fd,frame_ptr->data + got,MINISQL_PAGE_SIZE - got,(off_t)pgno*MINISQL_PAGE_SIZE + (off_t)got);
        if(n == -1)         {
            if(errno == EINTR) continue;
            frame_release(pager,frame_ptr);
            return minisql_errno_sys(errno);
        }
        if(n == 0){
            frame_release(pager,frame_ptr);
            return MINISQL_ERR_CORRUPT;
        }
         got += (size_t)n;
    }
    frame_ptr->pgno = pgno;
    frame_ptr->pin = 1;
    frame_ptr->dirty = false; /*刚通过 pread 从磁盘读进来的页，内容与磁盘完全一致*/
    hash_insert(pager,frame_ptr);
    lru_push_front(pager,frame_ptr);
    *page_out = frame_ptr;
    return MINISQL_OK;
}

/*读取下一个空闲页号*/
static minisql_pgno freelist_get_next(minisql_page *pg){
    minisql_pgno next_free;
    memcpy(&next_free,pg->data + MINISQL_FREELIST_NEXT_OFFSET,sizeof(next_free));
    return next_free;
}
/*设置下一个空闲页号
把下一个空闲页的页号，写入当前空闲页的 data 数组中
*/
static void freelist_set_next(minisql_page *pg,minisql_pgno next_free){    
    memcpy(pg->data+ MINISQL_FREELIST_NEXT_OFFSET,&next_free,sizeof(next_free));
    pg->dirty = true; /*修改了 pg->data，需要告诉 Pager：这个页面以后要写回磁盘。*/
}

/*负责决定使用哪个磁盘页
核心作用是：分配一个磁盘页面，可能复用旧页，也可能创建新页。并返回该页的页号和对应的缓存页指针。
 * 分配一个磁盘页面，优先复用空闲页，否则追加新页。
 * 返回新页号及对应的已固定（pin）的缓存页。
 * 成功返回 MINISQL_OK，失败返回错误码。
 */
minisql_errno pager_alloc(minisql_pager *pager,minisql_pgno *pgno_out, minisql_page **page_out)
{
    assert(pager != NULL);
    assert(pgno_out != NULL);
    assert(page_out != NULL);

    minisql_errno rc   = MINISQL_OK;
    minisql_pgno  pgno = MINISQL_PGNO_NULL;   /* 结果变量提到分支外 */
    minisql_page *pg   = NULL;

    if (pager->freelist_head != MINISQL_PGNO_NULL) {
        /* ================= 路径 A：复用 freelist 头 ================= */
        minisql_pgno head = pager->freelist_head;

        /* 页 0 是头页，绝不能出现在 freelist 里 */
        if (head < 1 || head >= pager->page_count) {
            return MINISQL_ERR_CORRUPT;
        }

        rc = pager_get(pager, head, &pg);
        if (rc != MINISQL_OK) {
            return rc;                    /* pager_get 内部已保证状态不变 */
        }

        minisql_pgno next = freelist_get_next(pg);

        /* 先校验、再改 freelist_head，不让脏值落进内存状态 */
        if (next != MINISQL_PGNO_NULL &&
            (next < 1 || next >= pager->page_count)) {
            pager_unpin(pager, pg, 0);    /* 校验失败，退货 */
            return MINISQL_ERR_CORRUPT;
        }

        pager->freelist_head = next;
        pgno = head;

    } else {
        /* ================= 路径 B：追加新页 ================= */
        minisql_pgno new_pgno = pager->page_count;

        /* ① 先让文件真的变长；失败时内存状态完全没动，直接返回 */
        while (ftruncate(pager->fd,(off_t)(new_pgno + 1) * MINISQL_PAGE_SIZE) != 0) {
            if (errno == EINTR) continue;
            return minisql_errno_sys(errno);
        }

        /* ② 文件已经有这页了，现在改 page_count 才是安全的 */
        pager->page_count = new_pgno + 1;

        /* ③ 把它装进缓存 */
        rc = pager_get(pager, new_pgno, &pg);
        if (rc != MINISQL_OK) {
            pager->page_count = new_pgno;  /* 回滚；文件不动，多出的页下次复用 */
            return rc;
        }

        pgno = new_pgno;
    }

    /* ======================= 汇合点 ======================= */
    memset(pg->data, 0, MINISQL_PAGE_SIZE);  /* 清空内容（路径 B 本来就是零，无妨） */
    pg->dirty = true;                        /* 必须标脏，否则不会写回 */

    *pgno_out = pgno;
    *page_out = pg;
    return MINISQL_OK;
}

/* 把一个在用页归还到 freelist。
 * 契约：调用方必须保证该页当前没有被其他持有者引用，
 *       且不得重复释放同一个页号（否则 freelist 成环）。
 * 不改变 page_count，不缩小文件。 */
minisql_errno pager_free(minisql_pager *pager, minisql_pgno pgno)
{
    if (pager == NULL) {
        return MINISQL_ERR_IO;
    }
    /* 页 0 是头页，不能回收；越界页号一定是调用方出错 */
    if (pgno < 1 || pgno >= pager->page_count) {
        return MINISQL_ERR_CORRUPT;
    }

    minisql_page *pg = NULL;
    minisql_errno rc = pager_get(pager, pgno, &pg);
    if (rc != MINISQL_OK) {
        return rc;
    }

    /* pager_get 之后 pin 至少是 1。大于 1 说明还有别的持有者，
     * 现在释放会让它手里的页变成可利用的空闲页 —— 拒绝。 */
    if (pg->pin != 1) {
        pager_unpin(pager, pg, 0);
        return MINISQL_ERR_INVALID;
    }

    /* 头插：我指向老的 head，再让自己成为新 head */
    freelist_set_next(pg, pager->freelist_head);
    pager->freelist_head = pgno;

    /* dirty=1 必须传：坑位信息要落盘，否则重启后这个页永久泄漏 */
    return pager_unpin(pager, pg, 1);
}

/* freelist_head 的读写口（db 层装载/取出用，权威只有 pager 这一份） */
minisql_pgno pager_freelist_head(const minisql_pager *pager)
{
    if (pager == NULL) {
        return MINISQL_PGNO_NULL;
    }
    return pager->freelist_head;
}

void pager_set_freelist_head(minisql_pager *pager, minisql_pgno head)
{
    if (pager == NULL) {
        return;
    }
    pager->freelist_head = head;
}
