#!/bin/csh -f
#
# next-mount.csh - mount the Linux gnfsd NFS share on NeXTSTEP/OPENSTEP.
#
# NeXTSTEP's default shell is csh, so this is a csh script (-f skips
# ~/.cshrc so it behaves the same for any user). gnfsd - this project's
# self-built NFSv2 server - runs on the Linux host; this mounts its
# export here so OPENSTEP can read and build the shared source tree.
# Run it again any time to re-mount (it unmounts first, flushing the
# NeXTSTEP attribute cache so host-side edits become visible; add -n
# for that).
#
# Usage (as root - mount(8) needs privilege):
#   ./next-mount.csh                       # use the defaults below
#   ./next-mount.csh <server>              # override server only
#   ./next-mount.csh <server> <export> <mountpoint>
#   ./next-mount.csh -n [<server> ...]     # add noac (instant host-edit
#                                          #   visibility, more traffic)
#   ./next-mount.csh -u                    # unmount only, then stop
#
# Examples:
#   ./next-mount.csh
#   ./next-mount.csh 192.0.2.10
#   ./next-mount.csh -n            # 소스를 계속 고치는 중일 때
#   ./next-mount.csh -u

# ---- defaults (override via arguments) ----
set server  = 192.0.2.10        # ← Linux 호스트(gnfsd) IP로 수정
set export   = /path/to/nfsd/serveNFS   # ← gnfsd export 경로로 수정
set mountpt = /nfstest

# NFS options, all classic 4.2BSD/SunOS NFS flags NeXTSTEP understands.
#   hard    - on timeout RETRY instead of failing the I/O. This is the
#             important one: with `soft` a single delayed reply during a
#             build or tar copy fails the operation outright, and the
#             failed mount tends to be left wedged (later remounts are
#             refused with "Device busy").
#   intr    - so a hard mount can still be interrupted with Ctrl-C if
#             gnfsd really is gone.
#   timeo   - initial timeout in TENTHS of a second (30 = 3.0s). The old
#             value of 10 (1.0s) gave up before a busy server or a lossy
#             vintage NIC could answer.
#   retrans - retransmits before escalating (with `hard` it keeps going).
set opts = "hard,intr,timeo=30,retrans=5"

# `noac` disables the client attribute cache so host-side edits show up
# immediately - but it multiplies GETATTR traffic, which is exactly what
# hurts during a build. It is therefore OPT-IN: pass -n when you are
# actively editing on the host and need instant visibility.
set noac = 0

# ---- argument parsing ----
set unmount_only = 0
if ("$1" == "-u" || "$1" == "unmount") then
    set unmount_only = 1
else if ("$1" == "-n") then
    set noac = 1
    if ("$2" != "") set server  = "$2"
    if ("$3" != "") set export   = "$3"
    if ("$4" != "") set mountpt = "$4"
else
    if ("$1" != "") set server  = "$1"
    if ("$2" != "") set export   = "$2"
    if ("$3" != "") set mountpt = "$3"
endif
if ($noac) set opts = "${opts},noac"

# ---- must be root ----
# NeXTSTEP `id' has no -u; whoami is the portable check here.
set me = `whoami`
if ("$me" != "root") then
    echo "next-mount: must run as root (mount needs privilege)."
    exit 1
endif

# ---- always unmount first ----
# This is also how you refresh the view after editing on the host: just
# run the script again. A mount left wedged by an earlier failure does
# not go away with a plain umount and then blocks the remount with
# "Device busy", so try the directory form, then the server:export form,
# then report what is still holding it.
umount $mountpt >& /dev/null
if ($status != 0) then
    umount ${server}:${export} >& /dev/null
endif
mount |& grep $mountpt >& /dev/null
if ($status == 0) then
    echo "next-mount: $mountpt is still mounted (busy)."
    echo "  something is using it - check with:  ps -ax"
    echo "  a shell sitting in $mountpt is the usual cause; cd out of it."
    if (! $unmount_only) then
        echo "  not remounting while busy."
        exit 1
    endif
endif

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
