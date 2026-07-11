# dist/ — 플랫폼별 배포 바이너리 (복사→실행)

각 원격 머신에 **해당 디렉터리 내용을 복사만 하면 바로 쓰는** 데몬
바이너리 모음. 소스는 저장소 루트, 빌드·검증 상세는 `doc/*.md`,
전체 계획은 [PLAN_06](../PLAN_06_CONSOLIDATION.md).

## 쓰는 법 (공통 3단계)
1. 대상 OS의 디렉터리(`dist/<platform>/`)를 원격 머신으로 복사.
2. `gcdsd.cnf` → `gcdsd.conf`로 복사하고 `token`을 바꾼다(내부망 전용,
   README 전제 5). 필요 시 `port`/`serial`/`async`도 조정.
3. 데몬 실행 → Linux 호스트에서 `gcds <alias> "<명령>"`.

호스트 클라이언트(`gcds`)와 로컬 데몬·NFS 서버는 `dist/linux/`에 있다.

## 매니페스트 (6/6 ✅)

| 플랫폼 | 바이너리 | 채널 | 빌드 출처(툴체인) | 수집 | 비고 |
|--------|----------|------|-------------------|:----:|------|
| linux  | gcds, gcdsd, gnfsd, gcdslog | TCP·시리얼 | 네이티브 gcc | ✅ | 호스트 클라이언트 + 로컬 데몬/NFS + 패닉 캡처 (ELF x86-64) |
| next   | gcdsd (+next-mount.csh) | TCP·시리얼 | 네이티브 NeXT cc 2.7 | ✅ | OPENSTEP, Mach-O i486. sgtty 시리얼 |
| macos  | gcdsd | TCP·시리얼 | 네이티브 clang | ✅ | Mach-O x86_64 (Darwin) |
| haiku  | gcdsd | TCP·시리얼 | 네이티브 gcc 2.95.3 | ✅ | BeOS=Haiku, ELF i386 (hrev53755) |
| win32  | gcdsd.exe | TCP·COM | 크로스 llvm-mingw(i686) | ✅ | PE32 i386. wine 기동 확인(LIVE INTERACTIVE) |
| dos    | gcdsd-serial.exe, gcdsd-tcp.exe | 시리얼 / TCP | 크로스 Open Watcom 16bit | ✅ | 비동기(ASYNC). serial=FOSSIL, tcp=Watt-32 |

✅ 6/6 수집 완료.

### win32/dos 크로스툴체인
win32/dos는 Linux 호스트에서 크로스컴파일한다(wine/DOSBox는 실행
검증용). 툴체인은 프로젝트 `toolchain/`에 편입돼 있고 설치·사용은
[doc/toolchain.md](../doc/toolchain.md) 참조. 요약:
```sh
./toolchain/setup.sh            # 최초 1회 (아카이브에서 배치)
./dist/harvest.sh win32 dos     # env.sh를 자동 인식해 빌드→dist 회수
```

> 바이너리 출처 구분: **네이티브**(next/macos/haiku)는 대상 OS에서
> 직접 컴파일, **크로스**(win32/dos)는 Linux 호스트에서 크로스컴파일.
> macOS 바이너리는 빌드한 macOS 버전·아키에 종속된다.

## 재수집 (harvest)
`./dist/harvest.sh [platform ...]` — 각 플랫폼을 빌드해 이 트리로
회수한다. 툴체인/대상 머신이 없으면 해당 플랫폼만 `SKIP`하고 나머지는
계속한다. 셋업에 따라 **Haiku와 NeXTSTEP이 동일 물리머신**일 수 있고
(한 번에 한 OS만 부팅) — 한쪽 수집 후 재부팅해 다른 쪽을 수집한다.
