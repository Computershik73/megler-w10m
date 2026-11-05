/*
 * printf, scanf and their kin, for the core's ELF on desktop Windows (docs/DESKTOP.md).
 *
 * The core is compiled for Android's ABI, and so is every caller of these: the core itself, and guest code through
 * bionic/mg_libc_stdio.c, which hands them a guest's va_list as it is. Windows' own functions cannot read such arguments
 * (on x86_64 and ARM64 Android's va_list is a structure over the registers, Windows' a pointer into the stack), so these
 * are the core's: va_arg here reads every argument where an Android caller put it. What they write is bionic's (and
 * C99's): wide strings and characters are 32-bit and become UTF-8, %p is 0x and hex, NaN and infinity are nan and inf.
 * A floating-point conversion alone is handed to Windows' snprintf, one argument at a time, through the bridge
 * (host/win32/mg_crt_bridge.c, mg_crt_format_double), which is given the conversion with its flags, width and precision
 * and nothing else to read.
 *
 * Compiled only into the core's ELF (MG_CORE_ELF); the phone's build and the tests use their C library's.
 */
#include "../mg_base.h"

#if defined(MG_CORE_ELF)

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Windows' snprintf for one floating-point conversion (mg_crt_bridge.c); called with the core's own convention. */
int mg_crt_format_double(char *out, size_t cap, const char *spec, double v);

/* ------------------------------------------------------------- the output */

typedef struct sink {
    char *buf;                         /* a string's: written up to cap - 1, then counted only */
    size_t cap;
    FILE *f;                           /* or a stream's, through block */
    char block[512];
    size_t inblock;
    size_t len;                        /* everything the output has, written or not */
    int error;
} sink;

static void flush_block(sink *s)
{
    if (s->f && s->inblock) {
        if (fwrite(s->block, 1, s->inblock, s->f) != s->inblock) s->error = 1;
        s->inblock = 0;
    }
}

static void put(sink *s, const char *p, size_t n)
{
    if (s->f) {
        while (n) {
            size_t room = sizeof(s->block) - s->inblock, k = n < room ? n : room;
            memcpy(s->block + s->inblock, p, k);
            s->inblock += k;
            p += k;
            n -= k;
            s->len += k;
            if (s->inblock == sizeof(s->block)) flush_block(s);
        }
        return;
    }
    if (s->buf && s->len + 1 < s->cap) {
        size_t room = s->cap - 1 - s->len, k = n < room ? n : room;
        memcpy(s->buf + s->len, p, k);
    }
    s->len += n;
}

static void pad(sink *s, char c, size_t n)
{
    char run[32];
    memset(run, c, sizeof(run));
    while (n) {
        size_t k = n < sizeof(run) ? n : sizeof(run);
        put(s, run, k);
        n -= k;
    }
}

/* ------------------------------------------------------------ the pieces */

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };
enum { L_NONE, L_HH, L_H, L_L, L_LL, L_BIG_L, L_J, L_Z, L_T };

static size_t utf8_of(uint32_t c, char *out)
{
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 63)); out[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    if (c < 0x110000) {
        out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 63));
        out[2] = (char)(0x80 | ((c >> 6) & 63)); out[3] = (char)(0x80 | (c & 63));
        return 4;
    }
    out[0] = '?';
    return 1;
}

/* A field of `n` bytes, padded to `width` as the flags say. */
static void field(sink *s, const char *p, size_t n, int width, int flags)
{
    size_t w = width > 0 ? (size_t)width : 0;
    if (!(flags & F_MINUS) && w > n) pad(s, ' ', w - n);
    put(s, p, n);
    if ((flags & F_MINUS) && w > n) pad(s, ' ', w - n);
}

static void integer(sink *s, uintmax_t v, bool negative, int base, bool upper, int flags, int width, int prec, bool pointer)
{
    char digits[80], head[4];
    size_t nd = 0, nh = 0, nz = 0, total;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    uintmax_t x = v;
    if (!(prec == 0 && v == 0)) {
        do {
            digits[sizeof(digits) - 1 - nd++] = set[x % (unsigned)base];
            x /= (unsigned)base;
        } while (x);
    }
    if (negative) head[nh++] = '-';
    else if (flags & F_PLUS) head[nh++] = '+';
    else if (flags & F_SPACE) head[nh++] = ' ';
    if (pointer || ((flags & F_HASH) && base == 16 && v)) {
        head[nh++] = '0';
        head[nh++] = upper ? 'X' : 'x';
    }
    if ((flags & F_HASH) && base == 8 && (prec <= (int)nd)) prec = (int)nd + 1;
    if (prec >= 0 && (size_t)prec > nd) nz = (size_t)prec - nd;
    else if (prec < 0 && (flags & F_ZERO) && !(flags & F_MINUS) && width > 0 && (size_t)width > nh + nd) nz = (size_t)width - nh - nd;
    total = nh + nz + nd;
    if (!(flags & F_MINUS) && width > 0 && (size_t)width > total) pad(s, ' ', (size_t)width - total);
    put(s, head, nh);
    pad(s, '0', nz);
    put(s, digits + sizeof(digits) - nd, nd);
    if ((flags & F_MINUS) && width > 0 && (size_t)width > total) pad(s, ' ', (size_t)width - total);
}

