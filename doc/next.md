# NeXTSTEP / OPENSTEP 데몬 노트

4.3BSD 파생이라 POSIX(2001) 이전 세대. 데몬(gcdsd)만 빌드하며
클라이언트(gcds)는 Linux 호스트에서 돈다.

## 검증 환경

**OPENSTEP** (NeXT Mach), i386, **NeXT cc = gcc 2.7.2.1**.
프로젝트가 목표한 gcc 2.x 하한대의 실기 검증.

## 빌드

```
make -f make/Makefile.next          # ./gcdsd (데몬)
```
- `-DGCDS_NEXT` + `common/gnext.h` shim이 POSIX 이전 차이를 흡수.
- **LIVE/RUNI 포함**(`GCDS_HAS_LIVE/IX`) — Linux/macOS와 동등한
  기능: 스트리밍 RUN, LIVE 제어세션, RUNI 대화형, K(프로세스 그룹
  종료), 비동기, PUT·GET. NeXT의 select는 4.2BSD 네이티브라
  소켓+파이프 동시 감시가 되어 가능.
- **NeXT make는 GNU make가 아니다.** 단순 suffix 규칙만 쓴 이
  Makefile은 동작하나, 개발 중엔 cc를 직접 호출해 빌드하기도 했다.

## 4.3BSD 이식 이슈와 해결 (gnext.h)

| 증상 | 원인 | 해결 |
|------|------|------|
| `socklen_t` undefined | POSIX 타입 없음 | `typedef int socklen_t` |
| `pid_t` undefined | 〃 | `typedef int pid_t` |
| `sys/select.h` not found | 4.3BSD엔 없음 | `sys/time.h`(FD_SET은 types.h) |
| `w_S`/`w_T` member 오류 | W매크로가 `union wait` 기준 | int 기반 W매크로 재정의 |
| `O_NOCTTY` undeclared | fcntl.h에 없음 | `#ifndef → 0` (no-op) |
| `size_t` (illegal call) | termios.h가 안 끌어옴 | `sys/types.h` 명시 include |
| `waitpid` 없음 | 4.3BSD엔 wait4만 | `waitpid → wait4(...,union wait*,...)` |
| `setpgid` 없음 | 4.3BSD는 setpgrp | `setpgid(p,g) → setpgrp(p,g)` |

`setpgid`는 인자 의미가 같으나 0-기본이 이식적이지 않아, live.c는
자식에서 `setpgid(0, getpid())`로 명시 pid를 준다(K의 프로세스 그룹
종료가 4.3BSD에서도 동작).

## 텍스트 인코딩

**ASCII 전제**다. NeXTSTEP은 자체 8비트 인코딩을 쓰고 한글을 지원하지
않아 실사용 텍스트(cc 에러, `/usr/adm/messages`)가 사실상 ASCII이므로
변환하지 않는다(textcv 미링크). greeting에 `UTF8`을 표기하지 않는다.

## 시리얼: sgtty 백엔드 (ser_next.c)

NeXTSTEP은 `<termios.h>`는 있으나 `tcsetattr`/`cfsetispeed` 등
**함수가 libc에 없다**(링크 시 undefined) — 커널이 구 BSD sgtty
ioctl을 쓰기 때문. `ser_next.c`가 `TIOCGETP`/`TIOCSETP` +
`struct sgttyb`(RAW 모드, XON/XOFF 없음)로 직접 구현한다. 속도
상수(B9600 등)는 4.2BSD 이래 고정값이라 헤더에 없으면 폴백.
장치: `/dev/ttya`, `/dev/ttyb`(NeXT 온보드 시리얼).

## 검증 결과 (실기, Linux gcds ↔ OPENSTEP gcdsd)

전부 통과:
- greeting `GCDS 1 posix <host> LIVE INTERACTIVE`, ping/stat
- **네이티브 NeXT cc 원격 컴파일·실행**, exit code, stdout/stderr 분리
- **스트리밍 RUN**(완료 전 출력 도착), **LIVE 제어세션**(실행 중
  ping/`busy`), **RUNI**(stdin sort), **K→exit 143**(프로세스 그룹
  종료로 손자까지 정리)
- **PUT/GET 512KB 바이너리 무결성**, 비동기 RUNA/RESULT
- 업로드→원격 cc 컴파일→다운로드 전 과정

## 통합 방식: gnfsd로 소스 전달

OPENSTEP은 sshfs가 없어(telnet만), **자작 NFS 서버(gnfsd)** 공유에
소스를 넣고 OPENSTEP이 mount 후 빌드했다. 즉 이 프로젝트의 두
산출물(NFS 서버 + 원격 실행 데몬)이 맞물려 동작함을 실증.

마운트는 csh 스크립트 `nfsd/next-mount.csh`로 한다(NeXTSTEP 기본
셸이 csh). OPENSTEP의 root 로그인 디렉터리(`/`)에 설치해 두었다:

```
/next-mount.csh          # gnfsd 공유를 /nfstest에 mount
/next-mount.csh -n       # noac 추가(호스트 편집 즉시 반영)
/next-mount.csh -u       # 언마운트
/next-mount.csh <server> <export> <mountpoint>   # 기본값 재정의
```

### NFS 마운트 옵션 (중요)

스크립트가 쓰는 기본값이자, 손으로 마운트할 때 권장하는 조합:

```
mount -t nfs -o hard,intr,timeo=30,retrans=5 <서버IP>:<export> /nfstest
```

| 옵션 | 값 | 이유 |
|------|-----|------|
| `hard` | — | 타임아웃 시 **실패가 아니라 재시도**. `soft`면 응답 하나가 밀리는 순간 I/O 에러가 나고, 실패한 마운트가 남아 이후 재마운트가 `Device busy`로 거부된다 |
| `intr` | — | `hard`여도 Ctrl-C로 빠져나올 수 있게. 서버가 정말 죽었을 때의 탈출구 |
| `timeo` | `30` | **1/10초 단위**라 3.0초. 기본값 10(1초)은 바쁜 서버나 구형 NIC이 답하기 전에 포기한다 |
| `retrans` | `5` | 재전송 횟수 |
| `noac` | (선택) | 속성 캐시를 꺼서 호스트 편집이 즉시 보인다. 대신 GETATTR 요청이 몇 배로 늘어 빌드 중엔 불리하므로 **평소엔 빼고**, 소스를 고치는 중일 때만 `-n`으로 켠다 |

NeXTSTEP의 `mount`는 4.2BSD/SunOS 계열 옵션을 받는다. 위 조합은 실기에서
검증됐다.

- 주의: NeXTSTEP NFS 클라이언트는 파일 속성을 캐시한다. 호스트에서
  소스를 갱신했는데 안 보이면 `/next-mount.csh`를 다시 실행하면 된다
  (umount 후 재mount로 캐시를 비운다). 편집을 계속할 예정이면
  `/next-mount.csh -n`으로 `noac`를 켜 두는 편이 편하다.
- gnfsd를 재시작해도 **클라이언트를 재마운트할 필요는 없다** — 파일
  핸들이 재시작을 넘겨 유효하다(nfsd/README.md).
- `mkdir -p` 금지: NeXTSTEP mkdir엔 `-p`가 없어 `-p`라는 디렉터리를
  만들어버린다. 스크립트는 `if (! -d) mkdir`로 처리한다.

## 시리얼

sgtty 시리얼 백엔드(ser_next.c)는 NeXT 온보드 포트
(`/dev/ttya`,`/dev/ttyb`)로 동작한다. TCP 경로는 전 기능 실기 통과.
