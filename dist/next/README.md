# dist/next — NeXTSTEP / OPENSTEP 데몬 (아키텍처별)

OPENSTEP/NeXTSTEP은 여러 CPU에서 돈다. 바이너리는 아키텍처별 하위
디렉터리에, 설정·헬퍼는 아키텍처 공통으로 둔다.

```
dist/next/
  gcdsd.cnf        공통 설정 (arch 무관)
  next-mount.csh   Linux gnfsd 공유를 OPENSTEP에 NFS mount (호스트 소스 전달용)
  i386/gcdsd       Intel OPENSTEP
  m68k/gcdsd       NeXT 흑箱(68040)   ← 빌드 예정(PLAN_07)
  sparc/gcdsd      Sun OPENSTEP        ← 빌드 예정(PLAN_07)
```

## 설치 (copy & go)

1. `gcdsd.cnf`, `next-mount.csh`, 그리고 **자기 아키텍처의 `<arch>/gcdsd`**
   를 OPENSTEP으로 복사.
   - 아키텍처 확인: 타깃에서 `arch`(예: `m68k`/`i386`/`sparc`), 또는
     접속 후 `gcds --ping`의 ostag(`openstep-<arch>`).
2. `gcdsd.cnf`의 `token` 수정(개명 불필요).
3. 실행: `./gcdsd` (TCP). 시리얼은 `gcdsd.cnf`에 `serial = /dev/ttya:9600`.

## 참고

- LIVE/RUNI/K + sgtty 시리얼 지원(Linux/macOS와 동등). greeting caps:
  `... LIVE INTERACTIVE`.
- greeting ostag가 아키텍처를 보고한다(`openstep-i386`/`-m68k`/`-sparc`).
  프로토콜은 텍스트 기반이라 엔디안 무관 — 동일 소스가 세 아키텍처에서
  동작한다(PLAN_07).
- `next-mount.csh`: Linux gnfsd 공유를 OPENSTEP에 NFS mount하는 csh 헬퍼.
  상세 `doc/next.md`.
- 빌드 출처: 각 아키텍처에서 네이티브 cc (`make -f make/Makefile.next`).
  i386은 gcc 2.7.2.1.
