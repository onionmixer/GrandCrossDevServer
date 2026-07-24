#!/usr/bin/env python3
# test-stress.py - stress driver for gnfsd. `make stress` runs the link
# and write modes against a throwaway instance; burst is a manual tool.
#   burst <port> <n> <export>      - never-await GETATTR burst: measures
#                                    rcvbuf headroom (a synthetic worst
#                                    case - real clients await replies)
#   link  <port> <cycles> <export> - LINK/REMOVE churn + DRC retransmit
#                                    safety (retransmit must replay, not
#                                    re-run, the non-idempotent op)
#   write <port> <mb> <export>     - 8K-block WRITE flood + readback md5
import socket, struct, sys, time, hashlib

HOST = "127.0.0.1"
NFSP = 100003; MOUNTP = 100005

def mk(xid, prog, vers, proc, args=b""):
    return (struct.pack(">IIIIII", xid, 0, 2, prog, vers, proc)
            + struct.pack(">IIII", 0, 0, 0, 0) + args)

_XID = [5000]
def one_call(port, prog, vers, proc, args=b"", timeout=5):
    # unique xid per request: reusing an xid from a reused ephemeral
    # port makes the DRC (correctly) replay instead of execute
    _XID[0] += 1
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.sendto(mk(_XID[0], prog, vers, proc, args), (HOST, port))
    d, _ = s.recvfrom(65536); s.close()
    off = 12
    vf, vl = struct.unpack(">II", d[off:off+8]); off += 8 + vl + 4
    return d[off:]

def s_string(sv):
    b = sv.encode(); pad = (-len(b)) % 4
    return struct.pack(">I", len(b)) + b + b"\x00"*pad

def root_fh(port, export):
    r = one_call(port, MOUNTP, 1, 1, s_string(export))
    st = struct.unpack(">I", r[:4])[0]
    assert st == 0, "MNT failed: %d" % st
    return r[4:36]

def burst(port, n, fh):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8*1024*1024)
    s.settimeout(2)
    t0 = time.time()
    for i in range(n):                      # fire without awaiting
        s.sendto(mk(10000+i, NFSP, 2, 1, fh), (HOST, port))
    t1 = time.time()
    got = 0
    try:
        while got < n:
            s.recvfrom(65536); got += 1
    except socket.timeout:
        pass
    t2 = time.time()
    print("burst: sent=%d replies=%d loss=%.1f%% send=%.2fs total=%.2fs"
          % (n, got, 100.0*(n-got)/n, t1-t0, t2-t0))

def link_churn(port, cycles, fh):
    # base file
    sattr = struct.pack(">IIIIIIII", 0o644, 0xffffffff, 0xffffffff, 0,
                        0xffffffff, 0, 0xffffffff, 0)
    one_call(port, NFSP, 2, 10, fh + s_string("gn_stress_base"))
    r = one_call(port, NFSP, 2, 9, fh + s_string("gn_stress_base") + sattr)
    st = struct.unpack(">I", r[:4])[0]; assert st == 0, "CREATE %d" % st
    r = one_call(port, NFSP, 2, 4, fh + s_string("gn_stress_base"))
    filefh = r[4:36]
    errs = 0
    t0 = time.time()
    for i in range(cycles):
        r = one_call(port, NFSP, 2, 12, filefh + fh + s_string("gn_hl_%d" % i))
        if struct.unpack(">I", r[:4])[0] != 0: errs += 1
        r = one_call(port, NFSP, 2, 10, fh + s_string("gn_hl_%d" % i))
        if struct.unpack(">I", r[:4])[0] != 0: errs += 1
    dt = time.time() - t0
    print("link churn: %d cycles, errs=%d, %.2fs (%.0f op/s)"
          % (cycles, errs, dt, 2*cycles/dt))
    # DRC retransmit safety: same xid LINK twice -> one link only
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(3)
    msg = mk(999999, NFSP, 2, 12, filefh + fh + s_string("gn_hl_drc"))
    s.sendto(msg, (HOST, port)); s.recvfrom(65536)
    s.sendto(msg, (HOST, port)); d, _ = s.recvfrom(65536)  # retransmit
    s.close()
    r = one_call(port, NFSP, 2, 1, filefh)          # GETATTR nlink
    nlink = struct.unpack(">I", r[4+8:4+12])[0]
    print("DRC retransmit: nlink=%d (expect 2 - replay, not re-run)" % nlink)
    one_call(port, NFSP, 2, 10, fh + s_string("gn_hl_drc"))
    one_call(port, NFSP, 2, 10, fh + s_string("gn_stress_base"))
    return errs == 0 and nlink == 2

def write_flood(port, mb, fh):
    sattr = struct.pack(">IIIIIIII", 0o644, 0xffffffff, 0xffffffff, 0,
                        0xffffffff, 0, 0xffffffff, 0)
    one_call(port, NFSP, 2, 10, fh + s_string("gn_stress_w"))
    r = one_call(port, NFSP, 2, 9, fh + s_string("gn_stress_w") + sattr)
    r = one_call(port, NFSP, 2, 4, fh + s_string("gn_stress_w"))
    filefh = r[4:36]
    blk = 8192; nblk = mb*1024*1024 // blk
    h = hashlib.md5(); errs = 0
    t0 = time.time()
    for i in range(nblk):
        data = struct.pack(">I", i) * (blk // 4)
        h.update(data)
        args = (filefh + struct.pack(">III", 0, i*blk, 0)
                + struct.pack(">I", blk) + data)
        r = one_call(port, NFSP, 2, 8, args)
        if struct.unpack(">I", r[:4])[0] != 0: errs += 1
    dt = time.time() - t0
    # readback
    h2 = hashlib.md5()
    for i in range(nblk):
        r = one_call(port, NFSP, 2, 6,
                     filefh + struct.pack(">III", i*blk, blk, 0))
        st = struct.unpack(">I", r[:4])[0]
        assert st == 0
        dlen = struct.unpack(">I", r[4+68:4+72])[0]
        h2.update(r[4+72:4+72+dlen])
    match = h.hexdigest() == h2.hexdigest()
    print("write flood: %dMB, %d writes, errs=%d, %.2fs (%.1f MB/s), md5 %s"
          % (mb, nblk, errs, dt, mb/dt, "MATCH" if match else "MISMATCH"))
    one_call(port, NFSP, 2, 10, fh + s_string("gn_stress_w"))
    return errs == 0 and match

if __name__ == "__main__":
    mode = sys.argv[1]; port = int(sys.argv[2]); arg = int(sys.argv[3])
    export = sys.argv[4]
    fh = root_fh(port, export)
    if mode == "burst": burst(port, arg, fh)
    elif mode == "link": sys.exit(0 if link_churn(port, arg, fh) else 1)
    elif mode == "write": sys.exit(0 if write_flood(port, arg, fh) else 1)
