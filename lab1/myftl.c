#include "myftl.h"
#include "test.h"

/* TODO */
int ftl_io(struct ssd *ssd, struct workload_entry cmd) {
	/* TODO
	 * 요청을 확인하고 알맞은 처리 함수 호출
	 */
	printf("ftl_read 구현 필요\n");
	return -1;  // 구현되지 않음
}

int ftl_read(struct ssd *ssd, uint64_t lpn, int page_count) {
	for (int i = 0; i < page_count; i++) {
		ssd->total_reads++;
		/* TODO
		 * LPN을 PPA로 변환하여 읽기 연산 수행 
		 * 매핑 테이블에서 LPN에 해당하는 PPA를 찾아 읽기
		 */
		printf("ftl_read 구현 필요\n");
		return -1;  // 구현되지 않음
	}
	return 0;
}

int ftl_write(struct ssd *ssd, uint64_t lpn, int page_count) {
	for (int i = 0; i < page_count; i++) {
		ssd->total_writes++;
		/* TODO
		 * LPN에 새로운 PPA 할당
		 * 이전 매핑이 있으면 invalid로 표시
		 * 새로운 페이지 할당 및 매핑 테이블 업데이트
		 */
		printf("ftl_write 구현 필요\n");
		return -1;  // 구현되지 않음

		// GC 필요성 체크
		if (ssd->sm.free_sb_cnt <= ssd->sm.gc_thres_sbs) {
			ftl_gc(ssd);
		}
	}
    return 0;
}

int ftl_gc(struct ssd *ssd) {
	ssd->total_gc_cnt++;
    /* TODO
	 * victim 슈퍼블록 선택 
	 * valid 데이터를 새로운 위치로 복사 
	 * (복사한 페이지 수만큼 ssd->total_gc_pages 증가)
	 * 블록 삭제 및 슈퍼블록 상태 업데이트
	 */
    
    printf("ftl_gc 구현 필요\n");
    return -1;  // 구현되지 않음
}

/* 초기화 함수(필요시 수정) */
static void ssd_init_page(struct page *pg) {
    pg->status = PG_FREE;
}

static void ssd_init_blk(struct block *blk) {
    for (int i = 0; i < PGS_PER_BLK; i++)
        ssd_init_page(&blk->pg[i]);
}

static void ssd_init_die(struct die *die) {
    for (int i = 0; i < BLKS_PER_DIE; i++)
        ssd_init_blk(&die->blk[i]);
}

static void ssd_init_ch(struct channel *ch) {
    for (int i = 0; i < DIES_PER_CH; i++)
        ssd_init_die(&ch->die[i]);
}

static void ssd_init_maptbl(struct ssd *ssd) {
    for (uint64_t i = 0; i < TOTAL_PGS; i++)
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
}

static void ssd_init_rmap(struct ssd *ssd) {
    for (uint64_t i = 0; i < TOTAL_PGS; i++)
        ssd->rmap[i] = INVALID_LPN;
}

static void ssd_init_sbs(struct ssd *ssd) {
    struct sb_mgmt *sm = &ssd->sm;
    struct superblock *sb;

	for (int i = 0; i < BLKS_PER_DIE; i++) {
		sb = &sm->sbs[i];
		sb->blk_id = i;
		sb->status = SB_FREE;
		sb->ipc = 0;
		sb->vpc = 0;
	}

	sm->sbs[0].status = SB_INUSE;
	sm->total_sb_cnt = BLKS_PER_DIE;
	sm->free_sb_cnt = BLKS_PER_DIE - 1;
	sm->victim_sb_cnt = 0;
	sm->full_sb_cnt = 0;
	sm->gc_thres_sbs = (100 - GC_THRES_PCENT) * BLKS_PER_DIE / 100;
}

static void ssd_init_write_pointer(struct ssd *ssd) {
    struct write_pointer *wpp = &ssd->wp;
    
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

struct ssd* ssd_init() {
    struct ssd *ssd = malloc(sizeof(struct ssd));
    
    for (int i = 0; i < NCHS; i++)
        ssd_init_ch(&ssd->ch[i]);
    
    ssd_init_maptbl(ssd);
    ssd_init_rmap(ssd);
    ssd_init_sbs(ssd);
    ssd_init_write_pointer(ssd);
    
    ssd->total_reads = 0;
    ssd->total_writes = 0;
    ssd->total_gc_cnt = 0;
    ssd->total_gc_pages = 0;
    
    return ssd;
}

/* 기본 함수들(수정 불필요) */
void ssd_destroy(struct ssd *ssd) {
    if (!ssd)
		return;
    
    free(ssd);
}

/* 매핑 테이블 접근 */
struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn) {
    assert(lpn < TOTAL_USER_PGS);
    return ssd->maptbl[lpn];
}

void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
    assert(lpn < TOTAL_USER_PGS);
    ssd->maptbl[lpn] = *ppa;
}

uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa) {
    uint64_t pgidx = ppa2pgidx(ppa);
    return ssd->rmap[pgidx];
}

void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
    uint64_t pgidx = ppa2pgidx(ppa);
    ssd->rmap[pgidx] = lpn;
}

uint64_t ppa2pgidx(struct ppa *ppa) {
    uint64_t pgidx;

    pgidx = ppa->g.ch  * DIES_PER_CH + 
            ppa->g.die * BLKS_PER_DIE +
            ppa->g.blk * PGS_PER_BLK + 
            ppa->g.pg;

    assert(pgidx < TOTAL_PGS);
    return pgidx;
}
