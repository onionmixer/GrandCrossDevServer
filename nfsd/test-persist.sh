#!/bin/bash
# 재시작 후 핸들 유효성 회귀 테스트
cd "$(dirname "$0")" 2>/dev/null
D=/mnt/USERS/onion/DATA_ORIGN/Workspace/GrandCrossDevServer/nfsd
W=$(mktemp -d); mkdir -p "$W/sub"; echo persist-ok > "$W/sub/deep.txt"
"$D/gnfsd" -p 12081 -n 12081 "$W" >/dev/null 2>&1 & sleep 1
GN_EXPORT=$W python3 test-persist.py 12081 get >/dev/null
pkill -x gnfsd; sleep 1
"$D/gnfsd" -p 12081 -n 12081 "$W" >/dev/null 2>&1 & sleep 1
OUT=$(GN_EXPORT=$W python3 test-persist.py 12081 use)
pkill -x gnfsd 2>/dev/null; rm -rf "$W"
echo "$OUT" | grep -q "OK(영속화 동작)" && echo "PASS handle survives server restart" || { echo "FAIL handle lost on restart"; exit 1; }
echo "$OUT" | grep -q "persist-ok" && echo "PASS content readable via old handle" || { echo "FAIL content unreadable"; exit 1; }
