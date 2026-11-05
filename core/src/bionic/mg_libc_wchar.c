/*
 * <wchar.h> and <wctype.h>.
 *
 * Android's wchar_t is 32 bits and the multibyte encoding is always UTF-8;
 * Windows' wchar_t is 16 bits. So none of this can be the host's: every
 * function here works on u32 code points itself.
 */
#include "mg_bionic.h"

#include <stdio.h>

typedef u32 gwc;

/* ------------------------------------------------------------------ UTF-8 */

/* Decodes one character from at most n bytes: its length, 0 for an incomplete sequence, -1 for a bad one. */
static int utf8_decode(const u8 *s, size_t n, gwc *out)
{
    gwc c;
    int len, i;
    if (!n) return 0;
    if (s[0] < 0x80) {
        *out = s[0];
        return 1;
    }
    if ((s[0] & 0xE0) == 0xC0) { len = 2; c = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { len = 3; c = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { len = 4; c = s[0] & 0x07; }
    else return -1;
    for (i = 1; i < len; i++) {
        if ((size_t)i >= n) return 0;
        if ((s[i] & 0xC0) != 0x80) return -1;
        c = (c << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10FFFF) return -1;
    *out = c;
    return len;
}

static int utf8_encode(gwc c, u8 *out)
{
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c < 0x800) { out[0] = (u8)(0xC0 | (c >> 6)); out[1] = (u8)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        out[0] = (u8)(0xE0 | (c >> 12));
        out[1] = (u8)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (u8)(0xF0 | (c >> 18));
        out[1] = (u8)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (u8)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (u8)(0x80 | (c & 0x3F));
        return 4;
    }
    return -1;
}

/* ---------------------------------------------------------------- strings */

static size_t g_wcslen(const gwc *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
static size_t g_wcsnlen(const gwc *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}
static gwc *g_wcscpy(gwc *d, const gwc *s)
{
    gwc *r = d;
    while ((*d++ = *s++) != 0) { }
    return r;
}
static gwc *g_wcsncpy(gwc *d, const gwc *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
static gwc *g_wcscat(gwc *d, const gwc *s)
{
    g_wcscpy(d + g_wcslen(d), s);
    return d;
}
static gwc *g_wcsncat(gwc *d, const gwc *s, size_t n)
{
    gwc *e = d + g_wcslen(d);
    size_t i;
    for (i = 0; i < n && s[i]; i++) e[i] = s[i];
    e[i] = 0;
    return d;
}
static int g_wcscmp(const gwc *a, const gwc *b)
{
    for (;; a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
}
static int g_wcsncmp(const gwc *a, const gwc *b, size_t n)
{
    for (; n; a++, b++, n--) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) return 0;
    }
    return 0;
}
static gwc *g_wcschr(const gwc *s, gwc c)
{
    for (;; s++) {
        if (*s == c) return (gwc *)s;
        if (!*s) return NULL;
    }
}
static gwc *g_wcsrchr(const gwc *s, gwc c)
{
    const gwc *last = NULL;
    for (;; s++) {
        if (*s == c) last = s;
        if (!*s) return (gwc *)last;
    }
}
static gwc *g_wcsstr(const gwc *hay, const gwc *needle)
{
    size_t n = g_wcslen(needle);
    if (!n) return (gwc *)hay;
    for (; *hay; hay++)
        if (!g_wcsncmp(hay, needle, n)) return (gwc *)hay;
    return NULL;
}
static size_t g_wcsspn(const gwc *s, const gwc *set)
{
    size_t n = 0;
    while (s[n] && g_wcschr(set, s[n])) n++;
    return n;
}
static size_t g_wcscspn(const gwc *s, const gwc *set)
{
    size_t n = 0;
    while (s[n] && !g_wcschr(set, s[n])) n++;
    return n;
}
static gwc *g_wcspbrk(const gwc *s, const gwc *set)
{
    s += g_wcscspn(s, set);
    return *s ? (gwc *)s : NULL;
}
static gwc *g_wcstok(gwc *s, const gwc *delim, gwc **save)
{
    gwc *end;
    if (!s) s = *save;
    if (!s) return NULL;
    s += g_wcsspn(s, delim);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    end = s + g_wcscspn(s, delim);
    if (*end) {
        *end = 0;
        *save = end + 1;
    } else {
        *save = NULL;
    }
    return s;
}
static gwc *g_wcsdup(const gwc *s)
{
    size_t n = (g_wcslen(s) + 1) * sizeof(gwc);
    gwc *d = (gwc *)mg_g_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
static gwc *g_wmemcpy(gwc *d, const gwc *s, size_t n) { return (gwc *)memcpy(d, s, n * sizeof(gwc)); }
static gwc *g_wmemmove(gwc *d, const gwc *s, size_t n) { return (gwc *)memmove(d, s, n * sizeof(gwc)); }
static gwc *g_wmemset(gwc *d, gwc c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) d[i] = c;
    return d;
}
static int g_wmemcmp(const gwc *a, const gwc *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static gwc *g_wmemchr(const gwc *s, gwc c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (s[i] == c) return (gwc *)(s + i);
    return NULL;
}
static size_t g_wcsxfrm(gwc *d, const gwc *s, size_t n)
{
    size_t len = g_wcslen(s);
    if (n) g_wcsncpy(d, s, n);
    return len;
}
static size_t g_wcslcpy(gwc *d, const gwc *s, size_t cap)
{
    size_t n = g_wcslen(s);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(d, s, c * sizeof(gwc));
        d[c] = 0;
    }
    return n;
}

/* ---------------------------------------------------------- conversions */

static size_t g_mbrtowc(gwc *out, const char *s, size_t n, void *state)
{
    gwc c = 0;
    int len;
    (void)state;
    if (!s) return 0;
    len = utf8_decode((const u8 *)s, n, &c);
    if (len < 0) {
        mg_set_errno(GE_ILSEQ);
        return (size_t)-1;
    }
    if (len == 0) return (size_t)-2;
    if (out) *out = c;
    return c ? (size_t)len : 0;
}
static int g_mbtowc(gwc *out, const char *s, size_t n)
{
    size_t r;
    if (!s) return 0;
    r = g_mbrtowc(out, s, n, NULL);
    return r >= (size_t)-2 ? -1 : (int)r;
}
static size_t g_mbrlen(const char *s, size_t n, void *state) { return g_mbrtowc(NULL, s, n, state); }
static int g_mblen(const char *s, size_t n) { return g_mbtowc(NULL, s, n); }
static int g_mbsinit(const void *state) { (void)state; return 1; }

static size_t g_wcrtomb(char *out, gwc c, void *state)
{
    u8 tmp[4];
    int len;
    (void)state;
    if (!out) return 1;
    len = utf8_encode(c, tmp);
    if (len < 0) {
        mg_set_errno(GE_ILSEQ);
        return (size_t)-1;
    }
    memcpy(out, tmp, (size_t)len);
    return (size_t)len;
}
static int g_wctomb(char *out, gwc c) { return out ? (int)g_wcrtomb(out, c, NULL) : 0; }

static size_t g_mbsnrtowcs(gwc *dst, const char **src, size_t nms, size_t len, void *state)
{
    const u8 *s = (const u8 *)*src;
    size_t n = 0;
    (void)state;
    while (nms && (!dst || n < len)) {
        gwc c;
        int k = utf8_decode(s, nms, &c);
        if (k < 0) {
            mg_set_errno(GE_ILSEQ);
            if (dst) *src = (const char *)s;
            return (size_t)-1;
        }
        if (k == 0) break;
        if (dst) dst[n] = c;
        if (!c) {
            if (dst) *src = NULL;
            return n;
        }
        s += k;
        nms -= (size_t)k;
        n++;
    }
    if (dst) *src = (const char *)s;
    return n;
}
static size_t g_mbsrtowcs(gwc *dst, const char **src, size_t len, void *state) { return g_mbsnrtowcs(dst, src, (size_t)-1 >> 1, len, state); }
static size_t g_mbstowcs(gwc *dst, const char *src, size_t len)
{
    const char *s = src;
    return g_mbsnrtowcs(dst, &s, (size_t)-1 >> 1, len, NULL);
}

static size_t g_wcsnrtombs(char *dst, const gwc **src, size_t nwc, size_t len, void *state)
{
    const gwc *s = *src;
    size_t n = 0;
    (void)state;
    while (nwc) {
        u8 tmp[4];
        int k = utf8_encode(*s, tmp);
        if (k < 0) {
            mg_set_errno(GE_ILSEQ);
            if (dst) *src = s;
            return (size_t)-1;
        }
        if (dst && n + (size_t)k > len) break;
        if (dst) memcpy(dst + n, tmp, (size_t)k);
        if (!*s) {
            if (dst) *src = NULL;
            return n;
        }
        n += (size_t)k;
        s++;
        nwc--;
    }
    if (dst) *src = s;
    return n;
}
static size_t g_wcsrtombs(char *dst, const gwc **src, size_t len, void *state) { return g_wcsnrtombs(dst, src, (size_t)-1 >> 1, len, state); }
static size_t g_wcstombs(char *dst, const gwc *src, size_t len)
{
    const gwc *s = src;
    return g_wcsnrtombs(dst, &s, (size_t)-1 >> 1, len, NULL);
}

static gwc g_btowc(int c) { return (unsigned)c < 0x80 ? (gwc)c : 0xFFFFFFFFu; }
static int g_wctob(gwc c) { return c < 0x80 ? (int)c : -1; }

/* Wide to a malloc'd UTF-8 string (the host allocator's). */
static char *narrow(const gwc *s, size_t max)
{
    size_t n = g_wcsnlen(s, max), i, at = 0;
    char *out = (char *)mg_alloc(n * 4 + 1);
    for (i = 0; i < n; i++) {
        int k = utf8_encode(s[i], (u8 *)out + at);
        if (k > 0) at += (size_t)k;
    }
    out[at] = 0;
    return out;
}

/* ------------------------------------------------------------------ numbers */

typedef long (*strtol_fn)(const char *, char **, int);
typedef unsigned long (*strtoul_fn)(const char *, char **, int);
typedef i64 (*strtoll_fn)(const char *, char **, int);
typedef u64 (*strtoull_fn)(const char *, char **, int);
typedef gdouble (*strtod_fn)(const char *, char **);
typedef gfloat (*strtof_fn)(const char *, char **);
typedef gldouble (*strtold_fn)(const char *, char **);

/* Number syntax is ASCII, so a narrow parser's "bytes consumed" is also "characters consumed". */
#define WPARSE(ret, name, fn_type, sym, call)                         \
    static ret name                                                   \
    {                                                                 \
        char *text = narrow(s, 256), *e = NULL;                       \
        ret v = ((fn_type)mg_bionic_find(sym))call;                   \
        if (end) *end = (gwc *)s + (e - text);                        \
        free(text);                                                   \
        return v;                                                     \
    }

WPARSE(long, g_wcstol(const gwc *s, gwc **end, int base), strtol_fn, "strtol", (text, &e, base))
WPARSE(unsigned long, g_wcstoul(const gwc *s, gwc **end, int base), strtoul_fn, "strtoul", (text, &e, base))
WPARSE(i64, g_wcstoll(const gwc *s, gwc **end, int base), strtoll_fn, "strtoll", (text, &e, base))
WPARSE(u64, g_wcstoull(const gwc *s, gwc **end, int base), strtoull_fn, "strtoull", (text, &e, base))
WPARSE(gdouble, g_wcstod(const gwc *s, gwc **end), strtod_fn, "strtod", (text, &e))
WPARSE(gfloat, g_wcstof(const gwc *s, gwc **end), strtof_fn, "strtof", (text, &e))
WPARSE(gldouble, g_wcstold(const gwc *s, gwc **end), strtold_fn, "strtold", (text, &e))

static long g_wcstol_l(const gwc *s, gwc **end, int base, void *loc) { (void)loc; return g_wcstol(s, end, base); }
static unsigned long g_wcstoul_l(const gwc *s, gwc **end, int base, void *loc) { (void)loc; return g_wcstoul(s, end, base); }
static i64 g_wcstoll_l(const gwc *s, gwc **end, int base, void *loc) { (void)loc; return g_wcstoll(s, end, base); }
static u64 g_wcstoull_l(const gwc *s, gwc **end, int base, void *loc) { (void)loc; return g_wcstoull(s, end, base); }
static gdouble g_wcstod_l(const gwc *s, gwc **end, void *loc) { (void)loc; return g_wcstod(s, end); }
static gfloat g_wcstof_l(const gwc *s, gwc **end, void *loc) { (void)loc; return g_wcstof(s, end); }
static gldouble g_wcstold_l(const gwc *s, gwc **end, void *loc) { (void)loc; return g_wcstold(s, end); }

/* --------------------------------------------------------------- formatting */

/*
 * vswprintf through the host's vsnprintf. The format is narrowed; %ls and %lc
 * cannot be handed over (the host's wide characters are not the guest's), so
 * a format that uses them is refused and noted. Numbers, %s and %c -- what
 * to_wstring and stream formatting use -- are the same in both.
 */
static int g_vswprintf(gwc *out, size_t cap, const gwc *fmt, va_list ap)
{
    char *nfmt = narrow(fmt, 4096), *text;
    int n;
    size_t i;
    if (strstr(nfmt, "%ls") || strstr(nfmt, "%lc") || strstr(nfmt, "%S") || strstr(nfmt, "%C")) {
        mg_note_once("swprintf: wide-string conversions are not supported (\"%s\")", nfmt);
        free(nfmt);
        return -1;
    }
    text = (char *)mg_alloc(cap * 4 + 16);
    n = vsnprintf(text, cap * 4 + 16, nfmt, ap);
    free(nfmt);
    if (n < 0 || (size_t)n >= cap) {
        free(text);
        if (cap) out[0] = 0;
        return -1;
    }
    for (i = 0; i <= (size_t)n; i++) out[i] = (u8)text[i];
    free(text);
    return n;
}

static int g_swprintf(gwc *out, size_t cap, const gwc *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vswprintf(out, cap, fmt, ap);
    va_end(ap);
    return r;
}

static int g_vswscanf(const gwc *s, const gwc *fmt, va_list ap)
{
    char *ns = narrow(s, 65536), *nfmt = narrow(fmt, 4096);
    int r = vsscanf(ns, nfmt, ap);
    free(ns);
    free(nfmt);
    return r;
}

static int g_swscanf(const gwc *s, const gwc *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vswscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

static size_t g_wcsftime(gwc *out, size_t cap, const gwc *fmt, const void *tm)
{
    typedef size_t (*strftime_fn)(char *, size_t, const char *, const void *);
    char *nfmt = narrow(fmt, 1024), *text = (char *)mg_alloc(cap * 4 + 4);
    size_t n = ((strftime_fn)mg_bionic_find("strftime"))(text, cap * 4 + 4, nfmt, tm), i;
    free(nfmt);
    if (!n || n >= cap) {
        free(text);
        return 0;
    }
    for (i = 0; i <= n; i++) out[i] = (u8)text[i];
    free(text);
    return n;
}

/* ---------------------------------------------------------- classification
 *
 * ASCII, and Latin-1/Cyrillic/Greek case mapping -- what text in a game's
 * interface needs; everything above ASCII counts as a printable letter.
 */
static int g_iswalpha(gwc c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7); }
static int g_iswdigit(gwc c) { return c >= '0' && c <= '9'; }
static int g_iswalnum(gwc c) { return g_iswalpha(c) || g_iswdigit(c); }
static int g_iswxdigit(gwc c) { return g_iswdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
static int g_iswspace(gwc c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000; }
static int g_iswblank(gwc c) { return c == ' ' || c == '\t'; }
static int g_iswcntrl(gwc c) { return c < 32 || (c >= 127 && c < 160); }
static int g_iswprint(gwc c) { return !g_iswcntrl(c); }
static int g_iswgraph(gwc c) { return !g_iswcntrl(c) && !g_iswspace(c); }
static int g_iswpunct(gwc c) { return c < 128 ? (c > 32 && c < 127 && !g_iswalnum(c)) : 0; }

static gwc g_towlower(gwc c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x410 && c <= 0x42F) || (c >= 0x391 && c <= 0x3A9 && c != 0x3A2)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
static gwc g_towupper(gwc c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x430 && c <= 0x44F) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2)) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
static int g_iswupper(gwc c) { return g_towlower(c) != c; }
static int g_iswlower(gwc c) { return g_towupper(c) != c; }

static int g_wcscasecmp(const gwc *a, const gwc *b)
{
    for (;; a++, b++) {
        gwc x = g_towlower(*a), y = g_towlower(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static int g_wcsncasecmp(const gwc *a, const gwc *b, size_t n)
{
    for (; n; a++, b++, n--) {
        gwc x = g_towlower(*a), y = g_towlower(*b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}

enum { WC_ALNUM = 1, WC_ALPHA, WC_BLANK, WC_CNTRL, WC_DIGIT, WC_GRAPH, WC_LOWER, WC_PRINT, WC_PUNCT, WC_SPACE, WC_UPPER, WC_XDIGIT };

/* A wctype_t is a long in bionic: one of these numbers, 0 for none. */
static glong g_wctype(const char *name)
{
    static const char *const names[] = { "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit" };
    int i;
    for (i = 0; i < 12; i++)
        if (!strcmp(name, names[i])) return i + 1;
    return 0;
}

static int g_iswctype(gwc c, glong type)
{
    switch (type) {
    case WC_ALNUM: return g_iswalnum(c);
    case WC_ALPHA: return g_iswalpha(c);
    case WC_BLANK: return g_iswblank(c);
    case WC_CNTRL: return g_iswcntrl(c);
    case WC_DIGIT: return g_iswdigit(c);
    case WC_GRAPH: return g_iswgraph(c);
    case WC_LOWER: return g_iswlower(c);
    case WC_PRINT: return g_iswprint(c);
    case WC_PUNCT: return g_iswpunct(c);
    case WC_SPACE: return g_iswspace(c);
    case WC_UPPER: return g_iswupper(c);
    case WC_XDIGIT: return g_iswxdigit(c);
    default: return 0;
    }
}

static int g_wcwidth(gwc c) { return c == 0 ? 0 : g_iswcntrl(c) ? -1 : (c >= 0x1100 && (c <= 0x115F || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFF60))) ? 2 : 1; }

#define WL1(name) static int name##_l(gwc c, void *loc) { (void)loc; return name(c); }
WL1(g_iswalpha) WL1(g_iswdigit) WL1(g_iswalnum) WL1(g_iswxdigit) WL1(g_iswspace) WL1(g_iswblank) WL1(g_iswcntrl) WL1(g_iswprint)
WL1(g_iswgraph) WL1(g_iswpunct) WL1(g_iswupper) WL1(g_iswlower)
static gwc g_towlower_l(gwc c, void *loc) { (void)loc; return g_towlower(c); }
static gwc g_towupper_l(gwc c, void *loc) { (void)loc; return g_towupper(c); }
static int g_wcscoll_l(const gwc *a, const gwc *b, void *loc) { (void)loc; return g_wcscmp(a, b); }
static size_t g_wcsxfrm_l(gwc *d, const gwc *s, size_t n, void *loc) { (void)loc; return g_wcsxfrm(d, s, n); }
static int g_iswctype_l(gwc c, glong type, void *loc) { (void)loc; return g_iswctype(c, type); }
static glong g_wctype_l(const char *name, void *loc) { (void)loc; return g_wctype(name); }

#define W(n, f) MG_WRAP(n, f)

const mg_sym mg_tab_wchar[] = {
    W("wcslen", g_wcslen), W("wcsnlen", g_wcsnlen), W("wcscpy", g_wcscpy), W("wcsncpy", g_wcsncpy), W("wcscat", g_wcscat),
    W("wcsncat", g_wcsncat), W("wcscmp", g_wcscmp), W("wcsncmp", g_wcsncmp), W("wcschr", g_wcschr), W("wcsrchr", g_wcsrchr),
    W("wcsstr", g_wcsstr), W("wcsspn", g_wcsspn), W("wcscspn", g_wcscspn), W("wcspbrk", g_wcspbrk), W("wcstok", g_wcstok),
    W("wcsdup", g_wcsdup), W("wmemcpy", g_wmemcpy), W("wmemmove", g_wmemmove), W("wmemset", g_wmemset), W("wmemcmp", g_wmemcmp),
    W("wmemchr", g_wmemchr), W("wcscoll", g_wcscmp), W("wcsxfrm", g_wcsxfrm), W("wcslcpy", g_wcslcpy),
    W("wcscasecmp", g_wcscasecmp), W("wcsncasecmp", g_wcsncasecmp),
    W("mbrtowc", g_mbrtowc), W("mbtowc", g_mbtowc), W("mbrlen", g_mbrlen), W("mblen", g_mblen), W("mbsinit", g_mbsinit),
    W("wcrtomb", g_wcrtomb), W("wctomb", g_wctomb), W("mbsnrtowcs", g_mbsnrtowcs), W("mbsrtowcs", g_mbsrtowcs),
    W("mbstowcs", g_mbstowcs), W("wcsnrtombs", g_wcsnrtombs), W("wcsrtombs", g_wcsrtombs), W("wcstombs", g_wcstombs),
    W("btowc", g_btowc), W("wctob", g_wctob),
    W("wcstol", g_wcstol), W("wcstoul", g_wcstoul), W("wcstoll", g_wcstoll), W("wcstoull", g_wcstoull), W("wcstod", g_wcstod),
    W("wcstold", g_wcstold), W("wcstof", g_wcstof), W("wcstoimax", g_wcstoll), W("wcstoumax", g_wcstoull),
    W("wcstol_l", g_wcstol_l), W("wcstoul_l", g_wcstoul_l), W("wcstoll_l", g_wcstoll_l), W("wcstoull_l", g_wcstoull_l),
    W("wcstod_l", g_wcstod_l), W("wcstold_l", g_wcstold_l), W("wcstof_l", g_wcstof_l),
    W("vswprintf", g_vswprintf), W("swprintf", g_swprintf), W("vswscanf", g_vswscanf), W("swscanf", g_swscanf), W("wcsftime", g_wcsftime),
    W("iswalpha", g_iswalpha), W("iswdigit", g_iswdigit), W("iswalnum", g_iswalnum), W("iswxdigit", g_iswxdigit),
    W("iswspace", g_iswspace), W("iswblank", g_iswblank), W("iswcntrl", g_iswcntrl), W("iswprint", g_iswprint),
    W("iswgraph", g_iswgraph), W("iswpunct", g_iswpunct), W("iswupper", g_iswupper), W("iswlower", g_iswlower),
    W("towlower", g_towlower), W("towupper", g_towupper), W("wctype", g_wctype), W("iswctype", g_iswctype), W("wcwidth", g_wcwidth),
    W("iswalpha_l", g_iswalpha_l), W("iswdigit_l", g_iswdigit_l), W("iswalnum_l", g_iswalnum_l), W("iswxdigit_l", g_iswxdigit_l),
    W("iswspace_l", g_iswspace_l), W("iswblank_l", g_iswblank_l), W("iswcntrl_l", g_iswcntrl_l), W("iswprint_l", g_iswprint_l),
    W("iswgraph_l", g_iswgraph_l), W("iswpunct_l", g_iswpunct_l), W("iswupper_l", g_iswupper_l), W("iswlower_l", g_iswlower_l),
    W("towlower_l", g_towlower_l), W("towupper_l", g_towupper_l), W("wcscoll_l", g_wcscoll_l), W("wcsxfrm_l", g_wcsxfrm_l),
    W("iswctype_l", g_iswctype_l), W("wctype_l", g_wctype_l),
    MG_END
};
