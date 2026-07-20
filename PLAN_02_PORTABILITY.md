# PLAN_02 — 이식성 규칙 (C89 / socket)

목표 컴파일러 하한: **gcc 2.5.x (NeXTSTEP 3.x)**, gcc 2.9x (BeOS R5),
MSVC 6.0 (Win32), **Open Watcom 16bit (MS-DOS)**, 그리고 현대
gcc/clang. 이 하한이 모든 코딩 규칙을 결정한다.

MS-DOS(16bit large model)가 추가되면서 유의할 상수 제약:
`int`는 16bit이다. **프로토콜 길이/카운트 값은 반드시 `long`**,
버퍼 인덱스도 상한이 4096을 넘는 경로는 `long`을 쓴다.
(PLAN_01의 프레임 상한 4096B는 16bit 환경을 함께 고려한 값)

## 1. C 언어 규칙 (강제)

- **엄격한 C89.** `gcc -ansi -pedantic -Wall`이 경고 없이 통과해야 한다.
- 선언은 블록 첫머리에만. `//` 주석 금지 (`/* */`만).
- `snprintf` **사용 금지** — C89에 없고 구식 libc에 부재.
  `common/`에 자체 `gcds_strlcpy`, `gcds_strlcat`과, 정수→문자열 변환
  `gcds_itoa`를 두고 이것만 사용한다. `sprintf`는 고정 포맷+상한이
  증명되는 곳에만 허용.
- `stdint.h`, `stdbool.h`, `inttypes.h` 금지. `int`/`long`/`unsigned long`
  만 사용. 프로토콜상 길이 값은 `long`으로 통일 (모든 대상에서 ≥32bit).
- 가변 인자 매크로, 인라인, `const` 캐스팅 트릭 금지.
- 동적 할당 최소화: 세션 상태는 전부 고정 크기 정적/스택 버퍼.
  (라인 1024B, 프레임 4096B, ENV 32개 — PLAN_01의 상한이 이를 보장)
- 파일명은 8.3 형식을 의식해 **기본 이름 8자 이내** 유지
  (구식 파일시스템/도구 호환).

## 2. socket 사용 규칙 (보수적 교집합)

**허용 함수 목록** — 이 밖의 네트워크 API는 사용하지 않는다:

```
socket, bind, listen, accept, connect,
send, recv, (closesocket | close),
setsockopt(SOL_SOCKET, SO_REUSEADDR),   /* 서버, 가능한 곳에서만 */
htons/htonl/ntohs/ntohl,
inet_addr, gethostbyname                 /* 클라이언트만 */
```

- **IPv4 전용.** `struct sockaddr_in` 직접 사용. `getaddrinfo` 금지
  (구세대 OS에 없음).
- 위 허용 목록은 **Watt-32(DOS)의 BSD 호환 계층에도 존재**하는
  교집합이다 — DOS 포팅에서 네트워크 코드 수정이 없도록 유지한다.
- **blocking I/O만.** 논블로킹 플래그, `ioctl(FIONBIO)` 금지.
  `select()` 허용 범위는 세 곳뿐:
  (1) Linux 클라이언트 (타임아웃/대화형 stdin 중계 용도),
  (2) `INTERACTIVE` 플랫폼 데몬의 **RUNI 처리 구간** (§5.1),
  (3) `LIVE` 플랫폼 데몬의 **작업 실행 감시 루프** (§5.2 —
      실행 중 제어 세션 수락용. 감시 대상은 소켓 위주라
      BeOS R5의 소켓 전용 select로도 충족).
  그 외 데몬 코드 경로(accept 대기, AUTH, 세션 명령 처리)는 여전히
  순수 blocking으로 유지 — 미지원 빌드는 select 코드가 컴파일되지
  않는다 (`GCDS_HAS_IX` / `GCDS_HAS_LIVE` 매크로로 격리).
- `send`/`recv`는 부분 전송을 전제로 반드시 루프 래퍼
  (`net_write_all`, `net_read_n`)를 통해서만 호출.
- `SIGPIPE`: POSIX 쪽에서 `signal(SIGPIPE, SIG_IGN)` 후 `send` 오류
  코드로 처리 (BeOS/NeXTSTEP에도 존재하는 가장 오래된 방식).
