#include "minisql/db.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * 头页布局
 * ------------------------------------------------------------------ */
#define HDR_MAGIC_OFF          0u
#define HDR_MAGIC_LEN          8u
#define HDR_VERSION_OFF        8u
#define HDR_PAGE_COUNT_OFF     12u
#define HDR_ROOT_OFF           16u
#define HDR_FREELIST_OFF       20u

#define MINISQL_FORMAT_VERSION 1u

static const uint8_t k_magic[HDR_MAGIC_LEN] = {
    'M', 'I', 'N', 'I', 'S', 'Q', 'L', '\0'
};

struct minisql_db {
    minisql_pager *pager;
    minisql_pgno   root_page;
};

/* ------------------------------------------------------------------ *
 * 页内字段读写：必须 memcpy。
 * data 是 uint8_t[]，偏移 12/16/20 不保证 4 字节对齐，
 * 直接 *(uint32_t*) 是 unaligned access，属于 UB（ARM 上会 SIGBUS）。
 * ------------------------------------------------------------------ */
static uint32_t hdr_read_u32(const uint8_t *data, uint32_t off)
{
    uint32_t v = 0;
    memcpy(&v, data + off, sizeof(v));
    return v;
}

static void hdr_write_u32(uint8_t *data, uint32_t off, uint32_t v)
{
    memcpy(data + off, &v, sizeof(v));
}

/* 前 MINISQL_HEADER_SIZE 字节是否全零（= 全新的空文件） */
static int hdr_is_blank(const uint8_t *data)
{
    for (uint32_t i = 0; i < MINISQL_HEADER_SIZE; i++) {
        if (data[i] != 0) return 0;
    }
    return 1;
}

/* 把一份头页内容写进页里（不负责 unpin） */
static void hdr_pack(uint8_t *data, uint32_t page_count,
                     minisql_pgno root, minisql_pgno freelist)
{
    memset(data, 0, MINISQL_HEADER_SIZE);
    memcpy(data + HDR_MAGIC_OFF, k_magic, HDR_MAGIC_LEN);
    hdr_write_u32(data, HDR_VERSION_OFF,    MINISQL_FORMAT_VERSION);
    hdr_write_u32(data, HDR_PAGE_COUNT_OFF, page_count);
    hdr_write_u32(data, HDR_ROOT_OFF,       root);
    hdr_write_u32(data, HDR_FREELIST_OFF,   freelist);
}

/* ------------------------------------------------------------------ *
 * open / close / flush
 * ------------------------------------------------------------------ */

