# PLAN_00 — 아키텍처 개요

## 1. 전체 그림

```
 [Linux 호스트]                          [원격 머신 N대]
 +-----------+     GCDSP over TCP        +-----------------+
 |   gcds     | -----------------------> |     gcdsd        |
 | (client)  | <----------------------- | (daemon)        |
 +-----------+   stdout/stderr/exit     +--------+--------+
       |                                          |
       |          NFS / SMB / AFP / FTP           v
       +----------------[ 공유 스토리지 ]---- 네이티브 컴파일러
                                            (msvc/llvm/gcc...)
```

- `gcds`(Linux)가 `gcdsd`(원격)에 TCP로 접속하여 작업 디렉토리,
  환경변수, 명령행을 전달한다.
- `gcdsd`는 해당 OS의 기본 셸(`/bin/sh -c`, `cmd.exe /c`)로 명령을
  실행하고, stdout/stderr와 종료 코드를 돌려보낸다.
- 소스/산출물은 공유 스토리지에 있으므로 데몬은 파일을 나르지 않는다.
  (공유가 불가능한 OS를 위해 PUT/GET 파일 전송을 제공 — PLAN_01)

## 2. 핵심 설계 결정

### D1. 반복(iterative) 단일 클라이언트 서버
- `accept()` → 세션 처리 → `close()` → 다시 `accept()`.
- 스레드 없음, fork 없음(Windows에 fork가 없음), `select()` 없음이
  기본형. 가장 보수적이며 모든 대상 OS에서 동일하게 동작한다.
- 컴파일 지시라는 용도상 동시 접속 필요성이 낮다. 병렬 빌드는
  "머신 여러 대 × 각 1세션"으로 달성한다.

### D2. 명령 실행은 두 경로
- **블로킹 캡처:** `system("cmd > out.tmp 2> err.tmp")`로 실행 완료
  후 임시 파일 내용을 전송. 단순하고 모든 OS에서 동작.
- **스트리밍:** 감시 루프(live.c / live_w32.c)가 자식을 비동기로
  띄우고(POSIX: fork+exec+waitpid(WNOHANG) / Win32: CreateProcess+
  폴링) 출력을 발생 순서대로 스트리밍한다. 두 경로는 **동일 와이어
  포맷**을 쓰므로 클라이언트는 구분할 필요가 없다.

### D3. 프로토콜은 rexec 모델을 계승한 자체 명세(GCDSP)
- 검증된 구조(명령 전송 → 출력 스트림 → exit code)를 따르되,
  stdout/stderr 구분과 바이너리 안전성을 위해 길이 접두 프레임을
  사용한다. 상세는 PLAN_01 참조.
- rexec/rshd가 내장된 시스템(NeXTSTEP 등)은 데몬 포팅 이전에도
  쓸 수 있도록, `gcds`에 rexec 어댑터를 확장으로 예약한다(Phase 4).

### D4. 인증과 보안 범위
- 내부망 전용 전제. 평문 프로토콜.
- 공유 토큰 1개(양쪽 설정 파일에 기재)를 세션 시작 시 검증.
- **허용 클라이언트 IP 목록(`allow=`)** — 구현됨(common/acl.c).
  공백/쉼표 구분 exact IP 또는 CIDR, 비어있으면 전체 허용. TCP
  전용(accept 직후 + 실행 중 제어세션 accept 모두 검사).
- **출력 상한(`maxout=`)** — 폭주 명령의 디스크/네트워크 고갈 방지.
  스트리밍은 누적 바이트, 비동기 JOB은 캡처 파일 크기로 감시,
  초과 시 자식 종료 + 잘림 통지(session.c/live.c/live_w32.c).
- **정상 종료 정리** — POSIX 데몬은 SIGTERM/SIGINT에서 캡처 임시
  파일을 지우고 종료(main.c on_term).
- 그 이상(TLS, 사용자 계정, 명령 화이트리스트)은 범위 밖.

### D5. 설정은 key=value 평문 파일
- 파서 의존성 없음. `#` 주석, 빈 줄 허용, 한 줄 최대 1023바이트.
- 데몬: `gcdsd.conf` — port, token, allow(선택), shell(선택),
  tmpdir(선택), logfile(선택), serial(선택 — 지정 시 TCP 대신
  시리얼 모드: `serial=COM1:9600`)
- 클라이언트: `gcds.conf` — 호스트 별칭 블록
  (`host.<alias>.addr`, `host.<alias>.port`, `host.<alias>.token`,
   또는 TCP 대신 `host.<alias>.serial=/dev/ttyUSB0:9600` — D10,
   `host.<alias>.map.<n>` 경로 매핑,
   `host.<alias>.tool.<name>` 도구 별칭 — `gcds <alias> @<name> ...`이
   해당 명령행 접두로 치환됨. OS마다 다른 디버거/도구 명령을
   호스트 설정에 캡슐화하는 용도. PLAN_04 §4)

### D6. 경로 매핑은 클라이언트 책임
- 같은 공유 폴더가 Linux에선 `/mnt/proj`, Windows에선 `Z:\proj`,
  NeXTSTEP에선 `/Net/...`으로 보인다.
