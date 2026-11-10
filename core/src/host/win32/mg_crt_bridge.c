/*
 * The C library functions the core's ELF calls, given it by Windows' own C library (the UCRT) on desktop Windows
 * (docs/DESKTOP.md, "The core as an ELF").
 *
 * The core is compiled for Android's ABI and against bionic's headers, so what it calls by those names has to behave as
 * bionic's would for code of that ABI. Most of it is the UCRT's function as it is, wrapped here only so that it is called
 * with Android's convention: on x86_64 every function below is System V's (MGC), which clang compiles as a shim around
 * the call of Windows' function; on ARM64 and x86 the conventions are one and the wrapper is a jump. What differs beyond
 * the convention is done here:
 *
 *   - `long` is 64 bits on Android's 64-bit ABIs and 32 on Windows': strtol, strtoul, lround, scalbln, time and struct tm
 *     take and give the guest's width (glong);
 *   - `wchar_t` is 32 bits on Android: wcslen counts those;
 *   - a comparator handed to qsort or bsearch is the core's code, called with the core's convention;
 *   - struct tm is bionic's, with tm_gmtoff and tm_zone after the nine fields Windows has;
 *   - stdout and stderr are objects the core reads (bionic's `FILE *stdout`), here holding the UCRT's streams;
 *   - thread-local data: the core's `__thread` variables are emulated (-femulated-tls), and __emutls_get_address is
 *     here, on fiber-local storage, whose destructor frees a thread's copies when it ends.
 *
 * printf, scanf and their kin are not here: their variable arguments are Android's, which Windows' functions cannot read,
 * so they are the core's own (core/src/crt/mg_crt_format.c), which calls mgc_format_double below for a floating-point
 * conversion.
 *
 * Compiled only into the desktop builds (MG_ELF_HOST).
 */
#if defined(MG_ELF_HOST)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mg_core_loader.h"

#if MG_ARCH_X86_64
#define MGC __attribute__((sysv_abi))
#else
#define MGC
#endif

#if MG_GUEST_LP64
typedef int64_t glong;
typedef uint64_t gulong;
#else
typedef int32_t glong;
typedef uint32_t gulong;
#endif

/* ------------------------------------------------------------------ math */

#define D1(n) static MGC double mgc_##n(double x) { return n(x); }
#define F1(n) static MGC float mgc_##n(float x) { return n(x); }
#define D2(n) static MGC double mgc_##n(double x, double y) { return n(x, y); }
#define F2(n) static MGC float mgc_##n(float x, float y) { return n(x, y); }

D1(acos) F1(acosf) D1(acosh) F1(acoshf) D1(asin) F1(asinf) D1(asinh) F1(asinhf) D1(atan) F1(atanf) D1(atanh) F1(atanhf)
D1(cbrt) F1(cbrtf) D1(ceil) F1(ceilf) D1(cos) F1(cosf) D1(cosh) F1(coshf) D1(erf) F1(erff) D1(erfc) F1(erfcf)
D1(exp) F1(expf) D1(exp2) F1(exp2f) D1(expm1) F1(expm1f) D1(floor) F1(floorf) D1(lgamma) F1(lgammaf)
D1(log) F1(logf) D1(log10) F1(log10f) D1(log1p) F1(log1pf) D1(log2) F1(log2f) D1(logb) F1(logbf)
D1(rint) F1(rintf) D1(round) F1(roundf) D1(sin) F1(sinf) D1(sinh) F1(sinhf) D1(sqrt) F1(sqrtf) D1(tan) F1(tanf)
D1(tanh) F1(tanhf) D1(tgamma) F1(tgammaf) D1(trunc) F1(truncf) D1(nearbyint) F1(nearbyintf) D1(fabs) F1(fabsf)
D2(atan2) F2(atan2f) D2(fdim) F2(fdimf) D2(fmod) F2(fmodf) D2(hypot) F2(hypotf) D2(nextafter) F2(nextafterf)
D2(pow) F2(powf) D2(remainder) F2(remainderf) D2(copysign) F2(copysignf) D2(fmax) F2(fmaxf) D2(fmin) F2(fminf)

