/*
 * libm for guest code.
 *
 * Every function here takes or returns floating point, so none can be the
 * host's own: the guest passes a double in two integer registers and a float
 * in one, the host expects them in VFP registers. Each wrapper takes the bits
 * and hands the host's function real values (hk_bionic.h).
 */
#include "hk_bionic.h"

#include <math.h>

#define D1(n) HK_SF_D_D(g_##n, n)
#define D2(n) HK_SF_D_DD(g_##n, n)
#define F1(n) HK_SF_F_F(g_##n, n)
#define F2(n) HK_SF_F_FF(g_##n, n)

D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh) D1(asinh) D1(acosh) D1(atanh)
D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p) D1(sqrt) D1(cbrt) D1(fabs) D1(floor) D1(ceil)
D1(round) D1(trunc) D1(rint) D1(nearbyint) D1(erf) D1(erfc) D1(tgamma) D1(lgamma) D1(logb)
D2(atan2) D2(pow) D2(hypot) D2(fmod) D2(remainder) D2(fmin) D2(fmax) D2(fdim) D2(copysign) D2(nextafter)

F1(sinf) F1(cosf) F1(tanf) F1(asinf) F1(acosf) F1(atanf) F1(sinhf) F1(coshf) F1(tanhf) F1(asinhf) F1(acoshf) F1(atanhf)
F1(expf) F1(exp2f) F1(expm1f) F1(logf) F1(log2f) F1(log10f) F1(log1pf) F1(sqrtf) F1(cbrtf) F1(fabsf) F1(floorf) F1(ceilf)
F1(roundf) F1(truncf) F1(rintf) F1(nearbyintf) F1(erff) F1(erfcf) F1(tgammaf) F1(lgammaf) F1(logbf)
F2(atan2f) F2(powf) F2(hypotf) F2(fmodf) F2(remainderf) F2(fminf) F2(fmaxf) F2(fdimf) F2(copysignf) F2(nextafterf)

static u64 g_exp10(u64 x) { return hk_db(pow(10.0, hk_d(x))); }
static u32 g_exp10f(u32 x) { return hk_fb(powf(10.0f, hk_f(x))); }
static u64 g_fma(u64 x, u64 y, u64 z) { return hk_db(fma(hk_d(x), hk_d(y), hk_d(z))); }
static u32 g_fmaf(u32 x, u32 y, u32 z) { return hk_fb(fmaf(hk_f(x), hk_f(y), hk_f(z))); }
static u64 g_ldexp(u64 x, int e) { return hk_db(ldexp(hk_d(x), e)); }
static u32 g_ldexpf(u32 x, int e) { return hk_fb(ldexpf(hk_f(x), e)); }
static u64 g_scalbln(u64 x, long e) { return hk_db(scalbln(hk_d(x), e)); }
static u32 g_scalblnf(u32 x, long e) { return hk_fb(scalblnf(hk_f(x), e)); }
static u64 g_frexp(u64 x, int *e) { return hk_db(frexp(hk_d(x), e)); }
static u32 g_frexpf(u32 x, int *e) { return hk_fb(frexpf(hk_f(x), e)); }
static u64 g_modf(u64 x, double *ip) { return hk_db(modf(hk_d(x), ip)); }
static u32 g_modff(u32 x, float *ip) { return hk_fb(modff(hk_f(x), ip)); }
static u64 g_remquo(u64 x, u64 y, int *q) { return hk_db(remquo(hk_d(x), hk_d(y), q)); }
static u32 g_remquof(u32 x, u32 y, int *q) { return hk_fb(remquof(hk_f(x), hk_f(y), q)); }
static u64 g_nan(const char *tag) { return hk_db(nan(tag)); }
static u32 g_nanf(const char *tag) { return hk_fb(nanf(tag)); }
static long g_lround(u64 x) { return lround(hk_d(x)); }
static long g_lroundf(u32 x) { return lroundf(hk_f(x)); }
static i64 g_llround(u64 x) { return llround(hk_d(x)); }
static i64 g_llroundf(u32 x) { return llroundf(hk_f(x)); }
static long g_lrint(u64 x) { return lrint(hk_d(x)); }
static long g_lrintf(u32 x) { return lrintf(hk_f(x)); }
static i64 g_llrint(u64 x) { return llrint(hk_d(x)); }
static i64 g_llrintf(u32 x) { return llrintf(hk_f(x)); }
static int g_ilogb(u64 x) { return ilogb(hk_d(x)); }
static int g_ilogbf(u32 x) { return ilogbf(hk_f(x)); }
static void g_sincos(u64 x, double *s, double *c) { *s = sin(hk_d(x)); *c = cos(hk_d(x)); }
static void g_sincosf(u32 x, float *s, float *c) { *s = sinf(hk_f(x)); *c = cosf(hk_f(x)); }
static u64 g_lgamma_r(u64 x, int *sign)
{
    double v = hk_d(x), r = lgamma(v);
    /* The gamma function is negative between -(2k+1) and -2k. */
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return hk_db(r);
}
static u32 g_lgammaf_r(u32 x, int *sign)
{
    double v = hk_f(x);
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return hk_fb(lgammaf(hk_f(x)));
}
static u64 g_significand(u64 x)
{
    int e;
    return hk_db(frexp(hk_d(x), &e) * 2.0);
}

