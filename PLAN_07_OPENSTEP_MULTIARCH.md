# PLAN_07 — OPENSTEP 다중 아키텍처 (i386 / m68k / sparc)

OPENSTEP/NeXTSTEP용 `gcdsd`를 현행 **i386** 외에 **m68k**(NeXT 흑箱,
68040)와 **sparc**로 확장하기 위한 작업 계획.

**사용자 결정**
- *mach 버전* = **m68k NeXT 흑箱 하드웨어**(Motorola 68040, 빅엔디안).
- 빌드 전략 = **아키텍처별 네이티브 빌드**(각 머신/에뮬에서 `cc`, fat
  바이너리 아님). *단 sparc는 하드웨어가 없어, .18(i386)의 NeXT
  다중아키텍처 툴체인으로 `cc -arch sparc` cross-build함(2026-09-15 발견).*

> 상태: **Phase 1(m68k) 완료(2026-09-14).** Phase 0(리팩터·os_tag·문서,
> 호스트 회귀 46/46) + **Phase 1: m68k 실기 빌드·검증 완료** — NeXTstation
> `nstationonion`(192.168.1.228, 68040)에서 gnfsd NFS로 소스 전달, 네이티브
> cc(gcc 2.7.2.1)로 빌드(Mach-O m68k), `gcds --ping`→`openstep-m68k`, 원격
> RUN·exit code·**네이티브 cc 컴파일 roundtrip**까지 통과. **엔디안-클린
> 전제가 빅엔디안 실기로 증명됨.** `dist/next/m68k/gcdsd` 수집. **i386 재빌드 완료(2026-09-15)** —
> `dist/next/i386/gcdsd`가 `openstep-i386` 보고(실기 .18). **sparc는 .18의
> NeXT 다중아키텍처 툴체인(`cc -arch sparc`)으로 cross-build 완료** —
> Mach-O SPARC(cputype 14)/`openstep-sparc`, `dist/next/sparc/gcdsd`.
> **단 sparc 실기 런타임 검증은 미완**(HW 없음; 엔디안 정합성은 m68k로 대리
> 입증). 남은 것: sparc 실기/에뮬 확보 시 런타임 검증.

## 0. 핵심 결론 (먼저)

이 작업은 **프로토콜·핵심 로직 변경이 아니다.** 조사로 확인된 세 사실:

1. **GCDSP는 텍스트 프로토콜 → 엔디안-클린.** 프레임 헤더가
   `sprintf(hdr, "%c %ld\n", tag, len)`(ASCII 십진수)이고 페이로드는 raw
   통과. 바이너리 정수 직렬화가 없다. → 빅엔디안 m68k·sparc에서 **프로토콜
   코드 수정 없이 동작**.
2. **`gnext.h` shim은 아키텍처 독립적.** socklen_t·`union wait`·wait4·
   setpgrp 등 4.3BSD/pre-POSIX libc 차이만 다룬다(CPU 무관). 세 아키텍처
   공통.
3. **셋 다 32비트**(`long`=32bit) → DOS식 16비트 함정 없음.

따라서 실제 범위는 **빌드·배포 매트릭스 + 아키텍처 식별 + 검증**뿐이다.

## 1. 범위 / 비범위

**범위**
- `dist/` 구조를 아키텍처별로 재편(§3).
- greeting `ostag`를 아키텍처별로 자동 표기(§4).
- 아키텍처별 네이티브 빌드 실행·검증(§6,§7).
- 엔디안 형식 감사 1회(§5).
- `harvest`·문서 갱신(§8,§9).

**비범위 (근거)**
- **프로토콜 변경** — 텍스트 기반이라 불필요(§0-1).
- **gnfsd / NFS** — Linux 호스트에서 돈다. 타깃 아키텍처와 무관.
- **클라이언트(gcds)** — 아키텍처 무관. `ostag`는 `--ping` 표시용으로만
  쓰고 값에 분기하지 않음(remote.c:70 저장, main.c:204 출력) → 변경 안전.
- **fat 바이너리(MAB)** — 사용자가 per-arch 네이티브를 선택.

## 2. 조사 근거 (코드 확인됨)

