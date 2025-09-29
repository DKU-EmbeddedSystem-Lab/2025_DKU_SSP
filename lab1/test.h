#ifndef __UTIL_H
#define __UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "ftl_sim.h"

// Forward declaration
struct ssd;

// 워크로드 파라미터 (하드코딩)
#define MAX_LPN             TOTAL_USER_PGS // 16K LPN 범위
#define MAX_PAGE_COUNT      8

// 파일명 고정
#define SEQUENTIAL_WORKLOAD     "workload/sequential.csv"
#define RANDOM_WORKLOAD         "workload/random.csv"

// 워크로드 타입
typedef enum {
    NVM_IO_READ = 0,
    NVM_IO_WRITE = 1
} operation_type;

// 워크로드 엔트리 구조체
struct workload_entry {
    int operation;      // 0: READ, 1: WRITE  
    uint64_t lpn;       // Logical Page Number
    int page_count;     // 페이지 수
};

// 워크로드 관리 함수들
int load_workload(const char* filename, struct workload_entry **workload, int *count);
int dump_mapping_table(struct ssd *ssd, const char* filename);

#endif // __UTIL_H
