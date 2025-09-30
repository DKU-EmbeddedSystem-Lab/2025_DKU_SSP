## 2025 DKU Semiconductor Software Practices

### 구조
- `lab1/`: FTL 시뮬레이터 구현 과제
- `lab2/`: OpenSSD 애뮬레이션

### 요구 환경
- Ubuntu 22.04
- gcc 11.4
- make 4.3
```bash
sudo apt update && sudo apt install -y build-essential
```

### Lab1
- 실행 방법
```bash
cd lab1
make           # ftl_sim 빌드
./ftl_sim      # 실행
# 정리: make clean
```
- 코드 구조
``` 
lab1/
├─ workload/           # 워크로드
├─ Makefile
├─ test.h
├─ test.c              # 테스트 코드
├─ ftl_sim.h           # 시뮬 파라미터/자료구조, FTL API 선언
└─ ftl_sim.c           # FTL 구현(매핑/할당/GC)
```

### Lab2
- 실행 방법
```bash
cd lab2
make           # cosmos_sim 빌드
./cosmos_sim   # 기본 실행
# 정리: make clean
```
- 실행 인자(요약)
  - `-n, --nworkers <N>`: 워커 수 설정(최대 10). 필수 먼저 지정
  - `-w, --worker "<pattern> <read%> <write%> <nblks>"`: 워커별 패턴/비율/블록수(여러 번 지정 가능). `read%+write%=100`
  - `-s, --size "p1 p2 ..."`: 각 워커 파티션 비율(%) 목록. 미지정 시 균등 분배
  - `-i, --inst <N>`: 총 작업 수(operations)
  - `-c, --condition`: 사전 조건화(preconditioning) 수행
  - `-r, --report`: 로그 파일 출력 활성화
  - `-o, --outputdir <DIR>`: 로그 출력 디렉토리(기본 `.`). `lat.csv`, `perf.csv` 생성
  - 예시
```bash
./cosmos_sim -n 2 \
  -w "0 70 30 4096" -w "1 50 50 4096" \
  -s "60 40" -i 100000 -c -r -o ./out
```
