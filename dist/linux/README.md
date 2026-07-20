# dist/linux — 호스트 클라이언트 + 로컬 데몬/NFS

Linux 호스트(지휘소)용. 원격에 복사할 필요 없이 여기서 실행.
- `gcds`   : 지시/수신 클라이언트. `gcds.cnf`를 `~/.gcds.cnf`로 두고 편집(호스트 별칭/토큰/경로매핑).
- `gcdsd`  : 로컬 테스트용 데몬(루프백). `gcdsd.cnf`의 token 수정 후 실행.
- `gnfsd`  : 자작 NFSv2 서버(공유 스토리지). 실행은 `nfsd/serve.sh` 참조.
- `gcdslog`: 대상 시리얼 콘솔에서 커널 패닉 캡처. `./gcdslog -b 115200 -o panic.log /dev/ttyUSB0` (doc/panic-capture.md).

사용: `./gcds local "gcc hello.c && ./a.out"` / `./gcds --ping <alias>`
빌드 출처: 네이티브 gcc(`make -f make/Makefile.posix`).
