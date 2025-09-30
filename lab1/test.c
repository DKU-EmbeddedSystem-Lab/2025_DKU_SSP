#include "myftl.h"
#include "test.h"

int load_workload(const char* filename, struct workload_entry **workload, int *count) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("에러: 워크로드 파일 '%s' 열기 실패\n", filename);
        return -1;
    }

    char line[256];
    *count = 0;

    // 초기 용량 설정 (동적 확장)
    int capacity = 1024;
    *workload = malloc(sizeof(struct workload_entry) * capacity);
    if (!*workload) {
        fclose(fp);
        printf("에러: 메모리 할당 실패\n");
        return -1;
    }

    // 헤더 라인 건너뛰기
    if (fgets(line, sizeof(line), fp) == NULL) {
        printf("에러: 워크로드 파일이 비어 있습니다\n");
        fclose(fp);
        free(*workload);
        *workload = NULL;
        return -1;
    }

    // 데이터 라인 읽기
    while (fgets(line, sizeof(line), fp)) {
        int op, page_count;
        unsigned long lpn;
        if (sscanf(line, "%d,%lu,%d", &op, &lpn, &page_count) == 3) {
            if (*count >= capacity) {
                int new_capacity = capacity * 2;
                struct workload_entry *new_buf = realloc(*workload, sizeof(struct workload_entry) * new_capacity);
                if (!new_buf) {
                    fclose(fp);
                    printf("에러: 메모리 확장 실패\n");
                    free(*workload);
                    *workload = NULL;
                    return -1;
                }
                *workload = new_buf;
                capacity = new_capacity;
            }
            (*workload)[*count].operation = op;
            (*workload)[*count].lpn = (uint64_t)lpn;
            (*workload)[*count].page_count = page_count;
            (*count)++;
        }
    }

    fclose(fp);

    // 실제 크기로 축소 (선택)
    if (*count > 0) {
        struct workload_entry *shrunk = realloc(*workload, sizeof(struct workload_entry) * (*count));
        if (shrunk) {
            *workload = shrunk;
        }
    }

    printf("워크로드 파일 '%s' 로드 완료: %d개 연산\n", filename, *count);
    return 0;
}

/* 매핑 테이블 덤프 생성 */
int dump_mapping_table(struct ssd *ssd, const char* filename) {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        printf("Failed to open %s\n", filename);
        return -1;
    }
    
    fprintf(fp, "%lu,%lu,%lu,%lu", ssd->total_reads, ssd->total_writes, ssd->total_gc_cnt, ssd->total_gc_pages);
    
    for (uint64_t lpn = 0; lpn < TOTAL_USER_PGS; lpn++) {
        struct ppa ppa = get_maptbl_ent(ssd, lpn);
		fprintf(fp, "%lu,%lu\n", lpn, ppa.ppa);
    }
    
    fclose(fp);
    printf("Done.\n");

    return 0;
}

int main() {
    // sequential workload
    struct ssd *ssd = ssd_init();
    struct workload_entry *workload = NULL;
    int workload_count = 0;
    
    load_workload(SEQUENTIAL_WORKLOAD, &workload, &workload_count);

    for (int op_idx = 0; op_idx < workload_count; op_idx++) {
		ftl_io(ssd, workload[op_idx]);
    }

    dump_mapping_table(ssd, "sequential_mapping_table.csv");

    free(workload);
    ssd_destroy(ssd);

    // random workload
    ssd = ssd_init();
    workload = NULL;
    workload_count = 0;

    load_workload(RANDOM_WORKLOAD, &workload, &workload_count);

    for (int op_idx = 0; op_idx < workload_count; op_idx++) {
		ftl_io(ssd, workload[op_idx]);
    }

    dump_mapping_table(ssd, "random_mapping_table.csv");

    free(workload);
    ssd_destroy(ssd);

    return 0;
}
