/* <string.h>, <ctype.h>, and the number parsing of <stdlib.h>. */
#include "mg_bionic.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------- what the host lacks */

static void *g_memrchr(const void *s, int c, size_t n)
{
    const u8 *p = (const u8 *)s + n;
    while (p > (const u8 *)s)
        if (*--p == (u8)c) return (void *)p;
    return NULL;
}

static void *g_memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
    const u8 *h = (const u8 *)hay;
    size_t i;
    if (!nn) return (void *)hay;
    if (hn < nn) return NULL;
    for (i = 0; i + nn <= hn; i++)
        if (h[i] == *(const u8 *)needle && !memcmp(h + i, needle, nn)) return (void *)(h + i);
    return NULL;
}

static void *g_mempcpy(void *d, const void *s, size_t n) { return (u8 *)memcpy(d, s, n) + n; }

static int g_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((u8)*a), y = tolower((u8)*b);
        if (x != y || !x) return x - y;
    }
}

static int g_strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; a++, b++, n--) {
        int x = tolower((u8)*a), y = tolower((u8)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}

static char *g_strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++)
        if (!g_strncasecmp(hay, needle, n)) return (char *)hay;
    return NULL;
}

static size_t g_strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

static size_t g_strlcat(char *dst, const char *src, size_t cap)
{
    size_t d = g_strnlen(dst, cap), s = strlen(src);
    if (d == cap) return cap + s;
    if (s < cap - d) memcpy(dst + d, src, s + 1);
    else {
        memcpy(dst + d, src, cap - d - 1);
        dst[cap - 1] = 0;
    }
    return d + s;
}

static char *g_stpcpy(char *d, const char *s)
{
    size_t n = strlen(s);
    memcpy(d, s, n + 1);
    return d + n;
}

static char *g_stpncpy(char *d, const char *s, size_t n)
{
    size_t len = g_strnlen(s, n);
    memcpy(d, s, len);
    memset(d + len, 0, n - len);
    return d + len;
}

static char *g_strndup(const char *s, size_t n)
{
    size_t len = g_strnlen(s, n);
    char *d = (char *)mg_g_malloc(len + 1);
    if (!d) return NULL;
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}

static char *g_strtok_r(char *s, const char *delim, char **save)
{
    char *end;
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) {
        *save = NULL;
        return NULL;
    }
    end = s + strcspn(s, delim);
    if (*end) {
        *end = 0;
        *save = end + 1;
    } else {
        *save = NULL;
    }
    return s;
}

static char *g_strtok(char *s, const char *delim)
{
    static MG_THREAD_LOCAL char *save;
    return g_strtok_r(s, delim, &save);
}

static char *g_strsep(char **sp, const char *delim)
{
    char *s = *sp, *end;
    if (!s) return NULL;
    end = s + strcspn(s, delim);
    if (*end) {
        *end = 0;
        *sp = end + 1;
    } else {
        *sp = NULL;
    }
    return s;
}

static char *g_strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

static void g_bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
static void g_bzero(void *d, size_t n) { memset(d, 0, n); }
static int g_bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static char *g_index(const char *s, int c) { return (char *)strchr(s, c); }
static char *g_rindex(const char *s, int c) { return (char *)strrchr(s, c); }

static int g_ffs(int v)
{
    int n = 1;
    if (!v) return 0;
    while (!(v & 1)) {
        v = (int)((unsigned)v >> 1);
        n++;
    }
    return n;
}

/* The host's own are intrinsics on some compilers; these make sure an address exists. */
static void *g_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *g_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *g_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int g_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static void *g_memchr(const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
static size_t g_strlen(const char *s) { return strlen(s); }
static char *g_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *g_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static char *g_strcat(char *d, const char *s) { return strcat(d, s); }
static char *g_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static int g_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int g_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static char *g_strchr(const char *s, int c) { return (char *)strchr(s, c); }
static char *g_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
static char *g_strstr(const char *a, const char *b) { return (char *)strstr(a, b); }
static size_t g_strspn(const char *a, const char *b) { return strspn(a, b); }
static size_t g_strcspn(const char *a, const char *b) { return strcspn(a, b); }
static char *g_strpbrk(const char *a, const char *b) { return (char *)strpbrk(a, b); }
static size_t g_strxfrm(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) mg_strlcpy(d, s, n);
    return len;
}

