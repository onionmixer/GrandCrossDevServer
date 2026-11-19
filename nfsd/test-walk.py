"""test-walk.py GNFSD PORT - gnfsd keeps answering while it walks.

After a restart an unknown old handle starts the export walk
(handle.c). The walk runs in slices between requests; a request whose
handle only the walk can resolve is dropped unanswered and the client's
retransmit (same socket, same xid, same bytes - like a real NFS client)
gets the answer. Checks (PLAN_handle_rebuild_fix.md 6, stage 2):

  - new MOUNT/LOOKUP/GETATTR are answered at once during the walk,
    even while a steady stream of them keeps arriving, and the walk
    still finishes (no starvation either way)
  - a dropped old-handle request is answered on retransmit: OK for a
    live path, STALE for a deleted one (only after the walk ends)
  - LINK / RENAME with only one of their two handles waiting, in both
    orders: dropped first, then done exactly once

Only gnfsd instances started here are killed, by their own PID.
"""
import os, select, shutil, socket, struct, subprocess, sys, tempfile, time

GNFSD, PORT = sys.argv[1], int(sys.argv[2])
NDIRS, PER = 400, 500                 # ~200k entries
MOUNT, NFS = 100005, 100003
OK, STALE = 0, 70
RETRY = 0.1                           # client retransmit interval (s)

fails = 0


def check(ok, what):
    global fails
    print(("  PASS " if ok else "  FAIL ") + what)
    fails += not ok


