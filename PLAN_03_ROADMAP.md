# PLAN_03 — 로드맵 & 현재 상태

구현 원칙: 각 단계는 이전 단계가 실기(또는 에뮬레이터)에서 검증된 뒤
진행한다. 이 문서는 **현재 상태 요약 + 잔여 작업**만 유지하며, 세부
구현 이력은 git log에 위임한다. 플랫폼별 지원 현황표는 README(SSOT),
빌드·검증 재현 절차는 doc/\*.md를 본다.

## 완료 (검증됨)

**코어 (Linux 루프백)**
- GCDSP v1: AUTH/CWD/ENV/RUN/RUNA/RESULT/RUNI/PUT/GET/PING/STAT/QUIT,
  O/E/X/D/I 프레임. ENV는 subshell export 접두로 주입(세션 누출 없음).
- 채널 추상화(chan.c): TCP(net.c) + 시리얼(ser_psx.c, HELLO 세션 경계).
- 클라이언트(gcds): 접속/인증, stdout/stderr/exit code 그대로 반영,
  경로 매핑(pathmap.c), 도구 별칭(toolias.c), 역변환 필터(`--mapback`).
- 프로토콜 위반 내성(쓰레기/과대 라인/AUTH 전 명령/오토큰/절단)에도
  데몬 생존.

**실행 모델**
- 통합 감시 루프(daemon/live.c): RUN 스트리밍 + LIVE 제어세션 + RUNI +
  비동기 JOB을 한 루프로 처리(설계의 ix_psx.c는 live.c로 흡수됨).
- LIVE: 실행 중 `--stat`→`busy`, `--ping`→OK, RUN 재시도→`ERR 409`.
- K(중단): 자식을 setpgid로 그룹 리더화, `kill(-pgid)`로 손자까지 종료
  → exit 143. Win32는 live_w32.c(select+PeekNamedPipe), K→exit 255.
- 비동기(RUNA/RESULT): 단일태스킹 OS(DOS)용 접수→폴링→결과.
- 파일 전송 PUT/GET: D-프레임(8bit clean), `maxout` 상한.

**플랫폼 이식·검증** (상세 doc/\*.md, 상태표 README)
- macOS: 실기(clang), 전 기능 통과.
- Windows: wine 엔드투엔드 전 기능 통과(TCP+COM), MSVC 실기 빌드·검증 완료.
- BeOS/Haiku: 실기(gcc 2.95), 전 기능 통과. BeOS=Haiku 동일 취급.
- MS-DOS 시리얼: DOSBox-X 검증(Open Watcom 크로스, FOSSIL/int14, 비동기).
- MS-DOS TCP: DOSBox-X 검증(Watt-32 16bit large model, 패킷드라이버).
- NeXTSTEP/OPENSTEP: 실기(NeXT cc 2.7.2.1), **LIVE/RUNI/K + sgtty
  시리얼 포함 전 기능 통과, INTERACTIVE 실기 확정**. gnext.h shim,
  ser_next.c(sgtty). 소스는 gnfsd NFS 공유로 전달.

**대화형 버퍼링 실측** (PLAN_04 §5)
- 전송 계층은 실시간(첫 줄 +0.1s). 지연이 보이는 경우는 **자식
  프로그램의 libc full-buffering**이며 `stdbuf -oL`로 완화됨을 측정으로
  확인. gdb는 스스로 flush해 RUNI에서 정상 동작.

**부속 산출물**
- 회귀 테스트(test/run.sh, 46 케이스).
- 자작 NFSv2 서버(gnfsd): ONC RPC/XDR + MOUNT + NFS v2, DRC,
  sub-second mtime, 권한 정규화. OPENSTEP 실기 mount 검증.
- dist/ 배포 바이너리 6/6(복사→실행, git 추적) + win32/dos 크로스
  툴체인 편입(PLAN_06).
- gcdslog: 시리얼 콘솔 커널 패닉 캡처 도구(tools/gcdslog.c, Linux 호스트
  전용). QEMU Linux 게스트 실제 커널 패닉 캡처로 검증(doc/panic-capture.md).

## 잔여 작업
- [ ] (선택) rexec 어댑터, `gcds --all` 병렬, 고전 Mac OS/OS2/AmigaOS
      시리얼 우선 이식, 시리얼 프레임 CRC 확장(capability `CRC`).

## 리스크 / 한계 메모
- **16bit int(DOS/Watcom)**: 길이/카운트에 `int`를 쓰면 Linux에선 안
  걸린다 → `long` 규칙(PLAN_02 §1)을 리뷰 항목으로 강제.
- **DOS conventional memory**: Watt-32 상주 데몬 + COMMAND.COM +
  컴파일러가 640K 공존. DGROUP 극빡빡(CONF_ENT_MAX 12 등), 대형 프로그램
  실행 시 메모리 부족(errcode 8) 가능.
- **파이프 + 비(非)tty 대화형 도구**: RUNI에서 프롬프트 미출력/블록
  버퍼링 도구 존재(PLAN_04 §5). 근본 대응은 "배치 우선"이며 pty는
  도입하지 않음(결정). gdb/lldb 외 도구는 실측 필요.
- **시리얼 무결성**: v1은 오류 검출 없음(단거리 케이블 전제). 프레임
  길이 필드가 깨지면 세션 어긋남 → HELLO 재동기로 복구. 필요 시
  CRC 확장을 앞당김.
- **DOS FOSSIL 의존**: 시리얼 DOS 빌드는 FOSSIL(X00/BNU) 상주 요구.
  직접 UART 제어는 하지 않기로 결정.
- **BeOS R5 실기 미검증**: net_server 소켓 비호환은 send/recv 강제
  규칙으로 선제 차단했으나 R5 실하드웨어 검증 전까지 확정 아님(Haiku는
  검증됨).
