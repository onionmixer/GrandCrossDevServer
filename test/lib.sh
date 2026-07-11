#!/bin/bash
# lib.sh - test harness helpers for GrandCrossDevServer.
# Sourced by run.sh. Provides assertions, daemon lifecycle, and a
# pass/fail tally. POSIX loopback + serial(socat) oriented.

# ---- state -----------------------------------------------------
T_PASS=0
T_FAIL=0
T_SKIP=0
T_CUR=""            # current test name
: "${T_ROOT:=}"     # project root (set by run.sh after sourcing)
T_WORK=""           # scratch dir for this run
T_PORT=19931        # loopback TCP test port
T_TOKEN="regtok"
T_DPID=""           # current daemon pid

# ---- output ----------------------------------------------------
_c_grn=$'\033[32m'; _c_red=$'\033[31m'; _c_yel=$'\033[33m'; _c_off=$'\033[0m'
[ -t 1 ] || { _c_grn=""; _c_red=""; _c_yel=""; _c_off=""; }

t_case() { T_CUR="$1"; }

t_ok()   { T_PASS=$((T_PASS+1)); echo "  ${_c_grn}PASS${_c_off} $T_CUR${1:+ - $1}"; }
t_bad()  { T_FAIL=$((T_FAIL+1)); echo "  ${_c_red}FAIL${_c_off} $T_CUR${1:+ - $1}"; }
t_skip() { T_SKIP=$((T_SKIP+1)); echo "  ${_c_yel}SKIP${_c_off} $T_CUR${1:+ - $1}"; }

# assert_eq <expected> <actual> [msg]
assert_eq() {
    if [ "$1" = "$2" ]; then t_ok "$3"; else
        t_bad "$3 (expected [$1], got [$2])"; fi
}
# assert_contains <haystack> <needle> [msg]
assert_contains() {
    case "$1" in
        *"$2"*) t_ok "$3" ;;
        *) t_bad "$3 (missing [$2] in [$1])" ;;
    esac
}
# assert_ok <rc> [msg]  -- rc==0
assert_ok() {
    if [ "$1" = 0 ]; then t_ok "$2"; else t_bad "$2 (rc=$1)"; fi
}

# ---- daemon lifecycle ------------------------------------------
# writes gcdsd.conf/gcds.conf into T_WORK. extra conf lines via $1.
t_write_conf() {
    cat > "$T_WORK/gcdsd.conf" <<EOF
port = $T_PORT
token = $T_TOKEN
tmpdir = $T_WORK
$1
EOF
    cat > "$T_WORK/gcds.conf" <<EOF
host.local.addr = 127.0.0.1
host.local.port = $T_PORT
host.local.token = $T_TOKEN
EOF
}

t_daemon_start() {
    t_daemon_stop
    "$T_ROOT/gcdsd" -c "$T_WORK/gcdsd.conf" > "$T_WORK/gcdsd.log" 2>&1 &
    T_DPID=$!
    # wait for listen (up to ~3s)
    local i
    for i in $(seq 1 30); do
        grep -q listening "$T_WORK/gcdsd.log" 2>/dev/null && return 0
        kill -0 "$T_DPID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

t_daemon_stop() {
    [ -n "$T_DPID" ] && kill "$T_DPID" 2>/dev/null
    wait "$T_DPID" 2>/dev/null
    T_DPID=""
}

# gcds wrapper against the loopback host
gcds() { GCDS_CONF="$T_WORK/gcds.conf" "$T_ROOT/gcds" "$@"; }

# read the greeting line only (connect, recv first line). Used to
# check advertised capabilities.
t_greeting() {
    T_RAW_PORT="$T_PORT" python3 - <<'PY'
import socket, os
s = socket.socket(); s.settimeout(2)
try:
    s.connect(("127.0.0.1", int(os.environ["T_RAW_PORT"])))
    buf = b""
    while b"\n" not in buf:
        d = s.recv(256)
        if not d: break
        buf += d
    print(buf.split(b"\n")[0].decode("latin1"))
except Exception:
    pass
PY
}

# speak the protocol correctly: AUTH, then the given "CMD arg" lines
# (one per remaining arg) as ENV/CWD replies-checked, then a final
# RUN, and return the concatenated O-frame stdout. Respects the
# RUN-silence rule: nothing is sent after RUN until X.
#   t_run_with_env <run-cmdline> <ENV=a> [ENV=b ...]
t_run_with_env() {
    T_RAW_PORT="$T_PORT" T_RAW_TOK="$T_TOKEN" \
    python3 - "$1" "${@:2}" <<'PY'
import socket, os, sys, time
port = int(os.environ["T_RAW_PORT"]); tok = os.environ["T_RAW_TOK"]
runcmd = sys.argv[1]; envs = sys.argv[2:]
s = socket.socket(); s.settimeout(3); f = None
def line():
    global buf
    while b"\n" not in buf:
        d = s.recv(4096)
        if not d: return None
        buf += d
    i = buf.index(b"\n"); ln = buf[:i]; buf = buf[i+1:]
    return ln.decode("latin1")
def readn(n):
    global buf
    while len(buf) < n:
        d = s.recv(n-len(buf))
        if not d: break
        buf += d
    r = buf[:n]; buf = buf[n:]; return r
buf = b""
try:
    s.connect(("127.0.0.1", port)); time.sleep(0.2)
    line()                                   # greeting
    s.sendall(("AUTH %s\n"%tok).encode()); line()
    for e in envs:
        s.sendall(("ENV %s\n"%e).encode()); line()
    s.sendall(("RUN %s\n"%runcmd).encode())  # nothing after this
    out = b""
    while True:
        ln = line()
        if ln is None: break
        if ln[:2] in ("O ","E "):
            n = int(ln.split()[1]); data = readn(n)
            if ln[0] == "O": out += data
        elif ln.startswith("X "):
            break
    sys.stdout.write(out.decode("latin1"))
except Exception:
    pass
PY
}

# run 'gcds -i <args>' in the background, send SIGINT after <delay>s,
# poll up to <max>s, echo its exit code (or HANG). Uses job control
# so the signal reaches gcds, not the whole script.
t_ix_kill() {
    local delay="$1"; shift
    local rcfile
    rcfile="$T_WORK/ixrc.$$"
    (
        set -m
        GCDS_CONF="$T_WORK/gcds.conf" "$T_ROOT/gcds" -i local "$@" \
            </dev/null >/dev/null 2>/dev/null &
        local gp=$!
        sleep "$delay"
        kill -INT "$gp"
        local i
        for i in $(seq 1 30); do
            kill -0 "$gp" 2>/dev/null || break
            sleep 0.5
        done
        if kill -0 "$gp" 2>/dev/null; then kill -9 "$gp"; echo HANG > "$rcfile"
        else wait "$gp"; echo $? > "$rcfile"; fi
    )
    cat "$rcfile"; rm -f "$rcfile"
}

t_summary() {
    echo ""
    echo "-------------------------------------------"
    echo "  ${_c_grn}PASS $T_PASS${_c_off}  ${_c_red}FAIL $T_FAIL${_c_off}  ${_c_yel}SKIP $T_SKIP${_c_off}"
    echo "-------------------------------------------"
    [ "$T_FAIL" -eq 0 ]
}
