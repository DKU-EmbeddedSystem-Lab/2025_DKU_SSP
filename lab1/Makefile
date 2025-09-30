CC = gcc
CFLAGS = -Wall -Wextra -g -std=c99 -D_GNU_SOURCE
LDFLAGS = -lm

# 타겟 실행파일들
TARGET = myftl

# 소스 파일들
HEADERS = test.h myftl.h
SRCS_MAIN = test.c myftl.c

# 기본 타겟 (모든 프로그램 빌드)
all: $(TARGET)

# FTL 시뮬레이터 빌드
$(TARGET):
	$(CC) $(CFLAGS) $(SRCS_MAIN) -o $(TARGET) $(LDFLAGS)

# 정리
clean:
	rm -f *.o $(TARGET)

.PHONY: all ftl_sim clean
