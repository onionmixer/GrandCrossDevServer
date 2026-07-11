#!/bin/csh -f
#
# next-mount.csh - mount the Linux gnfsd NFS share on NeXTSTEP/OPENSTEP.
#
# NeXTSTEP's default shell is csh, so this is a csh script (-f skips
# ~/.cshrc so it behaves the same for any user). gnfsd - this project's
# self-built NFSv2 server - runs on the Linux host; this mounts its
# export here so OPENSTEP can read and build the shared source tree.
# Run it again any time to re-mount (it unmounts first, flushing the
# NeXTSTEP attribute cache so host-side edits become visible).
#
# Usage (as root - mount(8) needs privilege):
#   ./next-mount.csh                       # use the defaults below
#   ./next-mount.csh <server>              # override server only
#   ./next-mount.csh <server> <export> <mountpoint>
#   ./next-mount.csh -u                    # unmount only, then stop
#
# Examples:
#   ./next-mount.csh
#   ./next-mount.csh 192.0.2.10
#   ./next-mount.csh -u

# ---- defaults (override via arguments) ----
set server  = 192.0.2.10        # ← Linux 호스트(gnfsd) IP로 수정
set export   = /path/to/nfsd/serveNFS   # ← gnfsd export 경로로 수정
set mountpt = /nfstest

# NFS options, all classic 4.2BSD/SunOS NFS flags NeXTSTEP understands:
#   noac    - gnfsd shares a LIVE Linux directory; disable attribute
#             caching so edits on the host are seen at once (README.md).
#   soft    - fail an operation instead of hanging forever if gnfsd dies.
#   intr    - let Ctrl-C break out of a stuck NFS call.
#   timeo   - initial timeout in tenths of a second (here 1.0s).
#   retrans - retries before a soft mount gives up.
set opts = "soft,intr,timeo=10,retrans=3,noac"

# ---- argument parsing ----
set unmount_only = 0
if ("$1" == "-u" || "$1" == "unmount") then
    set unmount_only = 1
else
    if ("$1" != "") set server  = "$1"
    if ("$2" != "") set export   = "$2"
    if ("$3" != "") set mountpt = "$3"
endif

# ---- must be root ----
# NeXTSTEP `id' has no -u; whoami is the portable check here.
set me = `whoami`
if ("$me" != "root") then
    echo "next-mount: must run as root (mount needs privilege)."
    exit 1
endif

# ---- always unmount first (ignore "not mounted") ----
# This is also how you refresh the view after editing on the host:
# just run the script again.
umount $mountpt >& /dev/null

if ($unmount_only) then
    echo "next-mount: $mountpt unmounted."
    exit 0
endif

# ---- ensure the mount point exists ----
# NOTE: do NOT use `mkdir -p' here - NeXTSTEP mkdir has no -p and would
# create a bogus directory literally named "-p".
if (! -d $mountpt) then
    mkdir $mountpt
    if ($status != 0) then
        echo "next-mount: cannot create mount point $mountpt"
        exit 1
    endif
endif

# ---- mount ----
echo "next-mount: mounting ${server}:${export}"
echo "            on ${mountpt} (${opts})"
mount -t nfs -o $opts ${server}:${export} $mountpt
if ($status != 0) then
    echo "next-mount: MOUNT FAILED."
    echo "  - is gnfsd running on ${server}?  (portmap 111 + nfs 2049)"
    echo "  - reachable?  try:  ping ${server}"
    exit 1
endif

echo "next-mount: mounted OK.  ${mountpt} contains:"
ls $mountpt
exit 0
