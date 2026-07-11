#!/bin/bash
# run.sh - GrandCrossDevServer POSIX regression suite.
#
#   test/run.sh            # build + run all applicable tests
#   test/run.sh --no-build # skip the build step (use existing bins)
#
# Exit 0 iff every non-skipped test passed. Serial tests need socat;
# they are skipped if it is absent.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib.sh"
T_ROOT="$(cd "$HERE/.." && pwd)"   # set after sourcing lib.sh

DO_BUILD=1
[ "${1:-}" = "--no-build" ] && DO_BUILD=0

T_WORK="$(mktemp -d "${TMPDIR:-/tmp}/gcds-test.XXXXXX")"
cleanup() { t_daemon_stop; [ -n "${SOCAT_PID:-}" ] && kill "$SOCAT_PID" 2>/dev/null; rm -rf "$T_WORK"; }
trap cleanup EXIT

echo "== GrandCrossDevServer regression =="
echo "work: $T_WORK"

# --------------------------------------------------------------
# 0. build
# --------------------------------------------------------------
echo "[build]"
if [ "$DO_BUILD" = 1 ]; then
    t_case "build"
    out="$(cd "$T_ROOT" && make -f make/Makefile.posix 2>&1)"
    if [ $? -ne 0 ]; then t_bad "make failed"; echo "$out"; t_summary; exit 1; fi
    warns="$(echo "$out" | grep -ciE 'warning|error')"
    assert_eq 0 "$warns" "compiles with no warnings"
fi
[ -x "$T_ROOT/gcdsd" ] && [ -x "$T_ROOT/gcds" ] || { echo "binaries missing"; exit 1; }

t_write_conf ""
t_daemon_start || { echo "daemon failed to start"; cat "$T_WORK/gcdsd.log"; exit 1; }

# --------------------------------------------------------------
# 1. basic exec + exit codes
# --------------------------------------------------------------
echo "[exec]"
t_case "basic stdout"
assert_eq "hello" "$(gcds local 'echo hello')" "echo roundtrip"

t_case "exit code 0";  gcds local 'true';    assert_eq 0  $? "exit 0"
t_case "exit code 3";  gcds local 'exit 3';  assert_eq 3  $? "exit 3"
t_case "exit code 42"; gcds local 'exit 42'; assert_eq 42 $? "exit 42"

t_case "stderr separation"
o="$(gcds local 'echo OUT; echo ERR >&2' 2>"$T_WORK/e")"
assert_eq "OUT" "$o" "stdout only"
assert_eq "ERR" "$(cat "$T_WORK/e")" "stderr only"

# --------------------------------------------------------------
# 2. binary integrity
# --------------------------------------------------------------
echo "[binary]"
t_case "1MB binary roundtrip"
dd if=/dev/urandom of="$T_WORK/rand.bin" bs=1024 count=1024 2>/dev/null
gcds local "cat $T_WORK/rand.bin" > "$T_WORK/rand.out"
if cmp -s "$T_WORK/rand.bin" "$T_WORK/rand.out"; then t_ok "identical"
else t_bad "binary differs"; fi

# --------------------------------------------------------------
# 3. ENV (session-scoped)
# --------------------------------------------------------------
echo "[env]"
# t_run_with_env speaks the protocol staged (AUTH/ENV/RUN), sending
# nothing after RUN — the client must stay silent from RUN until X
# (PLAN_01 4); anything after RUN would (correctly) abort the job.
t_case "ENV applied + not leaked"
assert_contains "$(t_run_with_env 'echo [$FOO]' 'FOO=barbaz')" "[barbaz]" \
    "env visible in same session"
assert_eq "[]" "$(gcds local 'echo [$FOO]')" "env not leaked to next session"

# --------------------------------------------------------------
# 4. path mapping + tool alias
# --------------------------------------------------------------
echo "[pathmap/alias]"
mkdir -p "$T_WORK/proj/sub" "$T_WORK/remote/sub"
cat >> "$T_WORK/gcds.conf" <<EOF
host.local.map.1 = $T_WORK/proj|$T_WORK/remote
host.local.tool.ec = echo alias:
EOF
t_case "cwd mapping"
assert_eq "$T_WORK/remote/sub" "$(cd "$T_WORK/proj/sub" && gcds local pwd)" "local cwd -> remote"
t_case "backslash separator"
cat >> "$T_WORK/gcds.conf" <<EOF
host.local.map.2 = $T_WORK/win|Z:\\proj
EOF
mkdir -p "$T_WORK/win/a"
out="$(cd "$T_WORK/win/a" && gcds local pwd 2>&1)"
assert_contains "$out" 'Z:\proj\a' "slash converted to backslash"
t_case "tool alias"
assert_eq "alias: hi" "$(gcds local @ec hi)" "@ec expands"
t_case "unknown alias errors"
gcds local @nope x >/dev/null 2>&1; [ $? -ne 0 ] && t_ok "nonzero on unknown alias" || t_bad "should fail"

