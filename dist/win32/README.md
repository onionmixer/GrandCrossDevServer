# dist/win32 — Windows 데몬

1. `gcdsd.exe`, `gcdsd.cnf`를 Windows로 복사.
2. `copy gcdsd.cnf gcdsd.conf` 후 `token` 수정.
3. 실행: `gcdsd.exe -c gcdsd.conf` (TCP). COM 시리얼은 `.conf`에 `serial = COM1:9600`.

- Winsock 1.1(NT4~11 동작). LIVE/RUNI 지원(K→exit 255). COM 모드는 INTERACTIVE 미표기.
- MSVC 사용 시 데몬을 vcvars 적용 환경에서 실행하거나 명령에 `vcvars32.bat &&` 접두(doc/win32.md).
- 빌드 출처: 크로스 llvm-mingw i686(`make -f make/Makefile.mgw`). MSVC 실기 빌드도 가능(doc/win32.md).