- 소켓 옵션 실패는 치명 오류로 취급하지 않는다 (SO_REUSEADDR가
  없거나 실패해도 계속 진행).

## 3. 소켓 추상화 계층 (`common/net.h`)

`#ifdef` 지옥을 한 파일에 가둔다. 다른 소스는 아래 타입/함수만 본다.

```c
/* net.h 개요 */
typedef int gcds_sock_t;            /* Win32에서는 SOCKET으로 typedef */

int  net_init(void);               /* Win32: WSAStartup(1.1). 그 외: no-op */
void net_cleanup(void);            /* Win32: WSACleanup. 그 외: no-op */
gcds_sock_t net_listen(unsigned short port);
gcds_sock_t net_accept(gcds_sock_t ls, char *peer_ip /*16B*/);
gcds_sock_t net_connect(const char *host, unsigned short port);
int  net_read_n(gcds_sock_t s, char *buf, long n);    /* 전량 수신 */
int  net_write_all(gcds_sock_t s, const char *buf, long n);
void net_close(gcds_sock_t s);
int  net_errno(void);              /* errno | WSAGetLastError */
```

플랫폼 분기 원칙: `#ifdef GCDS_WIN32`, `#ifdef GCDS_BEOS`,
`#ifdef GCDS_NEXT` 등 **자체 매크로**를 make에서 -D로 주입.
컴파일러 내장 매크로 추측에 의존하지 않는다.

### 3.1 전송 채널 추상화 (`common/chan.h`)

프로토콜 계층(lineio 이상)은 TCP/시리얼을 구분하지 않는다.

```c
/* chan.h 개요 */
typedef struct gcds_chan gcds_chan_t;   /* kind(TCP|SERIAL) + 핸들 */

int  chan_read_n(gcds_chan_t *c, char *buf, long n);
int  chan_write_all(gcds_chan_t *c, const char *buf, long n);
void chan_close(gcds_chan_t *c);
/* 열기는 백엔드별: net_listen/net_accept/net_connect (TCP),
   ser_open(포트명, 속도) (시리얼) — 결과를 chan으로 감싼다 */
```

시리얼 백엔드 (blocking read/write만 사용, 흐름제어 설정 외
장치 제어 최소화):
- `ser_psx.c` — POSIX termios (Linux/macOS/Haiku 공용).
- `ser_next.c` — NeXTSTEP 전용 sgtty 백엔드. termios 헤더는 있으나
  함수가 libc에 없어(구 BSD sgtty 사용) `TIOCGETP/SETP` +
  `struct sgttyb`(RAW)로 직접 구현 (`GCDS_NEXT`, doc/next.md).
- `ser_w32.c` — Win32 `CreateFile("COM1")` + `SetCommState`(DCB)
  + `ReadFile`/`WriteFile`.
- `ser_dos.c` — **FOSSIL 드라이버**(int 14h 확장, X00/BNU 등)
  경유. 인터럽트 구동 수신 버퍼를 제공하는 레트로 표준이라
  9600bps에서 폴링 유실이 없다. FOSSIL 부재 시의 직접 UART
  제어는 v1 범위 밖 (FOSSIL 상주를 요구사항으로 문서화).
- BeOS R5 — `/dev/ports/serial1`에 POSIX 경로(ser_psx) 사용.
- 데몬 기동 모드: `gcdsd`(TCP, 기본) / `gcdsd -serial <port> <baud>`.
  시리얼 모드의 세션 경계는 HELLO attention (PLAN_01 §1.1).

## 4. OS별 노트

### Win32 (MSVC / MinGW)
- Winsock **1.1**로 요청 (`WSAStartup(MAKEWORD(1,1))`) — NT4~11 전부 동작.
- 헤더 `winsock.h` (winsock2 아님), 라이브러리 `wsock32.lib`.
- `close` → `closesocket`, 오류 → `WSAGetLastError()`.
- 실행: CreateProcess + 폴링 하이브리드 감시 루프(live_w32.c).
  블로킹 캡처는 cmd.exe 리다이렉션 경유 `system()`.
- MSVC 사용 전제: 데몬을 vcvars 적용된 환경에서 띄우거나,
  `RUN`에서 `vcvars32.bat && cl ...` 형태로 호출 (문서화로 해결).
