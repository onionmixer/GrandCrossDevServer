# PLAN_06 — 중간정리 & 바이너리 재분류 (완료 기록)

프로젝트를 한 번 정리하고, 각 플랫폼 데몬을 **복사만 하면 바로 쓰도록**
바이너리를 재분류한 결과. 이 문서는 그 결론만 남긴다.

## 결정 · 결과

**문서**: README를 **단일 진실 소스(SSOT)** 로 삼는다 — 플랫폼 현황표는
README 한 곳, 실사용은 HOWTOUSE.md, 빌드·검증 재현은 doc/\*.md, 이력은
git log. PLAN_00~05는 설계·프로토콜·이식성 규칙을 현행 상태로 유지한다.

**dist/ 배포 바이너리 (git 추적, 복사→실행)**: 6/6 수집 완료.
- 네이티브: linux(gcds+gcdsd+gnfsd), macos, haiku(gcc 2.95.3),
  next(NeXT cc 2.7, Mach-O)
- 크로스(Linux 호스트): win32(llvm-mingw, PE32), dos(Open Watcom;
  gcdsd-serial.exe + gcdsd-tcp.exe)
- `dist/harvest.sh`가 각 플랫폼을 빌드·회수한다(툴체인/대상 머신이
  없으면 SKIP). 매니페스트는 dist/README.md.

**크로스 툴체인 편입**: win32/dos용 llvm-mingw · Open Watcom · Watt-32를
`toolchain/`에 두고(payload는 gitignore, ~1.3GB), 재현용 `setup.sh`·
`env.sh`만 추적한다. 사용법은 doc/toolchain.md. win32/dos는 Linux에서
크로스컴파일하며 wine/DOSBox는 실행 검증용이다.

## 잔여 (PLAN_03 로드맵과 동일)
- Windows MSVC 실기 빌드, MS-DOS 실기(FOSSIL+널모뎀) 재확인,
  gdb/lldb 실사용 버퍼링 실측. (실물 하드웨어 필요.)
