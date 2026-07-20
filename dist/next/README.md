# dist/next — NeXTSTEP / OPENSTEP 데몬

1. `gcdsd`, `gcdsd.cnf`, `next-mount.csh`를 OPENSTEP으로 복사.
2. `gcdsd.cnf`의 `token` 수정(개명 불필요).
3. 실행: `./gcdsd` (TCP). 시리얼은 `gcdsd.cnf`에 `serial = /dev/ttya:9600`.

- LIVE/RUNI/K + sgtty 시리얼 지원(Linux/macOS와 동등). greeting: `... LIVE INTERACTIVE`.
- `next-mount.csh`: Linux gnfsd 공유를 OPENSTEP에 NFS mount하는 csh 헬퍼(소스 전달용). 상세 doc/next.md.
- 빌드 출처: 네이티브 NeXT cc 2.7.2.1(`make -f make/Makefile.next`).