static MGC double mgc_fma(double x, double y, double z) { return fma(x, y, z); }
static MGC float mgc_fmaf(float x, float y, float z) { return fmaf(x, y, z); }
static MGC double mgc_frexp(double x, int *e) { return frexp(x, e); }
static MGC float mgc_frexpf(float x, int *e) { return frexpf(x, e); }
static MGC int mgc_ilogb(double x) { return ilogb(x); }
static MGC int mgc_ilogbf(float x) { return ilogbf(x); }
static MGC double mgc_ldexp(double x, int e) { return ldexp(x, e); }
static MGC float mgc_ldexpf(float x, int e) { return ldexpf(x, e); }
static MGC double mgc_scalbn(double x, int e) { return scalbn(x, e); }
static MGC float mgc_scalbnf(float x, int e) { return scalbnf(x, e); }
static MGC long long mgc_llround(double x) { return llround(x); }
static MGC long long mgc_llroundf(float x) { return llroundf(x); }
static MGC long long mgc_llrint(double x) { return llrint(x); }
static MGC long long mgc_llrintf(float x) { return llrintf(x); }
/* (A guest's long: the 64-bit rounding where it is 64 bits.) */
static MGC glong mgc_lround(double x) { return (glong)llround(x); }
static MGC glong mgc_lroundf(float x) { return (glong)llroundf(x); }
static MGC glong mgc_lrint(double x) { return (glong)llrint(x); }
static MGC glong mgc_lrintf(float x) { return (glong)llrintf(x); }
static int clamp_exp(glong n) { return n > 100000 ? 100000 : n < -100000 ? -100000 : (int)n; }
static MGC double mgc_scalbln(double x, glong n) { return scalbn(x, clamp_exp(n)); }
static MGC float mgc_scalblnf(float x, glong n) { return scalbnf(x, clamp_exp(n)); }
static MGC double mgc_modf(double x, double *i) { return modf(x, i); }
static MGC float mgc_modff(float x, float *i) { return modff(x, i); }
static MGC double mgc_nan(const char *s) { return nan(s); }
static MGC float mgc_nanf(const char *s) { return nanf(s); }
static MGC double mgc_remquo(double x, double y, int *q) { return remquo(x, y, q); }
static MGC float mgc_remquof(float x, float y, int *q) { return remquof(x, y, q); }
static MGC void mgc_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
static MGC void mgc_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* ------------------------------------------------------- memory, strings */

