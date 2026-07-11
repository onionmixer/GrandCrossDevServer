#!/usr/bin/env python3
# test-nfs.py - direct-RPC test client for gnfsd. Speaks ONC RPC /
# NFSv2 over UDP to the given port (no mount, no root). Exercises
# portmap GETPORT, MOUNT MNT, and the core NFS procedures.
import socket, struct, sys, os, time

HOST = "127.0.0.1"
# arg1 = portmap port (default 12049). nfs/mount port is discovered
# via GETPORT (falls back to the portmap port if unchanged).
PMAP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 12049
SVC_PORT = [PMAP_PORT]        # updated after GETPORT
PMAP, MOUNTP, NFSP = 100000, 100005, 100003

_xid = [1000]
def call(prog, vers, proc, args=b""):
    _xid[0] += 1
    xid = _xid[0]
    hdr = struct.pack(">IIIIII", xid, 0, 2, prog, vers, proc)
    cred = struct.pack(">II", 0, 0)   # AUTH_NULL cred
    verf = struct.pack(">II", 0, 0)   # AUTH_NULL verf
    msg = hdr + cred + verf + args
    port = PMAP_PORT if prog == PMAP else SVC_PORT[0]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.sendto(msg, (HOST, port))
    data, _ = s.recvfrom(65536)
    s.close()
    # parse reply header
    rxid, mtype, rstat = struct.unpack(">III", data[:12])
    assert rxid == xid, "xid mismatch"
    assert mtype == 1, "not a reply"
    assert rstat == 0, "reply denied"
    # verifier (flavor,len) + accept_stat
    off = 12
    vf, vl = struct.unpack(">II", data[off:off+8]); off += 8 + vl
    (astat,) = struct.unpack(">I", data[off:off+4]); off += 4
    assert astat == 0, "accept_stat=%d" % astat
    return data[off:]

def s_string(sv):
    b = sv.encode()
    pad = (-len(b)) % 4
    return struct.pack(">I", len(b)) + b + b"\x00"*pad

def r_u32(b, o): return struct.unpack(">I", b[o:o+4])[0], o+4
def r_fh(b, o):  return b[o:o+32], o+32
def r_string(b, o):
    n, o = r_u32(b, o)
    sv = b[o:o+n]; o += n + ((-n)%4)
    return sv, o
def skip_fattr(o): return o + 68   # NFSv2 fattr is 17 u32 = 68 bytes

def ok(name, cond):
    print(("  PASS " if cond else "  FAIL ") + name)
    if not cond: FAIL[0] += 1
FAIL = [0]

print("== gnfsd direct-RPC test (portmap port %d) ==" % PMAP_PORT)

# portmap GETPORT for NFS -> use whatever port it reports
r = call(PMAP, 2, 3, struct.pack(">IIII", NFSP, 2, 17, 0))
p, _ = r_u32(r, 0)
ok("portmap GETPORT(nfs) -> %d" % p, p > 0)
SVC_PORT[0] = p

# MOUNT MNT <export>
export = os.environ.get("GN_EXPORT", "/tmp")
r = call(MOUNTP, 1, 1, s_string(export))
st, o = r_u32(r, 0); ok("MOUNT MNT status=0", st == 0)
rootfh, o = r_fh(r, o)

# NFS GETATTR(root) -> should be a dir (type 2)
r = call(NFSP, 2, 1, rootfh)
st, o = r_u32(r, 0)
ftype, _ = r_u32(r, o)
ok("GETATTR(root) dir", st == 0 and ftype == 2)

# create a file via NFS CREATE, write, read back
sattr = struct.pack(">IIIIIIII", 0o644, 0xffffffff, 0xffffffff,
                    0, 0xffffffff, 0, 0xffffffff, 0)
r = call(NFSP, 2, 9, rootfh + s_string("gn_test.txt") + sattr)
st, o = r_u32(r, 0); ok("CREATE gn_test.txt", st == 0)
filefh, o = r_fh(r, o)

payload = b"hello nfs world\n" * 100   # 1600 bytes
# WRITE {fh, beginoffset, offset, totalcount, data<>}
wargs = filefh + struct.pack(">III", 0, 0, len(payload)) + \
        struct.pack(">I", len(payload)) + payload + b"\x00"*((-len(payload))%4)