- 서비스 등록은 범위 밖 — 콘솔 프로세스로 실행.
- **INTERACTIVE 지원.** 단 Winsock `select()`는 소켓 전용이라
  자식 파이프를 함께 감시할 수 없다 → RUNI 루프는
  `select(소켓, timeout 50~100ms)` + `PeekNamedPipe`(자식 출력)
  + `WaitForSingleObject(자식, 0)`의 폴링 하이브리드로 구현
  (live_w32.c). 폴링 주기만큼의 출력 지연은 수용한다.
- **LIVE 지원**: 위 폴링 루프의 select 집합에 listen
  소켓을 추가 — 실행 중 제어 세션 수락 (§5.2).

### BeOS / Haiku
본 프로젝트는 **BeOS를 Haiku와 동일 취급**한다(Haiku가 BeOS의
후속·호환 구현이며 gcc 2.95 하이브리드를 제공).

- 소스는 POSIX 공용, 링크만 `make -f make/Makefile.posix
  LIBS=-lnetwork`(소켓이 `libnetwork`에 있음). gcc 2.95는 프로젝트의
  **C89 하한 검증 플랫폼**.
- Haiku 소켓은 진짜 fd라 select가 소켓+파이프에 동작 → LIVE·RUNI
  그대로 지원(허용 소켓 함수 목록 유지, send/recv 사용). fork/exec/
  setpgid/termios 모두 동작.
- 상태·검증 결과는 README 현황표 및 doc/beos.md 참조.

### NeXTSTEP / OPENSTEP
- `make -f make/Makefile.next` (데몬만; 클라이언트는 Linux).
  4.3BSD 파생이라 POSIX 이전 세대 — `-DGCDS_NEXT` + `common/gnext.h`
  shim으로 흡수:
  - `socklen_t`/`pid_t` 없음 → typedef int
  - `sys/select.h` 없음 → sys/time.h (FD_SET은 sys/types.h)
  - `WIFEXITED` 등이 `union wait` 기준(.w_S/.w_T) → int 기반 매크로로
    재정의
  - `O_NOCTTY` 없음 → 0으로 no-op (ser_psx.c 이식성 가드)
  - `size_t`가 termios.h로 안 들어옴 → sys/types.h 명시 include
  - `waitpid`/`setpgid` 없음 → `wait4`/`setpgrp`로 매핑(live.c용)
- **LIVE/RUNI 포함** — NeXT의 select가 4.2BSD 네이티브라 소켓+파이프
  동시 감시 가능. 스트리밍 RUN/LIVE 제어세션/RUNI/K(프로세스 그룹
  종료)/비동기/PUT·GET 전부 동작 → Linux·macOS와 동등 기능.
- **시리얼: sgtty 백엔드(ser_next.c, §3.1)**. `/dev/ttya`,`/dev/ttyb`.
- 소스는 gnfsd NFS 공유로 전달해 OPENSTEP이 mount 후 빌드(자작 NFS +
  데몬 이식의 통합 실증). 상태·검증 결과는 README 및 doc/next.md 참조.

### macOS / 현대 Unix
- 특기 사항 없음. `Makefile.posix` 하나로 Linux와 공용.
- 경고: 현대 컴파일러에서 C89 코드에 나오는 deprecated 경고는 무시.
- **INTERACTIVE 지원** (live.c: fork + pipe 3개 + select).
- **LIVE 지원**: select 집합에 listen 소켓 추가 (§5.2).

### MS-DOS (Open Watcom 16bit + Watt-32)
- **컴파일러**: Open Watcom `wcc`, **large 메모리 모델(-ml)**,
  8086 타깃(-0) 기본. 빌드는 `wmake`(Makefile.dos) + `wlink`.
