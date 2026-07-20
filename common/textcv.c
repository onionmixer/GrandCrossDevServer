/*
 * textcv.c - wire text encoding normalization (see textcv.h).
 *
 * Only built for platforms that need it (GCDS_TEXTCV): Win32 codepage
 * conversion and macOS Hangul NFD->NFC composition. Everything else
 * already matches the wire convention and does not link this file.
 *
 * Callers must give txt_out()/txt_in() an output buffer with headroom:
 * conversion can grow the text (CP949 2 bytes -> UTF-8 3 bytes), so the
 * scratch buffer is sized 2x the frame size.
 */
#include "textcv.h"

#ifdef GCDS_TEXTCV

#include <string.h>

#define JOIN_MAX 8192

static int g_active = 0;

int txt_active(void) { return g_active; }

void txt_stream_init(txt_stream_t *s)
{
    s->npend = 0;
}

/* Splice any held-back bytes in front of the new chunk. Returns the
   buffer to read from and sets *len; clears the hold. */
static const char *splice(txt_stream_t *s, const char *in, long n,
                          char *joined, long jmax, long *len)
{
    if (s->npend <= 0) {
        *len = n;
        return in;
    }
    if (s->npend + n > jmax)
        n = jmax - s->npend;
    memcpy(joined, s->pend, (size_t)s->npend);
    memcpy(joined + s->npend, in, (size_t)n);
    *len = s->npend + n;
    s->npend = 0;
    return joined;
}

static void hold(txt_stream_t *s, const char *p, long n)
{
    if (n <= 0)
        return;
    if (n > (long)sizeof(s->pend))
        n = (long)sizeof(s->pend);     /* defensive; cannot happen */
    memcpy(s->pend, p, (size_t)n);
    s->npend = (int)n;
}

/* ---------------------------------------------------------------- */
#if defined(GCDS_WIN32)
/* Windows: local ANSI/console codepage <-> UTF-8 via UTF-16.
   MultiByteToWideChar/WideCharToMultiByte are kernel32 (Win95/NT4+),
   so this adds no dependency. */

#include <windows.h>

#define WBUF_MAX 4096
static WCHAR g_wbuf[WBUF_MAX];
static UINT  g_cp = 0;

void txt_init(const char *mode)
{
    long forced = 0;

    if (mode != NULL && mode[0] != '\0') {
        if (strcmp(mode, "off") == 0) { g_active = 0; g_cp = CP_UTF8; return; }
        if (strcmp(mode, "auto") != 0) {
            const char *p = mode;
            while (*p >= '0' && *p <= '9') { forced = forced * 10 + (*p - '0'); p++; }
        }
    }
    if (forced > 0) {
        g_cp = (UINT)forced;
    } else {
        g_cp = GetConsoleOutputCP();
        if (g_cp == 0)
            g_cp = GetACP();
    }
    /* already UTF-8 -> nothing to do, no per-frame cost */
    g_active = (g_cp != CP_UTF8) ? 1 : 0;
}

/* length of the longest prefix ending on a character boundary; a
   trailing DBCS lead byte with no trail byte is left for the next call */
static long dbcs_complete(const char *buf, long len)
{
    long i = 0, last = 0;
    while (i < len) {
        if (IsDBCSLeadByteEx(g_cp, (BYTE)buf[i])) {
            if (i + 1 >= len)
                break;                  /* split character */
            i += 2;
        } else {
            i += 1;
        }
        last = i;
    }
    return last;
}

long txt_out(txt_stream_t *s, const char *in, long n, char *out, long omax)
{
    static char joined[JOIN_MAX];
    const char *src;
    long srclen, take;
    int wn, on;

    src = splice(s, in, n, joined, (long)sizeof(joined), &srclen);
    take = dbcs_complete(src, srclen);
    hold(s, src + take, srclen - take);
    if (take <= 0)
        return 0;

    wn = MultiByteToWideChar(g_cp, 0, src, (int)take, g_wbuf, WBUF_MAX);
    if (wn <= 0)
        return -1;
    on = WideCharToMultiByte(CP_UTF8, 0, g_wbuf, wn, out, (int)omax,
                             NULL, NULL);
    if (on <= 0)
        return -1;
    return (long)on;
}

/* end of stream: emit whatever is still held (best effort) */
long txt_flush(txt_stream_t *s, char *out, long omax)
{
    long o;
    if (s->npend <= 0)
        return 0;
    {
        int wn = MultiByteToWideChar(g_cp, 0, s->pend, s->npend,
                                     g_wbuf, WBUF_MAX);
        int on;
        if (wn <= 0) { s->npend = 0; return 0; }
        on = WideCharToMultiByte(CP_UTF8, 0, g_wbuf, wn, out, (int)omax,
                                 NULL, NULL);
        o = (on > 0) ? (long)on : 0;
    }
    s->npend = 0;
    return o;
}

long txt_in(const char *in, long n, char *out, long omax)
{
    int wn, on;

    if (n <= 0)
        return 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, in, (int)n, g_wbuf, WBUF_MAX);
    if (wn <= 0)
        return -1;
    on = WideCharToMultiByte(g_cp, 0, g_wbuf, wn, out, (int)omax,
                             NULL, NULL);
    if (on <= 0)
        return -1;
    return (long)on;
}

/* ---------------------------------------------------------------- */
#else   /* __APPLE__ : macOS Hangul NFD -> NFC */

/* HFS+ stores names decomposed and APFS preserves whatever it was
   given, so a compiler echoing a filename can emit NFD. Both are valid
   UTF-8 and look alike, but the bytes differ - which breaks string
   compares and the client's --mapback path matching. Full Unicode NFC
   needs large tables; Hangul composition is pure arithmetic (Unicode
   3.12), so Hangul only is composed. Latin combining marks are left
   decomposed (documented in PLAN_02). */

