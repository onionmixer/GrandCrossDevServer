# dist/linux — 호스트 클라이언트 + 로컬 데몬/NFS

Linux 호스트(지휘소)용. 원격에 복사할 필요 없이 여기서 실행.
- `gcds`   : 지시/수신 클라이언트. `gcds.cnf`→`~/.gcds.conf` 편집(호스트 별칭/토큰/경로매핑).
- `gcdsd`  : 로컬 테스트용 데몬(루프백). `gcdsd.cnf`→`gcdsd.conf`.
- `gnfsd`  : 자작 NFSv2 서버(공유 스토리지). 실행은 `nfsd/serve.sh` 참조.

사용: `./gcds local "gcc hello.c && ./a.out"` / `./gcds --ping <alias>`
빌드 출처: 네이티브 gcc(`make -f make/Makefile.posix`).
