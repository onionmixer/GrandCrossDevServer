# dist/haiku — BeOS / Haiku 데몬

본 프로젝트는 BeOS를 Haiku와 동일 취급한다.
1. `gcdsd`, `gcdsd.cnf`를 Haiku로 복사.
2. `gcdsd.cnf`의 `token` 수정(개명 불필요).
3. 실행: `./gcdsd`.

- LIVE/RUNI/K 지원(Haiku 소켓은 진짜 fd라 select 정상).
- 빌드 출처: 네이티브 gcc 2.95(`make -f make/Makefile.posix LIBS=-lnetwork`).
- 주의: NeXTSTEP과 같은 물리머신일 수 있음. 수집은 Haiku 부팅 상태에서.
