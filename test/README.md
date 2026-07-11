# 회귀 테스트

POSIX 루프백 + 시리얼(socat) 기반 회귀 스위트. 지금까지 수동으로
검증한 항목을 재실행 가능한 형태로 고정한다.

## 실행

```sh
test/run.sh              # 빌드 + 전체 실행
test/run.sh --no-build   # 기존 바이너리로 실행만
```

- 모든 (스킵 아닌) 테스트가 통과하면 **종료 코드 0**. CI에 그대로
  물릴 수 있다.
- 시리얼 테스트는 `socat`이 있어야 실행되며, 없으면 SKIP.
- 컬러 출력은 tty일 때만. 파이프/CI에서는 평문.

## 커버리지

- 빌드: `-ansi -pedantic -Wall` 경고 0
- 실행: 기본 stdout, exit code(0/3/42), stdout/stderr 분리
- 바이너리 무결성: 1MB 무손상 왕복
- ENV: 세션 내 적용 + 다음 세션 미누출
- 경로: cwd 매핑, `\` 구분자 변환, 역변환(`--mapback`)
- 도구 별칭: `@name` 확장, 미정의 오류
- 스트리밍/LIVE: 완료 전 출력 도착, 실행 중 ping/stat busy/ERR 409
- RUNI: stdin 전달, exit code, K→exit 143(손자 프로세스 포함 종료)
- 비동기: ASYNC 광고, RUNA/RESULT 왕복, 결과 보존
- 내성: 쓰레기/과장 라인/AUTH 전 명령/오토큰 후 데몬 생존
- 시리얼: 기본 실행, exit code, 64KB 바이너리, ping (socat PTY)

## 파일

- `run.sh` — 테스트 케이스 + 판정
- `lib.sh` — 어서션, 데몬 수명주기, 프로토콜 헬퍼(t_greeting/
  t_run_with_env/t_ix_kill)

## 참고: 프로토콜 침묵 규칙

`t_run_with_env`가 AUTH→ENV→RUN을 단계적으로 보내는 이유 — RUN 이후
X 프레임 전까지 클라이언트는 침묵해야 한다(PLAN_01 §4). RUN과 같은
배치로 뒷바이트를 보내면 감시 루프가 침묵 위반으로 세션을 정상
중단시킨다. 테스트는 실제 클라이언트처럼 단계적으로 말해야 한다.

## 범위 밖

Windows(wine)/DOS(DOSBox-X)/macOS(SSH) 실행 검증은 에뮬레이터·원격
환경이 필요해 이 스위트에 포함하지 않는다. 각 플랫폼 노트는
`doc/*.md`, 빌드 검증은 각 Makefile 참조.
