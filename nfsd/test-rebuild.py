"""test-rebuild.py GNFSD PORT - restart cost must grow linearly with the tree.

After a restart the first old handle makes gnfsd walk the export to
re-resolve it (handle.c rebuild). That walk used to be quadratic (each
path was interned by a linear scan) - hours on a multi-million entry
export, with the server deaf meanwhile. Time it on an N and a 4N tree:
linear gives ~4x, quadratic ~16x. Only gnfsd instances started here are
killed, by their own PID (never pkill - see test-persist.sh).
"""
import os, shutil, socket, struct, subprocess, sys, tempfile, time

GNFSD, PORT = sys.argv[1], int(sys.argv[2])
SMALL, PER = 20000, 500


def make_tree(root, n):
    """n entries in dirs of PER files; returns the last file (walked last)"""
    made, d, last = 0, 0, None
    while made < n:
        dn = "dir%05d" % d
        os.mkdir(os.path.join(root, dn))
        made += 1
        d += 1
        for f in range(min(PER, n - made)):
            last = "%s/file%04d.c" % (dn, f)
            open(os.path.join(root, last), "w").close()
            made += 1
    return last


class Client:
    def __init__(self):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.xid = int(time.time()) & 0xFFFFFF

    def call(self, prog, proc, args):
        # retransmit like a real client: an old-handle request is dropped
        # until the walk reaches it. A short interval keeps the timing fine.
        self.xid += 1
        m = struct.pack(">IIIIII", self.xid, 0, 2, prog, 1 if prog == 100005 else 2,
                        proc) + struct.pack(">II", 0, 0) * 2 + args
        end = time.time() + 120
        while True:
            self.s.sendto(m, ("127.0.0.1", PORT))
            self.s.settimeout(0.01)
            try:
                d, _ = self.s.recvfrom(65536)
                if struct.unpack(">I", d[:4])[0] == self.xid:
                    break
            except socket.timeout:
                if time.time() > end:
                    raise
        vl = struct.unpack(">I", d[16:20])[0]
        r = d[24 + vl:]
        return struct.unpack(">I", r[:4])[0], r[4:]


def sstr(v):
    b = v.encode()
    return struct.pack(">I", len(b)) + b + b"\0" * ((4 - len(b) % 4) % 4)


def start(export):
    p = subprocess.Popen([GNFSD, "-p", str(PORT), "-n", str(PORT), export],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)
    if p.poll() is not None:
        sys.exit("gnfsd failed to start")
    return p


def stop(p):
    p.kill()
    p.wait()


def restart_cost(n):
    root = tempfile.mkdtemp(prefix="gn-rebuild-")
    try:
        last = make_tree(root, n)
        p = start(root)
        try:
            c = Client()
            st, r = c.call(100005, 1, sstr(root))
            assert st == 0, "MOUNT"
            fh = r[:32]
            for comp in last.split("/"):
                st, r = c.call(100003, 4, fh + sstr(comp))
                assert st == 0, "LOOKUP " + comp
                fh = r[:32]
        finally:
            stop(p)
        want = os.lstat(os.path.join(root, last)).st_ino & 0xFFFFFFFF
        best, ok = None, True
        for _ in range(3):                   # best of 3: damp timing noise
            p = start(root)
            try:
                t = time.time()
                st, r = Client().call(100003, 1, fh)    # GETATTR, old handle
                dt = time.time() - t
            finally:
                stop(p)
            ino = struct.unpack(">I", r[40:44])[0] if st == 0 else None
            ok = ok and st == 0 and ino == want
            best = dt if best is None else min(best, dt)
        return best, ok
    finally:
        shutil.rmtree(root)


t1, ok1 = restart_cost(SMALL)
t4, ok4 = restart_cost(SMALL * 4)
ratio = t4 / max(t1, 1e-3)
print("restart walk: %d entries %.3fs, %d entries %.3fs (x%.1f)"
      % (SMALL, t1, SMALL * 4, t4, ratio))
fail = 0
for ok, what in ((ok1 and ok4, "old handle resolves to the same file after restart"),
                 (ratio < 8, "restart walk grows ~linearly (quadratic would be ~16x)")):
    print(("  PASS " if ok else "  FAIL ") + what)
    fail += not ok
sys.exit(1 if fail else 0)