static void floating(sink *s, double v, char conv, int flags, int width, int prec)
{
    char spec[32], small[512], *out = small;
    size_t k = 0;
    int n;
    if (isnan(v) || isinf(v)) {
        char text[8];
        size_t t = 0;
        bool upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
        if (signbit(v)) text[t++] = '-';
        else if (flags & F_PLUS) text[t++] = '+';
        else if (flags & F_SPACE) text[t++] = ' ';
        memcpy(text + t, isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3);
        field(s, text, t + 3, width, flags & F_MINUS);
        return;
    }
    spec[k++] = '%';
    if (flags & F_MINUS) spec[k++] = '-';
    if (flags & F_PLUS) spec[k++] = '+';
    if (flags & F_SPACE) spec[k++] = ' ';
    if (flags & F_HASH) spec[k++] = '#';
    if (flags & F_ZERO) spec[k++] = '0';
    if (width > 0) k += (size_t)snprintf(spec + k, sizeof(spec) - k, "%d", width);
    if (prec >= 0) k += (size_t)snprintf(spec + k, sizeof(spec) - k, ".%d", prec);
    spec[k++] = conv;
    spec[k] = 0;
    n = mg_crt_format_double(small, sizeof(small), spec, v);
    if (n < 0) return;
    if ((size_t)n >= sizeof(small)) {
        out = (char *)malloc((size_t)n + 1);
        if (!out) return;
        mg_crt_format_double(out, (size_t)n + 1, spec, v);
    }
    put(s, out, (size_t)n);
    if (out != small) free(out);
}

/* ------------------------------------------------------------- the engine */