| 항목 | 확인 내용 | 위치 |
|------|-----------|------|
| 프레임 인코딩 | `"%c %ld\n"` 텍스트, 페이로드 raw | common/lineio.c:43 |
| 네트워크 바이너리 쓰기 | `sin_addr` memcpy뿐(이미 net order) | common/net.c:96,238 |
| BSD shim | arch 무관(타입·wait·pgrp) | common/gnext.h |
| 빌드 | 네이티브 `cc`, arch 파라미터 없음 | make/Makefile.next |
| ostag | 컴파일타임 `GCDS_OSTAG`(현 NeXT 빌드 미설정→"posix") | daemon/exec_psx.c:105 |
| 클라 ostag | 표시만, 분기 없음 | client/remote.c, client/main.c:204 |
| arch 매크로 | 코드에 기존 사용 없음(깨끗) | — |

## 3. dist 구조 변경 (핵심 "구조 변경")

현행: `dist/next/{gcdsd, gcdsd.cnf, next-mount.csh, README.md}` — i386
바이너리 1개, arch 무표기.

**제안(권장안 A):** 공유 설정 + 아키텍처별 바이너리 하위 디렉터리.
```
dist/next/
  README.md            (arch별 복사 안내)
  gcdsd.cnf            (공유 — arch 무관)
  next-mount.csh       (공유 — 호스트 NFS 마운트 헬퍼)
  i386/gcdsd
  m68k/gcdsd
  sparc/gcdsd
```
복사 규칙: `gcdsd.cnf`·`next-mount.csh` + 자기 아키텍처의 `<arch>/gcdsd`.

- **대안 B**: `dist/next/<arch>/`를 각각 자체완결(cnf/csh 중복, harvest가
  동기화). "디렉터리 하나 복사→실행" UX엔 더 맞으나 파일 중복.
- 타깃 아키텍처 판별: 타깃에서 `arch` 명령, 또는 연결 후 greeting `ostag`.
- **결정 필요**: A vs B. (권장 A — 설정 단일 소스, 중복 없음.)

## 4. 아키텍처 식별 (ostag 자동화)

`daemon/exec_psx.c`의 `os_tag()`를 **GCDS_NEXT 하에서 gcc 프리디파인드
매크로로 자동 판별**하도록 개선(Makefile 변경 불필요 — 네이티브 cc가
맞는 매크로를 정의):

```c
#ifdef GCDS_NEXT
#  if   defined(__m68k__)  return "openstep-m68k";
#  elif defined(__i386__)  return "openstep-i386";
#  elif defined(__sparc__) return "openstep-sparc";
#  elif defined(__hppa__)  return "openstep-hppa";
#  else                    return "openstep";
#  endif
#endif
```
- 각 네이티브 빌드가 자기 아키텍처를 자동 보고 → `gcds --ping`에 arch
  노출, 배포 라벨·진단에 활용.
- 클라 호환: ostag는 표시 전용이라 안전(§2). 다만 `--ping` 출력 변화만
  확인.
- gcc 2.x도 `__m68k__`/`__i386__`/`__sparc__`를 정의함(표준).

## 5. 엔디안 형식 감사 (1회)

텍스트 프레이밍이라 안전하나, 빅엔디안 타깃 추가 전에 **형식 감사**를 한
번 수행·문서화한다.
- 바이너리 정수 직렬화·`(uint32_t*)`류 캐스트·정수 memcpy·`union` 검색
  (현 조사상 없음 — 확정).
- conf 파서·acl(IP/CIDR)·lineio 숫자 파싱이 **문자열 파싱**(엔디안 무관)인지.
- **빅엔디안 실증**: m68k(Phase 1)에서 test 스모크. m68k가 통과하면 sparc의
  엔디안 축은 사실상 커버(둘 다 빅엔디안 32비트).

## 6. 아키텍처별 빌드

각 아키텍처에서 `make -f make/Makefile.next` **그대로**(네이티브 cc,
변경 없음). 흡수해야 할 차이:

| arch | 컴파일러(예상) | 확인 항목 |
|------|----------------|-----------|
| i386 | gcc 2.7.2.1 | 검증됨(현행) |
| m68k | 더 구버전 cc(gcc 2.5.x / NeXT cc 1.x 가능) | `-O`·헤더·gnext.h 커버리지 |
| sparc | OPENSTEP sparc cc | cc 버전·헤더 차이 |

- gnext.h가 세 곳 모두 빌드되는지 실빌드로 확인(예상: 커버 — BSD 공통).
- 빌드 실패 시 shim 보강은 `#ifdef GCDS_NEXT` 내 추가(arch 분기 최소화).

## 7. 검증 수단 (아키텍처별) — 리스크 포함

