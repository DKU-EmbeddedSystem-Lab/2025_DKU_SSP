#ifndef __FTL_SIM_STUB_H
#define __FTL_SIM_STUB_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>

// 기본 상수 정의
#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

// 시뮬레이션 파라미터
#define PGS_PER_BLK     128     // 블록당 페이지 수
#define BLKS_PER_DIE    64     // 플레인당 블록 수
#define DIES_PER_CH     8       // 채널당 LUN 수
#define NCHS            8       // 채널 수
#define TOTAL_PGS		(PGS_PER_BLK * BLKS_PER_DIE * DIES_PER_CH * NCHS )
#define TOTAL_USER_PGS	(TOTAL_PGS * (100 - OP_PCENT) / 100)
#define GC_THRES_PCENT  90      // GC 임계값 (%)
#define OP_PCENT  15      // GC 임계값 (%)

// 페이지/블록 상태
enum {
    SB_FREE = 0,
    SB_VICTIM = 1,
    SB_FULL = 2,
	SB_INUSE = 3,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

// OP 타입
enum {
    OP_READ = 0,
    OP_WRITE = 1,
};

// PPA 구조체 (Physical Page Address)
struct ppa {
    union {
        struct {
            uint64_t blk : 16;
            uint64_t pg  : 16;
            uint64_t die : 16;
            uint64_t ch  : 16;
        } g;
        uint64_t ppa;
    };
};

// SSD 구성요소
struct page {
    int status;
};

struct block {
    struct page pg[PGS_PER_BLK];
};

struct die {
    struct block blk[BLKS_PER_DIE];
};

struct channel {
    struct die die[DIES_PER_CH];
};

/* 자원 관리 구조 */
struct superblock {
    int blk_id;
	int status;
    int ipc;  // invalid page count
    int vpc;  // valid page count
};

struct sb_mgmt {
    struct superblock sbs[BLKS_PER_DIE];
    int total_sb_cnt;
    int free_sb_cnt;
    int victim_sb_cnt;
	int full_sb_cnt;
	int gc_thres_sbs;
};

struct write_pointer {
	// 다음 쓰기 수행할 위치
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct ssd {
    struct channel ch[NCHS];
    struct ppa maptbl[TOTAL_USER_PGS];  // LPN -> PPA 매핑
    uint64_t rmap[TOTAL_PGS];           // PPA -> LPN 역매핑 for GC
    struct write_pointer wp;
    struct sb_mgmt sm;
    
    // 통계
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_gc_cnt;
    uint64_t total_gc_pages;
};

/* TODO 구현 필요한 함수(필요시 자유롭게 추가) */
int ftl_read(struct ssd *ssd, uint64_t lpn, int page_count);
int ftl_write(struct ssd *ssd, uint64_t lpn, int page_count);
int ftl_gc(struct ssd *ssd);

/* 기본 함수들 */
struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn);
void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa);
uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa);
void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa);
uint64_t ppa2pgidx(struct ppa *ppa);
struct ssd* ssd_init();
void ssd_destroy(struct ssd *ssd);
int dump_mapping_table(struct ssd *ssd, const char* filename);

#endif // __FTL_SIM_STUB_H

