# dist/dos — MS-DOS 데몬 (16bit)

두 변형:
- `gcdsd-serial.exe` : 시리얼 전용(FOSSIL 드라이버 X00/BNU 상주 필요). NIC 불필요.
- `gcdsd-tcp.exe`    : TCP(Watt-32). 패킷 드라이버 + `WATTCP.CFG`(my_ip/netmask/gateway) 필요.

1. 해당 exe와 `gcdsd.cnf`를 DOS로 복사, `gcdsd.conf`로 복사 후 `token` 수정.
2. 시리얼: `serial = COM1:9600`. 실행 `gcdsd-serial.exe -c gcdsd.conf`.
3. TCP: 패킷드라이버·WATTCP.CFG 준비 후 `gcdsd-tcp.exe -c gcdsd.conf`.

- **단일 태스킹 → 비동기(ASYNC) 필수**(cnf에 `async = 1`). LIVE/INTERACTIVE 불가.
- 빌드 출처: 크로스 Open Watcom 16bit large model(`make -f make/Makefile.dos` / `.dtcp`). 상세·함정 doc/dos.md.
