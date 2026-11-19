#!/bin/bash
# harvest.sh - build each platform's daemon and collect the binary into
# dist/<platform>/ so it can be used by copy-and-go (PLAN_06).
#
# Run on the Linux host:
#   ./dist/harvest.sh                 # every platform that is buildable now
#   ./dist/harvest.sh linux next      # only the listed platforms
#   ./dist/harvest.sh next:sparc      # one OPENSTEP arch (next:all = all)
#
# Each platform is INDEPENDENT. If its toolchain or target machine is
# not available, that platform prints "SKIP: <reason>" and the rest
# continue - nothing is faked. Re-run any time to refresh.
#
# Toolchain / target locations (override via environment):
#   WATCOM   Open Watcom root (DOS builds)          default ~/ow
#   WATT     Watt-32 root (DOS TCP build)           default ~/watt32
#   MACOS_SSH   ssh target for the macOS builder    e.g. user@macos-host
#   HAIKU_SSH   ssh target for the Haiku builder    e.g. user@haiku-host
#   NEXT_HOST   telnet host for OPENSTEP            e.g. openstep-host
#   NEXT_HOST_<arch>  native builder for one arch   e.g. NEXT_HOST_m68k
#               (unset: NEXT_HOST cross-builds it with cc -arch <arch>)
#   NFS_SERVER  this host's IP as seen by OPENSTEP  e.g. 192.0.2.10
#   NFS_EXPORT  gnfsd export dir (source drop)      default: the export
#               of the gnfsd running on 111/2049 (see nfsd/serve.sh)
#   NEXT_MOUNT  where to mount it on OPENSTEP if it is not mounted yet
#               default /nfstest
# The defaults below are placeholders - set these to your own machines.
#
# NOTE: on some setups Haiku and NeXTSTEP are the SAME physical machine
# (only one OS booted at a time). harvest_haiku / harvest_next each
# detect what is actually running and SKIP if it is the other OS.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
cd "$ROOT"

# Pick up the in-repo win32/dos cross toolchains if they're set up
# (toolchain/setup.sh populated them). Sets WATCOM/WATT + PATH.
[ -f "$ROOT/toolchain/env.sh" ] && . "$ROOT/toolchain/env.sh"

WATCOM="${WATCOM:-$HOME/ow}"
WATT="${WATT:-$HOME/watt32}"
MACOS_SSH="${MACOS_SSH:-user@macos-host}"
HAIKU_SSH="${HAIKU_SSH:-user@haiku-host}"
NEXT_HOST="${NEXT_HOST:-openstep-host}"
NFS_SERVER="${NFS_SERVER:-192.0.2.10}"
NFS_EXPORT="${NFS_EXPORT:-}"            # empty: use the running gnfsd's
NEXT_MOUNT="${NEXT_MOUNT:-/nfstest}"      # used only if not mounted yet

say()  { echo "[harvest] $*"; }
skip() { echo "[harvest] SKIP $CUR: $*"; }
ok()   { echo "[harvest] OK   $CUR -> $*"; }

# Remove build products so one platform's output can't be mistaken for
# another's (mingw .o vs Watcom .obj, and a stale gcdsd.exe). Called
# before and after each cross build.
_clean_tree() {
    find common daemon client -maxdepth 1 \
        \( -name '*.o' -o -name '*.ow' -o -name '*.obj' \) -delete 2>/dev/null
    rm -f gcdsd gcdsd.exe gcds gcds.exe *.map 2>/dev/null
}

# --- linux: native, always available -------------------------------
harvest_linux() {
    CUR=linux
    make -f make/Makefile.posix >/dev/null 2>&1 || { skip "posix build failed"; return; }
    ( cd nfsd && make >/dev/null 2>&1 )   # gnfsd
    cp -f gcds gcdsd "$DIST/linux/" 2>/dev/null
    [ -x gcdslog ] && cp -f gcdslog "$DIST/linux/"      # panic-capture tool
    [ -x nfsd/gnfsd ] && cp -f nfsd/gnfsd "$DIST/linux/"
    ok "gcds gcdsd gcdslog gnfsd"
}

# --- win32: llvm-mingw / mingw cross --------------------------------
harvest_win32() {
    CUR=win32
    command -v i686-w64-mingw32-gcc >/dev/null 2>&1 || {
        skip "i686-w64-mingw32-gcc not found (toolchain/setup.sh mingw)"; return; }
    _clean_tree
    make -f make/Makefile.mgw >/dev/null 2>&1
    if [ -f gcdsd.exe ]; then cp -f gcdsd.exe "$DIST/win32/gcdsd.exe"; ok "gcdsd.exe"
    else skip "mgw build produced no gcdsd.exe"; fi
    _clean_tree
}