- `gcds`가 설정의 매핑 테이블로 로컬 cwd → 원격 경로를 변환하여
  `CWD`로 보낸다. 데몬은 받은 경로를 그대로 쓴다.
  (데몬을 단순하게 유지하기 위한 결정)
- **번역 대상은 CWD뿐** — 명령행 인자 속 경로는 번역하지 않는다.
  상대경로 원칙과 매핑 정밀 규칙(경계 매칭, 최장 일치, 구분자
  선택)은 **PLAN_05** 참조.
- 컴파일러 에러 메시지 내 경로의 역변환(원격→로컬)은 클라이언트
  출력 필터(`--mapback`)로 제공한다(선택 기능 — PLAN_05 §4).

### D7. MS-DOS 전용: 비동기 작업 모델 (submit → poll → fetch)
- DOS는 단일 태스킹이므로 자식(컴파일러) 실행 중 데몬이 recv는 물론
  Watt-32 스택 tick조차 할 수 없다. 장시간 컴파일 동안 연결을 붙들고
  있는 동기 RUN은 DOS에서 신뢰할 수 없다.
- 따라서 DOS 데몬은 **RUNA(접수 즉시 `OK <jobid>` 응답 후 연결을 닫고
  실행) + RESULT(재접속 후 결과 회수)** 모델을 쓴다. 상세는 PLAN_01 §7.
- 데몬은 인사(greeting)에 `ASYNC` capability를 표기하고, `gcds`는 이를
  감지해 접수→connect 재시도 폴링→RESULT 회수를 자동 수행한다.
  사용자에게는 동기 호스트와 동일한 UX(stdout/stderr/exit code)를 보인다.
- RUNA/RESULT의 기계장치는 플랫폼 무관이라 Linux 데몬/클라이언트와
  DOS(DOSBox-X)에서 동일하게 동작한다. (반복 단일 클라이언트 서버(D1)라
  Linux에서도 실행 중에는 accept를 못 하므로, 의미론이 DOS와 자연스럽게
  일치한다)

### D8. 대화형 실행은 capability 협상으로 (INTERACTIVE)
- 디버거 REPL, `dmesg -w`류 무한 로그 스트림에는 (1) 실행 중 stdin
  전달, (2) 원격 자식의 정상 중단 수단이 필요하다.
- 이를 위해 **RUNI**(대화형 실행)와 클라이언트→서버 프레임
  `I`(stdin)/`K`(중단)를 프로토콜에 추가한다 (PLAN_01 §5.1, §4).
- 데몬이 소켓과 자식 출력을 동시에 감시해야 하므로, "blocking 단일
  루프" 순수성을 **플랫폼별 capability로 깬다**: 다중 감시가 가능한
  플랫폼(POSIX/Win32/Haiku/NeXTSTEP, 고전 BeOS R5·DOS 제외)만
  greeting에 `INTERACTIVE`를 표기하고 RUNI를 받는다.
- 미지원 플랫폼에서의 폴백은 **입력 파일 리다이렉션 배치 실행**
  (`RUN debug < script.txt`) — 모든 OS의 셸이 `<`는 지원한다.
  시나리오별 매핑은 PLAN_04.
- pty는 채택하지 않는다(이식성). 자식 stdin/stdout은 파이프이며,
  tty 감지로 동작이 달라지는 도구의 한계는 PLAN_04에 문서화.

### D9. 커널 패닉 캡처는 out-of-band (gcdslog)
- 드라이버 개발 중 커널이 죽으면 gcdsd도 함께 죽는다 — userland
  데몬으로 패닉 메시지를 받는 것은 원리적으로 불가능하다.
- 정석대로 **시리얼 콘솔**을 보조 경로로 둔다: Linux 호스트에서
  원격 머신의 시리얼 출력을 수신·타임스탬프·기록하는 독립 도구
  `gcdslog`(tools/gcdslog.c). (대상 레트로 머신 대부분이 시리얼
  포트를 갖고 있어 현실적인 선택)
- gcdsd는 "OS가 살아 있는 동안의 로그 수신"(D8)까지만 담당한다.
  역할 경계는 PLAN_04 §3.

### D10. 전송 채널 추상화 — TCP + 시리얼 (네트워크 없는 OS)
- GCDSP는 처음부터 "신뢰 가능한 양방향 바이트 스트림"만 가정하도록
  규정한다. TCP는 그 구현 중 하나일 뿐이며, **시리얼(RS-232,
  기본 9600 8N1)이 두 번째 구현**이다.
- `common/chan.h`가 채널 추상화(read_n/write_all)를 제공하고,
  lineio 이하 전 계층은 채널만 본다. TCP 백엔드(net.c)와 시리얼
  백엔드(ser_psx/ser_w32/ser_dos)를 빌드/설정으로 선택.
- 시리얼에는 연결/해제 개념이 없으므로 세션 경계를 `HELLO`
  attention 라인으로 정의한다 (PLAN_01 §1.1). 오류 검출은 v1에서
  두지 않는다(내부 단거리 케이블 전제, CRC 확장 예약).
