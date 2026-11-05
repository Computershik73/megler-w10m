/*
 * libm for guest code.
 *
 * Every function here takes or returns floating point, so none can be the
 * host's own on 32-bit ARM: the guest passes a double in two integer
 * registers and a float in one, the host expects them in VFP registers. Each
 * wrapper takes them as the guest passes them -- gfloat and gdouble, the bits
 * there and the values elsewhere (mg_bionic.h) -- and hands the host's
 * function real values.
 *
 * long double is a double on 32-bit ARM and on x86, and IEEE's 128-bit quad on
 * the 64-bit ABIs: the *l functions take and return gldouble, and compute in
 * double.
 */
#include "mg_bionic.h"

#include <math.h>

#define D1(n) MG_SF_D_D(g_##n, n)
#define D2(n) MG_SF_D_DD(g_##n, n)
#define F1(n) MG_SF_F_F(g_##n, n)
#define F2(n) MG_SF_F_FF(g_##n, n)
#define L1(n) MG_SF_L_L(g_##n##l, n)
#define L2(n) MG_SF_L_LL(g_##n##l, n)

D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh) D1(asinh) D1(acosh) D1(atanh)
D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p) D1(sqrt) D1(cbrt) D1(fabs) D1(floor) D1(ceil)
D1(round) D1(trunc) D1(rint) D1(nearbyint) D1(erf) D1(erfc) D1(tgamma) D1(lgamma) D1(logb)
D2(atan2) D2(pow) D2(hypot) D2(fmod) D2(remainder) D2(fmin) D2(fmax) D2(fdim) D2(copysign) D2(nextafter)

F1(sinf) F1(cosf) F1(tanf) F1(asinf) F1(acosf) F1(atanf) F1(sinhf) F1(coshf) F1(tanhf) F1(asinhf) F1(acoshf) F1(atanhf)
F1(expf) F1(exp2f) F1(expm1f) F1(logf) F1(log2f) F1(log10f) F1(log1pf) F1(sqrtf) F1(cbrtf) F1(fabsf) F1(floorf) F1(ceilf)
F1(roundf) F1(truncf) F1(rintf) F1(nearbyintf) F1(erff) F1(erfcf) F1(tgammaf) F1(lgammaf) F1(logbf)
F2(atan2f) F2(powf) F2(hypotf) F2(fmodf) F2(remainderf) F2(fminf) F2(fmaxf) F2(fdimf) F2(copysignf) F2(nextafterf)

L1(sin) L1(cos) L1(tan) L1(asin) L1(acos) L1(atan) L1(exp) L1(log) L1(log10) L1(log2) L1(sqrt) L1(cbrt) L1(fabs)
L1(floor) L1(ceil) L1(round) L1(trunc) L1(rint) L1(nearbyint) L1(expm1) L1(log1p)
L2(atan2) L2(pow) L2(fmod) L2(hypot) L2(fmin) L2(fmax) L2(copysign) L2(nextafter) L2(remainder)

