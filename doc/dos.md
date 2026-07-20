# MS-DOS 데몬 노트

16비트 빌드 두 종류:
- **시리얼**(`Makefile.dos`): Watt-32 미링크, net_null 스텁. NIC 없는
  머신용. 기본·권장.
- **TCP**(`Makefile.dtcp`): Watt-32 링크, 패킷 드라이버 기반. NIC 있는
  머신용. 시리얼 백엔드도 함께 포함(TCP/시리얼 겸용 데몬).

## 시리얼 빌드 (Linux 호스트에서 Open Watcom v2 크로스)

```
make -f make/Makefile.dos WATCOM=/path/to/ow-snapshot
```

- Open Watcom v2 Linux 호스트 tarball(`ow-snapshot.tar.xz`)을 풀어
  `WATCOM=`으로 지정. sudo 설치 불필요.
- large 메모리 모델(`-ml`), 8086(`-0`), `-dGCDS_DOS -dGCDS_NO_NET`.
- 산출물 `gcdsd.exe` (약 35KB).

## 실행 (대상 DOS에서)

```
gcdsd
```
설정은 같은 디렉토리의 `gcdsd.cnf`를 자동으로 읽는다.
**DOS는 FAT 8.3이라 `.conf`(4자)가 존재할 수 없어 `.cnf`만 찾는다**
(`/`는 DOS에서 경로 구분자가 아니라 명령 스위치 문자이므로, 데몬이
만드는 임시 경로도 `\`로 결합된다 — `.\gcds_out.tmp`).

`gcdsd.cnf` 예:
```
token = dt
serial = COM1:9600
async = 1
```
- 시리얼 전용이므로 `serial=` 필수, `async=1` 권장(단일 태스킹 →
  greeting에 ASYNC 표기, 클라이언트가 RUNA/RESULT 자동 사용).
- 대상 실기에서는 **FOSSIL 드라이버**(X00/BNU) 상주 권장 —
  인터럽트 구동 버퍼로 9600bps 무손실. 없으면 BIOS int 14h 폴백
  (에뮬레이터/락스텝엔 충분, 실기 고속엔 유실 가능).

## TCP 빌드 (Watt-32)

### 1. Watt-32 16비트 large model 라이브러리 준비 (선행)

Watt-32 소스에서 Open Watcom으로 빌드한다. **주의: 스톡 Watt-32
large model 설정은 BSD socket API를 의도적으로 제외**한다
(src/config.h의 `__LARGE__` 블록, 공간 제약). 우리 net.c는 BSD
소켓을 쓰므로 이를 강제로 켜야 한다.

재현 절차:
1. Watt-32 소스 취득(gvanem/Watt-32).
2. util/mkmake를 Linux gcc로 빌드 — `gcc -o util/linux/mkmake
   util/mkmake.c -lslang` (libslang 필요).
3. large model makefile 생성:
   `util/linux/mkmake -w -o src/watcom_l.mak -d src/build/watcom/large
    src/makefile.all WATCOM LARGE`
4. **src/config.h의 `#if !defined(OPT_DEFINED) && defined(__LARGE__)`
   블록에 `#define USE_BSD_API` 추가** (안 하면 라이브러리에 socket/
   bind/accept 심볼이 아예 없음 — 링크 시 undefined). USE_BIND/
   USE_DEBUG 등은 DGROUP 절약 위해 빼는 게 좋다.
5. errno 파일 생성(Watcom 값 필요): errnos.c를 `wcl -bcl=dos -ml`로
   빌드 → DOSBox에서 `wcerr -e > watcom.err`, `wcerr -s > syserr.c`
   실행 → `inc/sys/watcom.err`, `src/build/watcom/{,large/}syserr.c`
   에 배치.
6. `wmake -h -f src/watcom_l.mak` → `lib/wattcpwl.lib` 생성.

### 2. 데몬 빌드

```
make -f make/Makefile.dtcp WATCOM=/path/to/ow WATT=/path/to/watt32
```
- `-DWATT32_STATIC` 필수(정적 링크 심볼 규약 일치).
- net.c는 Watt-32 헤더로 컴파일: socket→`_w32_*` 리맵, sock_init,
  closesocket. `net_init`이 `sock_init()`을 호출.
- **DGROUP 64K 제약이 빡빡하다** — Watt-32 데이터 + 우리 statics.
  DOS 빌드는 CONF_ENT_MAX 12로 축소(gcdsp.h/conf.h). DOS 데몬은
  listen만 하므로 net_connect의 gethostbyname(BIND 리졸버, 큰 데이터)
  을 컴파일 제외.

### 3. 실행 (대상 DOS에서)

```
NE2000 0x60 3 0x300         ; 패킷 드라이버 (Crynwr, IO/IRQ는 NIC에 맞춤)
gcdsd
```
- `WATTCP.CFG` 필요(같은 디렉토리): `my_ip`, `netmask`, `gateway`.
- `gcdsd.cnf`: `port`/`token`/`async = 1` (serial= 없으면 TCP 모드).
- 패킷 드라이버 + Watt-32 상주로 conventional memory가 줄어든다 —
  대형 컴파일러/프로그램 실행 시 부족(errcode=8) 가능. 내부 명령
  (echo 등)과 소형 도구는 OK. EMS/스왑 활용은 v1 범위 밖.

### 텍스트 인코딩

**ASCII 전제**다. 인코딩 변환 모듈(textcv)을 링크조차 하지 않아
DGROUP에 영향이 없고, 바이트는 그대로 통과한다. 따라서 greeting에
`UTF8` capability를 표기하지 않는다(PLAN_02 §5.2).

## 검증 (DOSBox-X + NE2000 + slirp)

DOSBox-X `[ne2000] backend=slirp` + `[ethernet,slirp]
tcp_port_forwards=2321:2321`로 호스트→게스트 포워딩. Crynwr NE2000
패킷 드라이버 로드 → Watt-32 sock_init 성공 → Linux `gcds`가
`127.0.0.1:2321`로 접속. 통과: greeting `GCDS 1 dos ... ASYNC`,
ping/STAT, RUNA→RESULT 왕복, 다중 프레임 출력, 연속 작업(jobid 증가).

## 검증 (DOSBox-X) — 시리얼

DOSBox-X의 nullmodem 시리얼을 TCP로 브리지(`serial1 = nullmodem
port:15234 transparent:1`), Linux에서 `socat pty,raw <-> tcp`로
`gcds`의 시리얼 포트에 연결. 통과 항목:

- greeting `GCDS 1 dos <host> ASYNC`, `--ping` alive
- `--stat` idle/result, RUNA→RESULT 왕복
- 원격 출력 캡처(echo/dir), **stdout/stderr 분리**
- **exit code 전달**: 프로그램은 정확히 전달(아래 실행 모델 참조)

## 실행 모델 (exec_dos.c)

COMMAND.COM은 `2>`를 못 하므로 `system()` 대신 `dup2()`로 핸들
1/2를 캡처 파일에 붙이고 자식을 spawn한다. 두 경로:

1. **직접 spawn (`spawnvp`)** — 셸 메타문자(`< > |`)가 없으면 명령을
   프로그램으로 간주하고 직접 실행. PATH 검색 + argv 분리.
   **자식 exit code를 정확히 전달**(컴파일러 호출의 일반 경우).
2. **셸 폴백 (`COMMAND.COM /C`)** — 직접 spawn이 실패(내부 명령
   echo/dir/type 등, 또는 미발견)하거나 셸 메타문자가 있으면 셸에
   위임. 출력/기능은 정상이나 **exit code는 셸에 의존**.

### 알려진 제약

- **DOSBox-X 내부 COMMAND.COM은 `/C` 자식 errorlevel을 전달하지
  않는다**(항상 0/1). 실기 MS-DOS COMMAND.COM은 정상 전달.
  → 직접 spawn 경로(프로그램 실행)는 이 문제와 무관하게 정확.
  → 내부 명령의 exit code만 DOSBox-X에서 신뢰 불가.
- 인자 분리는 공백 기준 단순 토크나이저(따옴표 처리 없음).
  8.3 경로엔 공백이 없어 실무상 무해.
- 시리얼 폴링 루프는 매 스핀마다 INT 28h(DOS idle)로 양보한다 —
  타이트 폴링이 호스트/TSR의 바이트 전달 기회를 굶기는 것을 방지
  (DOSBox-X에서 필수, 실기에서도 올바른 관례).
- large model DGROUP 64K 제약 때문에 DOS 빌드는 축소 상한 사용:
  FRAME_MAX 1024, ENV_MAX 16, CONF_ENT_MAX 24 (gcdsp.h/conf.h).
- job/res 상태(~9KB env 배열 포함)는 static — 16비트 스택
  (~2KB)에 지역변수로 두면 함수 진입 시 오버플로.
