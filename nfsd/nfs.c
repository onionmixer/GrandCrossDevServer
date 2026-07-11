/*
 * nfs.c - portmapper (GETPORT), MOUNT v1 and NFS v2 procedures.
 * RFC 1094 (NFS/MOUNT), RFC 1057 (RPC). Linux host tool.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "nfs.h"
#include "handle.h"

/* ---- NFSv2 status codes ---- */
#define NFS_OK            0
#define NFSERR_PERM       1
#define NFSERR_NOENT      2
#define NFSERR_IO         5
#define NFSERR_ACCES     13
#define NFSERR_EXIST     17
#define NFSERR_NOTDIR    20
#define NFSERR_ISDIR     21
#define NFSERR_NOSPC     28
#define NFSERR_NAMETOOLONG 63
#define NFSERR_NOTEMPTY  66
#define NFSERR_STALE     70

/* ftype */
#define NFNON 0
#define NFREG 1
#define NFDIR 2
#define NFBLK 3
#define NFCHR 4
#define NFLNK 5

#define NAMEMAX 255

static char g_root[1024];
static int  g_nfs_port;         /* where mount+nfs are served */

void svc_config(const char *export_root, int nfs_port)
{
    size_t n;
    strncpy(g_root, export_root, sizeof(g_root) - 1);
    g_root[sizeof(g_root) - 1] = '\0';
    n = strlen(g_root);
    while (n > 1 && g_root[n - 1] == '/')
        g_root[--n] = '\0';
    g_nfs_port = nfs_port;
}

/* map errno to an NFS status */
static uint32_t errno_to_nfs(int e)
{
    switch (e) {
    case 0:        return NFS_OK;
    case EPERM:    return NFSERR_PERM;
    case ENOENT:   return NFSERR_NOENT;
    case EACCES:   return NFSERR_ACCES;
    case EEXIST:   return NFSERR_EXIST;
    case ENOTDIR:  return NFSERR_NOTDIR;
    case EISDIR:   return NFSERR_ISDIR;
    case ENOSPC:   return NFSERR_NOSPC;
    case ENAMETOOLONG: return NFSERR_NAMETOOLONG;
    case ENOTEMPTY: return NFSERR_NOTEMPTY;
    default:       return NFSERR_IO;
    }
}

/* join dir + "/" + name into out, clamped to the export root for
   "..". Rejects names with '/'. returns 0 ok, -1 bad name. */
static int join_child(const char *dir, const char *name,
                      char *out, size_t max)
{
    if (name[0] == '\0' || strchr(name, '/') != NULL)
        return -1;
    if (strcmp(name, ".") == 0) {
        if (strlen(dir) >= max)
            return -1;
        strcpy(out, dir);
        return 0;
    }
    if (strcmp(name, "..") == 0) {
        char *slash;
        if (strcmp(dir, g_root) == 0) {     /* clamp at export root */
            strcpy(out, dir);
            return 0;
        }
        if (strlen(dir) >= max)
            return -1;
        strcpy(out, dir);
        slash = strrchr(out, '/');
        if (slash != NULL && slash != out)
            *slash = '\0';
        return 0;
    }
    if (strlen(dir) + 1 + strlen(name) >= max)
        return -1;
    sprintf(out, "%s/%s", dir, name);
    return 0;
}

static int ftype_of(mode_t m)
{
    if (S_ISREG(m))  return NFREG;
    if (S_ISDIR(m))  return NFDIR;
    if (S_ISBLK(m))  return NFBLK;
    if (S_ISCHR(m))  return NFCHR;
    if (S_ISLNK(m))  return NFLNK;
    return NFNON;
}