def sstr(v):
    b = v.encode()
    return struct.pack(">I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)


_xid = [int(time.time() * 1000) & 0xFFFFFF]


def msg(prog, proc, args):
    _xid[0] += 1
    return _xid[0], (struct.pack(">IIIIII", _xid[0], 0, 2, prog,
                                 1 if prog == MOUNT else 2, proc)
                     + struct.pack(">II", 0, 0) * 2 + args)


def body(d):
    vl = struct.unpack(">I", d[16:20])[0]
    r = d[24 + vl:]
    return struct.unpack(">I", r[:4])[0], r[4:]


class Retrier:
    """one request, retransmitted with the same xid until answered"""

    def __init__(self, prog, proc, args):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.xid, self.m = msg(prog, proc, args)
        self.sends, self.reply, self.t0 = 0, None, time.time()
        self.last = 0.0
        self.poke()

    def poke(self):
        if self.reply is None and time.time() - self.last >= RETRY:
            self.s.sendto(self.m, ("127.0.0.1", PORT))
            self.sends += 1
            self.last = time.time()

    def ready(self):
        while select.select([self.s], [], [], 0)[0]:
            d, _ = self.s.recvfrom(65536)
            if struct.unpack(">I", d[:4])[0] == self.xid:
                self.reply = body(d)
                self.t1 = time.time()
        return self.reply is not None

    def wait(self, limit=60):
        end = time.time() + limit
        while not self.ready() and time.time() < end:
            select.select([self.s], [], [], RETRY / 4)
            self.poke()
        return self.reply


def call(prog, proc, args, limit=5):
    r = Retrier(prog, proc, args)
    return r.wait(limit)


def mount(root):
    st, r = call(MOUNT, 1, sstr(root))
    assert st == OK, "MOUNT"
    return r[:32]


def lookup(fh, rel):
    for comp in rel.split("/"):
        st, r = call(NFS, 4, fh + sstr(comp))
        assert st == OK, "LOOKUP " + rel
        fh = r[:32]
    return fh


class Server:
    def __init__(self, export):
        self.p = subprocess.Popen([GNFSD, "-p", str(PORT), "-n", str(PORT), export],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.4)
        if self.p.poll() is not None:
            sys.exit("gnfsd failed to start")

    def stop(self):
        self.p.kill()
        self.p.wait()


root = tempfile.mkdtemp(prefix="gn-walk-")
srv = None
try:
    for d in range(NDIRS):
        dp = os.path.join(root, "dir%05d" % d)
        os.mkdir(dp)
        for f in range(PER):
            open(os.path.join(dp, "f%04d" % f), "w").close()
    for name in ("a", "b", "c", "d"):
        os.mkdir(os.path.join(root, "op" + name))
        open(os.path.join(root, "op" + name, "src"), "w").close()
    open(os.path.join(root, "dir00007", "gone"), "w").close()

    # --- lifetime 1: a client collects handles
    srv = Server(root)
    rfh = mount(root)
    old = {rel: lookup(rfh, rel) for rel in
           ("dir00007/gone", "dir00399/f0499", "dir00200",
            "opa", "opb", "opc", "opd", "opa/src", "opb/src", "opc/src", "opd/src")}
    srv.stop()
    os.unlink(os.path.join(root, "dir00007", "gone"))

    # --- lifetime 2: responsiveness during the walk
    print("walk vs. a steady stream of new requests")
    srv = Server(root)
    marker = Retrier(NFS, 1, old["dir00007/gone"])  # starts the walk
    lat, n = [], 0

    def timed(f, *a):
        t = time.time()
        v = f(*a)
        lat.append(time.time() - t)                  # each request alone
        return v
    while not marker.ready():
        fh = timed(mount, root)
        fh = timed(lookup, fh, "dir00123")
        fh = timed(lookup, fh, "f0042")
        timed(call, NFS, 1, fh)
        n += 1
        marker.poke()
    walk = marker.t1 - marker.t0
    print("  walk %.2fs; %d new MOUNT+LOOKUP+LOOKUP+GETATTR rounds meanwhile, "
          "worst request %.1f ms" % (walk, n, max(lat or [0]) * 1000))
    check(marker.reply[0] == STALE, "deleted path: STALE, only once the walk ended")
    check(marker.sends > 1, "first send was dropped, a retransmit answered")
    check(n >= 3, "new requests were served during the walk")
    check(max(lat or [9]) < 0.25, "new requests answered promptly during the walk")
    st, r = call(NFS, 1, old["dir00399/f0499"])
    ino = os.lstat(os.path.join(root, "dir00399/f0499")).st_ino & 0xFFFFFFFF
    check(st == OK and struct.unpack(">I", r[40:44])[0] == ino,
          "live old handle resolves to the same file")
    forged = old["dir00399/f0499"][:12] + b"\x12\x34\x56\x78\x9a\xbc\xde\xf0" + old["dir00399/f0499"][20:]
    st, _ = call(NFS, 1, forged)
    check(st == STALE, "unknown handle after the walk: STALE at once")
    srv.stop()

    # --- lifetimes 3-6: two-handle ops with one handle waiting
    print("LINK / RENAME with one of two handles waiting")
    # the waiting handle must name a path nobody has looked up in this
    # lifetime, or the fresh LOOKUP would have interned it already
    def j(*a):
        return os.path.join(root, *a)
    cases = (
        ("RENAME, 1st handle waiting", 11,
         lambda: old["opa"] + sstr("src") + lookup(mount(root), "dir00001") + sstr("moved_a"),
         lambda: not os.path.exists(j("opa", "src")) and os.path.exists(j("dir00001", "moved_a"))),
        ("RENAME, 2nd handle waiting", 11,
         lambda: lookup(mount(root), "opb") + sstr("src") + old["dir00200"] + sstr("moved_b"),
         lambda: not os.path.exists(j("opb", "src")) and os.path.exists(j("dir00200", "moved_b"))),
        ("LINK, 1st handle waiting", 12,
         lambda: old["opc/src"] + lookup(mount(root), "dir00002") + sstr("linked_c"),
         lambda: os.path.exists(j("dir00002", "linked_c")) and os.lstat(j("opc", "src")).st_nlink == 2),
        ("LINK, 2nd handle waiting", 12,
         lambda: lookup(mount(root), "opd/src") + old["dir00200"] + sstr("linked_d"),
         lambda: os.path.exists(j("dir00200", "linked_d")) and os.lstat(j("opd", "src")).st_nlink == 2),
    )
    for what, proc, args, done in cases:
        srv = Server(root)
        r = Retrier(NFS, proc, args())
        r.wait()
        check(r.reply is not None and r.reply[0] == OK and done() and r.sends > 1,
              "%s: dropped, then done once (status %s, %d sends)"
              % (what, r.reply and r.reply[0], r.sends))
        srv.stop()
finally:
    if srv is not None and srv.p.poll() is None:
        srv.stop()
    shutil.rmtree(root)

print("== ALL PASS ==" if fails == 0 else "== %d FAIL ==" % fails)
sys.exit(1 if fails else 0)