# --------------------------------------------------------------
# 5. mapback (reverse path filter)
# --------------------------------------------------------------
echo "[mapback]"
t_case "--mapback rewrites remote path"
out="$(gcds --mapback local "printf '%s\\n' 'Z:\\proj\\src\\foo.c(9): error'")"
assert_contains "$out" "$T_WORK/win/src/foo.c(9): error" "remote->local path"

# --------------------------------------------------------------
# 6. streaming + LIVE control sessions
# --------------------------------------------------------------
echo "[live]"
t_case "RUN streams before completion"
gcds local 'echo first; sleep 2; echo second' > "$T_WORK/s.out" &
sp=$!
sleep 0.8
early="$(tr '\n' ' ' < "$T_WORK/s.out")"
wait $sp
assert_eq "first " "$early" "first arrives at 0.8s"
assert_eq "first second " "$(tr '\n' ' ' < "$T_WORK/s.out")" "both after done"

t_case "greeting advertises LIVE INTERACTIVE"
assert_contains "$(t_greeting)" "LIVE INTERACTIVE" "caps present"

t_case "control session during RUN"
gcds local 'sleep 3' >/dev/null 2>&1 &
bp=$!
sleep 0.6
assert_contains "$(gcds --ping local)" "alive" "ping during run"
assert_contains "$(gcds --stat local)" "busy" "stat busy during run"
gcds local 'echo x' >/dev/null 2>&1; assert_eq 125 $? "second RUN rejected (ERR 409)"
wait $bp

# --------------------------------------------------------------
# 7. RUNI interactive
# --------------------------------------------------------------
echo "[runi]"
t_case "RUNI stdin forwarded"
assert_eq "$(printf 'a\nb\nc')" "$(printf 'c\na\nb\n' | gcds -i local sort)" "sort via stdin"
t_case "RUNI exit code"
printf 'x\n' | gcds -i local 'read a; exit 7'; assert_eq 7 $? "exit through RUNI"
t_case "RUNI K kills remote (incl. grandchild)"
rc="$(t_ix_kill 1.5 'sleep 30')"
assert_eq 143 "$rc" "Ctrl-C -> exit 143 (no hang)"

# --------------------------------------------------------------
# 8. async RUNA/RESULT
# --------------------------------------------------------------
echo "[async]"
t_write_conf "async = 1"
t_daemon_start || { echo "async daemon failed"; exit 1; }
t_case "greeting advertises ASYNC"
assert_contains "$(t_greeting)" "ASYNC" "ASYNC cap"
t_case "RUNA/RESULT roundtrip"
o="$(gcds local 'echo async-out; exit 5' 2>/dev/null)"
rc=$?
assert_eq "async-out" "$o" "async output"
assert_eq 5 "$rc" "async exit code"
t_case "result retained after fetch"
assert_contains "$(gcds --stat local)" "result" "stat shows retained result"

# back to sync daemon for remaining tests
t_write_conf ""
t_daemon_start || { echo "daemon restart failed"; exit 1; }

# --------------------------------------------------------------
# 9. protocol-violation resilience
# --------------------------------------------------------------
echo "[robustness]"
t_case "daemon survives abuse"
head -c 4000 /dev/urandom | timeout 2 bash -c "cat >/dev/tcp/127.0.0.1/$T_PORT" 2>/dev/null
python3 -c "print('A'*5000)" 2>/dev/null | timeout 2 bash -c "cat >/dev/tcp/127.0.0.1/$T_PORT" 2>/dev/null
printf 'RUN echo hack\n' | timeout 2 bash -c "cat >/dev/tcp/127.0.0.1/$T_PORT" 2>/dev/null
printf 'AUTH wrong\nPING\n' | timeout 2 bash -c "cat >/dev/tcp/127.0.0.1/$T_PORT" 2>/dev/null
sleep 0.3
assert_contains "$(gcds --ping local)" "alive" "daemon still responsive"

# --------------------------------------------------------------
# 8b. PUT/GET file transfer
# --------------------------------------------------------------
echo "[xfer]"
mkdir -p "$T_WORK/rd"
t_case "PUT uploads a file"
echo "put-payload-123" > "$T_WORK/up.txt"
gcds --put local "$T_WORK/up.txt" "$T_WORK/rd/up.txt"
assert_ok $? "put returns 0"
assert_eq "put-payload-123" "$(cat "$T_WORK/rd/up.txt" 2>/dev/null)" "remote file content"

t_case "GET downloads a file"
gcds --get local "$T_WORK/rd/up.txt" "$T_WORK/down.txt"
assert_ok $? "get returns 0"
if cmp -s "$T_WORK/up.txt" "$T_WORK/down.txt"; then t_ok "roundtrip identical"
else t_bad "roundtrip differs"; fi

t_case "PUT/GET 2MB binary integrity"
dd if=/dev/urandom of="$T_WORK/big.bin" bs=1024 count=2048 2>/dev/null
gcds --put local "$T_WORK/big.bin" "$T_WORK/rd/big.bin" >/dev/null 2>&1 &&
gcds --get local "$T_WORK/rd/big.bin" "$T_WORK/big.out" >/dev/null 2>&1
if cmp -s "$T_WORK/big.bin" "$T_WORK/big.out"; then t_ok "2MB identical"
else t_bad "2MB differs"; fi