/* Classification: bionic's own names behind <math.h>'s macros. */
static int g_isnan(u64 x) { return isnan(hk_d(x)) ? 1 : 0; }
static int g_isnanf(u32 x) { return isnan(hk_f(x)) ? 1 : 0; }
static int g_isinf(u64 x) { return isinf(hk_d(x)) ? 1 : 0; }
static int g_isinff(u32 x) { return isinf(hk_f(x)) ? 1 : 0; }
static int g_finite(u64 x) { return isfinite(hk_d(x)) ? 1 : 0; }
static int g_finitef(u32 x) { return isfinite(hk_f(x)) ? 1 : 0; }
static int g_isnormal(u64 x) { return isnormal(hk_d(x)) ? 1 : 0; }
static int g_isnormalf(u32 x) { return isnormal(hk_f(x)) ? 1 : 0; }
static int g_signbit(u64 x) { return (int)(x >> 63); }
static int g_signbitf(u32 x) { return (int)(x >> 31); }

/* bionic's FP_* values: FP_INFINITE 1, FP_NAN 2, FP_NORMAL 4, FP_SUBNORMAL 8, FP_ZERO 16. */
static int g_fpclassifyd(u64 x)
{
    double v = hk_d(x);
    if (isnan(v)) return 2;
    if (isinf(v)) return 1;
    if (v == 0) return 16;
    return isnormal(v) ? 4 : 8;
}
static int g_fpclassifyf(u32 x)
{
    float v = hk_f(x);
    if (isnan(v)) return 2;
    if (isinf(v)) return 1;
    if (v == 0) return 16;
    return isnormal(v) ? 4 : 8;
}

/* <fenv.h>: the rounding mode is the host thread's own, and these leave it alone. */
static int g_fegetround(void) { return 0; }
static int g_fesetround(int mode) { return mode == 0 ? 0 : -1; }
static int g_fenv_nop(void *p) { (void)p; return 0; }
static int g_fenv_nop_int(int e) { (void)e; return 0; }

#define DM(n) HK_WRAP(#n, g_##n)

