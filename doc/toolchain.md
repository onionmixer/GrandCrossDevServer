# 크로스 툴체인 (win32 / dos 빌드)

Windows·MS-DOS 데몬은 **Linux 호스트에서 크로스컴파일**한다. wine과
DOSBox-X는 빌드가 아니라 **실행 검증용**일 뿐이다. 이 문서는 그
크로스 툴체인을 설치·사용하는 방법을 다룬다.

관련: 배포 바이너리는 [dist/](../dist/README.md), 전체 이식성 규칙은
[PLAN_02](../PLAN_02_PORTABILITY.md), DOS 함정은 [doc/dos.md](dos.md).

## 무엇이 필요한가

| 대상 | 툴체인 | 빌드 |
|------|--------|------|
| Windows (`gcdsd.exe`, PE32 i386) | **llvm-mingw** (i686, msvcrt) | `make -f make/Makefile.mgw` |
| MS-DOS 시리얼 (`gcdsd-serial.exe`) | **Open Watcom v2** (16bit large model) | `make -f make/Makefile.dos` |
| MS-DOS TCP (`gcdsd-tcp.exe`) | Open Watcom + **Watt-32** (16bit large lib) | `make -f make/Makefile.dtcp` |

네이티브 빌드(macOS clang / Haiku gcc 2.95 / NeXT cc)는 이 툴체인이
필요 없다 — 각 대상 OS에서 직접 컴파일한다.

## 위치와 git 정책

툴체인은 프로젝트의 **`toolchain/`** 에 둔다(약 1.3GB):

```
toolchain/
  setup.sh            # (git 추적) 아카이브에서 툴체인 배치/다운로드
  env.sh              # (git 추적) WATCOM/WATT/PATH 를 셸에 설정
  archives/           # (gitignore) 버전 고정 아카이브 — 재현의 원본
    open-watcom-v2-snapshot.tar.xz
    llvm-mingw-20260616-msvcrt-i686.tar.xz
    watt32.zip
  llvm-mingw/         # (gitignore) 추출본, bin/i686-w64-mingw32-gcc
  ow/                 # (gitignore) 추출본, binl64/wcl  ← WATCOM 루트
  watt32/             # (gitignore) 추출본, lib/wattcpwl.lib ← WATT 루트
```

**바이너리 페이로드는 git에 넣지 않는다**(용량). 추적하는 것은
`setup.sh`·`env.sh` 뿐이며, 툴체인은 `setup.sh`로 재현한다. 단
`archives/`의 아카이브는 **버전 고정 원본**이므로 이 머신에 보존해
두면 네트워크 없이 재배치할 수 있다(스냅샷 태그가 상류에서 바뀌어도
동일 버전 유지). `.gitignore` 참조.

## 사용법

### 1) 셋업 (최초 1회)
```sh
./toolchain/setup.sh            # mingw + Open Watcom + Watt-32 모두
# ./toolchain/setup.sh mingw    # 일부만 (win32)
# ./toolchain/setup.sh ow watt  # 일부만 (dos)
```
- `archives/`에 아카이브가 있으면 추출만 한다(빠름, 네트워크 불필요).
- 없으면 문서화된 상류 URL에서 내려받아 추출한다(best effort — Open
  Watcom 스냅샷 태그는 회전하므로 아카이브 보존을 권장).
- 이미 추출돼 있으면 건드리지 않는다(idempotent).

### 2) 빌드
```sh
source toolchain/env.sh         # WATCOM/WATT 설정 + PATH에 컴파일러 추가
make -f make/Makefile.mgw       # → gcdsd.exe (win32)
make -f make/Makefile.dos       # → gcdsd.exe (dos 시리얼)
make -f make/Makefile.dtcp      # → gcdsd.exe (dos TCP)
```

### 3) 배포본으로 수집
`dist/harvest.sh`는 `toolchain/env.sh`가 있으면 **자동으로 읽어들여**
win32/dos를 빌드해 `dist/`로 회수한다(`source` 불필요):
```sh
./dist/harvest.sh win32 dos     # dist/win32/, dist/dos/ 채움
```
harvest는 빌드 사이에 오브젝트·산출물을 정리해(mingw `.o` ↔ Watcom
`.obj` 교차 오염 방지) 각 바이너리 종류가 섞이지 않게 한다.

## 툴체인별 상세

### llvm-mingw (win32)
- i686 타깃, msvcrt 런타임(구세대 Windows 호환). 출처:
  <https://github.com/mstorsjo/llvm-mingw> 릴리스.
- `Makefile.mgw`는 `-ansi -pedantic` 없이 빌드(MinGW Windows 헤더가
  엄격 C89-clean이 아님). 우리 코드 경고는 0.
- 검증: `wine dist/win32/gcdsd.exe -c <conf>` → greeting
  `GCDS 1 win32 <host> LIVE INTERACTIVE`.

### Open Watcom v2 (dos)
- Linux 호스트판 스냅샷(`binl64/`에 리눅스 실행 wcc/wcl/wlib).
  출처: <https://github.com/open-watcom/open-watcom-v2> 릴리스
  (`ow-snapshot.tar.xz`). **스냅샷 태그가 회전**하므로 받은 아카이브를
  `archives/`에 고정 보존.
- `WATCOM` = `toolchain/ow`, `INCLUDE` = `$WATCOM/h`(env.sh가 설정).
- large 메모리 모델·8086 타깃. DGROUP 64K 제약 등 함정은 doc/dos.md.

### Watt-32 (dos TCP)
- 16bit large model 라이브러리(`lib/wattcpwl.lib`)가 필요하다.
  아카이브에는 **미리 빌드된 lib**가 포함돼 있다.
- 새로 빌드해야 하면(소스만 있는 경우) `make/build-watt32.sh
  toolchain/watt32 toolchain/ow` — Open Watcom + DOSBox-X 필요
  (스톡 large 설정이 BSD API를 빼므로 `USE_BSD_API` 강제 등 처리를
  이 스크립트가 한다). 상세 doc/dos.md.

## 새 머신에서 재현
1. `toolchain/archives/`의 아카이브를 함께 옮긴다(또는 setup.sh가
   상류에서 받게 둔다 — 단 Open Watcom은 버전 회전 주의).
2. `./toolchain/setup.sh` 실행 → 추출.
3. `source toolchain/env.sh` → 빌드/harvest.
