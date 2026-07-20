# GrandCrossDevServer

Linux 호스트에서 이기종 원격 머신(Windows, macOS, BeOS, NeXTSTEP 등)에
**네이티브 컴파일을 지시하고 결과를 받아보는** 경량 TCP 서버 데몬 / 클라이언트.

## 프로젝트 전제

이 프로젝트의 모든 설계는 아래 전제 위에서 이루어진다.

1. **컴파일은 원격지에서, 원격지의 네이티브 컴파일러로 수행된다.**
   - Windows의 MSVC, macOS의 LLVM/clang, BeOS의 gcc 2.9x,
     NeXTSTEP의 구버전 gcc, MS-DOS의 Open Watcom 등.
   - distcc류의 분산/크로스 컴파일 방식은 목적에 맞지 않는다.
2. **Linux는 지휘소 역할만 한다.**
   - Linux에서 각 원격 환경에 컴파일을 지시하고,
     출력(stdout/stderr)과 종료 코드를 받아본다.
3. **소스와 산출물 공유는 기존 네트워크 파일시스템에 위임한다.**
   - Samba/NFS/AFP/FTP 등 각 OS가 이미 지원하는 수단을 사용한다.
   - 본 데몬은 명령 실행 채널이며, 파일 전송은 선택적 확장 기능이다.
4. **ssh/telnet이 없는 원격 시스템이 존재한다.**
   - 따라서 데몬은 TCP만 지원되면 동작해야 하며,
     외부 라이브러리·런타임(OpenSSL, Python, JVM 등)에 의존하지 않는다.
   - **네트워크(TCP/IP) 자체가 없는 머신은 시리얼(RS-232,
     9600bps 수준)로 연결한다.** 프로토콜은 바이트 스트림 위에서
     동작하므로 TCP와 시리얼은 같은 명세를 공유한다 (전제 9).
5. **internal network(신뢰 가능한 내부망) 전용이다.**
   - 암호화(TLS)는 구세대 OS에서 불가능하므로 채택하지 않는다.
   - 인증은 공유 토큰 수준으로 충분하다.
   - 외부망 노출은 지원 범위가 아니다.
6. **C로 제작한다. 이식성이 최우선이다.**
   - 엄격한 C89(ANSI C). gcc 2.x 세대 컴파일러와 Open Watcom(16bit)이
     컴파일할 수 있어야 한다.
   - socket은 고전 BSD socket API + Winsock 1.1 + Watt-32 호환 계층의
     교집합만 보수적으로 사용한다.
     (IPv4 전용, blocking I/O, `select()` 최소화)
   - 스레드를 사용하지 않는다.
7. **MS-DOS(TCP/IP 가능 환경)도 지원 대상이며, DOS에 한해
   비동기 작업 모델을 감수한다.**
   - DOS는 단일 태스킹이라 컴파일 실행 중 데몬이 네트워크에 응답할 수
     없다. 따라서 DOS 데몬은 작업을 접수하면 연결을 닫고(`RUNA`),
     완료 후 재접속한 클라이언트에 결과를 전달한다(`RESULT`).
   - 클라이언트(`gcds`)가 접수→폴링→결과 수신을 자동 처리하므로
     사용자 관점의 UX는 동기 호스트와 동일하다.
   - 툴체인: Open Watcom 16비트 + Watt-32(BSD socket 호환 계층),
     패킷 드라이버 기반.
8. **원격 실행 대상은 컴파일러만이 아니다 — 디버거, 커널 로그 등
   OS별 개발 도구 전반이다.**
   - 배치형 도구(gdb -batch, cdb -c, dmesg 등)는 기본 실행 모델로
     그대로 커버한다.
   - 대화형 도구와 끝나지 않는 로그 스트림(dmesg -w 등)은
     **대화형 실행 모드(RUNI + stdin/중단 프레임)** 로 지원하되,
     이는 소켓/자식 다중 감시가 가능한 OS에서만 제공한다
     (`INTERACTIVE` capability로 협상. 모든 OS에서 수신 동시성이
     확보될 수 없음을 전제로 수용한다).
   - 커널 패닉/BSOD 캡처는 userland 데몬의 원리적 한계 밖이므로,
     시리얼 콘솔 수신 보조 도구(`gcdslog`)를 별도 경로로 둔다.
9. **전송 채널은 TCP와 시리얼 두 가지다.**
   - GCDSP는 신뢰 가능한 양방향 바이트 스트림만 가정한다.
     TCP가 기본이고, 네트워크가 없는 머신은 시리얼(기본 9600 8N1)로
     같은 프로토콜을 사용한다 (세션 경계 등 시리얼 특칙은
     PLAN_01 §1.1).
   - 데몬의 생존(PING)/상태(STAT) 확인을 프로토콜에 포함한다.
     **멀티태스킹 OS에서는 작업 실행 중에도 응답해야 한다**
     (`LIVE` capability — 실행 중 제어 세션 수락, PLAN_01 §4.1).
     멀티태스킹이 안 되는 OS(MS-DOS)만 예외이며, 그 경우에 한해
     실행 중 무응답(busy)과 다운(dead)을 구분할 수 없는 한계를
     수용한다.

## 구성 요소

| 이름 | 역할 | 실행 위치 |
|------|------|-----------|
| `gcdsd` | 명령 실행 데몬 | 원격 머신 (Windows/macOS/BeOS/NeXTSTEP/DOS/...) |
| `gcds`  | 지시/수신 클라이언트 | Linux (호스트) |
| `gnfsd` | 간이 NFSv2 서버 — 공유 스토리지(선택, nfsd/) | Linux (호스트) |
| `gcdslog` | 시리얼 콘솔 커널 패닉 캡처(선택, tools/) | Linux (호스트) |