static int format(sink *s, const char *fmt, va_list ap)
{
    const char *p = fmt;
    while (*p) {
        int flags = 0, width = -1, prec = -1, len = L_NONE;
        char conv;
        if (*p != '%') {
            const char *q = p;
            while (*q && *q != '%') q++;
            put(s, p, (size_t)(q - p));
            p = q;
            continue;
        }
        p++;
        for (;; p++) {
            if (*p == '-') flags |= F_MINUS;
            else if (*p == '+') flags |= F_PLUS;
            else if (*p == ' ') flags |= F_SPACE;
            else if (*p == '#') flags |= F_HASH;
            else if (*p == '0') flags |= F_ZERO;
            else if (*p == '\'') ;
            else break;
        }
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_MINUS; width = -width; }
            p++;
        } else if (*p >= '0' && *p <= '9') {
            width = 0;
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                if (prec < 0) prec = -1;
                p++;
            } else {
                while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
            }
        }
        switch (*p) {
        case 'h': len = p[1] == 'h' ? (p++, L_HH) : L_H; p++; break;
        case 'l': len = p[1] == 'l' ? (p++, L_LL) : L_L; p++; break;
        case 'q': len = L_LL; p++; break;
        case 'L': len = L_BIG_L; p++; break;
        case 'j': len = L_J; p++; break;
        case 'z': len = L_Z; p++; break;
        case 't': len = L_T; p++; break;
        default: break;
        }
        conv = *p;
        if (!conv) break;
        p++;
        switch (conv) {
        case '%':
            put(s, "%", 1);
            break;
        case 'd': case 'i': {
            intmax_t v;
            switch (len) {
            case L_HH: v = (signed char)va_arg(ap, int); break;
            case L_H: v = (short)va_arg(ap, int); break;
            case L_L: v = va_arg(ap, long); break;
            case L_LL: v = va_arg(ap, long long); break;
            case L_J: v = va_arg(ap, intmax_t); break;
            case L_Z: v = va_arg(ap, ptrdiff_t); break;
            case L_T: v = va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, int); break;
            }
            integer(s, v < 0 ? (uintmax_t)0 - (uintmax_t)v : (uintmax_t)v, v < 0, 10, false, flags, width, prec, false);
            break;
        }
        case 'u': case 'o': case 'x': case 'X': {
            uintmax_t v;
            switch (len) {
            case L_HH: v = (unsigned char)va_arg(ap, unsigned); break;
            case L_H: v = (unsigned short)va_arg(ap, unsigned); break;
            case L_L: v = va_arg(ap, unsigned long); break;
            case L_LL: v = va_arg(ap, unsigned long long); break;
            case L_J: v = va_arg(ap, uintmax_t); break;
            case L_Z: v = va_arg(ap, size_t); break;
            case L_T: v = (uintmax_t)va_arg(ap, ptrdiff_t); break;
            default: v = va_arg(ap, unsigned); break;
            }
            integer(s, v, false, conv == 'u' ? 10 : conv == 'o' ? 8 : 16, conv == 'X', flags & ~(F_PLUS | F_SPACE), width, prec, false);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            integer(s, v, false, 16, false, flags & ~(F_PLUS | F_SPACE | F_HASH), width, prec, true);
            break;
        }
        case 'c': case 'C': {
            char b[4];
            size_t n;
            if (len == L_L || conv == 'C') n = utf8_of((uint32_t)va_arg(ap, wint_t), b);
            else { b[0] = (char)va_arg(ap, int); n = 1; }
            field(s, b, n, width, flags);
            break;
        }
        case 's': case 'S':
            if (len == L_L || conv == 'S') {
                const wchar_t *w = va_arg(ap, const wchar_t *);
                char b[4], *u;
                size_t n = 0, cap = 64, k;
                if (!w) {
                    field(s, "(null)", prec >= 0 && prec < 6 ? (size_t)prec : 6, width, flags);
                    break;
                }
                u = (char *)malloc(cap);
                if (!u) break;
                for (; *w; w++) {
                    k = utf8_of((uint32_t)*w, b);
                    if (prec >= 0 && n + k > (size_t)prec) break;
                    if (n + k > cap) {
                        char *g = (char *)realloc(u, cap * 2);
                        if (!g) break;
                        u = g;
                        cap *= 2;
                    }
                    memcpy(u + n, b, k);
                    n += k;
                }
                field(s, u, n, width, flags);
                free(u);
            } else {
                const char *t = va_arg(ap, const char *);
                size_t n;
                if (!t) t = "(null)";
                if (prec >= 0) {
                    const char *e = (const char *)memchr(t, 0, (size_t)prec);
                    n = e ? (size_t)(e - t) : (size_t)prec;
                } else {
                    n = strlen(t);
                }
                field(s, t, n, width, flags);
            }
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = len == L_BIG_L ? (double)va_arg(ap, long double) : va_arg(ap, double);
            floating(s, v, conv, flags, width, prec);
            break;
        }
        case 'n': {
            void *at = va_arg(ap, void *);
            switch (len) {
            case L_HH: *(signed char *)at = (signed char)s->len; break;
            case L_H: *(short *)at = (short)s->len; break;
            case L_L: *(long *)at = (long)s->len; break;
            case L_LL: case L_J: *(long long *)at = (long long)s->len; break;
            case L_Z: *(size_t *)at = s->len; break;
            default: *(int *)at = (int)s->len; break;
            }
            break;
        }
        case 'm': {
            char t[24];
            int n = snprintf(t, sizeof(t), "errno %d", errno);
            field(s, t, (size_t)n, width, flags);
            break;
        }
        default:
            /* An unknown conversion is written as it stands. */
            put(s, "%", 1);
            put(s, &conv, 1);
            break;
        }
    }
    return (int)s->len;
}

/* -------------------------------------------------------------- printf's */

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    sink s;
    memset(&s, 0, sizeof(s));
    s.buf = buf;
    s.cap = cap;
    format(&s, fmt, ap);
    if (buf && cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return s.len > 0x7fffffff ? -1 : (int)s.len;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    sink s;
    memset(&s, 0, sizeof(s));
    s.f = f;
    format(&s, fmt, ap);
    flush_block(&s);
    return s.error ? -1 : (int)s.len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *t)
{
    size_t n = strlen(t);
    if (fwrite(t, 1, n, stdout) != n || fputc('\n', stdout) == EOF) return EOF;
    return 1;
}

int putchar(int c) { return fputc(c, stdout); }

/* ---------------------------------------------------------------- scanf's */

