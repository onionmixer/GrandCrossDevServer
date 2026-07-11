# BeOS / Haiku 데몬 노트

**본 프로젝트는 BeOS를 Haiku와 동일 취급한다** — Haiku가 BeOS의
후속·호환 구현이고 BeOS 세대의 gcc 2.95 하이브리드를 제공하므로,
Haiku 검증을 BeOS 지원으로 간주한다.

BeOS/Haiku는 POSIX 공용 코드로 빌드된다. 소켓 API가 별도 라이브러리
(`libnetwork`)에 있어 `LIBS`만 지정하면 된다.

## 검증 환경

**Haiku** hrev53755 (2020-01), x86 32bit, **gcc 2.95.3-haiku**.
Haiku의 gcc2 하이브리드는 BeOS R5와 같은 컴파일러 세대라, 프로젝트의
엄격 C89 + gcc 2.x 이식성 주장을 실제로 검증한다.

## 빌드

```
make -f make/Makefile.posix LIBS=-lnetwork      # BeOS/Haiku
```

- 소스는 Linux/macOS와 동일. 소켓 함수(socket/bind/...)가
  `libnetwork`에 있어 링크 라이브러리만 지정하면 된다.
- **우리 코드는 gcc 2.95에서 경고 0으로 컴파일**된다. 빌드 로그의
  `#endif violates ANSI` / `unnamed structs/unions` 경고는 전부
  **Haiku 자체 시스템 헤더**(bsd/features.h, net/if.h)의 것이지
  우리 코드가 아니다.

## Haiku 검증 결과 (실기 SSH)

전부 통과:
- greeting `GCDS 1 posix <host> LIVE INTERACTIVE`, ping/stat
- 네이티브 gcc 2.95 원격 컴파일·실행, exit code, stdout/stderr 분리
- RUN 스트리밍, LIVE 제어 세션(실행 중 ping/`busy`)
- RUNI 대화형(sort stdin), **K→exit 143**(손자 프로세스 포함 종료)
- 비동기 RUNA/RESULT
- **PUT/GET 2MB 바이너리 왕복 무손상**
- sshfs 공유 스토리지 워크플로(로컬 편집 → 원격 gcc 빌드)

## 참고: 구형 BeOS R5의 소켓

프로젝트는 BeOS를 Haiku로 취급하므로 아래는 참고 사항이다. 구형
BeOS R5(비-BONE)의 net_server 소켓은 진짜 fd가 아니어서 소켓에
read/write가 안 되고 send/recv만 가능하며(우리 허용 함수 목록이
이미 이를 강제), select가 소켓 전용이었다. Haiku 소켓은 진짜 fd라
이 제약이 없고 send/recv 규칙 덕에 어느 쪽이든 안전하다.