| arch | 검증 수단 | 상태 |
|------|-----------|------|
| i386 | 실기 `192.168.1.18` | ✓ 보유 |
| m68k | **Previous 에뮬레이터**(NeXT 68040 정밀 에뮬, NeXTSTEP/OPENSTEP 구동) | ROM·설치미디어 확보 필요 |
| sparc | **빌드: i386(.18) cross(`cc -arch sparc`) 완료**. 런타임: NeXTSTEP/OPENSTEP sparc 실기(에뮬 빈약) 필요 |

- m68k는 Previous로 현실적. sparc는 하드웨어 확보 전까지 **조건부 연기**하되,
  엔디안은 m68k로 입증되므로 sparc 고유(cc·디바이스·시리얼) 검증만 남김.

## 8. 툴 갱신

- `dist/harvest.sh`: next 수집을 arch별로. 타깃 arch 판별(`arch`/ostag) 후
  `dist/next/<arch>/`에 배치. 아키텍처 혼입 방지(_clean_tree 유지 — win32/dos
  혼입 사건의 교훈).
- `serve.sh`·mount 툴: 호스트측이라 무관.
- `gcdsd.cnf` 템플릿: arch 공통(단일 유지).

## 9. 문서

- `doc/next.md`: "i386" 단일 서술 → **아키텍처 표**(i386/m68k/sparc: cc 버전·
  검증 상태·에뮬 안내·엔디안 메모).
- `dist/next/README.md`: arch별 복사 안내.
- `README.md`(SSOT) 플랫폼 표: NeXTSTEP 행을 arch로 세분.
- `PLAN_03_ROADMAP.md`: 다중 아키텍처 항목 반영.
- `HANDOFF.md`: 아키텍처별 빌드·검증 현황.

## 10. 단계별 진행 (순서·의존성)

- **Phase 0 — 동작 불변 리팩터 (하드웨어 추가 불필요, 즉시 가능)**
  - `dist/next/` → `dist/next/<arch>/` 구조(§3), 현 i386 바이너리를
    `dist/next/i386/`로 이동.
  - `os_tag()` arch 매크로화(§4).
  - 문서 골격(§9).
  - **검증: i386 실기 회귀** — 동작·greeting이 이전과 동일(ostag만
    `openstep-i386`로). 프로토콜·기능 불변 확인.
- **Phase 1 — m68k (빅엔디안 실증) ✅ 완료(2026-09-14)**
  - 실기 NeXTstation `nstationonion`(192.168.1.228) 사용(에뮬 불필요).
  - gnfsd NFS로 소스 전달, `make -f make/Makefile.next` → Mach-O m68k 빌드
    (경고 2건 무해: gnext.h W* 매크로가 이 OPENSTEP의 wait.h와 중복 정의,
    net.c send const). cc = gcc 2.7.2.1(i386과 동일).
  - `gcds --ping`→`openstep-m68k`, RUN/exit/네이티브 cc roundtrip 통과.
  - `dist/next/m68k/gcdsd` 수집. **엔디안-클린 전제 빅엔디안 실증 완료.**
- **Phase 2 — sparc**
  - **빌드 ✅ 완료(2026-09-15)**: .18(i386)의 NeXT 다중아키텍처 cc로
    `make ... CC='cc -arch sparc'` → Mach-O SPARC(`openstep-sparc`).
    별도 sparc 하드웨어 불필요(cross). `dist/next/sparc/gcdsd` 수집.
  - **런타임 검증 ⏳ 남음**: sparc 실기/에뮬 확보 시 실행·`--ping`·RUN.
    (엔디안 정합성은 m68k 빅엔디안 실기로 대리 입증.)
- **Phase 3 — 마무리**
  - `harvest` arch 자동화(§8), 문서 확정(§9), 로드맵·HANDOFF 갱신.

## 11. 리스크 / 미해결

- **sparc 검증 하드웨어** — 최대 리스크. Phase 2를 조건부로, 확보 전까지
  i386+m68k(빅엔디안 대표)로 아키텍처 독립성 입증.
- **m68k 구버전 cc 빌드 가능성** — Phase 1 초반에 확인, 실패 시 gnext.h 보강.
- **Previous ROM/설치 미디어 확보.**
- **dist 구조 A vs B 결정**(§3).

## 12. codex 교차검토

§5 엔디안 감사 결과와 §4 os_tag 변경을 codex로 교차검토하되, 회신을
신뢰하지 않고 전건 재검증(스킬 `codex-review-verify`). 특히 "텍스트
프로토콜이라 엔디안 안전"이라는 핵심 전제를 반증 시도로 점검.