/* FORTIFY's checked variants: the check is the compiler's business, the work is the plain function's. */
static void *g_memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *g_memmove_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memmove(d, s, n); }
static void *g_memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }
static char *g_strcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return strcpy(d, s); }
static char *g_strncpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncpy(d, s, n); }
static char *g_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) { (void)dn; (void)sn; return strncpy(d, s, n); }
static char *g_strcat_chk(char *d, const char *s, size_t dn) { (void)dn; return strcat(d, s); }
static char *g_strncat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return strncat(d, s, n); }
static size_t g_strlen_chk(const char *s, size_t sn) { (void)sn; return strlen(s); }
static char *g_strchr_chk(const char *s, int c, size_t sn) { (void)sn; return (char *)strchr(s, c); }
static char *g_strrchr_chk(const char *s, int c, size_t sn) { (void)sn; return (char *)strrchr(s, c); }
static size_t g_strlcpy_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return mg_strlcpy(d, s, n); }
static size_t g_strlcat_chk(char *d, const char *s, size_t n, size_t dn) { (void)dn; return g_strlcat(d, s, n); }
static void *g_memchr_chk(const void *s, int c, size_t n, size_t sn) { (void)sn; return (void *)memchr(s, c, n); }
static void *g_memrchr_chk(const void *s, int c, size_t n, size_t sn) { (void)sn; return g_memrchr(s, c, n); }
static char *g_stpcpy_chk(char *d, const char *s, size_t dn) { (void)dn; return g_stpcpy(d, s); }

/* ------------------------------------------------------------------ errors */

static const char *errno_text(int e)
{
    switch (e) {
    case 0: return "Success";
    case GE_PERM: return "Operation not permitted";
    case GE_NOENT: return "No such file or directory";
    case GE_SRCH: return "No such process";
    case GE_INTR: return "Interrupted system call";
    case GE_IO: return "I/O error";
    case GE_NXIO: return "No such device or address";
    case GE_2BIG: return "Argument list too long";
    case GE_NOEXEC: return "Exec format error";
    case GE_BADF: return "Bad file descriptor";
    case GE_CHILD: return "No child processes";
    case GE_AGAIN: return "Try again";
    case GE_NOMEM: return "Out of memory";
    case GE_ACCES: return "Permission denied";
    case GE_FAULT: return "Bad address";
    case GE_BUSY: return "Device or resource busy";
    case GE_EXIST: return "File exists";
    case GE_XDEV: return "Cross-device link";
    case GE_NODEV: return "No such device";
    case GE_NOTDIR: return "Not a directory";
    case GE_ISDIR: return "Is a directory";
    case GE_INVAL: return "Invalid argument";
    case GE_NFILE: return "File table overflow";
    case GE_MFILE: return "Too many open files";
    case GE_NOTTY: return "Not a typewriter";
    case GE_FBIG: return "File too large";
    case GE_NOSPC: return "No space left on device";
    case GE_SPIPE: return "Illegal seek";
    case GE_ROFS: return "Read-only file system";
    case GE_PIPE: return "Broken pipe";
    case GE_DOM: return "Math argument out of domain of func";
    case GE_RANGE: return "Math result not representable";
    case GE_DEADLK: return "Resource deadlock would occur";
    case GE_NAMETOOLONG: return "File name too long";
    case GE_NOSYS: return "Function not implemented";
    case GE_NOTEMPTY: return "Directory not empty";
    case GE_ILSEQ: return "Illegal byte sequence";
    case GE_OVERFLOW: return "Value too large for defined data type";
    case GE_NOTSOCK: return "Socket operation on non-socket";
    case GE_OPNOTSUPP: return "Operation not supported on transport endpoint";
    case GE_AFNOSUPPORT: return "Address family not supported by protocol";
    case GE_ADDRINUSE: return "Address already in use";
    case GE_NETUNREACH: return "Network is unreachable";
    case GE_CONNABORTED: return "Software caused connection abort";
    case GE_CONNRESET: return "Connection reset by peer";
    case GE_NOBUFS: return "No buffer space available";
    case GE_ISCONN: return "Transport endpoint is already connected";
    case GE_NOTCONN: return "Transport endpoint is not connected";
    case GE_TIMEDOUT: return "Connection timed out";
    case GE_CONNREFUSED: return "Connection refused";
    case GE_HOSTUNREACH: return "No route to host";
    case GE_ALREADY: return "Operation already in progress";
    case GE_INPROGRESS: return "Operation now in progress";
    default: return NULL;
    }
}