- **TCP/IP**: Watt-32의 BSD socket 호환 계층. 16bit large model로
  빌드한 Watt-32 라이브러리를 정적 링크. `net_init()`이
  `sock_init()`(Watt-32 초기화)에 대응. 대상 머신에는 NIC용
  **패킷 드라이버** 상주 + `WATTCP.CFG`(my_ip/netmask/gateway) 필요.
  빌드 재현 절차와 함정은 doc/dos.md.
  핵심 함정: **스톡 Watt-32 large model은 BSD API를 제외**하므로
  config.h에 `USE_BSD_API`를 강제해야 socket 심볼이 라이브러리에
  들어온다. 컴파일은 `-DWATT32_STATIC` 필수. DGROUP 64K가 매우
  빡빡해 DOS 데몬 전용 축소 상한(CONF_ENT_MAX 12)과 gethostbyname
  제외(데몬은 listen만)로 맞춘다.
- **비동기 작업 모델(PLAN_00 D7, PLAN_01 §7)이 기본.**
  단일 태스킹이라 자식 실행 중 스택 tick 불가 → greeting에 `ASYNC`
  플래그를 반드시 표기. 실행 중 도착하는 SYN은 무응답(클라이언트
  재시도로 처리). 같은 이유로 **INTERACTIVE·LIVE는 구조적으로
  불가**(실행 중 PING/STAT 무응답 — busy/dead 구분 불가 예외를
  이 플랫폼에 한해 수용, PLAN_00 D11). 대화형 도구는 입력 파일
  리다이렉션 배치로 폴백 (PLAN_04 §5. `debug < script.txt`는
  COMMAND.COM에서도 동작).
- **exec_dos.c**: COMMAND.COM은 `2>` 리다이렉션이 불가하므로
  `system()`을 쓰지 않는다. `dup2()`로 핸들 1/2를 임시 파일에 붙인 뒤
  `spawnl(P_WAIT, COMMAND.COM 경로(%COMSPEC%), "/C", <명령행>)` 실행 —
  DOS의 핸들 상속으로 stdout/stderr 분리 캡처. 반환값 = errorlevel.
- **메모리**: large model 데몬 + Watt-32 상주 상태에서 자식
  (COMMAND.COM + 컴파일러)이 남은 conventional memory로 돌아야 한다.
  고정 버퍼를 필요 최소로 유지하고, 대형 컴파일러가 필요한 경우의
  스왑 기법(EMS/디스크)은 v1 범위 밖 — 리스크로 관리.
- **ENV**: 자식 환경 블록 크기 제한이 세션 상한(32개)보다 먼저 걸릴
  수 있음. 데몬은 spawn 시점에 환경 병합 실패를 `ERR 500`으로 보고.
- 매크로 `GCDS_DOS`, os-tag `dos`. 파일명 8.3 규칙은 이 플랫폼 때문에
  전역 강제였음.
- **시리얼 모드가 특히 유용한 플랫폼**: NIC/패킷 드라이버가 없는
  DOS 머신은 `gcdsd -serial COM1 9600`(FOSSIL 필요, §3.1)으로
  Watt-32 없이 동작 — 이 구성에서는 Watt-32를 링크하지 않는
  별도 빌드(더 작은 상주 크기)로 만든다.
- **검증**: DOSBox-X(NE2000 패킷 드라이버) 완료. 실기(FOSSIL 상주 +
  널모뎀) 재확인은 잔여 작업(PLAN_03).

### 고전 Mac OS (7~9)
- BSD socket이 없다 (MacTCP/OpenTransport). TCP 경로는 **v1 범위에서
  제외**하고 로드맵에 후보로만 기록.
- 단, **시리얼 채널(D10)이 생기면서 현실적 후보로 격상** — TCP 스택
  없이 Serial Driver(고전 Mac 표준 API)만으로 gcdsd 포팅이 가능해짐.
  ser_mac.c + THINK C/CodeWarrior 빌드는 Phase 4 검토 항목.

## 5. 실행(exec) 추상화

```
daemon/exec.h:
  int run_command(const char *cmdline,
                  const char *outfile, const char *errfile);
  /* 반환: exit code 0..255, 실행 불가 시 -1 */
```
- `exec_psx.c`: `system()`으로 `<cmd> > outfile 2> errfile` 실행.
  셸 인용은 명령행을 그대로 전달하므로 문제 없음 (리다이렉션 부분만
  데몬이 덧붙임). exit code는 `WEXITSTATUS` 규약.
- `exec_w32.c`: `system()` 동일 구조. cmd.exe의 리다이렉션 사용.
- `exec_dos.c`: dup2 + spawnl 방식 (§4 MS-DOS 참조).

