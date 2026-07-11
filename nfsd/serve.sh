#!/bin/bash
# serve.sh - launch the simple NFSv2 server (gnfsd) over a folder.
#
#   ./serve.sh [-p PMAP] [-n NFS] [-v] [-f] <export-dir>
#
#   -p PMAP   portmapper port (default 111; retro clients look here,
#             which needs root). use a high port for testing.
#   -n NFS    mount+nfs port (default 2049; old clients assume it).
#   -u USER   drop privileges to this user after binding (default:
#             the export dir's owner). client-created files land
#             owned by USER, not root.
#   -v        verbose: log each RPC request
#   -f        foreground (default: background, PID printed)
#
# Builds gnfsd if missing. Prints mount instructions. Ctrl-C
# (foreground) or `kill <pid>` (background) to stop.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT=111
NFSPORT=2049
VERB=""
UOPT=""
FG=0

while [ $# -gt 0 ]; do
    case "$1" in
        -p) PORT="${2:?-p needs a port}"; shift 2 ;;
        -n) NFSPORT="${2:?-n needs a port}"; shift 2 ;;
        -u) UOPT="-u ${2:?-u needs a user}"; shift 2 ;;
        -v) VERB="-v"; shift ;;
        -f) FG=1; shift ;;
        -h|--help)
            sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *)  break ;;
    esac
done

DIR="${1:-}"
if [ -z "$DIR" ]; then
    echo "usage: $0 [-p PORT] [-f] <export-dir>" >&2
    exit 2
fi
if [ ! -d "$DIR" ]; then
    echo "serve.sh: '$DIR' is not a directory" >&2
    exit 2
fi
DIR="$(cd "$DIR" && pwd)"     # absolutise

# build if needed
if [ ! -x "$HERE/gnfsd" ]; then
    echo "serve.sh: building gnfsd..."
    make -C "$HERE" >/dev/null || { echo "build failed" >&2; exit 1; }
fi

# privileged-port warning
if [ "$PORT" -lt 1024 ] && [ "$(id -u)" != 0 ]; then
    echo "serve.sh: port $PORT is privileged and you are not root." >&2
    echo "  run with sudo, or pick a high port:  $0 -p 12049 $DIR" >&2
    exit 1
fi

HOSTIP="$(hostname -I 2>/dev/null | awk '{print $1}')"
[ -z "$HOSTIP" ] && HOSTIP="<this-host>"

print_info() {
    echo "gnfsd: sharing $DIR"
    echo "gnfsd: NFSv2/UDP  portmap=$PORT  mount+nfs=$NFSPORT"
    echo "mount from a client (e.g. NeXTSTEP/OPENSTEP):"
    echo "  mount -t nfs -o soft,timeo=10 $HOSTIP:$DIR /mnt"
    [ "$PORT" != 111 ] && echo "  (portmap on non-standard $PORT:" \
        "most clients need 111)"
}

if [ "$FG" = 1 ]; then
    print_info
    echo "gnfsd: foreground (Ctrl-C to stop)"
    exec "$HERE/gnfsd" -p "$PORT" -n "$NFSPORT" $UOPT $VERB "$DIR"
else
    "$HERE/gnfsd" -p "$PORT" -n "$NFSPORT" $UOPT $VERB "$DIR" &
    PID=$!
    sleep 0.4
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "serve.sh: gnfsd failed to start (port in use?)" >&2
        exit 1
    fi
    print_info
    echo "gnfsd: running as PID $PID   (stop:  kill $PID)"
fi
