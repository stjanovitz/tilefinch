/* newlib entry points the PSP browser image defines itself.

   The image formats text only through snprintf. Two newlib members would
   otherwise link the stream printf core (_vfprintf_r or _vfiprintf_r,
   11-14 KB of .text) for output the browser never produces:

   - __assert_func reports through fiprintf. The browser compiles with
     NDEBUG, so only library asserts (nghttp2, newlib's number conversion)
     reach it.
   - mallinfo lives in one section with malloc_stats and mstats, which print
     through fiprintf, so the heap queries pulled the printers in with it.

   Both keep newlib's behaviour: assert reports the failed expression on
   stderr and aborts, and mallinfo is the reentrant query for this thread.

   The string-scanner helpers below let the image keep one scanf core.
   newlib's integer-only core, __ssvfiscanf_r (about 9 KB, used only by
   tzset's siscanf), resolves to the full __ssvfscanf_r the browser's
   sscanf calls already link (--defsym in cmake/TilefinchTargets.cmake),
   which scans every integer format identically. The full core needs
   _sungetc_r, __ssrefill_r and _sfread_r, which newlib defines only in
   the integer core's member, so without these the alias would still
   link that member. They are newlib's string-only versions
   (libc/stdio/vfscanf.c): a string stream has no file to refill from,
   only the ungetc buffer.

   Host builds use their C library's. */

#include <assert.h>
#include <malloc.h>
#include <reent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __assert_func(const char *file, int line, const char *function,
                   const char *expression)
{
    char message[256];
    if (snprintf(message, sizeof(message),
                 "assertion \"%s\" failed: file \"%s\", line %d%s%s\n",
                 expression, file, line,
                 function != NULL ? ", function: " : "",
                 function != NULL ? function : "") > 0)
        (void) fputs(message, stderr);
    abort();
}

struct mallinfo mallinfo(void)
{
    return _mallinfo_r(_REENT);
}

/* The scanf core's reentrant __submore grows a stream's ungetc buffer
   (newlib, libc/stdio/ungetc.c). */
int __submore(struct _reent *reent, FILE *stream);

/* Push c back onto a string stream: back up over the same character when
   it is the one just read (never writing into the caller's string), and
   otherwise switch to the ungetc buffer. */
int _sungetc_r(struct _reent *reent, int c, FILE *stream)
{
    if (c == EOF)
        return EOF;
    stream->_flags &= ~__SEOF;
    c = (unsigned char) c;
    if (stream->_ub._base != NULL) {
        if (stream->_r >= stream->_ub._size && __submore(reent, stream))
            return EOF;
        *--stream->_p = (unsigned char) c;
        stream->_r++;
        return c;
    }
    if (stream->_bf._base != NULL && stream->_p > stream->_bf._base
        && stream->_p[-1] == c) {
        stream->_p--;
        stream->_r++;
        return c;
    }
    stream->_ur = stream->_r;
    stream->_up = stream->_p;
    stream->_ub._base = stream->_ubuf;
    stream->_ub._size = sizeof(stream->_ubuf);
    stream->_ubuf[sizeof(stream->_ubuf) - 1] = (unsigned char) c;
    stream->_p = &stream->_ubuf[sizeof(stream->_ubuf) - 1];
    stream->_r = 1;
    return c;
}

/* A string stream's only further input is its ungetc buffer: resume the
   string where it was when that buffer was made, or report end of input. */
int __ssrefill_r(struct _reent *reent, FILE *stream)
{
    if (stream->_ub._base != NULL) {
        if (stream->_ub._base != stream->_ubuf)
            _free_r(reent, stream->_ub._base);
        stream->_ub._base = NULL;
        if ((stream->_r = stream->_ur) != 0) {
            stream->_p = stream->_up;
            return 0;
        }
    }
    stream->_p = stream->_bf._base;
    stream->_r = 0;
    stream->_flags |= __SEOF;
    return EOF;
}

/* fread from a string stream (scanf's %c and %[ with a width). Returns the
   whole items read; a short read stops at the end of input. */
size_t _sfread_r(struct _reent *reent, void *buffer, size_t size,
                 size_t count, FILE *stream)
{
    size_t remaining = count * size;
    size_t total = remaining;
    char *out = buffer;
    int available;

    if (remaining == 0)
        return 0;
    while (remaining > (size_t) (available = stream->_r)) {
        memcpy(out, stream->_p, (size_t) available);
        stream->_p += available;
        stream->_r = 0;
        out += available;
        remaining -= (size_t) available;
        if (__ssrefill_r(reent, stream))
            return (total - remaining) / size;
    }
    memcpy(out, stream->_p, remaining);
    stream->_r -= (int) remaining;
    stream->_p += remaining;
    return count;
}

