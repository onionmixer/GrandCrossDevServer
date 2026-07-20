# Win32 데몬 노트

## 빌드

Visual Studio(6.0 이상) 명령 프롬프트에서 프로젝트 루트 기준:

```
nmake /f make\Makefile.win32
```

MinGW 크로스 빌드(Linux에서 컴파일 확인용):
`make -f make/Makefile.mgw` (i686-w64-mingw32-gcc 필요)

## MSVC 사용을 위한 vcvars 확보 (PLAN_02 §4)

gcdsd는 자신이 물려받은 환경에서 명령을 실행한다. `cl`/`nmake`를
쓰려면 두 방법 중 하나:

1. **권장: vcvars가 적용된 셸에서 데몬 기동.** 시작 스크립트 예:
   ```bat
   @echo off
   call "C:\Program Files\Microsoft Visual Studio\...\vcvars32.bat"
   cd C:\gcds
   gcdsd.exe
   ```
2. 매 명령에 포함: `gcds win "vcvars32.bat && cl /nologo hello.c"`
   (경로에 공백이 있으면 원격지에 래퍼 배치 파일을 두는 편이 깔끔)

## 알려진 제약

- ENV 값에 큰따옴표(`"`)를 넣을 수 없다 (`ERR 500` — cmd.exe
  인용 규칙상 안전하게 합성 불가).
- ENV 값 속 `%VAR%`는 cmd.exe가 확장한다.
- **같은 RUN 명령행 안에서 방금 설정한 ENV를 `%NAME%`으로 참조할 수
  없다** — cmd.exe는 한 줄 전체의 `%…%`를 실행 전에 확장하기 때문
  (cmd.exe 고유 동작, wine에서 실측 확인). 자식 프로세스의 환경
  블록에는 정상 반영되므로 컴파일러 env(INCLUDE/LIB 등) 용도는
  문제없다. 명령행 참조가 필요하면 `cmd /c` 한 겹을 끼운다.
- exit code는 255로 클램프된다 (PLAN_01 §5).
- 설정은 `gcdsd.cnf`를 먼저 찾고 없으면 `gcdsd.conf`로 폴백한다
  (`-c <파일>`로 명시 가능). 배포본의 `gcdsd.cnf`를 개명 없이 쓰면 된다.
- tmpdir 기본값은 `.`(데몬 실행 디렉토리), `gcdsd.cnf`에서 변경.
  임시 경로는 네이티브 구분자로 결합되어 `.\gcds_out.tmp`가 되며,
  이 경로가 cmd.exe 리다이렉션(`> "..."`)에 그대로 들어간다.
- **공유 폴더는 드라이브 문자로 마운트해서 매핑할 것** — UNC 경로
  (`\\server\share`)는 cmd.exe의 작업 디렉토리가 될 수 없다
  (PLAN_05 §3).

## wine 검증 결과 (llvm-mingw i686 빌드)

wine에서 통과: TCP(greeting/PING/exit code/stderr 분리/1MB 바이너리
무손상/ENV 자식 전달/비동기 RUNA·RESULT), COM1 시리얼(HELLO 세션,
실행, PING — dosdevices→PTY). wine cmd의 `set FOO` 나열 오동작은
wine 쪽 결함(자식 env는 정상; MSVC 실기 빌드 시 함께 확인).

**live_w32(감시 루프)도 wine 통과**: RUN 스트리밍,
실행 중 PING/STAT(busy)/`ERR 409`, RUNI stdin 왕복·exit code·
K(TerminateProcess, exit 255), 비동기 작업 중 `busy <jobid>`.
Win32 고유 제약: K는 즉시 강제 종료(POSIX처럼 SIGTERM 유예 없음),
시리얼(COM) 모드에서는 INTERACTIVE/LIVE 미제공.

**K는 Job Object로 손자 프로세스까지 종료** —
`cmd.exe /c`가 띄운 컴파일러/자식이 K 후에도 파이프를 붙들어
감시 루프가 행이 걸리는 것을 방지(POSIX 프로세스 그룹의 등가물).
CreateJobObject/AssignProcessToJobObject/TerminateJobObject는
런타임 GetProcAddress로 로드 → Win2000+ 에서 동작, NT4에서는
자동으로 단일 TerminateProcess로 폴백(손자 잔존 한계 감수).
CreateProcess는 CREATE_SUSPENDED로 만들어 job 할당 후 ResumeThread
(손자 생성 전 할당, 레이스 방지).

## 남은 작업 (MSVC 실기)

전 기능은 wine(llvm-mingw 빌드)에서 검증됐다. 남은 것은 **MSVC 실기
빌드**뿐: `nmake` 빌드(/W3 경고 확인)와, wine cmd의 `set FOO` 나열
오동작이 실기에서 재현되지 않는지 확인.
