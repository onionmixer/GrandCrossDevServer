#!/bin/sh
# test-stress.sh - LINK churn + WRITE-integrity stress against a
# throwaway gnfsd instance (no root, no mount). Heavier than test-nfs
# so it is a separate target: `make stress`.
set -e
DIR=$(mktemp -d)
./gnfsd -p 12111 -n 12049 "$DIR" &
PID=$!
trap 'kill $PID 2>/dev/null; rm -rf "$DIR"' 0
sleep 0.3
python3 test-stress.py link  12049 2000 "$DIR"
python3 test-stress.py write 12049 32   "$DIR"
echo "== STRESS PASS =="