static MGC void *mgc_malloc(size_t n) { return malloc(n); }
static MGC void *mgc_calloc(size_t n, size_t size) { return calloc(n, size); }
static MGC void *mgc_realloc(void *p, size_t n) { return realloc(p, n); }
static MGC void mgc_free(void *p) { free(p); }
static MGC void *mgc_memchr(const void *s, int c, size_t n) { return (void *)memchr(s, c, n); }
static MGC int mgc_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static MGC void *mgc_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static MGC void *mgc_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static MGC void *mgc_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static MGC char *mgc_strcat(char *d, const char *s) { return strcat(d, s); }
static MGC char *mgc_strchr(const char *s, int c) { return (char *)strchr(s, c); }
static MGC int mgc_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static MGC char *mgc_strcpy(char *d, const char *s) { return strcpy(d, s); }
static MGC size_t mgc_strcspn(const char *s, const char *r) { return strcspn(s, r); }
static MGC size_t mgc_strlen(const char *s) { return strlen(s); }
static MGC size_t mgc_strnlen(const char *s, size_t n) { return strnlen(s, n); }
static MGC char *mgc_strncat(char *d, const char *s, size_t n) { return strncat(d, s, n); }
static MGC int mgc_strncmp(const char *a, const char *b, size_t n) { return strncmp(a, b, n); }
static MGC char *mgc_strncpy(char *d, const char *s, size_t n) { return strncpy(d, s, n); }
static MGC char *mgc_strpbrk(const char *s, const char *a) { return (char *)strpbrk(s, a); }
static MGC char *mgc_strrchr(const char *s, int c) { return (char *)strrchr(s, c); }
static MGC size_t mgc_strspn(const char *s, const char *a) { return strspn(s, a); }
static MGC char *mgc_strstr(const char *s, const char *f) { return (char *)strstr(s, f); }
static MGC char *mgc_strdup(const char *s) { return _strdup(s); }
static MGC int mgc_strcasecmp(const char *a, const char *b) { return _stricmp(a, b); }
static MGC int mgc_strncasecmp(const char *a, const char *b, size_t n) { return _strnicmp(a, b, n); }
/* Android's wchar_t is 32 bits. */
static MGC size_t mgc_wcslen(const uint32_t *s)
{
    const uint32_t *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

#define CT(n) static MGC int mgc_##n(int c) { return n(c); }
CT(isalnum) CT(isalpha) CT(isblank) CT(iscntrl) CT(isdigit) CT(isgraph) CT(islower) CT(isprint) CT(ispunct) CT(isspace)
CT(isupper) CT(isxdigit) CT(tolower) CT(toupper)

/* ------------------------------------------------------------------ stdlib */

static MGC int mgc_atoi(const char *s) { return atoi(s); }
static MGC glong mgc_atol(const char *s) { return (glong)_atoi64(s); }
static MGC long long mgc_atoll(const char *s) { return _atoi64(s); }
static MGC double mgc_atof(const char *s) { return atof(s); }
static MGC double mgc_strtod(const char *s, char **end) { return strtod(s, end); }
static MGC float mgc_strtof(const char *s, char **end) { return strtof(s, end); }
#if MG_GUEST_LP64
static MGC glong mgc_strtol(const char *s, char **end, int base) { return _strtoi64(s, end, base); }
static MGC gulong mgc_strtoul(const char *s, char **end, int base) { return _strtoui64(s, end, base); }
#else
static MGC glong mgc_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
static MGC gulong mgc_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
#endif
static MGC long long mgc_strtoll(const char *s, char **end, int base) { return _strtoi64(s, end, base); }
static MGC unsigned long long mgc_strtoull(const char *s, char **end, int base) { return _strtoui64(s, end, base); }
static MGC char *mgc_getenv(const char *name) { return getenv(name); }
static MGC void mgc_abort(void) { abort(); }
static MGC void mgc_exit(int code) { exit(code); }
static MGC void mgc__Exit(int code) { _Exit(code); }
static MGC int mgc_abs(int x) { return abs(x); }
static MGC glong mgc_labs(glong x) { return x < 0 ? -x : x; }
static MGC long long mgc_llabs(long long x) { return x < 0 ? -x : x; }

/* A comparator of the core's, which has the core's convention: called through Windows' qsort_s and bsearch_s. */
typedef int (MGC *core_cmp)(const void *, const void *);
static int __cdecl cmp_with(void *ctx, const void *a, const void *b) { return ((core_cmp)ctx)(a, b); }
static MGC void mgc_qsort(void *base, size_t n, size_t size, core_cmp cmp) { qsort_s(base, n, size, cmp_with, (void *)cmp); }
static MGC void *mgc_bsearch(const void *key, const void *base, size_t n, size_t size, core_cmp cmp)
{
    return bsearch_s(key, base, n, size, cmp_with, (void *)cmp);
}

/* ------------------------------------------------------------------- time */

/* bionic's struct tm: Windows' nine fields, then the offset from UTC and the zone's name. */
typedef struct gtm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    glong tm_gmtoff;
    const char *tm_zone;
} gtm;

static MGC glong mgc_time(glong *out)
{
    glong t = (glong)_time64(NULL);
    if (out) *out = t;
    return t;
}

static __declspec(thread) gtm t_tm;

static gtm *tm_of(const struct tm *w, bool local, __time64_t t)
{
    if (!w) return NULL;
    t_tm.tm_sec = w->tm_sec;
    t_tm.tm_min = w->tm_min;
    t_tm.tm_hour = w->tm_hour;
    t_tm.tm_mday = w->tm_mday;
    t_tm.tm_mon = w->tm_mon;
    t_tm.tm_year = w->tm_year;
    t_tm.tm_wday = w->tm_wday;
    t_tm.tm_yday = w->tm_yday;
    t_tm.tm_isdst = w->tm_isdst;
    t_tm.tm_gmtoff = 0;
    t_tm.tm_zone = "UTC";
    if (local) {
        struct tm u;
        if (!_gmtime64_s(&u, &t)) t_tm.tm_gmtoff = (glong)(_mkgmtime64((struct tm *)w) - _mkgmtime64(&u));
        t_tm.tm_zone = w->tm_isdst > 0 ? "DST" : "LOC";
    }
    return &t_tm;
}

static MGC gtm *mgc_localtime(const glong *t)
{
    struct tm w;
    __time64_t v = (__time64_t)*t;
    return _localtime64_s(&w, &v) ? NULL : tm_of(&w, true, v);
}