## 플랫폼 지원 현황

| 플랫폼 | 빌드 | 검증 | 채널 | 비고 |
|--------|------|------|------|------|
| Linux/POSIX | Makefile.posix | ✅ 루프백 | TCP·시리얼 | 기준 플랫폼, LIVE·RUNI |
| macOS | Makefile.posix | ✅ 실기(SSH) | TCP·시리얼 | clang, sshfs 워크플로 |
| Windows | Makefile.win32(nmake)/mgw | ✅ 실기(MSVC)·wine | TCP·COM | Winsock 1.1, Job Object |
| MS-DOS(시리얼) | Makefile.dos | ✅ DOSBox-X | 시리얼 | FOSSIL/int14, 비동기 |
| MS-DOS(TCP) | Makefile.dtcp | ✅ DOSBox-X | TCP | Watt-32, 패킷 드라이버 |
| BeOS/Haiku | Makefile.posix `LIBS=-lnetwork` | ✅ 실기(Haiku, SSH) | TCP·시리얼 | gcc 2.95, BeOS 세대 |
| NeXTSTEP/OPENSTEP | Makefile.next | ✅ 실기(OPENSTEP, telnet) | TCP·시리얼 | NeXT cc 2.7, LIVE·RUNI·sgtty |

- macOS·Haiku·NeXTSTEP·Windows는 실기, MS-DOS는 DOSBox-X로 검증됐다.
  각 플랫폼 상세는 `doc/*.md`.

## 시작하기

실사용 전반(설치·설정·명령 실행·대화형·파일전송·시리얼·공유 스토리지·
문제 해결)은 **[HOWTOUSE.md](HOWTOUSE.md)** 에 있다. 요점만:

```sh
make -f make/Makefile.posix     # gcds(호스트) + gcdsd(데몬) 빌드
cp etc/gcdsd.cnf gcdsd.cnf      # 원격: token 수정 후  ./gcdsd
cp etc/gcds.cnf ~/.gcds.cnf     # 호스트: 별칭/토큰 수정
./gcds local "gcc hello.c && ./a.out"   # 원격 실행, exit code 그대로
```

각 원격 머신에 **복사만 하면 바로 쓰는** 데몬 바이너리는 플랫폼별로
[`dist/`](dist/README.md)에 정리돼 있다(`dist/<platform>/` 복사 →
`gcdsd.cnf`의 `token` 수정 → 실행).

## 알려진 한계 (v1)

- **데몬 보안은 공유 토큰 + 선택적 `allow` IP 목록 + `maxout`
  출력 상한.** 내부망 전용 전제. RUN 실행 시간 타임아웃은 없음
  (필요 시 명령 자체에 timeout 래핑).
- **시리얼 오류 검출(CRC) 없음** — 단거리 케이블 전제.
- **멀티태스킹 OS도 한 번에 한 클라이언트** — 병렬 빌드는
  "머신 여러 대 × 각 1세션"으로.
- 상세·우선순위는 [PLAN_03_ROADMAP.md](PLAN_03_ROADMAP.md).

## 문서

- **[HOWTOUSE.md](HOWTOUSE.md) — 사용법 전반(설치·설정·실행·문제 해결)**
- [PLAN_00_OVERVIEW.md](PLAN_00_OVERVIEW.md) — 아키텍처 개요와 핵심 설계 결정
- [PLAN_01_PROTOCOL.md](PLAN_01_PROTOCOL.md) — 유선 프로토콜(GCDSP) 명세
- [PLAN_02_PORTABILITY.md](PLAN_02_PORTABILITY.md) — C89 / socket 이식성 규칙, OS별 노트
- [PLAN_03_ROADMAP.md](PLAN_03_ROADMAP.md) — 단계별 구현 로드맵
- [PLAN_04_DEBUG.md](PLAN_04_DEBUG.md) — 디버거/커널 로그/패닉 캡처
  시나리오 설계
- [PLAN_05_PATHS.md](PLAN_05_PATHS.md) — OS별 경로 처리 정책
  (상대경로 원칙, 매핑 규칙, OS별 제약 표)
- [PLAN_06_CONSOLIDATION.md](PLAN_06_CONSOLIDATION.md) — 중간정리 &
  바이너리 재분류(dist/) 완료 기록
- [doc/win32.md](doc/win32.md) — Win32 데몬 빌드/제약/검증
- [doc/dos.md](doc/dos.md) — MS-DOS 시리얼/TCP(Watt-32) 빌드·검증
- [doc/macos.md](doc/macos.md) — macOS 빌드/sshfs 공유 스토리지 워크플로
- [doc/beos.md](doc/beos.md) — BeOS/Haiku 빌드(gcc 2.95)·검증·R5 차이
- [doc/next.md](doc/next.md) — NeXTSTEP/OPENSTEP 이식(gnext.h)·검증
- [doc/toolchain.md](doc/toolchain.md) — win32/dos 크로스 툴체인
  (llvm-mingw / Open Watcom / Watt-32) 설치·사용
- [doc/panic-capture.md](doc/panic-capture.md) — gcdslog 시리얼 콘솔
  커널 패닉 캡처(대상OS별 설정, QEMU 검증)
- [test/README.md](test/README.md) — 회귀 테스트 (`test/run.sh`)
- [nfsd/README.md](nfsd/README.md) — 간이 NFSv2 서버(gnfsd, 공유
  스토리지 계층)

테스트는 `test/run.sh`(빌드 + POSIX 루프백/시리얼 회귀). 실행 방법은
[HOWTOUSE.md](HOWTOUSE.md) §12.
