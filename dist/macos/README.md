# dist/macos — macOS 데몬

1. `gcdsd`, `gcdsd.cnf`를 macOS로 복사.
2. `cp gcdsd.cnf gcdsd.conf` 후 `token` 수정.
3. 실행: `./gcdsd -c gcdsd.conf`.

- LIVE/RUNI/K 전 기능 지원. 공유 스토리지는 sshfs 워크플로(doc/macos.md).
- 빌드 출처: 네이티브 clang(`make -f make/Makefile.posix`). **빌드한 macOS 버전·아키에 종속**.