static char *g_strerror(int e)
{
    static MG_THREAD_LOCAL char buf[40];
    const char *t = errno_text(e);
    if (t) return (char *)t;
    snprintf(buf, sizeof(buf), "Unknown error %d", e);
    return buf;
}

/* bionic's strerror_r is the POSIX one (an int result) unless the app asked for GNU's; both fill the buffer. */
static int g_strerror_r(int e, char *buf, size_t n)
{
    mg_strlcpy(buf, g_strerror(e), n);
    return 0;
}

static char *g_gnu_strerror_r(int e, char *buf, size_t n)
{
    mg_strlcpy(buf, g_strerror(e), n);
    return buf;
}

static char *g_strsignal(int sig)
{
    static MG_THREAD_LOCAL char buf[32];
    switch (sig) {
    case 2: return (char *)"Interrupt";
    case 4: return (char *)"Illegal instruction";
    case 6: return (char *)"Aborted";
    case 7: return (char *)"Bus error";
    case 8: return (char *)"Floating point exception";
    case 9: return (char *)"Killed";
    case 11: return (char *)"Segmentation fault";
    case 13: return (char *)"Broken pipe";
    case 15: return (char *)"Terminated";
    default:
        snprintf(buf, sizeof(buf), "Signal %d", sig);
        return buf;
    }
}

/* ----------------------------------------------------------------- numbers */

/* The host's parsers set the host's errno; ERANGE has the same number on Linux, in bionic and in the UCRT. */
#define PARSE(ret, name, call)        \
    {                                 \
        ret v;                        \
        errno = 0;                    \
        v = call;                     \
        if (errno) mg_set_errno(GE_RANGE); \
        return v;                     \
    }

static long g_strtol(const char *s, char **end, int base) PARSE(long, strtol, strtol(s, end, base))
static unsigned long g_strtoul(const char *s, char **end, int base) PARSE(unsigned long, strtoul, strtoul(s, end, base))
static i64 g_strtoll(const char *s, char **end, int base) PARSE(i64, strtoll, strtoll(s, end, base))
static u64 g_strtoull(const char *s, char **end, int base) PARSE(u64, strtoull, strtoull(s, end, base))
static double parse_double(const char *s, char **end) PARSE(double, strtod, strtod(s, end))
static float parse_float(const char *s, char **end) PARSE(float, strtof, strtof(s, end))
static gdouble g_strtod(const char *s, char **end) { return mg_gdb(parse_double(s, end)); }
static gfloat g_strtof(const char *s, char **end) { return mg_gfb(parse_float(s, end)); }
static gldouble g_strtold(const char *s, char **end) { return mg_gldb(parse_double(s, end)); }
static long g_strtol_l(const char *s, char **end, int base, void *loc) { (void)loc; return g_strtol(s, end, base); }
static unsigned long g_strtoul_l(const char *s, char **end, int base, void *loc) { (void)loc; return g_strtoul(s, end, base); }
static i64 g_strtoll_l(const char *s, char **end, int base, void *loc) { (void)loc; return g_strtoll(s, end, base); }
static u64 g_strtoull_l(const char *s, char **end, int base, void *loc) { (void)loc; return g_strtoull(s, end, base); }
static gdouble g_strtod_l(const char *s, char **end, void *loc) { (void)loc; return g_strtod(s, end); }
static gfloat g_strtof_l(const char *s, char **end, void *loc) { (void)loc; return g_strtof(s, end); }
static gldouble g_strtold_l(const char *s, char **end, void *loc) { (void)loc; return g_strtold(s, end); }
static int g_atoi(const char *s) { return (int)strtol(s, NULL, 10); }
static long g_atol(const char *s) { return strtol(s, NULL, 10); }
static i64 g_atoll(const char *s) { return strtoll(s, NULL, 10); }
static gdouble g_atof(const char *s) { return mg_gdb(strtod(s, NULL)); }
static int g_abs(int v) { return v < 0 ? -v : v; }
static glong g_labs(glong v) { return v < 0 ? -v : v; }
static i64 g_llabs(i64 v) { return v < 0 ? -v : v; }