minisql_errno db_open(const char *filename, uint32_t cache_pages,
                      minisql_db **db_out)
{
    if (filename == NULL || db_out == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_errno rc = MINISQL_OK;

    minisql_pager *pager = NULL;
    rc = pager_open(filename, cache_pages, &pager);
    if (rc != MINISQL_OK) {
        return rc;
    }

    minisql_db *db = calloc(1, sizeof(minisql_db));
    if (db == NULL) {
        pager_close(pager);
        return MINISQL_ERR_NOMEM;
    }
    db->pager     = pager;
    db->root_page = MINISQL_PGNO_NULL;

    minisql_page *hdr = NULL;
    rc = pager_get(pager, MINISQL_PGNO_HEADER, &hdr);
    if (rc != MINISQL_OK) {
        goto fail_db;
    }

    uint8_t *data = page_data(hdr);
    int dirty = 0;

    if (hdr_is_blank(data)) {
        /* ---------- 新库：初始化头页 ---------- */
        uint32_t pc = pager_page_count(pager);
        hdr_pack(data, pc, MINISQL_PGNO_NULL, MINISQL_PGNO_NULL);
        /* freelist 空链，pager 里也是这个值，保持一致 */
        pager_set_freelist_head(pager, MINISQL_PGNO_NULL);
        db->root_page = MINISQL_PGNO_NULL;
        dirty = 1;
    } else if (memcmp(data + HDR_MAGIC_OFF, k_magic, HDR_MAGIC_LEN) == 0) {
        /* ---------- 老库：校验后装载 ---------- */
        uint32_t version = hdr_read_u32(data, HDR_VERSION_OFF);
        uint32_t pc      = hdr_read_u32(data, HDR_PAGE_COUNT_OFF);
        minisql_pgno root = (minisql_pgno)hdr_read_u32(data, HDR_ROOT_OFF);
        minisql_pgno fl   = (minisql_pgno)hdr_read_u32(data, HDR_FREELIST_OFF);

        /* 以物理文件大小为权威；头页的 page_count 只用来交叉校验 */
        uint32_t phys = pager_page_count(pager);

        if (version != MINISQL_FORMAT_VERSION || pc != phys) {
            pager_unpin(pager, hdr, 0);
            goto fail_db;
        }
        /* 空树/空链用 NULL 哨兵；非空则必须落在 [1, phys) 内 */
        if (root != MINISQL_PGNO_NULL && (root < 1 || root >= phys)) {
            pager_unpin(pager, hdr, 0);
            goto fail_db;
        }
        if (fl != MINISQL_PGNO_NULL && (fl < 1 || fl >= phys)) {
            pager_unpin(pager, hdr, 0);
            goto fail_db;
        }

        db->root_page = root;
        pager_set_freelist_head(pager, fl);
    } else {
        /* ---------- 魔数不对：不是我们的文件，或者已损坏 ---------- */
        pager_unpin(pager, hdr, 0);
        goto fail_db;
    }

    rc = pager_unpin(pager, hdr, dirty);
    if (rc != MINISQL_OK) {
        goto fail_db;
    }

    *db_out = db;
    return MINISQL_OK;

fail_db:
    /* 头页已经 unpin 过（或用不上），这里只回收 db 和 pager */
    pager_close(pager);
    free(db);
    return MINISQL_ERR_CORRUPT;
}

minisql_errno db_flush(minisql_db *db)
{
    if (db == NULL || db->pager == NULL) {
        return MINISQL_ERR_INVALID;
    }

    minisql_page *hdr = NULL;
    minisql_errno rc = pager_get(db->pager, MINISQL_PGNO_HEADER, &hdr);
    if (rc != MINISQL_OK) {
        return rc;
    }

    /* 每次都按当前的真实状态重新打包，避免残留旧字节 */
    hdr_pack(page_data(hdr),
             pager_page_count(db->pager),
             db->root_page,
             pager_freelist_head(db->pager));

    return pager_unpin(db->pager, hdr, 1);   /* dirty=1 */
}

minisql_errno db_close(minisql_db *db)
{
    if (db == NULL) {
        return MINISQL_OK;
    }

    minisql_errno rc = MINISQL_OK;

    /* 头页写回失败不阻止释放内存，但要保留第一个错误码 */
    minisql_errno f = db_flush(db);
    if (f != MINISQL_OK) {
        rc = f;
    }

    /* pager_close 内部会 pager_flush，把脏页（含刚标脏的头页）刷盘 */
    minisql_errno c = pager_close(db->pager);
    if (c != MINISQL_OK && rc == MINISQL_OK) {
        rc = c;
    }

    free(db);
    return rc;
}

/* ------------------------------------------------------------------ *
 * 简单访问器
 * ------------------------------------------------------------------ */

minisql_pgno db_root_page(const minisql_db *db)
{
    if (db == NULL) {
        return MINISQL_PGNO_NULL;
    }
    return db->root_page;
}

minisql_errno db_set_root_page(minisql_db *db, minisql_pgno root)
{
    if (db == NULL) {
        return MINISQL_ERR_INVALID;
    }
    /* 允许设成 NULL（清空树），非空则必须在合法范围内 */
    if (root != MINISQL_PGNO_NULL) {
        if (root < 1 || root >= pager_page_count(db->pager)) {
            return MINISQL_ERR_CORRUPT;
        }
    }
    db->root_page = root;
    return MINISQL_OK;
}

minisql_pager *db_pager(minisql_db *db)
{
    return (db == NULL) ? NULL : db->pager;
}