# --- dos: Open Watcom cross (serial + optional Watt-32 TCP) ---------
harvest_dos() {
    CUR=dos
    [ -x "$WATCOM/binl64/wcl" ] || { skip "Open Watcom not at \$WATCOM ($WATCOM); run toolchain/setup.sh ow"; return; }
    _clean_tree
    make -f make/Makefile.dos WATCOM="$WATCOM" >/dev/null 2>&1
    if [ -f gcdsd.exe ]; then cp -f gcdsd.exe "$DIST/dos/gcdsd-serial.exe"; ok "gcdsd-serial.exe"
    else skip "serial (Makefile.dos) build produced no exe"; fi
    _clean_tree
    if [ -f "$WATT/lib/wattcpwl.lib" ]; then
        make -f make/Makefile.dtcp WATCOM="$WATCOM" WATT="$WATT" >/dev/null 2>&1
        if [ -f gcdsd.exe ]; then cp -f gcdsd.exe "$DIST/dos/gcdsd-tcp.exe"; ok "gcdsd-tcp.exe"
        else say "dos: Watt-32 TCP build produced no exe"; fi
        _clean_tree
    else
        say "dos: TCP build skipped (Watt-32 lib not at $WATT/lib; toolchain/setup.sh watt)"
    fi
}

# --- macos: native clang over ssh -----------------------------------
harvest_macos() {
    CUR=macos
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$MACOS_SSH" true 2>/dev/null || {
        skip "$MACOS_SSH unreachable"; return; }
    _harvest_over_ssh "$MACOS_SSH" "make -f make/Makefile.posix" gcdsd "$DIST/macos/gcdsd"
}

# --- haiku: native gcc over ssh (same box as NeXTSTEP) --------------
harvest_haiku() {
    CUR=haiku
    ssh -o ConnectTimeout=5 -o BatchMode=yes "$HAIKU_SSH" true 2>/dev/null || {
        skip "$HAIKU_SSH unreachable (booted into NeXTSTEP? reboot to Haiku)"; return; }
    local os; os="$(ssh -o BatchMode=yes "$HAIKU_SSH" uname -s 2>/dev/null)"
    case "$os" in *Haiku*|*BeOS*) : ;; *) skip "target is '$os', not Haiku"; return;; esac
    _harvest_over_ssh "$HAIKU_SSH" "make -f make/Makefile.posix LIBS=-lnetwork" gcdsd "$DIST/haiku/gcdsd"
}

# Send source to an ssh target, build, copy the binary back. Uses
# tar-over-ssh (not rsync) because some targets lack rsync (e.g. Haiku).
_harvest_over_ssh() {
    local tgt="$1" buildcmd="$2" bin="$3" dest="$4"
    local rdir="gcds-harvest"
    ssh -o BatchMode=yes "$tgt" "rm -rf $rdir && mkdir -p $rdir" 2>/dev/null \
        || { skip "$tgt unreachable for build"; return; }
    tar czf - --exclude='.git' --exclude='dist' --exclude='*.o' \
              --exclude='gcds' --exclude='gcdsd' --exclude='gnfsd' \
              include common daemon client make nfsd test etc *.md 2>/dev/null \
        | ssh -o BatchMode=yes "$tgt" "cd $rdir && tar xzf -" 2>/dev/null \
        || { skip "source transfer to $tgt failed"; return; }
    ssh -o BatchMode=yes "$tgt" "cd $rdir && $buildcmd" >/dev/null 2>&1 \
        || { skip "remote build failed on $tgt"; return; }
    scp -o BatchMode=yes "$tgt:$rdir/$bin" "$dest" >/dev/null 2>&1 \
        && { chmod +x "$dest"; ok "$(basename "$dest")"; } \
        || skip "no $bin produced on $tgt"
    ssh -o BatchMode=yes "$tgt" "rm -rf $rdir" 2>/dev/null
}