- 효과: NIC/패킷 드라이버가 없는 DOS 머신, 나아가 TCP 스택이 없는
  OS(고전 Mac 등)까지 지원 후보가 넓어진다. "TCP가 지원되는 OS"
  제약이 "시리얼 포트와 C 컴파일러가 있는 OS"로 완화된다.
- gcdslog(D9)와는 용도가 다르다: gcdslog는 커널 콘솔 raw 캡처,
  이것은 GCDSP 대화 채널. **같은 시리얼 포트를 동시에 겸용할 수 없다.**

### D11. 생존/상태 확인 (PING / STAT) — 멀티태스킹 OS는 실행 중에도
- `PING`(생존)과 `STAT`(상태: idle/busy/result) 명령을 프로토콜에
  둔다 (PLAN_01 §4). `gcds --ping <host>`, `gcds --stat <host>`로 노출.
- **멀티태스킹 OS의 데몬은 작업 실행 중에도 PING/STAT에 응답해야
  한다.** 이를 위해 실행 감시 루프가 listen 소켓을 함께 감시하여
  **제어 세션**(AUTH/PING/STAT/QUIT만 허용, 그 외 `ERR 409 busy`)을
  수락한다 — greeting에 `LIVE` capability로 표기 (PLAN_01 §4.1).
  이는 반복 단일 클라이언트 원칙(D1)의 의도된 예외다.
- LIVE는 자식을 비동기로 감시하는 실행 구조를 전제로 한다. 지원:
  POSIX/Win32/BeOS(감시 대상이 전부 소켓이라 R5 select 제약에 안
  걸림)/NeXTSTEP(실기 확정).
- **단일 태스킹 OS(MS-DOS)만 예외**: 실행 중 어떤 요청에도 응답
  불가 → 무응답은 busy/dead를 구분하지 못하며, 판별은 실행 종료 후
  재접속 폴링으로 한다 (ASYNC 흐름이 이 전제로 동작).
- 클라이언트 보고 규칙: 무응답 시 해당 호스트가 LIVE면 "다운",
  아니면 "busy 또는 다운"으로 표시.

## 3. 사용 시나리오 (목표 UX)

```sh
# Linux에서:
$ gcds beos  "cd /boot/home/proj && make"
$ gcds win   "nmake /f Makefile.msvc"          # cwd는 매핑으로 자동 변환
$ gcds next  "cc -o hello hello.c && ./hello"
$ echo $?    # 원격 명령의 exit code가 그대로 전달됨

# 디버깅/로그 (PLAN_04):
$ gcds win   "cdb -c 'g; kv; q' -z crash.dmp"  # 배치 디버거 — 일반 RUN
$ gcds -i macos "lldb ./prog"                  # 대화형(RUNI) — 키 입력 전달,
                                              #   Ctrl-C 는 K 프레임(원격 중단)
$ gcds -i linux2 "dmesg -w"                    # 커널 로그 스트림, Ctrl-C로 종료
$ gcds win @debug crash.dmp                    # 호스트별 도구 별칭(gcds.conf)
```

- 원격 stdout → 로컬 stdout, 원격 stderr → 로컬 stderr,
  원격 exit code → `gcds`의 exit code. 이 규칙 덕에 `gcds`를
  Makefile/스크립트 안에서 일반 명령처럼 조합할 수 있다.

## 4. 소스 트리

```
GrandCrossDevServer/
  README.md, PLAN_*.md
  include/gcdsp.h          프로토콜 상수/프레임 정의 (공용)
  common/                 양쪽 공용: 채널 추상화, 라인 I/O, conf 파서
    chan.c chan.h         전송 채널 추상화 (TCP/시리얼 공통 인터페이스)
    net.c net.h           TCP 백엔드 (BSD/Winsock 흡수)
    ser_psx.c ser_w32.c   시리얼 백엔드 (POSIX termios / Win32 COM)
    ser_next.c            시리얼 백엔드 (NeXTSTEP sgtty ioctl)
    ser_dos.c             시리얼 백엔드 (DOS: FOSSIL)
    lineio.c lineio.h     라인/프레임 송수신 (채널 위에서 동작)
    conf.c conf.h         key=value 파서
  daemon/                 gcdsd
    main.c session.c exec_psx.c exec_w32.c exec_dos.c
    live.c                POSIX 감시 루프 — RUN 스트리밍/RUNI/
                          비동기 JOB/제어 세션 통합
    live_w32.c            Win32 감시 루프 (select + PeekNamedPipe 폴링)
  client/                 gcds
    main.c remote.c pathmap.c asyncjob.c ixmode.c toolias.c xfer.c
  tools/                  보조 도구 (Linux 호스트)
    gcdslog.c              시리얼 콘솔 수신기 (Linux 전용, 규칙 제약 없음)
  make/                   플랫폼별 Makefile (autotools 미사용)
    Makefile.posix Makefile.win32 Makefile.beos Makefile.next
    Makefile.dos (Open Watcom wmake 문법)
```