#define L_BASE  0x1100L
#define V_BASE  0x1161L
#define T_BASE  0x11A7L
#define S_BASE  0xAC00L
#define L_CNT   19L
#define V_CNT   21L
#define T_CNT   28L
#define N_CNT   (V_CNT * T_CNT)          /* 588   */
#define S_CNT   (L_CNT * N_CNT)          /* 11172 */

void txt_init(const char *mode)
{
    g_active = (mode != NULL && strcmp(mode, "off") == 0) ? 0 : 1;
}

static int is_L(long c) { return c >= L_BASE && c < L_BASE + L_CNT; }
static int is_V(long c) { return c >= V_BASE && c < V_BASE + V_CNT; }
static int is_T(long c) { return c >  T_BASE && c < T_BASE + T_CNT; }
static int is_S(long c) { return c >= S_BASE && c < S_BASE + S_CNT; }

/* decode one UTF-8 char: >0 bytes used, 0 incomplete, -1 invalid */
static int u8dec(const char *p, long n, long *cp)
{
    unsigned char b0 = (unsigned char)p[0];
    if (b0 < 0x80) { *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0) {
        if (n < 2) return 0;
        *cp = ((long)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (n < 3) return 0;
        *cp = ((long)(b0 & 0x0F) << 12) | ((long)(p[1] & 0x3F) << 6)
              | (p[2] & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (n < 4) return 0;
        *cp = ((long)(b0 & 0x07) << 18) | ((long)(p[1] & 0x3F) << 12)
              | ((long)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    return -1;
}

static int u8enc(long c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000L) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* How much of src can be converted now: whole codepoints only, and
   never cutting an L+V(+T) cluster in half. A trailing T (or any
   non-Hangul codepoint) closes the cluster, so everything is safe;
   a trailing L, L+V, or precomposed LV may still take more from the
   next chunk and is held back (at most 2 codepoints). */
static long safe_end(const char *src, long srclen)
{
    long st0 = -1, st1 = -1, cv0 = 0, cv1 = 0;
    long p = 0, complete = 0;

    while (p < srclen) {
        long c;
        int k = u8dec(src + p, srclen - p, &c);
        if (k == 0)
            break;                       /* incomplete tail */
        if (k < 0) { c = -1; k = 1; }    /* invalid byte passes through */
        st0 = st1; cv0 = cv1;
        st1 = p;   cv1 = c;
        p += k;
        complete = p;
    }
    if (st1 < 0)
        return complete;
    if (is_L(cv1))
        return st1;                      /* L still needs its V */
    if (is_V(cv1)) {                     /* L+V may still take a T */
        if (st0 >= 0 && is_L(cv0))
            return st0;
        return st1;
    }
    if (is_S(cv1) && ((cv1 - S_BASE) % T_CNT) == 0)
        return st1;                      /* LV may still take a T */
    return complete;                     /* T or non-Hangul: closed */
}

/* compose src[0..lim) into out; returns bytes written or -1 */
static long compose(const char *src, long lim, char *out, long omax)
{
    long i = 0, o = 0, cp, cp2, cp3;
    int k, k2, k3;

    while (i < lim) {
        k = u8dec(src + i, lim - i, &cp);
        if (k == 0)
            break;
        if (k < 0) {
            if (o >= omax) return -1;
            out[o++] = src[i++];
            continue;
        }
        if (is_L(cp) && i + k < lim) {
            k2 = u8dec(src + i + k, lim - i - k, &cp2);
            if (k2 > 0 && is_V(cp2)) {
                long sy = S_BASE + ((cp - L_BASE) * N_CNT)
                                 + ((cp2 - V_BASE) * T_CNT);
                long adv = k + k2;
                if (i + adv < lim) {
                    k3 = u8dec(src + i + adv, lim - i - adv, &cp3);
                    if (k3 > 0 && is_T(cp3)) {
                        sy += cp3 - T_BASE;
                        adv += k3;
                    }
                }
                if (o + 3 > omax) return -1;
                o += u8enc(sy, out + o);
                i += adv;
                continue;
            }
        }
        if (is_S(cp) && ((cp - S_BASE) % T_CNT) == 0 && i + k < lim) {
            k2 = u8dec(src + i + k, lim - i - k, &cp2);
            if (k2 > 0 && is_T(cp2)) {
                if (o + 3 > omax) return -1;
                o += u8enc(cp + (cp2 - T_BASE), out + o);
                i += k + k2;
                continue;
            }
        }
        if (o + k > omax) return -1;
        o += u8enc(cp, out + o);
        i += k;
    }
    return o;
}

long txt_out(txt_stream_t *s, const char *in, long n, char *out, long omax)
{
    static char joined[JOIN_MAX];
    const char *src;
    long srclen, lim;

    src = splice(s, in, n, joined, (long)sizeof(joined), &srclen);
    lim = safe_end(src, srclen);
    hold(s, src + lim, srclen - lim);
    return compose(src, lim, out, omax);
}

/* end of stream: emit whatever is still held, composing what we can */
long txt_flush(txt_stream_t *s, char *out, long omax)
{
    long o;
    if (s->npend <= 0)
        return 0;
    o = compose(s->pend, (long)s->npend, out, omax);
    s->npend = 0;
    return o;
}

long txt_in(const char *in, long n, char *out, long omax)
{
    /* the host already sends UTF-8 (NFC); macOS accepts either form */
    if (n > omax)
        return -1;
    memcpy(out, in, (size_t)n);
    return n;
}

#endif  /* GCDS_WIN32 / __APPLE__ */

#else   /* platform already matches the wire convention */
/* ISO C forbids an empty translation unit */
typedef int gcds_textcv_unused;
#endif  /* GCDS_TEXTCV */