/* div_t, ldiv_t (two longs: two ints in 32-bit Android, as div_t) and lldiv_t, returned by value. */
typedef struct g_div_t { int quot, rem; } g_div_t;
typedef struct g_ldiv_t { glong quot, rem; } g_ldiv_t;
typedef struct g_lldiv_t { i64 quot, rem; } g_lldiv_t;
static g_div_t g_div(int a, int b) { g_div_t r; r.quot = a / b; r.rem = a % b; return r; }
static g_ldiv_t g_ldiv(glong a, glong b) { g_ldiv_t r; r.quot = a / b; r.rem = a % b; return r; }
static g_lldiv_t g_lldiv(i64 a, i64 b) { g_lldiv_t r; r.quot = a / b; r.rem = a % b; return r; }

/* ------------------------------------------------------------------- ctype
 *
 * The C locale, always: Android has no other for these. Old NDK headers test
 * a table (_ctype_) inline instead of calling a function, so the table exists.
 */
#define CT_U 0x01
#define CT_L 0x02
#define CT_N 0x04
#define CT_S 0x08
#define CT_P 0x10
#define CT_C 0x20
#define CT_X 0x40
#define CT_B 0x80

static char g_ctype_table[257];
static short g_tolower_table[257], g_toupper_table[257];
static const char *g_ctype_ptr = g_ctype_table;
static const short *g_tolower_ptr = g_tolower_table, *g_toupper_ptr = g_toupper_table;