static MGC gtm *mgc_gmtime(const glong *t)
{
    struct tm w;
    __time64_t v = (__time64_t)*t;
    return _gmtime64_s(&w, &v) ? NULL : tm_of(&w, false, v);
}

/* ------------------------------------------------------------------ stdio */

/* bionic's stdout and stderr, objects the core reads: here they hold the UCRT's own streams. */
static FILE *g_stdout, *g_stderr, *g_stdin;

static MGC FILE *mgc_fopen(const char *path, const char *mode) { return fopen(path, mode); }
static MGC int mgc_fclose(FILE *f) { return fclose(f); }
static MGC int mgc_fflush(FILE *f) { return fflush(f); }
static MGC int mgc_fputc(int c, FILE *f) { return fputc(c, f); }
static MGC int mgc_fputs(const char *s, FILE *f) { return fputs(s, f); }
static MGC size_t mgc_fwrite(const void *p, size_t size, size_t n, FILE *f) { return fwrite(p, size, n, f); }
static MGC size_t mgc_fread(void *p, size_t size, size_t n, FILE *f) { return fread(p, size, n, f); }
static MGC int mgc_remove(const char *path) { return remove(path); }

/*
 * One floating-point conversion of printf's, `spec` a whole one ("%-12.5e", no length modifier), into `out`: the core's
 * printf (core/src/crt/mg_crt_format.c) does everything else itself. What snprintf returns.
 */
static MGC int mgc_format_double(char *out, size_t cap, const char *spec, double v) { return snprintf(out, cap, spec, v); }

/* ------------------------------------------------------- errno, assert */

static MGC int *mgc___errno(void) { return _errno(); }

static MGC void mgc___assert2(const char *file, int line, const char *func, const char *msg)
{
    char text[1024];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "megler-core: assertion \"%s\" failed: %s:%d, %s\n", msg, file, line, func ? func : "?");
    OutputDebugStringA(text);
    fputs(text, stderr);
    fflush(stderr);
    abort();
}

/* ------------------------------------------------ thread-local variables */

/* What the compiler emits for each `__thread` variable of the core's under -femulated-tls. */
typedef struct emutls_control {
    size_t size, align;
    union {
        uintptr_t index;
        void *address;
    } object;
    void *value;                      /* its initial bytes, or NULL for zeros */
} emutls_control;

typedef struct emutls_array {
    size_t count;
    void *data[1];
} emutls_array;

static DWORD g_emutls_slot = FLS_OUT_OF_INDEXES;
static SRWLOCK g_emutls_lock = SRWLOCK_INIT;
static uintptr_t g_emutls_next;

static void WINAPI emutls_free(void *p)
{
    emutls_array *a = (emutls_array *)p;
    size_t i;
    if (!a) return;
    for (i = 0; i < a->count; i++)
        if (a->data[i]) _aligned_free(a->data[i]);
    free(a);
}

/*
 * A variable's number is read as it is, with acquire order (it is written once, under the lock, with release order): every
 * thread reads it at every access to the variable, and an interlocked compare-exchange there -- a write to the variable's
 * control, which all threads share -- had every thread that touched a thread-local variable at once wait for the others'
 * caches: the guest's malloc, which reads its thread's cache through one at every malloc and free, ran 58 M/s of
 * malloc-and-free pairs on one thread and 31 on four.
 */
static MGC void *mgc___emutls_get_address(emutls_control *c)
{
    uintptr_t index = (uintptr_t)ReadPointerAcquire((PVOID const volatile *)&c->object.index);
    emutls_array *a;
    void *p;
    if (!index) {
        AcquireSRWLockExclusive(&g_emutls_lock);
        if (g_emutls_slot == FLS_OUT_OF_INDEXES) g_emutls_slot = FlsAlloc(emutls_free);
        index = c->object.index;
        if (!index) {
            index = ++g_emutls_next;
            InterlockedExchangePointer((void *volatile *)&c->object.index, (void *)index);
        }
        ReleaseSRWLockExclusive(&g_emutls_lock);
    }
    a = (emutls_array *)FlsGetValue(g_emutls_slot);
    if (!a || a->count < index) {
        size_t n = index + 16, i, had = a ? a->count : 0;
        emutls_array *b = (emutls_array *)realloc(a, sizeof(emutls_array) + sizeof(void *) * n);
        if (!b) abort();
        for (i = had; i < n; i++) b->data[i] = NULL;
        b->count = n;
        a = b;
        FlsSetValue(g_emutls_slot, a);
    }
    p = a->data[index - 1];
    if (!p) {
        size_t align = c->align > sizeof(void *) ? c->align : sizeof(void *);
        p = _aligned_malloc(c->size ? c->size : 1, align);
        if (!p) abort();
        if (c->value) memcpy(p, c->value, c->size);
        else memset(p, 0, c->size);
        a->data[index - 1] = p;
    }
    return p;
}