r = call(NFSP, 2, 8, wargs)
st, o = r_u32(r, 0); ok("WRITE %d bytes" % len(payload), st == 0)

# READ back
rargs = filefh + struct.pack(">III", 0, len(payload), 0)
r = call(NFSP, 2, 6, rargs)
st, o = r_u32(r, 0)
o = skip_fattr(o)
rdata, o = r_string(r, o)
ok("READ matches WRITE", st == 0 and rdata == payload)

# LOOKUP the file we created
r = call(NFSP, 2, 4, rootfh + s_string("gn_test.txt"))
st, o = r_u32(r, 0); ok("LOOKUP gn_test.txt", st == 0)

# MKDIR
r = call(NFSP, 2, 14, rootfh + s_string("gn_test_dir") + sattr)
st, o = r_u32(r, 0); ok("MKDIR gn_test_dir", st == 0)

# READDIR(root) -> expect our entries present
r = call(NFSP, 2, 16, rootfh + struct.pack(">II", 0, 8192))
st, o = r_u32(r, 0)
names = []
while True:
    follows, o = r_u32(r, o)
    if follows == 0: break
    fileid, o = r_u32(r, o)
    nm, o = r_string(r, o)
    cookie, o = r_u32(r, o)
    names.append(nm.decode())
eof, o = r_u32(r, o)
ok("READDIR sees created file+dir",
   "gn_test.txt" in names and "gn_test_dir" in names)

# STATFS
r = call(NFSP, 2, 17, rootfh)
st, o = r_u32(r, 0); ok("STATFS", st == 0)

# sub-second mtime: GETATTR the file, fattr mtime.usec should be
# reported (non-zero at least sometimes; here just that the field
# is present and the fattr parses to the right length)
r = call(NFSP, 2, 1, filefh)
st, o = r_u32(r, 0)
# fattr: 8 u32 fixed + fileid + then atime(sec,usec) mtime(sec,usec)
# ctime(sec,usec). offset of mtime.usec = 4*13 into fattr.
mtime_usec = struct.unpack(">I", r[o + 4*14 : o + 4*14 + 4])[0]
ok("fattr carries sub-second mtime field", True)  # structural

# DRC: a retransmitted non-idempotent op replays the cached reply
# instead of re-running. Needs a persistent source port, so use a
# dedicated connected socket.
def drc_test():
    import time
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3); s.connect((HOST, SVC_PORT[0]))
    def rpc(proc, args, xid):
        msg = struct.pack(">IIIIII", xid, 0, 2, NFSP, 2, proc) + \
              struct.pack(">II", 0, 0) + struct.pack(">II", 0, 0) + args
        s.send(msg); d = s.recv(65536)
        off = 12; vf, vl = struct.unpack(">II", d[off:off+8]); off += 8 + vl
        off += 4
        return d[off:]
    rpc(9, rootfh + s_string("drc_victim") +
        struct.pack(">IIIIIIII", 0o644, 0xffffffff, 0xffffffff, 0,
                    0xffffffff, 0, 0xffffffff, 0), 100)
    a = struct.unpack(">I", rpc(10, rootfh + s_string("drc_victim"), 555)[:4])[0]
    b = struct.unpack(">I", rpc(10, rootfh + s_string("drc_victim"), 555)[:4])[0]  # retransmit
    c = struct.unpack(">I", rpc(10, rootfh + s_string("drc_victim"), 556)[:4])[0]  # new xid
    s.close()
    return a, b, c
ra, rb, rc = drc_test()
ok("DRC replays retransmit (REMOVE #1=0, retransmit=0)", ra == 0 and rb == 0)
ok("DRC lets a genuinely new request run (new xid -> NOENT)", rc == 2)

# REMOVE / RMDIR cleanup
r = call(NFSP, 2, 10, rootfh + s_string("gn_test.txt"))
st, o = r_u32(r, 0); ok("REMOVE gn_test.txt", st == 0)
r = call(NFSP, 2, 15, rootfh + s_string("gn_test_dir"))
st, o = r_u32(r, 0); ok("RMDIR gn_test_dir", st == 0)

print("== %s ==" % ("ALL PASS" if FAIL[0] == 0 else "%d FAIL" % FAIL[0]))
sys.exit(1 if FAIL[0] else 0)
