# PLAN_04 — 디버거 / 커널 로그 / 패닉 캡처 설계

원격 실행 대상은 컴파일러만이 아니라 각 OS의 디버거, 로그 도구 등
개발 도구 전반이다(README 전제 8). 이 문서는 디버깅 시나리오를
프로토콜의 실행 모드에 매핑하고, OS별 도구 차이와 원리적 한계를
정리한다.

## 1. 시나리오 → 실행 모드 매핑

| 시나리오 | 실행 모드 | 요구 capability |
|----------|-----------|-----------------|
| 배치 디버거 (스크립트화된 조사) | `RUN` | 없음 (전 플랫폼) |
| 사후(post-mortem) 덤프 분석 | `RUN` | 없음 |
| 대화형 디버거 REPL | `RUNI` | `INTERACTIVE` |
| 끝나지 않는 로그 스트림 (dmesg -w 등) | `RUNI` + `K`로 종료 | `INTERACTIVE` |
| 일회성 로그 덤프 | `RUN` | 없음 |
| DOS에서의 디버거 | `RUNA` + 입력 파일 리다이렉션 | (ASYNC) |
| 커널 패닉 / BSOD 메시지 | **gcdsd 범위 밖** → gcdslog (§3) | — |

원칙: **배치로 표현할 수 있으면 배치(RUN)가 우선이다.**
RUNI는 진짜로 실행 중 개입이 필요할 때만 쓴다. 배치는 전 플랫폼에서
동작하고, 재현 가능하며, Makefile/스크립트에 넣을 수 있다.

## 2. OS별 도구 예시 (도구 별칭의 기본값 후보)

| OS | 배치 디버거 호출 예 | 로그 수단 |
|----|--------------------|-----------|
| Linux/현대 Unix | `gdb -batch -ex run -ex bt ./prog` | `dmesg`, `dmesg -w`(RUNI) |
| macOS | `lldb -b -o run -o bt ./prog` | `log show`, `log stream`(RUNI) |
| Windows | `cdb -c "g; kv; q" prog.exe` / `-z crash.dmp` | `wevtutil qe System`, DebugView류(RUNI) |
| BeOS R5 | gdb 포팅판 배치 호출 | `tail /var/log/syslog` (배치) |
| NeXTSTEP | gdb (구버전) 배치 호출 | `/usr/adm/messages` 덤프 |
| MS-DOS | `debug < script.txt` (RUNA) | (해당 없음) |

- 정확한 명령은 각 머신의 설치 상태에 따라 다르다 — 프로토콜은
  명령행을 해석하지 않으므로 무엇이든 통과한다. 위 표는
  `gcds.conf` 도구 별칭(§4)의 기본값을 정할 때의 출발점이다.

## 3. 커널 패닉 캡처 — gcdslog (out-of-band)

구현됨: `tools/gcdslog.c`. 대상OS별 콘솔 설정·사용법·검증은
**doc/panic-capture.md**. QEMU Linux 게스트의 실제 커널 패닉을
시리얼 콘솔로 캡처해 검증 완료.

**gcdsd로는 원리적으로 불가능하다.** 드라이버가 커널을 잡으면
userland 데몬도 함께 죽는다. 역할 경계를 명시한다:

- **gcdsd 담당**: OS가 살아 있는 동안의 로그 수신
  (§1의 로그 스트림/덤프 시나리오).
- **gcdslog 담당**: OS가 죽는 순간의 메시지 — 시리얼 콘솔 경유.

gcdslog 개요:
- Linux 전용 독립 도구 (`tools/gcdslog.c`). 대상이 Linux뿐이므로
  C89/보수적 socket 규칙의 **적용 대상이 아니다** (termios 등 자유).
- 동작: 지정 시리얼 포트(`/dev/ttyUSB0` 등, 속도 설정 포함)를 열어
  수신 바이트를 타임스탬프와 함께 stdout/로그 파일에 기록.
  gcdsd/gcds와 프로토콜 연동 없음 — 단순한 병행 도구.
