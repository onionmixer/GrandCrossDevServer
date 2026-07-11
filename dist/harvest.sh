#!/bin/bash
# harvest.sh - build each platform's daemon and collect the binary into
# dist/<platform>/ so it can be used by copy-and-go (PLAN_06).
#
# Run on the Linux host:
#   ./dist/harvest.sh                 # every platform that is buildable now
#   ./dist/harvest.sh linux next      # only the listed platforms
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
#   NFS_SERVER  this host's IP as seen by OPENSTEP  e.g. 192.0.2.10
#   NFS_EXPORT  gnfsd export dir (source drop)      default nfsd/serveNFS
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
NFS_EXPORT="${NFS_EXPORT:-$ROOT/nfsd/serveNFS}"

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
    [ -x nfsd/gnfsd ] && cp -f nfsd/gnfsd "$DIST/linux/"
    ok "gcds gcdsd gnfsd"
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

# --- next: native NeXT cc via gnfsd NFS share + telnet --------------
# OPENSTEP has no ssh; source is delivered through this project's own
# gnfsd export and built over a telnet session. This step needs an
# `expect` helper and gnfsd running - if absent, it prints the manual
# procedure (dist/next/README.md) and SKIPs.
harvest_next() {
    CUR=next
    command -v expect >/dev/null 2>&1 || { skip "expect not installed"; return; }
    ss -ulnp 2>/dev/null | grep -q ':2049 ' || { skip "gnfsd not running (see nfsd/serve.sh)"; return; }
    if ! (echo > "/dev/tcp/$NEXT_HOST/23") 2>/dev/null; then
        skip "$NEXT_HOST:23 (telnet) closed - booted into Haiku? see dist/next/README.md"; return
    fi
    say "next: OPENSTEP build is interactive (telnet+NFS)."
    say "      follow dist/next/README.md; binary lands in dist/next/gcdsd."
    skip "automated telnet build not wired here (manual per README)"
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
        *) echo "[harvest] unknown platform: $p" ;;
    esac
done
say "done. contents:"
find "$DIST" -type f \( -name 'gcds*' -o -name 'gnfsd' \) | sort