void mg_ctype_init(void)
{
    int c;
    for (c = 0; c < 256; c++) {
        char f = 0;
        if (c < 128) {
            if (c >= 'A' && c <= 'Z') f |= CT_U;
            if (c >= 'a' && c <= 'z') f |= CT_L;
            if (c >= '0' && c <= '9') f |= CT_N;
            if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= CT_X;
            if (c == ' ' || (c >= 9 && c <= 13)) f |= CT_S;
            if (c == ' ') f |= (char)CT_B;
            if (c < 32 || c == 127) f |= CT_C;
            if (c > 32 && c < 127 && !(f & (CT_U | CT_L | CT_N))) f |= CT_P;
        }
        g_ctype_table[c + 1] = f;
        g_tolower_table[c + 1] = (short)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        g_toupper_table[c + 1] = (short)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    g_tolower_table[0] = g_toupper_table[0] = -1;
}

#define CT(c) ((unsigned)(c) < 256 ? g_ctype_table[(c) + 1] : 0)
static int g_isalpha(int c) { return (CT(c) & (CT_U | CT_L)) != 0; }
static int g_isupper(int c) { return (CT(c) & CT_U) != 0; }
static int g_islower(int c) { return (CT(c) & CT_L) != 0; }
static int g_isdigit(int c) { return (CT(c) & CT_N) != 0; }
static int g_isxdigit(int c) { return (CT(c) & (CT_N | CT_X)) != 0; }
static int g_isspace(int c) { return (CT(c) & CT_S) != 0; }
static int g_ispunct(int c) { return (CT(c) & CT_P) != 0; }
static int g_isalnum(int c) { return (CT(c) & (CT_U | CT_L | CT_N)) != 0; }
static int g_isprint(int c) { return c >= 32 && c < 127; }
static int g_isgraph(int c) { return c > 32 && c < 127; }
static int g_iscntrl(int c) { return (CT(c) & CT_C) != 0; }
static int g_isblank(int c) { return c == ' ' || c == '\t'; }
static int g_isascii(int c) { return (unsigned)c < 128; }
static int g_toascii(int c) { return c & 127; }
static int g_tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int g_toupper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

#define CT_L_VARIANT(name) static int name##_l(int c, void *loc) { (void)loc; return name(c); }
CT_L_VARIANT(g_isalpha) CT_L_VARIANT(g_isupper) CT_L_VARIANT(g_islower) CT_L_VARIANT(g_isdigit) CT_L_VARIANT(g_isxdigit)
CT_L_VARIANT(g_isspace) CT_L_VARIANT(g_ispunct) CT_L_VARIANT(g_isalnum) CT_L_VARIANT(g_isprint) CT_L_VARIANT(g_isgraph)
CT_L_VARIANT(g_iscntrl) CT_L_VARIANT(g_isblank) CT_L_VARIANT(g_tolower) CT_L_VARIANT(g_toupper)

static int g_strcoll_l(const char *a, const char *b, void *loc) { (void)loc; return strcmp(a, b); }
static size_t g_strxfrm_l(char *d, const char *s, size_t n, void *loc) { (void)loc; return g_strxfrm(d, s, n); }
static int g_strcasecmp_l(const char *a, const char *b, void *loc) { (void)loc; return g_strcasecmp(a, b); }
static int g_strncasecmp_l(const char *a, const char *b, size_t n, void *loc) { (void)loc; return g_strncasecmp(a, b, n); }

/*
 * fnmatch, as bionic's (OpenBSD's): `*`, `?`, bracket expressions (ranges; `!` or `^` to negate; a `]` first is one of
 * them; one not closed is a `[` like any other), and a backslash escaping the next unless FNM_NOESCAPE. FNM_PATHNAME: a
 * slash is matched only by a slash. FNM_PERIOD: a leading period (and, with FNM_PATHNAME, one after a slash) only by a
 * period. FNM_LEADING_DIR: the string may go on after a slash where the pattern ends. FNM_CASEFOLD: letters in either
 * case. 0 when the string matches, FNM_NOMATCH when not.
 */
enum { G_FNM_NOMATCH = 1, G_FNM_NOESCAPE = 1, G_FNM_PATHNAME = 2, G_FNM_PERIOD = 4, G_FNM_LEADING_DIR = 8, G_FNM_CASEFOLD = 0x10 };

static int fn_fold(int c, int flags) { return (flags & G_FNM_CASEFOLD) && c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

static bool fn_leading_period(const char *s, const char *start, int flags)
{
    return (flags & G_FNM_PERIOD) && *s == '.' && (s == start || ((flags & G_FNM_PATHNAME) && s[-1] == '/'));
}

/* The bracket expression at p (just after its `[`) against c: 1 in it, 0 not, -1 not closed. *end: just after its `]`. */
static int fn_bracket(const char *p, int c, int flags, const char **end)
{
    bool negate = *p == '!' || *p == '^', in = false;
    if (negate) p++;
    c = fn_fold(c, flags);
    do {
        int lo = (u8)*p, hi;
        if (!lo) return -1;
        if (lo == '\\' && !(flags & G_FNM_NOESCAPE) && p[1]) lo = (u8)*++p;
        p++;
        hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            hi = (u8)*++p;
            if (hi == '\\' && !(flags & G_FNM_NOESCAPE) && p[1]) hi = (u8)*++p;
            p++;
        }
        if (fn_fold(lo, flags) <= c && c <= fn_fold(hi, flags)) in = true;
    } while (*p != ']');
    *end = p + 1;
    return in != negate;
}

static int fn_match(const char *p, const char *s, const char *start, int flags)
{
    for (;;) {
        int c = (u8)*p++, r;
        const char *after;
        switch (c) {
        case 0:
            return !*s || ((flags & G_FNM_LEADING_DIR) && *s == '/') ? 0 : G_FNM_NOMATCH;
        case '?':
            if (!*s || (*s == '/' && (flags & G_FNM_PATHNAME)) || fn_leading_period(s, start, flags)) return G_FNM_NOMATCH;
            s++;
            break;
        case '*':
            while (*p == '*') p++;
            if (fn_leading_period(s, start, flags)) return G_FNM_NOMATCH;
            if (!*p) return !(flags & G_FNM_PATHNAME) || (flags & G_FNM_LEADING_DIR) || !strchr(s, '/') ? 0 : G_FNM_NOMATCH;
            if (*p == '/' && (flags & G_FNM_PATHNAME)) {
                s = strchr(s, '/');
                if (!s) return G_FNM_NOMATCH;
                break;
            }
            for (; *s; s++) {
                if (!fn_match(p, s, s, flags & ~G_FNM_PERIOD)) return 0;
                if (*s == '/' && (flags & G_FNM_PATHNAME)) break;
            }
            return G_FNM_NOMATCH;
        case '[':
            if (!*s || (*s == '/' && (flags & G_FNM_PATHNAME)) || fn_leading_period(s, start, flags)) return G_FNM_NOMATCH;
            r = fn_bracket(p, (u8)*s, flags, &after);
            if (r == 0) return G_FNM_NOMATCH;
            if (r > 0) {
                p = after;
                s++;
                break;
            }
            if ((u8)*s != '[') return G_FNM_NOMATCH;             /* not closed: a `[` like any other */
            s++;
            break;
        case '\\':
            if (!(flags & G_FNM_NOESCAPE) && *p) c = (u8)*p++;
            /* fall through */
        default:
            if (fn_fold(c, flags) != fn_fold((u8)*s, flags)) return G_FNM_NOMATCH;
            s++;
            break;
        }
    }
}

static int g_fnmatch(const char *pattern, const char *s, int flags) { return fn_match(pattern, s, s, flags); }

const mg_sym mg_tab_str[] = {
    MG_WRAP("fnmatch", g_fnmatch),
    MG_WRAP("memcpy", g_memcpy), MG_WRAP("memmove", g_memmove), MG_WRAP("memset", g_memset), MG_WRAP("memcmp", g_memcmp),
    MG_WRAP("memchr", g_memchr), MG_WRAP("memrchr", g_memrchr), MG_WRAP("memmem", g_memmem), MG_WRAP("mempcpy", g_mempcpy),
    MG_WRAP("strlen", g_strlen), MG_WRAP("strnlen", g_strnlen), MG_WRAP("strcpy", g_strcpy), MG_WRAP("strncpy", g_strncpy),
    MG_WRAP("strcat", g_strcat), MG_WRAP("strncat", g_strncat), MG_WRAP("strcmp", g_strcmp), MG_WRAP("strncmp", g_strncmp),
    MG_WRAP("strchr", g_strchr), MG_WRAP("strrchr", g_strrchr), MG_WRAP("strstr", g_strstr), MG_WRAP("strspn", g_strspn),
    MG_WRAP("strcspn", g_strcspn), MG_WRAP("strpbrk", g_strpbrk), MG_WRAP("strcoll", g_strcmp), MG_WRAP("strxfrm", g_strxfrm),
    MG_WRAP("strcasecmp", g_strcasecmp), MG_WRAP("strncasecmp", g_strncasecmp), MG_WRAP("strcasestr", g_strcasestr),
    MG_WRAP("strlcpy", mg_strlcpy), MG_WRAP("strlcat", g_strlcat), MG_WRAP("stpcpy", g_stpcpy), MG_WRAP("stpncpy", g_stpncpy),
    MG_WRAP("strdup", mg_g_strdup), MG_WRAP("strndup", g_strndup), MG_WRAP("strtok", g_strtok), MG_WRAP("strtok_r", g_strtok_r),
    MG_WRAP("strsep", g_strsep), MG_WRAP("strchrnul", g_strchrnul), MG_WRAP("bcopy", g_bcopy), MG_WRAP("bzero", g_bzero),
    MG_WRAP("bcmp", g_bcmp), MG_WRAP("index", g_index), MG_WRAP("rindex", g_rindex), MG_WRAP("ffs", g_ffs),
    MG_WRAP("__memcpy_chk", g_memcpy_chk), MG_WRAP("__memmove_chk", g_memmove_chk), MG_WRAP("__memset_chk", g_memset_chk),
    MG_WRAP("__strcpy_chk", g_strcpy_chk), MG_WRAP("__strncpy_chk", g_strncpy_chk), MG_WRAP("__strncpy_chk2", g_strncpy_chk2),
    MG_WRAP("__strcat_chk", g_strcat_chk), MG_WRAP("__strncat_chk", g_strncat_chk), MG_WRAP("__strlen_chk", g_strlen_chk),
    MG_WRAP("__strchr_chk", g_strchr_chk), MG_WRAP("__strrchr_chk", g_strrchr_chk), MG_WRAP("__strlcpy_chk", g_strlcpy_chk),
    MG_WRAP("__strlcat_chk", g_strlcat_chk), MG_WRAP("__memchr_chk", g_memchr_chk), MG_WRAP("__memrchr_chk", g_memrchr_chk),
    MG_WRAP("__stpcpy_chk", g_stpcpy_chk),
    MG_WRAP("strerror", g_strerror), MG_WRAP("strerror_r", g_strerror_r), MG_WRAP("__gnu_strerror_r", g_gnu_strerror_r),
    MG_WRAP("strerror_l", g_strerror), MG_WRAP("strsignal", g_strsignal),
    MG_WRAP("strtol", g_strtol), MG_WRAP("strtoul", g_strtoul), MG_WRAP("strtoll", g_strtoll), MG_WRAP("strtoull", g_strtoull),
    MG_WRAP("strtoimax", g_strtoll), MG_WRAP("strtoumax", g_strtoull), MG_WRAP("strtoq", g_strtoll), MG_WRAP("strtouq", g_strtoull),
    MG_WRAP("strtod", g_strtod), MG_WRAP("strtold", g_strtold), MG_WRAP("strtof", g_strtof),
    MG_WRAP("strtol_l", g_strtol_l), MG_WRAP("strtoul_l", g_strtoul_l), MG_WRAP("strtoll_l", g_strtoll_l),
    MG_WRAP("strtoull_l", g_strtoull_l), MG_WRAP("strtod_l", g_strtod_l), MG_WRAP("strtold_l", g_strtold_l), MG_WRAP("strtof_l", g_strtof_l),
    MG_WRAP("atoi", g_atoi), MG_WRAP("atol", g_atol), MG_WRAP("atoll", g_atoll), MG_WRAP("atof", g_atof),
    MG_WRAP("abs", g_abs), MG_WRAP("labs", g_labs), MG_WRAP("llabs", g_llabs), MG_WRAP("imaxabs", g_llabs),
    MG_WRAP("div", g_div), MG_WRAP("ldiv", g_ldiv), MG_WRAP("lldiv", g_lldiv), MG_WRAP("imaxdiv", g_lldiv),
    MG_WRAP("isalpha", g_isalpha), MG_WRAP("isupper", g_isupper), MG_WRAP("islower", g_islower), MG_WRAP("isdigit", g_isdigit),
    MG_WRAP("isxdigit", g_isxdigit), MG_WRAP("isspace", g_isspace), MG_WRAP("ispunct", g_ispunct), MG_WRAP("isalnum", g_isalnum),
    MG_WRAP("isprint", g_isprint), MG_WRAP("isgraph", g_isgraph), MG_WRAP("iscntrl", g_iscntrl), MG_WRAP("isblank", g_isblank),
    MG_WRAP("isascii", g_isascii), MG_WRAP("toascii", g_toascii), MG_WRAP("tolower", g_tolower), MG_WRAP("toupper", g_toupper),
    MG_WRAP("_tolower", g_tolower), MG_WRAP("_toupper", g_toupper),
    MG_WRAP("isalpha_l", g_isalpha_l), MG_WRAP("isupper_l", g_isupper_l), MG_WRAP("islower_l", g_islower_l),
    MG_WRAP("isdigit_l", g_isdigit_l), MG_WRAP("isxdigit_l", g_isxdigit_l), MG_WRAP("isspace_l", g_isspace_l),
    MG_WRAP("ispunct_l", g_ispunct_l), MG_WRAP("isalnum_l", g_isalnum_l), MG_WRAP("isprint_l", g_isprint_l),
    MG_WRAP("isgraph_l", g_isgraph_l), MG_WRAP("iscntrl_l", g_iscntrl_l), MG_WRAP("isblank_l", g_isblank_l),
    MG_WRAP("tolower_l", g_tolower_l), MG_WRAP("toupper_l", g_toupper_l),
    MG_WRAP("strcoll_l", g_strcoll_l), MG_WRAP("strxfrm_l", g_strxfrm_l), MG_WRAP("strcasecmp_l", g_strcasecmp_l),
    MG_WRAP("strncasecmp_l", g_strncasecmp_l),
    MG_DATA("_ctype_", &g_ctype_ptr), MG_DATA("_tolower_tab_", &g_tolower_ptr), MG_DATA("_toupper_tab_", &g_toupper_ptr),
    MG_END
};