static gdouble g_exp10(gdouble x) { return mg_gdb(pow(10.0, mg_gd(x))); }
static gfloat g_exp10f(gfloat x) { return mg_gfb(powf(10.0f, mg_gf(x))); }
static gdouble g_fma(gdouble x, gdouble y, gdouble z) { return mg_gdb(fma(mg_gd(x), mg_gd(y), mg_gd(z))); }
static gfloat g_fmaf(gfloat x, gfloat y, gfloat z) { return mg_gfb(fmaf(mg_gf(x), mg_gf(y), mg_gf(z))); }
static gldouble g_fmal(gldouble x, gldouble y, gldouble z) { return mg_gldb(fma(mg_gld(x), mg_gld(y), mg_gld(z))); }
static gdouble g_ldexp(gdouble x, int e) { return mg_gdb(ldexp(mg_gd(x), e)); }
static gfloat g_ldexpf(gfloat x, int e) { return mg_gfb(ldexpf(mg_gf(x), e)); }
static gldouble g_ldexpl(gldouble x, int e) { return mg_gldb(ldexp(mg_gld(x), e)); }
static gdouble g_scalbln(gdouble x, glong e) { return mg_gdb(scalbln(mg_gd(x), e)); }
static gfloat g_scalblnf(gfloat x, glong e) { return mg_gfb(scalblnf(mg_gf(x), e)); }
static gdouble g_frexp(gdouble x, int *e) { return mg_gdb(frexp(mg_gd(x), e)); }
static gfloat g_frexpf(gfloat x, int *e) { return mg_gfb(frexpf(mg_gf(x), e)); }
static gldouble g_frexpl(gldouble x, int *e) { return mg_gldb(frexp(mg_gld(x), e)); }
/* (The whole part goes where the guest says, as a double, a float or a long double of its own.) */
static gdouble g_modf(gdouble x, double *ip) { return mg_gdb(modf(mg_gd(x), ip)); }
static gfloat g_modff(gfloat x, float *ip) { return mg_gfb(modff(mg_gf(x), ip)); }
static gldouble g_modfl(gldouble x, gldouble *ip)
{
    double whole, part = modf(mg_gld(x), &whole);
    *ip = mg_gldb(whole);
    return mg_gldb(part);
}
static gdouble g_remquo(gdouble x, gdouble y, int *q) { return mg_gdb(remquo(mg_gd(x), mg_gd(y), q)); }
static gfloat g_remquof(gfloat x, gfloat y, int *q) { return mg_gfb(remquof(mg_gf(x), mg_gf(y), q)); }
static gdouble g_nan(const char *tag) { return mg_gdb(nan(tag)); }
static gfloat g_nanf(const char *tag) { return mg_gfb(nanf(tag)); }
static glong g_lround(gdouble x) { return lround(mg_gd(x)); }
static glong g_lroundf(gfloat x) { return lroundf(mg_gf(x)); }
static glong g_lroundl(gldouble x) { return lround(mg_gld(x)); }
static i64 g_llround(gdouble x) { return llround(mg_gd(x)); }
static i64 g_llroundf(gfloat x) { return llroundf(mg_gf(x)); }
static i64 g_llroundl(gldouble x) { return llround(mg_gld(x)); }
static glong g_lrint(gdouble x) { return lrint(mg_gd(x)); }
static glong g_lrintf(gfloat x) { return lrintf(mg_gf(x)); }
static glong g_lrintl(gldouble x) { return lrint(mg_gld(x)); }
static i64 g_llrint(gdouble x) { return llrint(mg_gd(x)); }
static i64 g_llrintf(gfloat x) { return llrintf(mg_gf(x)); }
static i64 g_llrintl(gldouble x) { return llrint(mg_gld(x)); }
static int g_ilogb(gdouble x) { return ilogb(mg_gd(x)); }
static int g_ilogbf(gfloat x) { return ilogbf(mg_gf(x)); }
/* nexttoward: a double toward a long double. */
static gdouble g_nexttoward(gdouble x, gldouble y) { return mg_gdb(nextafter(mg_gd(x), mg_gld(y))); }
static void g_sincos(gdouble x, double *s, double *c) { *s = sin(mg_gd(x)); *c = cos(mg_gd(x)); }
static void g_sincosf(gfloat x, float *s, float *c) { *s = sinf(mg_gf(x)); *c = cosf(mg_gf(x)); }
static gdouble g_lgamma_r(gdouble x, int *sign)
{
    double v = mg_gd(x), r = lgamma(v);
    /* The gamma function is negative between -(2k+1) and -2k. */
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return mg_gdb(r);
}
static gfloat g_lgammaf_r(gfloat x, int *sign)
{
    double v = mg_gf(x);
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return mg_gfb(lgammaf(mg_gf(x)));
}
static gdouble g_significand(gdouble x)
{
    int e;
    return mg_gdb(frexp(mg_gd(x), &e) * 2.0);
}

/* Classification: bionic's own names behind <math.h>'s macros. */
static int g_isnan(gdouble x) { return isnan(mg_gd(x)) ? 1 : 0; }
static int g_isnanf(gfloat x) { return isnan(mg_gf(x)) ? 1 : 0; }
static int g_isnanl(gldouble x) { return isnan(mg_gld(x)) ? 1 : 0; }
static int g_isinf(gdouble x) { return isinf(mg_gd(x)) ? 1 : 0; }
static int g_isinff(gfloat x) { return isinf(mg_gf(x)) ? 1 : 0; }
static int g_isinfl(gldouble x) { return isinf(mg_gld(x)) ? 1 : 0; }
static int g_finite(gdouble x) { return isfinite(mg_gd(x)) ? 1 : 0; }
static int g_finitef(gfloat x) { return isfinite(mg_gf(x)) ? 1 : 0; }
static int g_finitel(gldouble x) { return isfinite(mg_gld(x)) ? 1 : 0; }
static int g_isnormal(gdouble x) { return isnormal(mg_gd(x)) ? 1 : 0; }
static int g_isnormalf(gfloat x) { return isnormal(mg_gf(x)) ? 1 : 0; }
static int g_isnormall(gldouble x) { return isnormal(mg_gld(x)) ? 1 : 0; }
static int g_signbit(gdouble x) { return signbit(mg_gd(x)) ? 1 : 0; }
static int g_signbitf(gfloat x) { return signbit(mg_gf(x)) ? 1 : 0; }
static int g_signbitl(gldouble x) { return signbit(mg_gld(x)) ? 1 : 0; }