const hk_sym hk_tab_math[] = {
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
    DM(significand), DM(finite), DM(finitef),
    HK_WRAP("scalbn", g_ldexp), HK_WRAP("scalbnf", g_ldexpf), HK_WRAP("drem", g_remainder), HK_WRAP("dremf", g_remainderf),
    HK_WRAP("gamma", g_lgamma), HK_WRAP("gammaf", g_lgammaf), HK_WRAP("nexttoward", g_nextafter), HK_WRAP("pow10", g_exp10),
    HK_WRAP("pow10f", g_exp10f),
    /* long double is double on 32-bit ARM. */
    HK_WRAP("sinl", g_sin), HK_WRAP("cosl", g_cos), HK_WRAP("tanl", g_tan), HK_WRAP("asinl", g_asin), HK_WRAP("acosl", g_acos),
    HK_WRAP("atanl", g_atan), HK_WRAP("atan2l", g_atan2), HK_WRAP("expl", g_exp), HK_WRAP("logl", g_log), HK_WRAP("log10l", g_log10),
    HK_WRAP("log2l", g_log2), HK_WRAP("powl", g_pow), HK_WRAP("sqrtl", g_sqrt), HK_WRAP("cbrtl", g_cbrt), HK_WRAP("fabsl", g_fabs),
    HK_WRAP("floorl", g_floor), HK_WRAP("ceill", g_ceil), HK_WRAP("roundl", g_round), HK_WRAP("truncl", g_trunc),
    HK_WRAP("rintl", g_rint), HK_WRAP("nearbyintl", g_nearbyint), HK_WRAP("fmodl", g_fmod), HK_WRAP("hypotl", g_hypot),
    HK_WRAP("ldexpl", g_ldexp), HK_WRAP("scalbnl", g_ldexp), HK_WRAP("frexpl", g_frexp), HK_WRAP("modfl", g_modf),
    HK_WRAP("fminl", g_fmin), HK_WRAP("fmaxl", g_fmax), HK_WRAP("copysignl", g_copysign), HK_WRAP("lroundl", g_lround),
    HK_WRAP("llroundl", g_llround), HK_WRAP("lrintl", g_lrint), HK_WRAP("llrintl", g_llrint), HK_WRAP("fmal", g_fma),
    HK_WRAP("nextafterl", g_nextafter), HK_WRAP("remainderl", g_remainder), HK_WRAP("expm1l", g_expm1), HK_WRAP("log1pl", g_log1p),
    HK_WRAP("isnan", g_isnan), HK_WRAP("__isnan", g_isnan), HK_WRAP("isnanf", g_isnanf), HK_WRAP("__isnanf", g_isnanf),
    HK_WRAP("__isnanl", g_isnan), HK_WRAP("isinf", g_isinf), HK_WRAP("__isinf", g_isinf), HK_WRAP("__isinff", g_isinff),
    HK_WRAP("isinff", g_isinff), HK_WRAP("__isinfl", g_isinf), HK_WRAP("__isfinite", g_finite), HK_WRAP("__isfinitef", g_finitef),
    HK_WRAP("__isfinitel", g_finite), HK_WRAP("__isnormal", g_isnormal), HK_WRAP("__isnormalf", g_isnormalf),
    HK_WRAP("__isnormall", g_isnormal), HK_WRAP("__signbit", g_signbit), HK_WRAP("__signbitf", g_signbitf),
    HK_WRAP("__signbitl", g_signbit), HK_WRAP("__fpclassifyd", g_fpclassifyd), HK_WRAP("__fpclassify", g_fpclassifyd),
    HK_WRAP("__fpclassifyf", g_fpclassifyf), HK_WRAP("__fpclassifyl", g_fpclassifyd),
    HK_WRAP("fegetround", g_fegetround), HK_WRAP("fesetround", g_fesetround), HK_WRAP("fegetenv", g_fenv_nop),
    HK_WRAP("fesetenv", g_fenv_nop), HK_WRAP("feholdexcept", g_fenv_nop), HK_WRAP("feupdateenv", g_fenv_nop),
    HK_WRAP("feclearexcept", g_fenv_nop_int), HK_WRAP("feraiseexcept", g_fenv_nop_int), HK_WRAP("fetestexcept", g_fenv_nop_int),
    HK_END
};
