# macOS 데몬 노트

macOS는 POSIX 공용 코드로 빌드된다 — 전용 백엔드가 없다.
검증 환경: macOS 11.7.10 (Big Sur), x86_64, Apple clang 13.

## 빌드

```
make -f make/Makefile.posix        # gcdsd, gcds 모두 생성
```

- `-ansi`가 링크 단계에서 "unused argument" 경고를 내지만(clang이
  링크에 -ansi를 무시) **코드 경고는 0**. 무해.
- Apple clang / Xcode Command Line Tools만 있으면 된다.

## 공유 스토리지 워크플로 (sshfs)

Linux 호스트에서 mac의 작업 디렉토리를 sshfs로 마운트하여
"로컬 편집 → 원격 컴파일"을 구현한다 (README 전제 3).

```sh
# Linux 호스트:
sshfs user@mac:gcds_work /mnt/mac -o reconnect
# gcds.cnf 경로 매핑:
#   host.mac.map.1 = /mnt/mac|/Users/user/gcds_work
cd /mnt/mac
gcds mac 'clang -o app app.c'      # cwd가 원격 경로로 자동 매핑
```

- 소스/산출물은 sshfs 공유 폴더에 있고, gcdsd는 명령만 실행한다.
- 클라이언트의 경로 매핑(PLAN_05)이 로컬 마운트 경로를 mac의
  네이티브 경로로 변환해 `CWD`로 보낸다.

## 검증 항목 (실기 SSH)

전부 통과:
- greeting `GCDS 1 posix <host> LIVE INTERACTIVE`, ping/stat
- 네이티브 clang 원격 컴파일 → Mach-O x86_64 생성·실행
- exit code 전달, stdout/stderr 분리, RUN 스트리밍
- LIVE 제어 세션(실행 중 ping/`busy`/`ERR 409`)
- RUNI 대화형(sort stdin 왕복), **K→원격 중단 exit 143**
- 도구 별칭 `@build`, 경로 매핑(sshfs 마운트 cwd 자동 변환)
- 배치 디버거 lldb(`lldb -b -o run ...`) — RUN만으로
- 비동기 RUNA/RESULT, 프로토콜 위반 내성(데몬 생존)

## 텍스트 인코딩 (NFD → NFC)

macOS는 파일명을 **분해형(NFD)** 으로 다루는 경우가 있다(HFS+는 강제
정규화, APFS는 보존). 그래서 컴파일러나 `ls`가 뱉는 한글 파일명이
NFD 바이트로 나올 수 있는데, 화면상 NFC와 똑같아 보여도 바이트가 달라
**문자열 비교와 클라이언트의 `--mapback` 경로 매칭이 어긋난다.**

데몬이 출력 텍스트의 **한글을 NFC로 조합**해서 보낸다(Unicode 3.12
산술식, 테이블 불필요). 실기 확인: 분해형 이름으로 만든 파일이
실제로 NFD로 저장되고(`e18492 e185a1 e186ab …`), 그것을 `ls` 한 출력이
클라이언트에는 NFC로 도착한다.

- 라틴 결합문자(é 등)는 조합하지 않는다 — 전체 유니코드 NFC는 대형
  테이블이 필요해 범위 밖(PLAN_02 §5.2).
- **PUT/GET(D 프레임)은 변환하지 않는다.**

## K(중단)의 프로세스 그룹 종료

자식을 **프로세스 그룹 리더**(setpgid)로 만들고 K/종료 시
`kill(-pgid, ...)`로 그룹 전체를 종료한다(live.c). `sh -c "( ... )"`가
띄운 손자까지 정리되어, 손자가 출력 파이프를 붙든 채 감시 루프가
EOF를 무한 대기하는 것을 막는다. K는 exit 143(128+SIGTERM).