/* encode NFSv2 fattr from a stat */
static int put_fattr(xdr_t *o, const struct stat *st)
{
    xdr_put_u32(o, (uint32_t)ftype_of(st->st_mode));
    xdr_put_u32(o, (uint32_t)st->st_mode);   /* mode incl format bits */
    xdr_put_u32(o, (uint32_t)st->st_nlink);
    xdr_put_u32(o, (uint32_t)st->st_uid);
    xdr_put_u32(o, (uint32_t)st->st_gid);
    xdr_put_u32(o, (uint32_t)st->st_size);
    xdr_put_u32(o, (uint32_t)(st->st_blksize ? st->st_blksize : 4096));
    xdr_put_u32(o, (uint32_t)st->st_rdev);
    xdr_put_u32(o, (uint32_t)st->st_blocks);
    xdr_put_u32(o, (uint32_t)st->st_dev);        /* fsid */
    xdr_put_u32(o, (uint32_t)st->st_ino);        /* fileid */
    /* report sub-second times (nsec -> usec) so clients can tell a
       file changed even within the same second - matters for rapid
       edit/rebuild cycles that a whole-second mtime would hide */
    xdr_put_u32(o, (uint32_t)st->st_atime);
    xdr_put_u32(o, (uint32_t)(st->st_atim.tv_nsec / 1000));
    xdr_put_u32(o, (uint32_t)st->st_mtime);
    xdr_put_u32(o, (uint32_t)(st->st_mtim.tv_nsec / 1000));
    xdr_put_u32(o, (uint32_t)st->st_ctime);
    xdr_put_u32(o, (uint32_t)(st->st_ctim.tv_nsec / 1000));
    return o->err ? -1 : 0;
}

/* read a 32-byte fh from `in` and resolve; sets *path, or writes a
   stale-status word to `out` and returns -1. */
static int arg_fh(xdr_t *in, xdr_t *out, const char **path)
{
    unsigned char fh[NFS_FHSIZE];
    const char *p;

    if (xdr_get_fixed(in, fh, NFS_FHSIZE) < 0) {
        xdr_put_u32(out, NFSERR_STALE);
        return -1;
    }
    p = fh_to_path(fh);
    if (p == NULL) {
        xdr_put_u32(out, NFSERR_STALE);
        return -1;
    }
    *path = p;
    return 0;
}

/* status + fattr(path) helper */
static int reply_stat_fattr(xdr_t *out, const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    xdr_put_u32(out, NFS_OK);
    return put_fattr(out, &st);
}

/* ================= PORTMAP ================= */

static int pmap_null(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; (void)in; (void)out; return 0;
}

static int pmap_getport(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    uint32_t prog, vers, prot, port;
    uint32_t result = 0;
    (void)c; (void)vers;

    if (xdr_get_u32(in, &prog) < 0 || xdr_get_u32(in, &vers) < 0 ||
        xdr_get_u32(in, &prot) < 0 || xdr_get_u32(in, &port) < 0)
        return -1;
    if (prog == MOUNT_PROG || prog == NFS_PROG)
        result = (uint32_t)g_nfs_port;   /* mount+nfs port (e.g. 2049) */
    xdr_put_u32(out, result);
    return 0;
}

/* ================= MOUNT v1 ================= */

static int mnt_null(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; (void)in; (void)out; return 0;
}

static int mnt_mnt(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    char dir[1024];
    unsigned char fh[NFS_FHSIZE];
    struct stat st;
    (void)c;

    if (xdr_get_string(in, dir, sizeof(dir)) < 0)
        return -1;
    /* only the exact export root (or the root itself) is served */
    if (strcmp(dir, g_root) != 0) {
        xdr_put_u32(out, NFSERR_ACCES);
        return 0;
    }
    if (stat(g_root, &st) < 0 || !S_ISDIR(st.st_mode)) {
        xdr_put_u32(out, NFSERR_NOTDIR);
        return 0;
    }
    if (fh_from_path(g_root, fh) < 0) {
        xdr_put_u32(out, NFSERR_IO);
        return 0;
    }
    xdr_put_u32(out, 0);                    /* status OK */
    xdr_put_fixed(out, fh, NFS_FHSIZE);
    return 0;
}

static int mnt_umnt(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    char dir[1024];
    (void)c; (void)out;
    xdr_get_string(in, dir, sizeof(dir));   /* no reply body */
    return 0;
}