t_case "GET missing file errors"
gcds --get local "$T_WORK/nope" "$T_WORK/x" >/dev/null 2>&1
[ $? -ne 0 ] && t_ok "nonzero on missing file" || t_bad "should fail"

t_case "upload -> remote compile -> download"
printf '#include <stdio.h>\nint main(void){printf("xfer-build\\n");return 0;}\n' > "$T_WORK/h.c"
gcds --put local "$T_WORK/h.c" "$T_WORK/rd/h.c" >/dev/null 2>&1
gcds local "cd $T_WORK/rd && gcc -o h h.c" >/dev/null 2>&1
gcds --get local "$T_WORK/rd/h" "$T_WORK/h.bin" >/dev/null 2>&1
chmod +x "$T_WORK/h.bin" 2>/dev/null
assert_eq "xfer-build" "$("$T_WORK/h.bin" 2>/dev/null)" "downloaded binary runs"

# --------------------------------------------------------------
# 9b. allow-list, output cap, shutdown cleanup
# --------------------------------------------------------------
echo "[hardening]"

t_case "allow-list permits listed peer"
t_write_conf "allow = 127.0.0.1"
t_daemon_start || { echo "acl daemon failed"; exit 1; }
assert_contains "$(gcds --ping local)" "alive" "127.0.0.1 allowed"

t_case "allow-list denies unlisted peer"
t_write_conf "allow = 10.99.99.0/24"
t_daemon_start || { echo "acl daemon failed"; exit 1; }
gcds --ping local >/dev/null 2>&1
assert_eq 125 $? "loopback denied when not in list"
assert_contains "$(cat "$T_WORK/gcdsd.log")" "rejected" "rejection logged"

t_case "output cap truncates"
t_write_conf "maxout = 100000"
t_daemon_start || { echo "cap daemon failed"; exit 1; }
# produce ~1MB; expect far less delivered + a truncation notice
out="$(gcds local 'head -c 1000000 /dev/zero | tr "\\0" "x"' 2>&1)"
n=${#out}
if [ "$n" -lt 200000 ]; then t_ok "delivered $n bytes (< cap+slack)"
else t_bad "cap not enforced ($n bytes)"; fi
assert_contains "$out" "truncated at cap" "truncation notice sent"

t_case "SIGTERM cleans temp files"
t_write_conf ""
t_daemon_start || { echo "daemon failed"; exit 1; }
gcds local 'echo x' >/dev/null 2>&1        # create/refresh temp paths
kill -TERM "$T_DPID" 2>/dev/null
wait "$T_DPID" 2>/dev/null; T_DPID=""
leftover="$(ls "$T_WORK"/gcds_*.tmp 2>/dev/null | wc -l)"
assert_eq 0 "$leftover" "no gcds_*.tmp left after SIGTERM"

# restore a plain daemon for serial section reuse of helpers
t_write_conf ""
t_daemon_start || { echo "daemon restart failed"; exit 1; }

# --------------------------------------------------------------
# 10. serial channel (socat)  -- optional
# --------------------------------------------------------------
echo "[serial]"
if command -v socat >/dev/null 2>&1; then
    t_daemon_stop
    socat -d pty,raw,echo=0,link="$T_WORK/ttyA" \
             pty,raw,echo=0,link="$T_WORK/ttyB" >/dev/null 2>&1 &
    SOCAT_PID=$!
    sleep 0.5
    cat > "$T_WORK/ser_d.conf" <<EOF
token = $T_TOKEN
tmpdir = $T_WORK
serial = $T_WORK/ttyA:9600
EOF
    cat > "$T_WORK/ser_c.conf" <<EOF
host.ser.serial = $T_WORK/ttyB:9600
host.ser.token = $T_TOKEN
EOF
    "$T_ROOT/gcdsd" -c "$T_WORK/ser_d.conf" > "$T_WORK/ser_d.log" 2>&1 &
    T_DPID=$!
    sleep 0.5
    sgcds() { GCDS_CONF="$T_WORK/ser_c.conf" "$T_ROOT/gcds" "$@"; }

    t_case "serial basic run"
    assert_eq "over-serial" "$(sgcds ser 'echo over-serial')" "echo over serial"
    t_case "serial exit code"
    sgcds ser 'exit 4'; assert_eq 4 $? "exit over serial"
    t_case "serial binary 64KB"
    dd if=/dev/urandom of="$T_WORK/s.bin" bs=1024 count=64 2>/dev/null
    sgcds ser "cat $T_WORK/s.bin" > "$T_WORK/s.bout"
    if cmp -s "$T_WORK/s.bin" "$T_WORK/s.bout"; then t_ok "serial binary identical"
    else t_bad "serial binary differs"; fi
    t_case "serial ping"
    assert_contains "$(sgcds --ping ser)" "alive" "ping over serial"
else
    t_case "serial (socat)"; t_skip "socat not installed"
fi

# --------------------------------------------------------------
t_summary