### 5.1 대화형 실행 (RUNI, INTERACTIVE 플랫폼만)

RUNI 처리는 별도 모듈이 아니라 **감시 루프(daemon/live.c, §5.2)에
흡수**되어 있다(설계 초안의 ix_psx.c/ix_w32.c는 live.c/live_w32.c로
통합됨). 한 건의 RUNI 전 과정 — 자식 생성(파이프 연결), 소켓 I/K
프레임 수신 ↔ 자식 stdin 전달, 자식 stdout/stderr → O/E 프레임 송신,
종료 시 exit code(X 프레임), K 처리(프로세스 그룹 강제 종료) — 을
같은 루프가 담당한다.
- POSIX(live.c): fork + pipe(stdin/stdout/stderr) + select(소켓, 파이프 2개).
- Win32(live_w32.c): CreateProcess(핸들 파이프) + 폴링 하이브리드 (§4 Win32).
- 컴파일 격리: `GCDS_HAS_IX`가 없으면 세션 코드는 RUNI에 `ERR 501`을
  응답하는 한 줄만 남아, 미지원 플랫폼 빌드에 select/파이프 코드가
  전혀 들어가지 않는다.

### 5.2 실행 감시 루프 (LIVE)

`GCDS_HAS_LIVE` 플랫폼은 blocking `system()` 대신 자식을 비동기로
띄우고 감시 루프를 돈다:

```
자식 실행 중 반복:
  select({listen 소켓, [세션 소켓], [자식 출력]}, timeout)
  - listen 소켓 readable → 제어 세션 1건 인라인 처리
    (greeting → AUTH → PING/STAT/QUIT. 그 외 ERR 409. PLAN_01 §4.1)
  - 자식 출력 있음 → O/E 프레임 송신 (동기 RUN 스트리밍)
  - 자식 종료 확인 (waitpid WNOHANG / WaitForSingleObject 0)
    → X 프레임 후 루프 탈출
```

- 플랫폼별 "자식 출력" 감시 수단만 다르다:
  POSIX = 파이프 select / Win32 = PeekNamedPipe 폴링 /
  BeOS = 임시 파일 증가분 폴링 (모두 §4의 각 절 참조).
- 제어 세션 처리 중에는 자식 출력 수집이 잠시 멈춘다 — 제어
  세션이 4개 명령만 허용하는 짧은 대화이므로 수용한다.
- RUNA(비동기 접수) 작업의 실행 중에도 같은 루프를 쓴다. 이때는
  세션 소켓이 없고(접수 후 닫힘) listen + 자식 출력만 감시하며,
  출력은 임시 파일에 쌓는다.
- 임시 파일: 설정 `tmpdir` 아래 `gcds_out.tmp`/`gcds_err.tmp` 고정 이름
  (단일 클라이언트 서버라 충돌 없음). RUN 완료 후 삭제.
- **경로 결합은 네이티브 구분자로 한다**: Win32/DOS는 `\`, 그 외는 `/`
  (session.c `join_tmp`). Win32는 이 경로를 cmd.exe 리다이렉션에 넘기고,
  **DOS에서 `/`는 경로 구분자가 아니라 명령 스위치 문자**라 COMMAND.COM에
  닿는 경로에 쓰면 안 된다. `tmpdir`이 이미 `/`나 `\`로 끝나면 중복
  추가하지 않는다(양쪽 표기 모두 허용). Win32/DOS 기본 `tmpdir`은 `.`
  이므로 결과는 `.\gcds_out.tmp`.
- **설정 파일명은 8.3 우선**: `.cnf`를 먼저 찾고 `.conf`로 폴백한다.
  DOS는 FAT 8.3이라 4자 확장자가 존재할 수 없어 `.cnf`만 찾는다
  (`GCDS_DOS` 조건부 — 문자열도 줄어 DGROUP에 유리).

## 6. 빌드

- autotools/CMake 금지. 플랫폼별 수동 Makefile (`make/` 디렉토리).
- 소스는 전 플랫폼 공용, 차이는 -D 매크로와 링크 라이브러리뿐.
- 줄바꿈: 저장소는 LF 통일. (구식 MSVC가 LF 소스를 수용함)