- 원격 머신 쪽 설정은 OS별 문서화 항목:
  - Linux 타깃: 부트 파라미터 `console=ttyS0,115200`
  - Windows: 커널 디버그(KD) 시리얼 설정 — gcdslog는 raw 캡처만,
    심볼 해석은 WinDbg 몫임을 명시
  - BeOS: 부트 옵션의 serial debug output
  - NeXTSTEP: 시리얼 콘솔 지정
- 레트로 머신 대부분이 시리얼 포트를 갖고 있어 이 경로가 현실적이다.
  USB-시리얼 어댑터 N개로 Linux 호스트 한 대가 여러 대상을 동시 수신.
- **GCDSP 시리얼 채널(PLAN_01 §1.1)과의 구분**: 같은 시리얼 포트를
  GCDSP 대화(gcdsd)와 콘솔 캡처(gcdslog)에 동시 사용할 수 없다.
  둘 다 필요하면 포트 2개(또는 시리얼은 콘솔 전용, GCDSP는 TCP)로
  구성한다.

## 4. 도구 별칭 (클라이언트 기능)

OS마다 디버거/도구 명령이 전부 다르다는 문제는 프로토콜이 아니라
클라이언트 설정으로 흡수한다:

```
# gcds.conf
host.win.tool.debug = cdb -c "g; kv; q" -z
host.mac.tool.debug = lldb -b -o run -o bt
host.next.tool.log  = cat /usr/adm/messages
```
```sh
$ gcds win  @debug crash.dmp    # → RUN cdb -c "g; kv; q" -z crash.dmp
$ gcds next @log                # → RUN cat /usr/adm/messages
```

- `@<name>`이 첫 인자일 때 해당 별칭의 명령행 접두로 치환하고
  나머지 인자를 뒤에 붙인다. 그 이상의 기능(변수 치환 등)은 두지
  않는다 — 복잡한 조합은 원격지의 배치/셸 스크립트로.

## 5. 대화형 모드의 한계 (명시적 수용 사항)

1. **동시성은 OS에 따라 확보되지 않는다.** INTERACTIVE는
   POSIX/Win32/Haiku/NeXTSTEP(실기 확정)에서 지원하고, 고전 BeOS
   R5(select가 소켓 전용)와 DOS(단일 태스킹)는 제외 (PLAN_02 §4).
   미지원 플랫폼의 폴백은 아래 3항.
2. **pty가 아니라 파이프다.** stdin이 tty가 아니면 동작을 바꾸는
   도구가 있다 — 프롬프트 미출력, 출력 블록 버퍼링(라인 단위로
   안 나옴) 등. 우회: 도구별 배치 플래그(gdb `-batch`, lldb `-b`),
   Linux면 `stdbuf -oL`, 그리고 근본적으로는 배치 우선 원칙(§1).
   pty 채택은 이식성 비용 때문에 하지 않는다 (PLAN_00 D8).
3. **범용 폴백 = 입력 파일 리다이렉션.** `<`는 COMMAND.COM을 포함해
   모든 대상 셸이 지원한다. 대화형 도구에 미리 작성한 명령
   스크립트를 먹이는 `RUN dbgtool < script.txt` 패턴은 INTERACTIVE
   없이 전 플랫폼에서 동작한다. (스크립트 파일은 공유 스토리지로
   전달)
4. **Win32 RUNI는 폴링 하이브리드**라 50~100ms 수준의 출력 지연이
   있다 (PLAN_02 §4). 대화형 사용성에는 충분하다.

## 6. gcds 클라이언트의 대화형 UX (ixmode.c)

- `gcds -i <host> "<명령>"` → RUNI 사용.
- 로컬 stdin(키 입력) → `I` 프레임, 로컬 **Ctrl-C → `K` 프레임**
  (원격 자식 중단. gcds 자신은 X 수신까지 대기 후 정상 종료).
  두 번째 Ctrl-C는 gcds 자체를 끊는다(비정상 종료 — 데몬은 연결
  단절 시 자식 정리 규정으로 복구, PLAN_01 §4 RUN).
- 로컬 stdin의 EOF(Ctrl-D) → `I 0` (원격 stdin 닫기).
- 클라이언트는 select로 {로컬 stdin, 소켓}을 감시 (Linux 전용이므로
  제약 없음).