/* -------------------------------------------------------------- the table */

#define E(n) { #n, (void *)mgc_##n }
#define DATA(n, v) { #n, (void *)&v }

const mg_core_import mg_crt_bridge_imports[] = {
    E(acos), E(acosf), E(acosh), E(acoshf), E(asin), E(asinf), E(asinh), E(asinhf), E(atan), E(atanf), E(atanh), E(atanhf),
    E(cbrt), E(cbrtf), E(ceil), E(ceilf), E(cos), E(cosf), E(cosh), E(coshf), E(erf), E(erff), E(erfc), E(erfcf),
    E(exp), E(expf), E(exp2), E(exp2f), E(expm1), E(expm1f), E(floor), E(floorf), E(lgamma), E(lgammaf),
    E(log), E(logf), E(log10), E(log10f), E(log1p), E(log1pf), E(log2), E(log2f), E(logb), E(logbf),
    E(rint), E(rintf), E(round), E(roundf), E(sin), E(sinf), E(sinh), E(sinhf), E(sqrt), E(sqrtf), E(tan), E(tanf),
    E(tanh), E(tanhf), E(tgamma), E(tgammaf), E(trunc), E(truncf), E(nearbyint), E(nearbyintf), E(fabs), E(fabsf),
    E(atan2), E(atan2f), E(fdim), E(fdimf), E(fmod), E(fmodf), E(hypot), E(hypotf), E(nextafter), E(nextafterf),
    E(pow), E(powf), E(remainder), E(remainderf), E(copysign), E(copysignf), E(fmax), E(fmaxf), E(fmin), E(fminf),
    E(fma), E(fmaf), E(frexp), E(frexpf), E(ilogb), E(ilogbf), E(ldexp), E(ldexpf), E(scalbn), E(scalbnf),
    E(llround), E(llroundf), E(llrint), E(llrintf), E(lround), E(lroundf), E(lrint), E(lrintf), E(scalbln), E(scalblnf),
    E(modf), E(modff), E(nan), E(nanf), E(remquo), E(remquof), E(sincos), E(sincosf),
    E(malloc), E(calloc), E(realloc), E(free), E(memchr), E(memcmp), E(memcpy), E(memmove), E(memset),
    E(strcat), E(strchr), E(strcmp), E(strcpy), E(strcspn), E(strlen), E(strnlen), E(strncat), E(strncmp), E(strncpy),
    E(strpbrk), E(strrchr), E(strspn), E(strstr), E(strdup), E(strcasecmp), E(strncasecmp), E(wcslen),
    E(isalnum), E(isalpha), E(isblank), E(iscntrl), E(isdigit), E(isgraph), E(islower), E(isprint), E(ispunct), E(isspace),
    E(isupper), E(isxdigit), E(tolower), E(toupper),
    E(atoi), E(atol), E(atoll), E(atof), E(strtod), E(strtof), E(strtol), E(strtoul), E(strtoll), E(strtoull), E(getenv),
    E(abort), E(exit), E(_Exit), E(abs), E(labs), E(llabs), E(qsort), E(bsearch),
    E(time), E(localtime), E(gmtime),
    E(fopen), E(fclose), E(fflush), E(fputc), E(fputs), E(fwrite), E(fread), E(remove), { "mg_crt_format_double", (void *)mgc_format_double },
    DATA(stdout, g_stdout), DATA(stderr, g_stderr), DATA(stdin, g_stdin),
    E(__errno), E(__assert2), { "mg_crt_emutls_get_address", (void *)mgc___emutls_get_address },
    { NULL, NULL }
};

/* Before the core is relocated: what bionic's stdout and stderr are to hold. */
void mg_crt_bridge_init(void)
{
    g_stdin = stdin;
    g_stdout = stdout;
    g_stderr = stderr;
}

#endif /* MG_ELF_HOST */