static int mnt_export(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; (void)in;
    /* one export: {dir, groups=empty}, then list terminator */
    xdr_put_u32(out, 1);                    /* value follows */
    xdr_put_bytes(out, g_root, (uint32_t)strlen(g_root));
    xdr_put_u32(out, 0);                    /* no groups */
    xdr_put_u32(out, 0);                    /* end of list */
    return 0;
}

/* ================= NFS v2 ================= */

static int nfs_null(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; (void)in; (void)out; return 0;
}

static int nfs_getattr(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    (void)c;
    if (arg_fh(in, out, &path) < 0)
        return 0;
    return reply_stat_fattr(out, path);
}

static int nfs_setattr(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    uint32_t mode, uid, gid, size;
    uint32_t asec, ausec, msec, musec;
    (void)c;
    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (xdr_get_u32(in, &mode) < 0 || xdr_get_u32(in, &uid) < 0 ||
        xdr_get_u32(in, &gid) < 0 || xdr_get_u32(in, &size) < 0 ||
        xdr_get_u32(in, &asec) < 0 || xdr_get_u32(in, &ausec) < 0 ||
        xdr_get_u32(in, &msec) < 0 || xdr_get_u32(in, &musec) < 0)
        return -1;
    if (mode != 0xffffffffUL) {
        if (chmod(path, mode & 07777) < 0)
            { /* best effort */ }
    }
    if (size != 0xffffffffUL) {
        if (truncate(path, (off_t)size) < 0)
            { /* best effort */ }
    }
    /* uid/gid/times ignored: no fine permission control (by design) */
    return reply_stat_fattr(out, path);
}