# --- next: NeXT cc via gnfsd NFS share + telnet, per arch (PLAN_07) --
# OPENSTEP has no ssh. The source is dropped into the export of the
# gnfsd that is running now (one gnfsd owns 111/2049, so whatever it
# exports is the share), built over telnet, and the binary is picked up
# from the same share - no copy back over the wire.
#
#   next          the native arch of $NEXT_HOST
#   next:<arch>   i386 | m68k | sparc   (next:all = all three)
#
# Build host per arch: $NEXT_HOST_<arch> (e.g. NEXT_HOST_m68k) builds
# natively when set; otherwise $NEXT_HOST cross-builds with
# `cc -arch <arch>`, which works where NeXT's multi-arch toolchain is
# installed (/lib/<arch>/specs). A binary only reaches dist/next/<arch>/
# if its Mach-O cputype and its ostag string match <arch>.
NEXT_ARCHES="i386 m68k sparc"
NEXT_PROMPT='[a-zA-Z0-9_-]+:[0-9]+#'     # csh root prompt, e.g. host:3#

# Run one csh command line as root over telnet; prints only its output
# and returns its exit status (2 = no login, 3 = timeout). The output is
# cut out between two markers; they are sent as HV_""BEGIN so the echo of
# the typed line can never be mistaken for the marker itself.
_next_run() {
    local host="$1" tmo="$2" cmd="$3" rc
    expect -f - "$host" "$tmo" "$cmd" "$NEXT_PROMPT" <<'EOS' | tr -d '\r' \
        | awk '/^HV_BEGIN$/ { on = 1; next } /^HV_END=[0-9]+$/ { on = 0 } on'
set host [lindex $argv 0]; set timeout [lindex $argv 1]
set cmd  [lindex $argv 2]; set prompt  [lindex $argv 3]
log_user 0
set env(TERM) vt100
spawn telnet $host
expect {
    -re {login: ?$} { send "root\r" }
    timeout { exit 2 }
    eof     { exit 2 }
}
expect {
    -re {Password: ?}        { send "\r"; exp_continue }
    -re {TERM = \(unknown\)} { send "vt100\r"; exp_continue }
    -re $prompt {}
    timeout { exit 2 }
}
log_user 1
send -- "echo HV_\"\"BEGIN ; $cmd ; echo HV_\"\"END=\$status\r"
set rc 3
expect {
    -re {\nHV_END=([0-9]+)} { set rc $expect_out(1,string) }
    timeout {}
}
log_user 0
send "logout\r"
expect eof
exit $rc
EOS
    rc=${PIPESTATUS[0]}
    return "$rc"
}

# Mach-O cputype of a file (7 i386, 6 m68k, 14 sparc), or empty.
# The header is in the target's byte order: feedface read LE = i386.
_macho_cpu() {
    local h; h="$(od -An -tx1 -N8 "$1" 2>/dev/null | tr -d ' \n')"
    case "$h" in
        cefaedfe*) echo $((16#${h:14:2}${h:12:2}${h:10:2}${h:8:2})) ;;
        feedface*) echo $((16#${h:8:8})) ;;
    esac
}

# export of the gnfsd serving the standard NFS port (default 2049)
_running_export() {
    ps -C gnfsd -o args= 2>/dev/null | while read -r line; do
        case " $line " in
            *" -n 2049 "*) ;;               # explicit standard port
            *" -n "*) continue ;;           # a test instance on a high port
        esac
        echo "${line##* }"; break
    done
}

harvest_next() {
    CUR=next
    local want="${1:-}" arches a
    command -v expect >/dev/null 2>&1 || { skip "expect not installed"; return; }
    ss -ulnp 2>/dev/null | grep -q ':2049 ' || { skip "gnfsd not running (see nfsd/serve.sh)"; return; }
    local export="${NFS_EXPORT:-$(_running_export)}"
    [ -n "$export" ] && [ -d "$export" ] || { skip "cannot tell the running gnfsd's export (set NFS_EXPORT)"; return; }
    case "$want" in
        "")   arches="" ;;                    # native arch of $NEXT_HOST
        all)  arches="$NEXT_ARCHES" ;;
        *)    case " $NEXT_ARCHES " in *" $want "*) arches="$want" ;;
                  *) skip "unknown arch '$want' (one of: $NEXT_ARCHES, all)"; return ;; esac ;;
    esac
    if [ -z "$arches" ]; then
        (echo > "/dev/tcp/$NEXT_HOST/23") 2>/dev/null || {
            skip "$NEXT_HOST:23 (telnet) closed - booted into Haiku?"; return; }
        arches="$(_next_run "$NEXT_HOST" 30 arch | tr -d '\r' | grep -E '^[a-z0-9]+$' | head -1)"
        [ -n "$arches" ] || { skip "could not read the arch of $NEXT_HOST"; return; }
    fi
    for a in $arches; do _harvest_next_arch "$a" "$export"; done
}

