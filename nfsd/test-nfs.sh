#!/bin/bash
# test-nfs.sh - build-free harness: start gnfsd on a high port over a
# temp export, run the direct-RPC test client, report. No root/mount.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-12049}"
WORK="$(mktemp -d)"
cleanup() { [ -n "${GP:-}" ] && kill "$GP" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

echo "seed" > "$WORK/pre.txt"
# dedicated high ports so a test run never collides with (or needs
# to kill) a production server on 111/2049
"$HERE/gnfsd" -p "$PORT" -n 12050 "$WORK" >/dev/null 2>&1 &
GP=$!
sleep 0.6
if ! kill -0 "$GP" 2>/dev/null; then echo "gnfsd failed to start"; exit 1; fi

GN_EXPORT="$WORK" python3 "$HERE/test-nfs.py" "$PORT"