static bool is_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int digit_of(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* A number of at most `width` characters at *pp in `base` (0: as C's prefixes say), its end at *pp; false when there was none. */
static bool scan_int(const char **pp, int width, int base, uintmax_t *out, bool *negative)
{
    const char *p = *pp;
    int left = width > 0 ? width : 0x7fffffff;
    uintmax_t v = 0;
    bool any = false;
    *negative = false;
    if (left && (*p == '+' || *p == '-')) { *negative = *p == '-'; p++; left--; }
    if (base == 0) {
        if (left >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_of(p[2]) < 16) { base = 16; p += 2; left -= 2; }
        else if (left && p[0] == '0') base = 8;
        else base = 10;
    } else if (base == 16 && left >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_of(p[2]) < 16) {
        p += 2;
        left -= 2;
    }
    while (left && digit_of((unsigned char)*p) < base) {
        v = v * (uintmax_t)base + (uintmax_t)digit_of((unsigned char)*p);
        p++;
        left--;
        any = true;
    }
    if (!any) return false;
    *out = v;
    *pp = p;
    return true;
}

static void store_int(void *at, int len, uintmax_t v)
{
    switch (len) {
    case L_HH: *(unsigned char *)at = (unsigned char)v; break;
    case L_H: *(unsigned short *)at = (unsigned short)v; break;
    case L_L: *(unsigned long *)at = (unsigned long)v; break;
    case L_LL: case L_J: *(unsigned long long *)at = (unsigned long long)v; break;
    case L_Z: case L_T: *(size_t *)at = (size_t)v; break;
    default: *(unsigned *)at = (unsigned)v; break;
    }
}

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str, *p = fmt;
    int assigned = 0;
    bool any_input = false;
    while (*p) {
        int width = -1, len = L_NONE;
        bool skip = false;
        char conv;
        if (is_space((unsigned char)*p)) {
            while (is_space((unsigned char)*p)) p++;
            while (is_space((unsigned char)*s)) s++;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') p++;
            if (*s != *p) break;
            s++;
            p++;
            continue;
        }
        p++;
        if (*p == '*') { skip = true; p++; }
        if (*p >= '0' && *p <= '9') {
            width = 0;
            while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        }
        switch (*p) {
        case 'h': len = p[1] == 'h' ? (p++, L_HH) : L_H; p++; break;
        case 'l': len = p[1] == 'l' ? (p++, L_LL) : L_L; p++; break;
        case 'q': len = L_LL; p++; break;
        case 'L': len = L_BIG_L; p++; break;
        case 'j': len = L_J; p++; break;
        case 'z': len = L_Z; p++; break;
        case 't': len = L_T; p++; break;
        default: break;
        }
        conv = *p++;
        if (!conv) break;
        if (conv != 'c' && conv != '[' && conv != 'n')
            while (is_space((unsigned char)*s)) s++;
        if (!*s && conv != 'n') return any_input || assigned ? assigned : EOF;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            uintmax_t v;
            bool negative;
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            if (!scan_int(&s, width, base, &v, &negative)) return assigned;
            any_input = true;
            if (negative) v = (uintmax_t)0 - v;
            if (!skip) {
                if (conv == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                else store_int(va_arg(ap, void *), len, v);
                assigned++;
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            char num[512];
            char *end;
            double v;
            size_t n = strlen(s);
            if (width > 0 && (size_t)width < n) n = (size_t)width;
            if (n >= sizeof(num)) n = sizeof(num) - 1;
            memcpy(num, s, n);
            num[n] = 0;
            v = strtod(num, &end);
            if (end == num) return assigned;
            s += end - num;
            any_input = true;
            if (!skip) {
                if (len == L_L) *va_arg(ap, double *) = v;
                else if (len == L_BIG_L) *va_arg(ap, long double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 's': {
            char *out = skip ? NULL : va_arg(ap, char *);
            int left = width > 0 ? width : 0x7fffffff;
            while (*s && !is_space((unsigned char)*s) && left--) {
                if (out) *out++ = *s;
                s++;
            }
            if (out) { *out = 0; assigned++; }
            any_input = true;
            break;
        }
        case 'c': {
            char *out = skip ? NULL : va_arg(ap, char *);
            int n = width > 0 ? width : 1;
            if ((int)strlen(s) < n) return assigned;
            if (out) { memcpy(out, s, (size_t)n); assigned++; }
            s += n;
            any_input = true;
            break;
        }
        case '[': {
            bool set[256], invert = false;
            char *out = skip ? NULL : va_arg(ap, char *);
            int left = width > 0 ? width : 0x7fffffff;
            const char *start = s;
            memset(set, 0, sizeof(set));
            if (*p == '^') { invert = true; p++; }
            if (*p == ']') { set[(unsigned char)']'] = true; p++; }
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    int c;
                    for (c = (unsigned char)p[0]; c <= (unsigned char)p[2]; c++) set[c] = true;
                    p += 3;
                } else {
                    set[(unsigned char)*p++] = true;
                }
            }
            if (*p == ']') p++;
            while (*s && left-- && set[(unsigned char)*s] != invert) {
                if (out) *out++ = *s;
                s++;
            }
            if (s == start) return assigned;
            if (out) { *out = 0; assigned++; }
            any_input = true;
            break;
        }
        case 'n':
            if (!skip) store_int(va_arg(ap, void *), len, (uintmax_t)(s - str));
            break;
        default:
            return assigned;
        }
    }
    return assigned;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsscanf(str, fmt, ap);
    va_end(ap);
    return n;
}

#endif /* MG_CORE_ELF */
