/*
 * textcv.h - wire text encoding normalization.
 *
 * GCDSP wire convention: TEXT carries UTF-8, composed (NFC) where
 * practical. That covers O/E frames, the command line, CWD/ENV values
 * and RUNI I frames. **D frames (PUT/GET) are raw bytes and are never
 * converted** - converting them would corrupt binaries.
 *
 * Each daemon normalizes its own environment to that convention:
 *   Win32   - local ANSI/console codepage (e.g. CP949) <-> UTF-8
 *   macOS   - UTF-8 already, but the filesystem hands out decomposed
 *             (NFD) names; Hangul is composed to NFC on the way out
 *   Linux/Haiku - already UTF-8 NFC, nothing to do
 *   MS-DOS / NeXTSTEP - assumed ASCII (see PLAN_02); no conversion, and
 *             these builds do not link this module at all
 *
 * Conversion is only compiled where it is needed (GCDS_TEXTCV).
 */
#ifndef TEXTCV_H
#define TEXTCV_H

#if defined(GCDS_WIN32) || defined(__APPLE__)
#define GCDS_TEXTCV 1
#endif

/* Does this daemon guarantee UTF-8 text on the wire? Reported as the
   `UTF8` greeting capability. DOS/NeXTSTEP only assume ASCII, so they
   do not claim it. */
#if defined(GCDS_DOS) || defined(GCDS_NEXT)
#define TXT_WIRE_UTF8 0
#else
#define TXT_WIRE_UTF8 1
#endif

#ifdef GCDS_TEXTCV

/* Hold-back state: a multi-byte character split across two reads must
   not be converted in halves, so the tail is carried to the next call. */
typedef struct {
    char pend[16];
    int  npend;
} txt_stream_t;

/* Call once at startup. `mode` comes from the daemon config `codepage`:
   "auto" (default) detects the local encoding, "off" disables all
   conversion, or an explicit codepage number (Win32, e.g. "949"). */
void txt_init(const char *mode);
int  txt_active(void);            /* 1 = conversion actually needed */
void txt_stream_init(txt_stream_t *s);

/* local -> wire. Returns bytes written to out, or -1 if out is too
   small. Incomplete trailing sequences are held in *s. */
long txt_out(txt_stream_t *s, const char *in, long n, char *out, long omax);

/* end of stream: emit whatever is still held back. */
long txt_flush(txt_stream_t *s, char *out, long omax);

/* wire -> local, for whole lines/frames (no hold-back). */
long txt_in(const char *in, long n, char *out, long omax);

#endif /* GCDS_TEXTCV */
#endif /* TEXTCV_H */
