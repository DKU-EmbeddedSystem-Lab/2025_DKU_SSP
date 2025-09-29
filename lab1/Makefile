CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99 -D_GNU_SOURCE
LDFLAGS = -lm

# 타겟 실행파일들
TARGET = ftl_sim

# 소스 파일들
SRCS_MAIN = test.c ftl_sim.c
HEADERS = ftl_sim.h test.h

# 기본 타겟 (모든 프로그램 빌드)
all: ftl_sim

# FTL 시뮬레이터 빌드
ftl_sim:
	$(CC) $(CFLAGS) $(SRCS_MAIN) -o $(TARGET) $(LDFLAGS)

# 정리
clean:
	rm -f *.o $(TARGET)

.PHONY: all ftl_sim clean
