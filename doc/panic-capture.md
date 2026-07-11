# 커널 패닉 캡처 — gcdslog

드라이버가 커널을 죽이면 원격의 gcdsd도 함께 죽으므로, **죽는 순간의
메시지는 대상의 시리얼 콘솔에서 호스트가 out-of-band로 받아야** 한다
(PLAN_04 §3). `gcdslog`가 그 역할을 한다 — 시리얼 포트를 열어 수신
바이트를 **줄 단위 타임스탬프**와 함께 stdout/로그 파일에 기록하는
Linux 호스트 전용 도구다. GCDSP와 연동하지 않는 단순 로거다.

호스트가 항상 Linux이므로 이 도구는 프로젝트의 C89/보수적 socket 규칙
적용 대상이 아니다(termios 자유 사용).

## 빌드 · 사용

```sh
make -f make/Makefile.posix gcdslog      # (또는 all 에 포함)

./gcdslog [-b baud] [-o logfile] [-a] [-r] <device>
```
- `<device>` 대상 콘솔에 물린 시리얼 포트(예: USB-시리얼 `/dev/ttyUSB0`).
- `-b baud` 회선 속도(기본 115200 — 콘솔 표준).
- `-o file` 타임스탬프 줄을 이 파일에도 append.
- `-a` 개행 없이 멈춘(행) 출력도 유휴 시 방출 — 개행 없는 hang 대비.
- `-r` 재접속 안 함(포트가 닫히면 종료). 기본은 어댑터 재삽입 시 자동 재접속.
- Ctrl-C로 종료.

예:
```sh
./gcdslog -b 115200 -o /var/log/target-panic.log /dev/ttyUSB0
```
USB-시리얼 어댑터 여러 개로 Linux 호스트 한 대가 여러 대상을 동시 수신할 수 있다.

## 대상(원격) 쪽 설정 — OS별

콘솔 출력을 시리얼로 보내도록 대상을 설정한다. 호스트와는 널모뎀(또는
USB-시리얼) 케이블로 연결.

| 대상 | 설정 |
|------|------|
| **Linux** | 부트 파라미터 `console=ttyS0,115200`(GRUB `linux` 라인 또는 커널 cmdline). 패닉 메시지·oops·스택 트레이스가 ttyS0로 나감. |
| **Windows** | 커널 디버그(KD) 시리얼: `bcdedit /debug on`, `bcdedit /dbgsettings serial debugport:1 baudrate:115200`. gcdslog는 **raw 바이트만** 캡처 — 심볼 해석은 WinDbg 몫. |
| **BeOS/Haiku** | 부트 옵션에서 serial debug output 활성화(부트 로더 메뉴의 시리얼 디버그). |
| **NeXTSTEP/OPENSTEP** | 시리얼 콘솔 지정(콘솔을 온보드 시리얼 포트로). |
| **MS-DOS** | 커널 개념이 없어 해당 없음(패닉 캡처 대상 아님). |

## GCDSP 시리얼 채널과의 구분

같은 시리얼 포트를 **GCDSP 대화(gcdsd)** 와 **콘솔 캡처(gcdslog)** 에
동시에 쓸 수 없다. 둘 다 필요하면 포트 2개를 쓰거나, 시리얼은 콘솔
캡처 전용으로 두고 GCDSP는 TCP로 운용한다.

## QEMU로 검증 (실기 없이)

실제 하드웨어 없이 gcdslog를 검증하려면 QEMU Linux 게스트의 콘솔을
시리얼(호스트 PTY)로 빼내고, 게스트에서 커널 패닉을 유발한다. 이
프로젝트는 이 방식으로 실제 패닉 캡처를 확인했다.

핵심 절차:
1. 게스트를 `-append "console=ttyS0,115200 sysrq_always_enabled=1"`,
   `-serial pty`(게스트 ttyS0 → 호스트 `/dev/pts/N`)로 부팅.
2. QEMU 출력의 `char device redirected to /dev/pts/N`에서 PTY 경로를 얻어
   `./gcdslog -b 115200 -o capture.log /dev/pts/N` 실행.
3. 게스트에서 패닉 유발: `echo c > /proc/sysrq-trigger`(sysrq crash) 또는
   PID1 종료.

캡처 결과 예(gcdslog 출력):
```
[2026-... ] [    3.455728] Kernel panic - not syncing: sysrq triggered crash
[2026-... ] [    3.456326] CPU: 0 PID: 1 Comm: init ... 6.6.x
[2026-... ] [    3.458213] Call Trace:
[2026-... ]  ... sysrq_handle_crash / __handle_sysrq / vfs_write ...
[2026-... ] ---[ end Kernel panic - not syncing: sysrq triggered crash ]---
```
Kernel panic 헤더부터 Call Trace·레지스터·`end Kernel panic` 마커까지
전부 줄 단위 타임스탬프와 함께 캡처된다. Windows/BeOS/NeXTSTEP 대상도
동일하게 raw 콘솔 바이트를 받아 기록한다.