static int nfs_lookup(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *dir;
    char name[NAMEMAX + 1];
    char child[1024];
    unsigned char fh[NFS_FHSIZE];
    struct stat st;
    (void)c;
    if (arg_fh(in, out, &dir) < 0)
        return 0;
    if (xdr_get_string(in, name, sizeof(name)) < 0)
        return -1;
    if (join_child(dir, name, child, sizeof(child)) < 0) {
        xdr_put_u32(out, NFSERR_NOENT);
        return 0;
    }
    if (lstat(child, &st) < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    if (fh_from_path(child, fh) < 0) {
        xdr_put_u32(out, NFSERR_IO);
        return 0;
    }
    xdr_put_u32(out, NFS_OK);
    xdr_put_fixed(out, fh, NFS_FHSIZE);
    return put_fattr(out, &st);
}

static int nfs_read(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    uint32_t offset, count, total;
    static unsigned char data[8192];
    struct stat st;
    int fd;
    ssize_t got;
    (void)c; (void)total;
    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (xdr_get_u32(in, &offset) < 0 || xdr_get_u32(in, &count) < 0 ||
        xdr_get_u32(in, &total) < 0)
        return -1;
    if (count > sizeof(data))
        count = sizeof(data);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    got = pread(fd, data, count, (off_t)offset);
    if (got < 0) {
        close(fd);
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    fstat(fd, &st);
    close(fd);
    xdr_put_u32(out, NFS_OK);
    put_fattr(out, &st);
    xdr_put_bytes(out, data, (uint32_t)got);
    return 0;
}

static int nfs_write(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    uint32_t begin, offset, total;
    const unsigned char *data;
    uint32_t len;
    struct stat st;
    int fd;
    ssize_t put;
    (void)c; (void)begin; (void)total;
    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (xdr_get_u32(in, &begin) < 0 || xdr_get_u32(in, &offset) < 0 ||
        xdr_get_u32(in, &total) < 0)
        return -1;
    if (xdr_get_bytes(in, &data, &len) < 0)
        return -1;
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    put = pwrite(fd, data, len, (off_t)offset);
    if (put < 0) {
        close(fd);
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    fstat(fd, &st);
    close(fd);
    xdr_put_u32(out, NFS_OK);
    return put_fattr(out, &st);
}

/* CREATE / MKDIR share the {dirfh, name, sattr} arg shape */
static int create_common(xdr_t *in, xdr_t *out, int dir_mode)
{
    const char *dir;
    char name[NAMEMAX + 1];
    char child[1024];
    unsigned char fh[NFS_FHSIZE];
    uint32_t mode, uid, gid, size, a1, a2, m1, m2;
    struct stat st;
    int rc;

    if (arg_fh(in, out, &dir) < 0)
        return 0;
    if (xdr_get_string(in, name, sizeof(name)) < 0)
        return -1;
    if (xdr_get_u32(in, &mode) < 0 || xdr_get_u32(in, &uid) < 0 ||
        xdr_get_u32(in, &gid) < 0 || xdr_get_u32(in, &size) < 0 ||
        xdr_get_u32(in, &a1) < 0 || xdr_get_u32(in, &a2) < 0 ||
        xdr_get_u32(in, &m1) < 0 || xdr_get_u32(in, &m2) < 0)
        return -1;
    if (join_child(dir, name, child, sizeof(child)) < 0) {
        xdr_put_u32(out, NFSERR_ACCES);
        return 0;
    }
    if (mode == 0xffffffffUL)
        mode = dir_mode ? 0755 : 0644;
    if (dir_mode)
        rc = mkdir(child, mode & 07777);
    else {
        int fd = open(child, O_CREAT | O_WRONLY | O_TRUNC,
                      mode & 07777);
        if (fd >= 0)
            close(fd);
        rc = (fd >= 0) ? 0 : -1;
    }
    if (rc < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    if (lstat(child, &st) < 0 || fh_from_path(child, fh) < 0) {
        xdr_put_u32(out, NFSERR_IO);
        return 0;
    }
    xdr_put_u32(out, NFS_OK);
    xdr_put_fixed(out, fh, NFS_FHSIZE);
    return put_fattr(out, &st);
}

static int nfs_create(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; return create_common(in, out, 0);
}
static int nfs_mkdir(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; return create_common(in, out, 1);
}

/* REMOVE / RMDIR share {dirfh, name} -> status */
static int remove_common(xdr_t *in, xdr_t *out, int is_dir)
{
    const char *dir;
    char name[NAMEMAX + 1];
    char child[1024];
    int rc;

    if (arg_fh(in, out, &dir) < 0)
        return 0;
    if (xdr_get_string(in, name, sizeof(name)) < 0)
        return -1;
    if (join_child(dir, name, child, sizeof(child)) < 0) {
        xdr_put_u32(out, NFSERR_ACCES);
        return 0;
    }
    rc = is_dir ? rmdir(child) : unlink(child);
    xdr_put_u32(out, rc < 0 ? errno_to_nfs(errno) : NFS_OK);
    return 0;
}

static int nfs_remove(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; return remove_common(in, out, 0);
}
static int nfs_rmdir(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    (void)c; return remove_common(in, out, 1);
}

static int nfs_rename(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *fromdir, *todir;
    char fname[NAMEMAX + 1], tname[NAMEMAX + 1];
    char fpath[1024], tpath[1024];
    unsigned char fh[NFS_FHSIZE];
    const char *tp;
    int rc;
    (void)c;

    if (arg_fh(in, out, &fromdir) < 0)
        return 0;
    if (xdr_get_string(in, fname, sizeof(fname)) < 0)
        return -1;
    {   /* second fh, resolved into a stable interned pointer */
        if (xdr_get_fixed(in, fh, NFS_FHSIZE) < 0)
            return -1;
        tp = fh_to_path(fh);
        if (tp == NULL) {
            xdr_put_u32(out, NFSERR_STALE);
            return 0;
        }
        todir = tp;
    }
    if (xdr_get_string(in, tname, sizeof(tname)) < 0)
        return -1;
    if (join_child(fromdir, fname, fpath, sizeof(fpath)) < 0 ||
        join_child(todir, tname, tpath, sizeof(tpath)) < 0) {
        xdr_put_u32(out, NFSERR_ACCES);
        return 0;
    }
    rc = rename(fpath, tpath);
    xdr_put_u32(out, rc < 0 ? errno_to_nfs(errno) : NFS_OK);
    return 0;
}

static int nfs_readdir(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    uint32_t cookie, count;
    DIR *d;
    struct dirent *de;
    long idx;
    long emitted;
    (void)c; (void)count;

    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (xdr_get_u32(in, &cookie) < 0 || xdr_get_u32(in, &count) < 0)
        return -1;
    d = opendir(path);
    if (d == NULL) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    xdr_put_u32(out, NFS_OK);

    /* cookie = 1-based index of the last entry already returned. we
       skip that many, then emit until the reply nears the UDP cap.
       O(n^2) over calls but fine for a lightweight server. entries
       skipped for size are re-read next call (cookie < their idx). */
    idx = 0;
    emitted = 0;
    (void)emitted;
    while ((de = readdir(d)) != NULL) {
        struct stat st;
        char child[1024];
        size_t need_bytes;

        idx++;
        if ((uint32_t)idx <= cookie)
            continue;
        need_bytes = 4 + 4 + 4 + strlen(de->d_name) + 4 + 4;
        if (xdr_len(out) + need_bytes > 7500)
            break;                          /* stop; not eof */
        if (join_child(path, de->d_name, child, sizeof(child)) < 0)
            continue;
        if (lstat(child, &st) < 0)
            continue;
        xdr_put_u32(out, 1);                        /* value follows */
        xdr_put_u32(out, (uint32_t)st.st_ino);      /* fileid */
        xdr_put_bytes(out, de->d_name,
                      (uint32_t)strlen(de->d_name)); /* name */
        xdr_put_u32(out, (uint32_t)idx);            /* cookie */
        emitted++;
    }
    xdr_put_u32(out, 0);                    /* no more entries */
    xdr_put_u32(out, (de == NULL) ? 1 : 0);/* eof */
    closedir(d);
    return 0;
}

static int nfs_statfs(rpc_call_t *c, xdr_t *in, xdr_t *out)
{
    const char *path;
    struct statvfs vf;
    (void)c;
    if (arg_fh(in, out, &path) < 0)
        return 0;
    if (statvfs(path, &vf) < 0) {
        xdr_put_u32(out, errno_to_nfs(errno));
        return 0;
    }
    xdr_put_u32(out, NFS_OK);
    xdr_put_u32(out, 8192);                         /* tsize */
    xdr_put_u32(out, (uint32_t)vf.f_bsize);         /* bsize */
    xdr_put_u32(out, (uint32_t)vf.f_blocks);        /* blocks */
    xdr_put_u32(out, (uint32_t)vf.f_bfree);         /* bfree */
    xdr_put_u32(out, (uint32_t)vf.f_bavail);        /* bavail */
    return 0;
}

/* ================= routes ================= */

/* last field: cache=1 for non-idempotent ops (dup-request cache) */
const rpc_route_t GN_ROUTES[] = {
    { PMAP_PROG,  PMAP_VERS,  0, pmap_null,     0 },
    { PMAP_PROG,  PMAP_VERS,  3, pmap_getport,  0 },

    { MOUNT_PROG, MOUNT_VERS, 0, mnt_null,      0 },
    { MOUNT_PROG, MOUNT_VERS, 1, mnt_mnt,       0 },
    { MOUNT_PROG, MOUNT_VERS, 3, mnt_umnt,      0 },
    { MOUNT_PROG, MOUNT_VERS, 5, mnt_export,    0 },

    { NFS_PROG,   NFS_VERS,   0, nfs_null,      0 },
    { NFS_PROG,   NFS_VERS,   1, nfs_getattr,   0 },
    { NFS_PROG,   NFS_VERS,   2, nfs_setattr,   1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,   4, nfs_lookup,    0 },
    { NFS_PROG,   NFS_VERS,   6, nfs_read,      0 },
    { NFS_PROG,   NFS_VERS,   8, nfs_write,     1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,   9, nfs_create,    1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,  10, nfs_remove,    1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,  11, nfs_rename,    1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,  14, nfs_mkdir,     1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,  15, nfs_rmdir,     1 },  /* non-idempotent */
    { NFS_PROG,   NFS_VERS,  16, nfs_readdir,   0 },
    { NFS_PROG,   NFS_VERS,  17, nfs_statfs,    0 }
};
const int GN_NROUTES = (int)(sizeof(GN_ROUTES) / sizeof(GN_ROUTES[0]));
