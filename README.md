## 2025 DKU Semiconductor Software Practices

### 구조
- `lab1/`: FTL 시뮬레이터 구현 과제
- `lab2/`: OpenSSD 애뮬레이션 환경에서 GreedyFTL 개선

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
./myftl        # 실행
make clean     # 정리
```
- 코드 구조
``` 
lab1/
├─ workload/         # 워크로드 파일
├─ Makefile
├─ test.h
├─ test.c            # 테스트 코드
├─ myftl.h           # 시뮬 파라미터/자료구조, FTL API 선언
└─ myftl.c           # FTL 구현(매핑/할당/GC)
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
  - `-w, --worker "<pattern> <read%> <write%> <nblks>"`: 워커별 패턴/비율/블록수(여러 번 지정 가능). `read%+write%=100`, `nblks`는 4 KiB 블록 단위로 1~128
  - `-s, --size "p1 p2 ..."`: 각 워커 파티션 비율(%) 목록. 미지정 시 균등 분배
  - `-i, --inst <N>`: 총 작업 수(operations)
  - `-c, --condition`: 사전 조건화(preconditioning) 수행
  - `-r, --report`: 로그 파일 출력 활성화
  - `-o, --outputdir <DIR>`: 로그 출력 디렉토리(기본 `.`). `lat.csv`, `perf.csv` 생성
  - 예시
```bash
./cosmos_sim -n 2 \
  -w "0 70 30 4" -w "1 50 50 4" \
  -s "60 40" -i 100000 -c -r -o ./out
```

#### fio job 파일로 실행

`lab2/workloads/example.fio`처럼 fio INI 형식으로 workload를 작성한 뒤 파일 하나만 넘길 수 있다.

```bash
cd lab2
./cosmos_sim workloads/example.fio
# 같은 의미: ./cosmos_sim --fio-job workloads/example.fio
```

- `[global]` 설정은 뒤따르는 job section에 상속된다.
- 일반 job section 하나가 simulator worker 하나가 되며 `numjobs`만큼 복제된다(전체 최대 10개).
- 지원 workload 옵션: `rw`/`readwrite`, `rwmixread`, `rwmixwrite`, `bs`/`blocksize`, `size`, `io_size`/`io_limit`, `offset`, `offset_increment`, `number_ios`, `numjobs`, `iodepth`.
- 모든 job은 하나의 simulated NVMe namespace를 대상으로 한다. `ioengine`, `direct`, `filename`, `thread`, `group_reporting` 등 파일/OS 실행에만 관계된 옵션은 호환을 위해 읽되 simulator에서는 효과가 없다.
- `bs`, `offset`, `size`, `io_size`는 4 KiB LBA 단위에 맞아야 하고, `bs`는 4 KiB~512 KiB 범위여야 한다. 전송량으로 쓰이는 `io_size`(없으면 `size`)는 `bs`의 정확한 배수여야 한다.
- 크기 suffix는 fio 기본값인 `kb_base=1024` 규칙을 따른다(`4k` = 4096 bytes). `kb_base` 옵션 자체는 지원하지 않으므로 파일에서 생략해야 한다. `size`만 `%` 표기를 지원하고 `io_size`는 byte 크기여야 한다.
- job별 요청 수는 `io_size / bs`, `io_size`가 없으면 `size / bs`로 계산하며, `number_ios`가 있으면 그 수를 넘지 않도록 제한한다.
- 시간 기반 실행(`runtime`, `time_based`), trim, verify, `bssplit`/`bsrange`, rate/thinktime, zoned workload는 현재 simulator 모델과 의미가 달라 오류로 거부한다.
- random LBA는 기존 simulator 동작을 유지해 가능한 경우 16 KiB page 경계에 맞춘다. 범위 안에 그런 시작 주소가 없으면 범위의 첫 LBA를 사용하며 주소가 반복될 수 있다. mixed read/write 선택도 난수 대신 지정 비율을 따르는 결정적 순서이므로, 같은 job 파일을 실제 fio로 실행한 결과와 요청 분포가 완전히 같지는 않다.
- 기존 `-c`, `-r`, `-o`는 fio 파일 실행과 함께 사용할 수 있다. workload를 정의하는 `-n`, `-w`, `-s`, `-i`와 `--fio-job`은 함께 사용할 수 없다.

parser 단위 테스트는 다음과 같이 실행한다.

```bash
make test
```
