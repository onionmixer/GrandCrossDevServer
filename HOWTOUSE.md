# GrandCrossDevServer 사용법 (HOWTOUSE)

Linux 호스트에서 이기종 원격 머신에 **네이티브 컴파일·실행을 지시하고
결과를 받는** 실사용 안내서. 프로젝트 개요·전제·설계는
[README.md](README.md)를, 프로토콜/이식성 등 설계 상세는 `PLAN_*.md`를
본다.

- [1. 두 프로그램](#1-두-프로그램)
- [2. 5분 시작 (로컬 루프백)](#2-5분-시작-로컬-루프백)
- [3. 원격 데몬 올리기](#3-원격-데몬-올리기)
- [4. 설정 레퍼런스](#4-설정-레퍼런스)
- [5. 명령 실행](#5-명령-실행)
- [6. 상태 확인 · 비동기](#6-상태-확인--비동기)
- [7. 대화형 실행 (디버거/로그)](#7-대화형-실행-디버거로그)
- [8. 파일 전송 (PUT/GET)](#8-파일-전송-putget)
- [9. 경로 매핑 · 도구 별칭 · 출력 역변환](#9-경로-매핑--도구-별칭--출력-역변환)
- [10. 시리얼 연결](#10-시리얼-연결)
- [11. 공유 스토리지 (gnfsd / sshfs)](#11-공유-스토리지-gnfsd--sshfs)
- [12. 직접 빌드 (네이티브 · 크로스)](#12-직접-빌드-네이티브--크로스)
- [13. 문제 해결](#13-문제-해결)

---

## 1. 두 프로그램

| 프로그램 | 실행 위치 | 역할 |
|----------|-----------|------|
| `gcds`  | Linux 호스트(지휘소) | 원격에 명령을 지시하고 stdout/stderr/exit code를 받는다 |
| `gcdsd` | 원격 머신 | 명령을 실제로 실행하는 데몬 |

원격 stdout→로컬 stdout, 원격 stderr→로컬 stderr, 원격 exit code→
`gcds`의 exit code. 그래서 `gcds`를 Makefile·스크립트 안에서 일반
명령처럼 조합할 수 있다.

## 2. 5분 시작 (로컬 루프백)

한 대의 Linux에서 데몬·클라이언트를 동시에 띄워 감을 잡는다.

```sh
# 1) 빌드
make -f make/Makefile.posix            # gcds, gcdsd 생성

# 2) 데몬 설정 후 실행 (다른 터미널)
cp etc/gcdsd.cnf gcdsd.cnf             # token 을 원하는 값으로 수정
./gcdsd                                # "listening on port 9910"

# 3) 클라이언트 설정 (gcds.cnf의 host.local.* 사용)
cp etc/gcds.cnf ~/.gcds.cnf            # host.local.token 을 위와 동일하게

# 4) 원격(여기선 로컬) 실행
./gcds local "gcc hello.c && ./a.out"
echo $?                                # 원격 exit code가 그대로
./gcds --ping local                    # local: alive (...)
./gcds --stat local                    # local: idle
```

배포용 바이너리는 이미 `dist/`에 있으니, 빌드 없이 바로 원격에 올려도
된다(3장).

## 3. 원격 데몬 올리기

### 3-1. 배포본 복사 (권장, 빌드 불필요)
플랫폼별 데몬 바이너리가 [`dist/<platform>/`](dist/README.md)에 있다.

1. 대상 OS의 디렉터리를 원격 머신으로 복사
   (예: `dist/win32/` → Windows).
2. `gcdsd.cnf`의 `token`을 바꾼다(이름 그대로 로드됨 — 개명 불필요).
3. 데몬 실행.

| 대상 | 복사할 것 | 실행 |
|------|-----------|------|
| Linux | `dist/linux/gcdsd` | `./gcdsd` |
| macOS | `dist/macos/gcdsd` | `./gcdsd` |
| Haiku/BeOS | `dist/haiku/gcdsd` | `./gcdsd` |
| NeXTSTEP | `dist/next/gcdsd` | `./gcdsd` |
| Windows | `dist/win32/gcdsd.exe` | `gcdsd.exe` |
| MS-DOS(시리얼) | `dist/dos/gcdsd-serial.exe` | `gcdsd-serial.exe` |
| MS-DOS(TCP) | `dist/dos/gcdsd-tcp.exe` | `gcdsd-tcp.exe` |

### 3-2. 플랫폼별 실행 요령
- **Windows/MSVC**: 컴파일을 시키려면 데몬을 vcvars 적용 환경에서 띄우거나,
  명령에 `vcvars32.bat && cl ...`처럼 접두한다(doc/win32.md). COM
  시리얼도 지원(`serial = COM1:9600`).
- **MS-DOS**: 단일 태스킹이라 **비동기 필수** — `gcdsd.cnf`에
  `async = 1`(배포 템플릿은 기본 1). 시리얼판은 **FOSSIL 드라이버**
  (X00/BNU) 상주가 필요하고, TCP판은 **패킷 드라이버 + `WATTCP.CFG`**
  (my_ip/netmask/gateway)가 필요하다(doc/dos.md).
- **NeXTSTEP/OPENSTEP**: ssh가 없으면 소스·바이너리를 자작 NFS(gnfsd)로
  전달한다(11장, `dist/next/next-mount.csh`). LIVE/RUNI/sgtty 시리얼
  지원.
- **공통**: 특권 포트가 필요 없다(기본 9910). 방화벽에서 해당 포트를
  내부망에 연다.

## 4. 설정 레퍼런스

### 4-1. 데몬 설정 `gcdsd.cnf` (템플릿 `etc/gcdsd.cnf`)
```ini
port   = 9910                 # TCP 포트 (기본 9910)
token  = changeme             # 공유 시크릿(필수, 내부망 전용)
tmpdir = /tmp                 # RUN 출력 캡처 임시파일 위치
                              # (Windows/DOS 기본값은 `.`; 경로는 각
                              #  OS 네이티브 구분자 `\`로 결합된다)
async  = 0                    # 1이면 ASYNC 광고(DOS 필수)
#serial = /dev/ttyS0:9600     # 있으면 TCP 대신 시리얼 모드
#allow  = 192.168.1.0/24 10.0.0.5   # TCP 접속 허용 IP/CIDR(비면 전체 허용)
#maxout = 16777216            # 작업당 출력 상한(byte, 0=무제한)
```
실행: `gcdsd` — 같은 폴더에서 `gcdsd.cnf` → `gcdsd.conf` 순으로 자동
탐색한다(MS-DOS는 8.3이라 `.cnf`만). 다른 경로면 `gcdsd -c <파일>`.

### 4-2. 클라이언트 설정 `gcds.cnf` (템플릿 `etc/gcds.cnf`)
검색 순서: `$GCDS_CONF` → `./gcds.cnf` → `./gcds.conf` → `~/.gcds.cnf`
→ `~/.gcds.conf` (3자 `.cnf`를 먼저 찾는다).
```ini
# TCP 호스트
host.win.addr  = 192.168.0.20
host.win.port  = 9910
host.win.token = changeme
host.win.map.1 = /mnt/proj|Z:\proj      # 경로 매핑(9장)
host.win.tool.debug = cdb -c            # 도구 별칭(9장): gcds win @debug ...

# 시리얼 호스트(네트워크 없는 원격)
host.dosbox.serial = /dev/ttyUSB0:9600
host.dosbox.token  = changeme

# 로컬 루프백(테스트)
host.local.addr  = 127.0.0.1
host.local.port  = 9910
host.local.token = changeme
```

## 5. 명령 실행

```sh
gcds <host> <command...>
```
- `gcds beos "cd /boot/home/proj && make"`
- `gcds win "nmake /f Makefile.msvc"` — cwd는 경로 매핑으로 자동 변환
- `gcds next "cc -o hello hello.c && ./hello"`

명령은 원격 셸에서 실행된다. 종료 코드·stdout·stderr가 그대로 전달돼
스크립트에서 조합 가능:
```sh
gcds win "cl /nologo app.c" && gcds win "app.exe" || echo "빌드 실패 $?"
```

## 6. 상태 확인 · 비동기

```sh
gcds --ping <host>     # alive 여부
gcds --stat <host>     # idle / busy <jobid> / result <jobid>
gcds --async <host> "make"   # RUNA/RESULT 비동기 모델 강제
```
- **멀티태스킹 OS**(Linux/macOS/Win/Haiku/NeXT)는 데몬이 `LIVE`라 작업
  실행 중에도 `--ping`/`--stat`에 응답한다(그때 새 RUN 시도는
  `ERR 409 busy`).
- **MS-DOS**는 단일 태스킹 → 실행 중 무응답. 데몬이 `ASYNC`를 광고하면
  `gcds`가 접수→폴링→결과 수신을 자동 처리하므로 UX는 동기와 같다.
  `--async`로 강제도 가능.

## 7. 대화형 실행 (디버거/로그)

```sh
gcds -i <host> <command...>
```
- stdin이 원격 자식으로 전달되고, **Ctrl-C**는 원격 명령을 중단(K
  프레임, 프로세스 그룹 종료 → exit 143), **Ctrl-D**는 stdin EOF.
- 용도: 대화형 디버거(`gcds -i macos "lldb ./prog"`), 끝나지 않는 로그
  스트림(`gcds -i linux2 "dmesg -w"`).
- `INTERACTIVE` capability가 있는 데몬만 지원(Linux/macOS/Win/Haiku/
  NeXTSTEP). 미지원(고전 BeOS R5·DOS·시리얼)에서는 **배치 우선**으로
  폴백: `gcds host "gdb -batch -ex run -ex bt ./prog"` 또는
  `gcds host "debug < script.txt"`.

### 커널 패닉 캡처 (gcdslog)
커널이 죽으면 원격 gcdsd도 함께 죽으므로, 죽는 순간의 메시지는
대상의 **시리얼 콘솔**에서 별도로 받아야 한다. `gcdslog`(호스트 도구)가
시리얼 포트를 열어 콘솔 출력을 타임스탬프와 함께 로그한다:
```sh
./gcdslog -b 115200 -o panic.log /dev/ttyUSB0
```
대상은 콘솔을 시리얼로 보내도록 설정한다(Linux `console=ttyS0,115200`,
Windows KD, 등). 상세·QEMU 검증은 [doc/panic-capture.md](doc/panic-capture.md).

## 8. 파일 전송 (PUT/GET)

공유 파일시스템이 없는 원격지(대표적으로 DOS)에서 업로드→컴파일→
다운로드:
```sh
gcds --put dos hello.c 'C:\HELLO.C'      # 로컬→원격
gcds      dos "wcl hello.c"              # 원격 컴파일
gcds --get dos 'C:\HELLO.EXE' out.exe    # 원격→로컬
```
- 8bit clean(바이너리 안전). 크기 상한은 데몬 `maxout`.
- 공유 스토리지(11장)가 있으면 PUT/GET 없이 로컬에서 편집→원격
  컴파일이 더 편하다.

## 9. 경로 매핑 · 도구 별칭 · 출력 역변환

### 경로 매핑
로컬 경로를 원격 경로로 자동 변환(cwd 및 명령 인자). `gcds.cnf`:
```ini
host.win.map.1 = /mnt/proj|Z:\proj       # local|remote[|sep]
# 구분자는 3번째 필드, 없으면 remote에 '\'가 있으면 '\'.
# 고전 Mac 스타일: host.mac.map.1 = /mnt/mac|MacHD:proj|:
```
- 컴포넌트 경계 매칭, 최장 로컬 접두 우선. UNC가 아니라 **마운트된
  드라이브 문자**를 쓴다. 정책 상세는 PLAN_05.

### 도구 별칭
자주 쓰는 도구를 호스트별 별칭으로:
```ini
host.win.tool.debug = cdb -c
```
```sh
gcds win @debug 'g; kv; q' -z crash.dmp   # @debug → "cdb -c" 로 치환
```

### 출력 역변환 (`--mapback`)
원격이 뱉은 경로(`Z:\proj\a.c`)를 로컬 경로(`/mnt/proj/a.c`)로 되돌려
출력(텍스트 전용, 기본 꺼짐):
```sh
gcds --mapback win "cl app.c"            # MSVC 에러 라인의 경로를 로컬로
```

## 10. 시리얼 연결

네트워크가 없는 머신은 같은 프로토콜을 시리얼로 쓴다(기본 9600 8N1).

- **데몬**(원격): `gcdsd.cnf`에 `serial = <device>:<baud>`
  (예: DOS `serial = COM1:9600`, Unix `serial = /dev/ttyS0:9600`).
  DOS 시리얼은 FOSSIL 상주 필요.
- **클라이언트**(호스트): `host.<alias>.serial = /dev/ttyUSB0:9600`.
- 시리얼은 연결 개념이 없어 세션 경계를 `HELLO` 라인으로 잡는다.
  **LIVE/INTERACTIVE는 시리얼에서 미지원**(두 번째 레인이 없음) —
  시리얼 호스트는 배치/비동기로 쓴다.
- 로컬 테스트: `socat -d -d pty,raw,echo=0 pty,raw,echo=0`로 PTY 쌍을
  만들어 양단에 물릴 수 있다.

## 11. 공유 스토리지 (gnfsd / sshfs)

컴파일 자체는 gcds가 지시하지만, **소스·산출물 공유는 파일시스템에
위임**한다(전제 3).

### sshfs (ssh 되는 원격: macOS/Haiku 등)
로컬에서 원격 디렉터리를 마운트해 편집→원격 컴파일:
```sh
sshfs user@mac:/Users/me/proj ~/mnt/mac
# 편집은 ~/mnt/mac 에서, 컴파일은:
gcds mac "cd proj && clang app.c"
```

### gnfsd (ssh가 없는 원격: OPENSTEP 등)
이 프로젝트에 포함된 **자작 NFSv2 서버**. Linux 폴더를 레트로 클라이언트가
mount:
```sh
sudo ./nfsd/serve.sh <공유폴더>          # portmap 111 + nfs 2049
```
- OPENSTEP에서 마운트는 csh 헬퍼 `dist/next/next-mount.csh`(대상에
  복사해 실행): `/next-mount.csh` (mount), `/next-mount.csh -u`
  (umount). 상세 nfsd/README.md, doc/next.md.

## 12. 직접 빌드 (네이티브 · 크로스)

배포본(`dist/`) 대신 직접 빌드하려면:

```sh
make -f make/Makefile.posix              # Linux/macOS (gcds + gcdsd)
make -f make/Makefile.posix LIBS=-lnetwork   # BeOS/Haiku (대상에서)
make -f make/Makefile.next               # NeXTSTEP (대상에서, gcdsd만)
```
- **win32/dos는 Linux에서 크로스컴파일**한다(wine/DOSBox는 검증용).
  툴체인은 `toolchain/`에 편입돼 있다:
  ```sh
  ./toolchain/setup.sh                    # 최초 1회(아카이브에서 배치)
  source toolchain/env.sh
  make -f make/Makefile.mgw               # win32 → gcdsd.exe
  make -f make/Makefile.dos               # dos 시리얼
  make -f make/Makefile.dtcp              # dos TCP(Watt-32)
  ```
  자세한 건 [doc/toolchain.md](doc/toolchain.md).
- 배포 트리로 한 번에 회수: `./dist/harvest.sh [platform ...]`
  (원격/툴체인이 없는 플랫폼은 SKIP).

회귀 테스트:
```sh
test/run.sh                              # 빌드 + POSIX 루프백/시리얼 회귀
```

## 13. 문제 해결

| 증상 | 원인 / 대응 |
|------|-------------|
| `no response (busy or down)` | 데몬 미기동/포트 차단, 또는 LIVE 아닌 데몬이 작업 중. `--ping`으로 구분(LIVE면 무응답=다운) |
| `AUTH`/토큰 오류 | 양단 `token` 불일치. `gcdsd.cnf`와 `host.<alias>.token` 확인 |
| `does not support interactive` | 대상이 `INTERACTIVE` 미광고(DOS/시리얼/BeOS R5). 배치 폴백(7장) |
| DOS에서 큰 프로그램 실행 실패(errcode 8) | conventional memory 부족. 상주 최소화(doc/dos.md) |
| DOS 시리얼 무반응 | FOSSIL(X00/BNU) 미상주. TCP판은 패킷드라이버·`WATTCP.CFG` 확인 |
| Windows exit code가 이상 | cmd.exe errorlevel 특성. 255로 클램프됨(doc/win32.md) |
| 경로가 원격에서 안 맞음 | `host.<alias>.map.N` 매핑 확인(9장, PLAN_05). 상대경로 권장 |
| OPENSTEP mount 후 소스가 옛날 것 | NeXTSTEP NFS 속성 캐시. `/next-mount.csh` 재실행(umount→mount) |
| 시리얼 세션이 어긋남 | v1은 CRC 없음. `HELLO` 재동기로 복구. 케이블/노이즈 점검 |

플랫폼별 깊은 내용은 `doc/win32.md`, `doc/dos.md`, `doc/macos.md`,
`doc/beos.md`, `doc/next.md`, `doc/toolchain.md`.