_harvest_next_arch() {
    local arch="$1" export="$2"
    local CUR="next/$arch"
    local hv="NEXT_HOST_$arch" host cc native mp out rc cpu want_cpu
    host="${!hv:-$NEXT_HOST}"
    case "$arch" in i386) want_cpu=7 ;; m68k) want_cpu=6 ;; sparc) want_cpu=14 ;; esac
    (echo > "/dev/tcp/$host/23") 2>/dev/null || { skip "$host:23 (telnet) closed"; return; }

    native="$(_next_run "$host" 30 arch | tr -d '\r' | grep -E '^[a-z0-9]+$' | head -1)"
    if [ "$native" = "$arch" ]; then
        cc="cc"
    else
        # test -f, not ls: this OPENSTEP ls exits 0 on a missing file
        _next_run "$host" 30 "test -f /lib/$arch/specs" >/dev/null || {
            skip "$host is $native and has no /lib/$arch/specs (set $hv to a $arch machine)"; return; }
        cc="cc -arch $arch"
        say "$CUR: cross-building on $host ($native) with '$cc'"
    fi

    # where the export is mounted on the target; mount it if it is not
    mp="$(_next_run "$host" 30 "mount" | tr -d '\r' \
          | awk -v src="$NFS_SERVER:$export" '$1 == src && $2 == "on" { print $3; exit }')"
    if [ -z "$mp" ]; then
        mp="$NEXT_MOUNT"
        _next_run "$host" 60 "if (! -d $mp) mkdir $mp ; mount -t nfs -o hard,intr,timeo=30,retrans=5 $NFS_SERVER:$export $mp" >/dev/null \
            || { skip "could not mount $NFS_SERVER:$export on $host:$mp"; return; }
        say "$CUR: mounted $NFS_SERVER:$export at $host:$mp"
    fi

    # drop the source into the share (fresh dir: no stale attribute cache)
    local drop="gcds-harvest.$arch.$$"
    mkdir "$export/$drop" || { skip "cannot write into $export"; return; }
    # no host objects: NeXT make would take a newer ELF .o as up to date
    tar cf - --exclude='*.o' --exclude='*.obj' --exclude='*.ow' \
        include common daemon make | ( cd "$export/$drop" && tar xf - ) \
        || { rm -rf "${export:?}/$drop"; skip "source drop failed"; return; }

    # under sh -c: a failing csh builtin (cd) would abort the whole line,
    # end marker included, and leave us waiting for the timeout
    _next_run "$host" 900 "sh -c \"cd $mp/$drop && make -f make/Makefile.next CC='$cc' > build.log 2>&1\"" >/dev/null
    rc=$?
    if [ $rc -ne 0 ] || [ ! -f "$export/$drop/gcdsd" ]; then
        skip "build failed on $host (rc=$rc); last lines:"
        tail -5 "$export/$drop/build.log" 2>/dev/null | sed 's/^/    /'
        rm -rf "${export:?}/$drop"; return
    fi
    cpu="$(_macho_cpu "$export/$drop/gcdsd")"
    if [ "$cpu" != "$want_cpu" ]; then
        skip "built binary is cputype '$cpu', not $arch ($want_cpu) - not collected"
    elif ! grep -aq "openstep-$arch" "$export/$drop/gcdsd"; then
        skip "built binary lacks ostag openstep-$arch - not collected"
    else
        mkdir -p "$DIST/next/$arch"
        cp -f "$export/$drop/gcdsd" "$DIST/next/$arch/gcdsd" && chmod +x "$DIST/next/$arch/gcdsd"
        ok "next/$arch/gcdsd (Mach-O cputype $cpu, built on $host with '$cc')"
    fi
    rm -rf "${export:?}/$drop"
}

PLATFORMS=("$@")
[ ${#PLATFORMS[@]} -eq 0 ] && PLATFORMS=(linux win32 dos macos haiku next)
for p in "${PLATFORMS[@]}"; do
    case "$p" in
        linux) harvest_linux ;;
        win32) harvest_win32 ;;
        dos)   harvest_dos ;;
        macos) harvest_macos ;;
        haiku) harvest_haiku ;;
        next)  harvest_next ;;
        next:*) harvest_next "${p#next:}" ;;
        *) echo "[harvest] unknown platform: $p" ;;
    esac
done
say "done. contents:"
find "$DIST" -type f \( -name 'gcds*' -o -name 'gnfsd' \) | sort