#ifdef TILEFINCH_PSP_VALIDATION_LOG
/* Validation-only boot check of the aliased scanf core and the string-stream
   helpers above: integer bases, widths and length modifiers, %n, %s/%c/%[
   widths, floats and their pushback, the profile-footer and Content-Range
   formats the browser scans, siscanf, and a tzset parse (tzset is the
   alias's own caller). Each case prints a tilefinch-scanf-check line to the
   validation log, then one `outcome=pass|fail cases=N failures=M` line,
   which the PPSSPP runners print. Shipping images contain none of it. */
#include <math.h>
#include <time.h>

#include "tilefinch/psp_log.h"
#define printf psp_log_printf

int siscanf(const char *string, const char *format, ...);

static int scanf_check_failures;
static int scanf_check_cases;

static void scanf_check_report(const char *name, int ok, int ret)
{
    scanf_check_cases++;
    if (!ok)
        scanf_check_failures++;
    printf("tilefinch-scanf-check: case=%s ok=%d ret=%d\n", name, ok, ret);
}

void tilefinch_scanf_check(void)
{
    int a = 0, b = 0, c = 0, n = -1, ret;
    unsigned u = 0, x = 0;
    char s1[8], s2[16], c3[4], c2[3], set[4];
    float f1 = 0, f3 = 0;
    double d1 = 0, d2 = 0;
    long l = 0;
    short h = 0;
    unsigned char hh[4] = {0, 0, 0, 0};
    size_t z = 0;
    unsigned long long q1 = 0, q2 = 0, q3 = 0;
    char tail = 0;

    ret = sscanf("  -1234 0x1F 077 4294967295", "%d %x %o %u", &a, &x, (unsigned *) &b, &u);
    scanf_check_report("int-bases",
                       ret == 4 && a == -1234 && x == 0x1F && b == 077
                           && u == 4294967295u, ret);

    ret = sscanf("0x1F 077 -12", "%i %i %i", &a, &b, &c);
    scanf_check_report("int-i", ret == 3 && a == 31 && b == 63 && c == -12,
                       ret);

    ret = sscanf("123456", "%3d%2d%d", &a, &b, &c);
    scanf_check_report("int-width", ret == 3 && a == 123 && b == 45 && c == 6,
                       ret);

    ret = sscanf("300 -40000", "%hhu %hd", &hh[0], &h);
    scanf_check_report("int-short", ret == 2 && hh[0] == 44 && hh[1] == 0
                           && h == (short) -40000, ret);

    ret = sscanf("abc  42xyz", "abc %d%n", &a, &n);
    scanf_check_report("percent-n", ret == 1 && a == 42 && n == 7, ret);
    printf("tilefinch-scanf-check: percent-n a=%d n=%d\n", a, n);

    n = -1;
    ret = sscanf("12", "%d%n", &a, &n);
    scanf_check_report("percent-n-end", ret == 1 && a == 12 && n == 2, ret);

    memset(s1, 'Z', sizeof(s1));
    memset(s2, 'Z', sizeof(s2));
    ret = sscanf("hello_world rest", "%5s%15s", s1, s2);
    scanf_check_report("string-width", ret == 2 && strcmp(s1, "hello") == 0
                           && strcmp(s2, "_world") == 0 && s1[6] == 'Z', ret);

    ret = sscanf("abcdefgh", "%3c%2c%2[a-z]", c3, c2, set);
    scanf_check_report("char-set-width", ret == 3 && memcmp(c3, "abc", 3) == 0
                           && memcmp(c2, "de", 2) == 0
                           && strcmp(set, "fg") == 0, ret);

    /* newlib assigns a short %c read (whole items _sfread_r got). */
    memset(c3, 0, sizeof(c3));
    ret = sscanf("ab", "%3c", c3);
    scanf_check_report("char-short", ret == 1 && memcmp(c3, "ab", 2) == 0,
                       ret);

    ret = sscanf("3.25 -1e-3 1e5 .5", "%f %lf %g %lf", &f1, &d1, &f3, &d2);
    scanf_check_report("float", ret == 4 && f1 == 3.25f && d1 == -1e-3
                           && f3 == 1e5f && d2 == 0.5, ret);
    printf("tilefinch-scanf-check: float f1=%.9g d1=%.17g f3=%.9g d2=%.17g\n",
           (double) f1, d1, (double) f3, d2);

    ret = sscanf("2.71828182845904523536 -0", "%lf %lf", &d1, &d2);
    scanf_check_report("float-precise", ret == 2 && d1 == 2.718281828459045
                           && d2 == 0.0, ret);
    /* newlib's scanner drops the sign of an all-zero mantissa: "-0" scans
       as +0 with or without the alias (control run), so this is reported,
       not counted. */
    printf("tilefinch-scanf-check: info=negative-zero signbit=%d\n",
           signbit(d2) ? 1 : 0);
    printf("tilefinch-scanf-check: float-precise d1=%.17g d1-e=%.17g "
           "d2=%.17g signbit=%d\n", d1, d1 - 2.718281828459045, d2,
           signbit(d2) ? 1 : 0);

    /* Width 4 ends inside the exponent: "1.5e" backs up over the 'e'. */
    ret = sscanf("1.5e2x", "%4lf%15s", &d1, s2);
    scanf_check_report("float-width", ret == 2 && d1 == 1.5
                           && strcmp(s2, "e2x") == 0, ret);
    printf("tilefinch-scanf-check: float-width d1=%.17g rest=%s\n", d1, s2);

    s2[0] = 0;
    ret = sscanf("1e+x", "%lf%15s", &d1, s2);
    scanf_check_report("float-pushback", ret == 2 && d1 == 1.0
                           && strcmp(s2, "e+x") == 0, ret);
    printf("tilefinch-scanf-check: float-pushback ret=%d d1=%.17g rest=%s\n",
           ret, d1, s2);

    ret = sscanf("-inf nan", "%lf %lf", &d1, &d2);
    scanf_check_report("float-special", ret == 2 && isinf(d1) && d1 < 0
                           && isnan(d2), ret);

    ret = sscanf("0 0 0 0", "%f %f %f %f", &f1, &f3, &f1, &f3);
    scanf_check_report("svg-viewbox-zero", ret == 4 && f1 == 0.0f, ret);

    ret = sscanf("END\t1234\t0000ABCD\n", "END\t%zu\t%8X%c", &z, &x, &tail);
    scanf_check_report("profile-footer", ret == 3 && z == 1234
                           && x == 0xABCDu && tail == '\n', ret);

    ret = sscanf("bytes 100-199/1000", "bytes %llu-%llu/%llu%c",
                 &q1, &q2, &q3, &tail);
    scanf_check_report("content-range", ret == 3 && q1 == 100 && q2 == 199
                           && q3 == 1000, ret);

    ret = sscanf("HTTP/1.1 206 Partial", "HTTP/%*s %ld", &l);
    scanf_check_report("suppress", ret == 1 && l == 206, ret);

    ret = sscanf("", "%d", &a);
    scanf_check_report("empty-eof", ret == EOF, ret);

    ret = sscanf("   ", "%d", &a);
    scanf_check_report("space-eof", ret == EOF, ret);

    ret = sscanf("x", "%d", &a);
    scanf_check_report("mismatch", ret == 0, ret);

    a = b = 0;
    ret = siscanf("42 -7", "%d %d", &a, &b);
    scanf_check_report("siscanf", ret == 2 && a == 42 && b == -7, ret);

    {
        const char *saved = getenv("TZ");
        char saved_copy[64];
        int have = saved != NULL && strlen(saved) < sizeof(saved_copy);
        if (have)
            strcpy(saved_copy, saved);
        setenv("TZ", "TFA+5:30TFB+4:15,M3.2.0/2,M11.1.0/2", 1);
        tzset();
        printf("tilefinch-scanf-check: tzset timezone=%ld daylight=%d "
               "names=%s/%s\n", (long) _timezone, _daylight, _tzname[0],
               _tzname[1]);
        scanf_check_report("tzset", _timezone == 5 * 3600 + 30 * 60
                               && _daylight == 1
                               && strcmp(_tzname[0], "TFA") == 0
                               && strcmp(_tzname[1], "TFB") == 0, 0);
        if (have)
            setenv("TZ", saved_copy, 1);
        else
            unsetenv("TZ");
        tzset();
    }

    printf("tilefinch-scanf-check: outcome=%s cases=%d failures=%d\n",
           scanf_check_failures == 0 ? "pass" : "fail", scanf_check_cases,
           scanf_check_failures);
}
#endif
