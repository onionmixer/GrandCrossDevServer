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

## K(중단)의 프로세스 그룹 종료

자식을 **프로세스 그룹 리더**(setpgid)로 만들고 K/종료 시
`kill(-pgid, ...)`로 그룹 전체를 종료한다(live.c). `sh -c "( ... )"`가
띄운 손자까지 정리되어, 손자가 출력 파이프를 붙든 채 감시 루프가
EOF를 무한 대기하는 것을 막는다. K는 exit 143(128+SIGTERM).
