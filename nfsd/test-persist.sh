#!/bin/bash
# 재시작 후 핸들 유효성 회귀 테스트
#
# 중요: 이 테스트가 띄운 gnfsd 인스턴스만 PID로 죽인다. 예전엔
# `pkill -x gnfsd`를 썼는데, 그건 이름이 "gnfsd"인 모든 프로세스를
# 죽여서 같은 호스트에서 111/2049로 돌던 프로덕션 서버까지 함께
# 종료시켰다(= make test 한 번에 실서버가 죽던 원인).
cd "$(dirname "$0")" 2>/dev/null
D=/mnt/USERS/onion/DATA_ORIGN/Workspace/GrandCrossDevServer/nfsd
W=$(mktemp -d); mkdir -p "$W/sub"; echo persist-ok > "$W/sub/deep.txt"
"$D/gnfsd" -p 12081 -n 12081 "$W" >/dev/null 2>&1 & PID1=$!; sleep 1
GN_EXPORT=$W python3 test-persist.py 12081 get >/dev/null
kill "$PID1" 2>/dev/null; sleep 1        # 우리 인스턴스만 종료(재시작 흉내)
"$D/gnfsd" -p 12081 -n 12081 "$W" >/dev/null 2>&1 & PID2=$!; sleep 1
OUT=$(GN_EXPORT=$W python3 test-persist.py 12081 use)
kill "$PID2" 2>/dev/null; rm -rf "$W"    # 우리 인스턴스만 정리
echo "$OUT" | grep -q "OK(영속화 동작)" && echo "PASS handle survives server restart" || { echo "FAIL handle lost on restart"; exit 1; }
echo "$OUT" | grep -q "persist-ok" && echo "PASS content readable via old handle" || { echo "FAIL content unreadable"; exit 1; }