/* bionic's FP_* values: FP_INFINITE 1, FP_NAN 2, FP_NORMAL 4, FP_SUBNORMAL 8, FP_ZERO 16. */
static int fp_class(double v)
{
    if (isnan(v)) return 2;
    if (isinf(v)) return 1;
    if (v == 0) return 16;
    return isnormal(v) ? 4 : 8;
}
static int g_fpclassifyd(gdouble x) { return fp_class(mg_gd(x)); }
static int g_fpclassifyf(gfloat x)
{
    float v = mg_gf(x);
    if (isnan(v)) return 2;
    if (isinf(v)) return 1;
    if (v == 0) return 16;
    return isnormal(v) ? 4 : 8;
}
static int g_fpclassifyl(gldouble x) { return fp_class(mg_gld(x)); }

/* <fenv.h>: the rounding mode is the host thread's own, and these leave it alone. */
static int g_fegetround(void) { return 0; }
static int g_fesetround(int mode) { return mode == 0 ? 0 : -1; }
static int g_fenv_nop(void *p) { (void)p; return 0; }
static int g_fenv_nop_int(int e) { (void)e; return 0; }

/*
 * Bessel functions of the first kind, of orders 0 and 1: their power series up to 12, and Hankel's asymptotic expansion
 * beyond, where the series' terms cancel too much. Taken to the term where the expansion starts to grow, it is good to
 * about 1e-11 at 12 and better further out. Zalo's call library (libzrtc.so) imports j0.
 */
static double bessel_series(int n, double x)
{
    double q = -x * x / 4, term = n ? x / 2 : 1, sum = term;
    int k;
    for (k = 1; k < 300; k++) {
        term *= q / ((double)k * (k + n));
        sum += term;
        if (fabs(term) <= 1e-17 * fabs(sum)) break;
    }
    return sum;
}

static double bessel_asymptotic(int n, double x)
{
    const double pi = 3.14159265358979323846;
    double mu = 4.0 * n * n, p = 1, q = 0, term = 1, w = x - (n * 0.5 + 0.25) * pi;
    int k;
    for (k = 1; k < 60; k++) {
        double next = term * (mu - (2.0 * k - 1) * (2.0 * k - 1)) / (k * 8.0 * x);
        if (fabs(next) >= fabs(term)) break;
        term = next;
        /* P takes the even terms, Q the odd, each with alternating signs. */
        switch (k & 3) {
        case 1: q += term; break;
        case 2: p -= term; break;
        case 3: q -= term; break;
        default: p += term; break;
        }
    }
    return sqrt(2 / (pi * x)) * (p * cos(w) - q * sin(w));
}

static double bessel_j0(double x)
{
    x = fabs(x);
    return x < 12 ? bessel_series(0, x) : bessel_asymptotic(0, x);
}
static double bessel_j1(double x)
{
    double a = fabs(x), v = a < 12 ? bessel_series(1, a) : bessel_asymptotic(1, a);
    return x < 0 ? -v : v;
}
/* (As the guest passes them: on 32-bit ARM these took a double where the guest gives its bits, and answered garbage.) */
static gdouble g_j0(gdouble x) { return mg_gdb(bessel_j0(mg_gd(x))); }
static gdouble g_j1(gdouble x) { return mg_gdb(bessel_j1(mg_gd(x))); }
static gfloat g_j0f(gfloat x) { return mg_gfb((float)bessel_j0(mg_gf(x))); }
static gfloat g_j1f(gfloat x) { return mg_gfb((float)bessel_j1(mg_gf(x))); }

#define DM(n) MG_WRAP(#n, g_##n)

const mg_sym mg_tab_math[] = {
    DM(sin), DM(cos), DM(tan), DM(asin), DM(acos), DM(atan), DM(sinh), DM(cosh), DM(tanh), DM(asinh), DM(acosh), DM(atanh),
    DM(exp), DM(exp2), DM(expm1), DM(log), DM(log2), DM(log10), DM(log1p), DM(sqrt), DM(cbrt), DM(fabs), DM(floor), DM(ceil),
    DM(round), DM(trunc), DM(rint), DM(nearbyint), DM(erf), DM(erfc), DM(tgamma), DM(lgamma), DM(logb),
    DM(atan2), DM(pow), DM(hypot), DM(fmod), DM(remainder), DM(fmin), DM(fmax), DM(fdim), DM(copysign), DM(nextafter),
    DM(sinf), DM(cosf), DM(tanf), DM(asinf), DM(acosf), DM(atanf), DM(sinhf), DM(coshf), DM(tanhf), DM(asinhf), DM(acoshf), DM(atanhf),
    DM(expf), DM(exp2f), DM(expm1f), DM(logf), DM(log2f), DM(log10f), DM(log1pf), DM(sqrtf), DM(cbrtf), DM(fabsf), DM(floorf), DM(ceilf),
    DM(roundf), DM(truncf), DM(rintf), DM(nearbyintf), DM(erff), DM(erfcf), DM(tgammaf), DM(lgammaf), DM(logbf),
    DM(atan2f), DM(powf), DM(hypotf), DM(fmodf), DM(remainderf), DM(fminf), DM(fmaxf), DM(fdimf), DM(copysignf), DM(nextafterf),
    DM(exp10), DM(exp10f), DM(fma), DM(fmaf), DM(ldexp), DM(ldexpf), DM(scalbln), DM(scalblnf), DM(frexp), DM(frexpf),
    DM(modf), DM(modff), DM(remquo), DM(remquof), DM(nan), DM(nanf), DM(lround), DM(lroundf), DM(llround), DM(llroundf),
    DM(lrint), DM(lrintf), DM(llrint), DM(llrintf), DM(ilogb), DM(ilogbf), DM(sincos), DM(sincosf), DM(lgamma_r), DM(lgammaf_r),
    DM(significand), DM(finite), DM(finitef), DM(j0), DM(j1), DM(j0f), DM(j1f), DM(nexttoward),
    MG_WRAP("scalbn", g_ldexp), MG_WRAP("scalbnf", g_ldexpf), MG_WRAP("drem", g_remainder), MG_WRAP("dremf", g_remainderf),
    MG_WRAP("gamma", g_lgamma), MG_WRAP("gammaf", g_lgammaf), MG_WRAP("pow10", g_exp10), MG_WRAP("pow10f", g_exp10f),
    /* long double (gldouble): a double on 32-bit ARM and x86, a quad on the 64-bit ABIs; computed in double. */
    DM(sinl), DM(cosl), DM(tanl), DM(asinl), DM(acosl), DM(atanl), DM(atan2l), DM(expl), DM(logl), DM(log10l), DM(log2l),
    DM(powl), DM(sqrtl), DM(cbrtl), DM(fabsl), DM(floorl), DM(ceill), DM(roundl), DM(truncl), DM(rintl), DM(nearbyintl),
    DM(fmodl), DM(hypotl), DM(ldexpl), MG_WRAP("scalbnl", g_ldexpl), DM(frexpl), DM(modfl), DM(fminl), DM(fmaxl), DM(copysignl),
    DM(lroundl), DM(llroundl), DM(lrintl), DM(llrintl), DM(fmal), DM(nextafterl), DM(remainderl), DM(expm1l), DM(log1pl),
    MG_WRAP("isnan", g_isnan), MG_WRAP("__isnan", g_isnan), MG_WRAP("isnanf", g_isnanf), MG_WRAP("__isnanf", g_isnanf),
    MG_WRAP("__isnanl", g_isnanl), MG_WRAP("isinf", g_isinf), MG_WRAP("__isinf", g_isinf), MG_WRAP("__isinff", g_isinff),
    MG_WRAP("isinff", g_isinff), MG_WRAP("__isinfl", g_isinfl), MG_WRAP("__isfinite", g_finite), MG_WRAP("__isfinitef", g_finitef),
    MG_WRAP("__isfinitel", g_finitel), MG_WRAP("__isnormal", g_isnormal), MG_WRAP("__isnormalf", g_isnormalf),
    MG_WRAP("__isnormall", g_isnormall), MG_WRAP("__signbit", g_signbit), MG_WRAP("__signbitf", g_signbitf),
    MG_WRAP("__signbitl", g_signbitl), MG_WRAP("__fpclassifyd", g_fpclassifyd), MG_WRAP("__fpclassify", g_fpclassifyd),
    MG_WRAP("__fpclassifyf", g_fpclassifyf), MG_WRAP("__fpclassifyl", g_fpclassifyl),
    MG_WRAP("fegetround", g_fegetround), MG_WRAP("fesetround", g_fesetround), MG_WRAP("fegetenv", g_fenv_nop),
    MG_WRAP("fesetenv", g_fenv_nop), MG_WRAP("feholdexcept", g_fenv_nop), MG_WRAP("feupdateenv", g_fenv_nop),
    MG_WRAP("feclearexcept", g_fenv_nop_int), MG_WRAP("feraiseexcept", g_fenv_nop_int), MG_WRAP("fetestexcept", g_fenv_nop_int),
    MG_END
};
