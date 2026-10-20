/*
 * libm for guest code.
 *
 * Every function here takes or returns floating point, so none can be the
 * host's own: the guest passes a double in two integer registers and a float
 * in one, the host expects them in VFP registers. Each wrapper takes the bits
 * and hands the host's function real values (mg_bionic.h).
 */
#include "mg_bionic.h"

#include <math.h>

#define D1(n) MG_SF_D_D(g_##n, n)
#define D2(n) MG_SF_D_DD(g_##n, n)
#define F1(n) MG_SF_F_F(g_##n, n)
#define F2(n) MG_SF_F_FF(g_##n, n)

D1(sin) D1(cos) D1(tan) D1(asin) D1(acos) D1(atan) D1(sinh) D1(cosh) D1(tanh) D1(asinh) D1(acosh) D1(atanh)
D1(exp) D1(exp2) D1(expm1) D1(log) D1(log2) D1(log10) D1(log1p) D1(sqrt) D1(cbrt) D1(fabs) D1(floor) D1(ceil)
D1(round) D1(trunc) D1(rint) D1(nearbyint) D1(erf) D1(erfc) D1(tgamma) D1(lgamma) D1(logb)
D2(atan2) D2(pow) D2(hypot) D2(fmod) D2(remainder) D2(fmin) D2(fmax) D2(fdim) D2(copysign) D2(nextafter)

F1(sinf) F1(cosf) F1(tanf) F1(asinf) F1(acosf) F1(atanf) F1(sinhf) F1(coshf) F1(tanhf) F1(asinhf) F1(acoshf) F1(atanhf)
F1(expf) F1(exp2f) F1(expm1f) F1(logf) F1(log2f) F1(log10f) F1(log1pf) F1(sqrtf) F1(cbrtf) F1(fabsf) F1(floorf) F1(ceilf)
F1(roundf) F1(truncf) F1(rintf) F1(nearbyintf) F1(erff) F1(erfcf) F1(tgammaf) F1(lgammaf) F1(logbf)
F2(atan2f) F2(powf) F2(hypotf) F2(fmodf) F2(remainderf) F2(fminf) F2(fmaxf) F2(fdimf) F2(copysignf) F2(nextafterf)

static u64 g_exp10(u64 x) { return mg_db(pow(10.0, mg_d(x))); }
static u32 g_exp10f(u32 x) { return mg_fb(powf(10.0f, mg_f(x))); }
static u64 g_fma(u64 x, u64 y, u64 z) { return mg_db(fma(mg_d(x), mg_d(y), mg_d(z))); }
static u32 g_fmaf(u32 x, u32 y, u32 z) { return mg_fb(fmaf(mg_f(x), mg_f(y), mg_f(z))); }
static u64 g_ldexp(u64 x, int e) { return mg_db(ldexp(mg_d(x), e)); }
static u32 g_ldexpf(u32 x, int e) { return mg_fb(ldexpf(mg_f(x), e)); }
static u64 g_scalbln(u64 x, long e) { return mg_db(scalbln(mg_d(x), e)); }
static u32 g_scalblnf(u32 x, long e) { return mg_fb(scalblnf(mg_f(x), e)); }
static u64 g_frexp(u64 x, int *e) { return mg_db(frexp(mg_d(x), e)); }
static u32 g_frexpf(u32 x, int *e) { return mg_fb(frexpf(mg_f(x), e)); }
static u64 g_modf(u64 x, double *ip) { return mg_db(modf(mg_d(x), ip)); }
static u32 g_modff(u32 x, float *ip) { return mg_fb(modff(mg_f(x), ip)); }
static u64 g_remquo(u64 x, u64 y, int *q) { return mg_db(remquo(mg_d(x), mg_d(y), q)); }
static u32 g_remquof(u32 x, u32 y, int *q) { return mg_fb(remquof(mg_f(x), mg_f(y), q)); }
static u64 g_nan(const char *tag) { return mg_db(nan(tag)); }
static u32 g_nanf(const char *tag) { return mg_fb(nanf(tag)); }
static long g_lround(u64 x) { return lround(mg_d(x)); }
static long g_lroundf(u32 x) { return lroundf(mg_f(x)); }
static i64 g_llround(u64 x) { return llround(mg_d(x)); }
static i64 g_llroundf(u32 x) { return llroundf(mg_f(x)); }
static long g_lrint(u64 x) { return lrint(mg_d(x)); }
static long g_lrintf(u32 x) { return lrintf(mg_f(x)); }
static i64 g_llrint(u64 x) { return llrint(mg_d(x)); }
static i64 g_llrintf(u32 x) { return llrintf(mg_f(x)); }
static int g_ilogb(u64 x) { return ilogb(mg_d(x)); }
static int g_ilogbf(u32 x) { return ilogbf(mg_f(x)); }
static void g_sincos(u64 x, double *s, double *c) { *s = sin(mg_d(x)); *c = cos(mg_d(x)); }
static void g_sincosf(u32 x, float *s, float *c) { *s = sinf(mg_f(x)); *c = cosf(mg_f(x)); }
static u64 g_lgamma_r(u64 x, int *sign)
{
    double v = mg_d(x), r = lgamma(v);
    /* The gamma function is negative between -(2k+1) and -2k. */
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return mg_db(r);
}
static u32 g_lgammaf_r(u32 x, int *sign)
{
    double v = mg_f(x);
    if (sign) *sign = (v < 0 && floor(v) != v && fmod(floor(v), 2.0) != 0) ? -1 : 1;
    return mg_fb(lgammaf(mg_f(x)));
}
static u64 g_significand(u64 x)
{
    int e;
    return mg_db(frexp(mg_d(x), &e) * 2.0);
}

/* Classification: bionic's own names behind <math.h>'s macros. */
static int g_isnan(u64 x) { return isnan(mg_d(x)) ? 1 : 0; }
static int g_isnanf(u32 x) { return isnan(mg_f(x)) ? 1 : 0; }
static int g_isinf(u64 x) { return isinf(mg_d(x)) ? 1 : 0; }
static int g_isinff(u32 x) { return isinf(mg_f(x)) ? 1 : 0; }
static int g_finite(u64 x) { return isfinite(mg_d(x)) ? 1 : 0; }
static int g_finitef(u32 x) { return isfinite(mg_f(x)) ? 1 : 0; }
static int g_isnormal(u64 x) { return isnormal(mg_d(x)) ? 1 : 0; }
static int g_isnormalf(u32 x) { return isnormal(mg_f(x)) ? 1 : 0; }
static int g_signbit(u64 x) { return (int)(x >> 63); }
static int g_signbitf(u32 x) { return (int)(x >> 31); }

/* bionic's FP_* values: FP_INFINITE 1, FP_NAN 2, FP_NORMAL 4, FP_SUBNORMAL 8, FP_ZERO 16. */
static int g_fpclassifyd(u64 x)
{
    double v = mg_d(x);
    if (isnan(v)) return 2;
    if (isinf(v)) return 1;
    if (v == 0) return 16;
    return isnormal(v) ? 4 : 8;
}
static int g_fpclassifyf(u32 x)
{
    float v = mg_f(x);
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
    DM(significand), DM(finite), DM(finitef),
    MG_WRAP("scalbn", g_ldexp), MG_WRAP("scalbnf", g_ldexpf), MG_WRAP("drem", g_remainder), MG_WRAP("dremf", g_remainderf),
    MG_WRAP("gamma", g_lgamma), MG_WRAP("gammaf", g_lgammaf), MG_WRAP("nexttoward", g_nextafter), MG_WRAP("pow10", g_exp10),
    MG_WRAP("pow10f", g_exp10f),
    /* long double is double on 32-bit ARM. */
    MG_WRAP("sinl", g_sin), MG_WRAP("cosl", g_cos), MG_WRAP("tanl", g_tan), MG_WRAP("asinl", g_asin), MG_WRAP("acosl", g_acos),
    MG_WRAP("atanl", g_atan), MG_WRAP("atan2l", g_atan2), MG_WRAP("expl", g_exp), MG_WRAP("logl", g_log), MG_WRAP("log10l", g_log10),
    MG_WRAP("log2l", g_log2), MG_WRAP("powl", g_pow), MG_WRAP("sqrtl", g_sqrt), MG_WRAP("cbrtl", g_cbrt), MG_WRAP("fabsl", g_fabs),
    MG_WRAP("floorl", g_floor), MG_WRAP("ceill", g_ceil), MG_WRAP("roundl", g_round), MG_WRAP("truncl", g_trunc),
    MG_WRAP("rintl", g_rint), MG_WRAP("nearbyintl", g_nearbyint), MG_WRAP("fmodl", g_fmod), MG_WRAP("hypotl", g_hypot),
    MG_WRAP("ldexpl", g_ldexp), MG_WRAP("scalbnl", g_ldexp), MG_WRAP("frexpl", g_frexp), MG_WRAP("modfl", g_modf),
    MG_WRAP("fminl", g_fmin), MG_WRAP("fmaxl", g_fmax), MG_WRAP("copysignl", g_copysign), MG_WRAP("lroundl", g_lround),
    MG_WRAP("llroundl", g_llround), MG_WRAP("lrintl", g_lrint), MG_WRAP("llrintl", g_llrint), MG_WRAP("fmal", g_fma),
    MG_WRAP("nextafterl", g_nextafter), MG_WRAP("remainderl", g_remainder), MG_WRAP("expm1l", g_expm1), MG_WRAP("log1pl", g_log1p),
    MG_WRAP("isnan", g_isnan), MG_WRAP("__isnan", g_isnan), MG_WRAP("isnanf", g_isnanf), MG_WRAP("__isnanf", g_isnanf),
    MG_WRAP("__isnanl", g_isnan), MG_WRAP("isinf", g_isinf), MG_WRAP("__isinf", g_isinf), MG_WRAP("__isinff", g_isinff),
    MG_WRAP("isinff", g_isinff), MG_WRAP("__isinfl", g_isinf), MG_WRAP("__isfinite", g_finite), MG_WRAP("__isfinitef", g_finitef),
    MG_WRAP("__isfinitel", g_finite), MG_WRAP("__isnormal", g_isnormal), MG_WRAP("__isnormalf", g_isnormalf),
    MG_WRAP("__isnormall", g_isnormal), MG_WRAP("__signbit", g_signbit), MG_WRAP("__signbitf", g_signbitf),
    MG_WRAP("__signbitl", g_signbit), MG_WRAP("__fpclassifyd", g_fpclassifyd), MG_WRAP("__fpclassify", g_fpclassifyd),
    MG_WRAP("__fpclassifyf", g_fpclassifyf), MG_WRAP("__fpclassifyl", g_fpclassifyd),
    MG_WRAP("fegetround", g_fegetround), MG_WRAP("fesetround", g_fesetround), MG_WRAP("fegetenv", g_fenv_nop),
    MG_WRAP("fesetenv", g_fenv_nop), MG_WRAP("feholdexcept", g_fenv_nop), MG_WRAP("feupdateenv", g_fenv_nop),
    MG_WRAP("feclearexcept", g_fenv_nop_int), MG_WRAP("feraiseexcept", g_fenv_nop_int), MG_WRAP("fetestexcept", g_fenv_nop_int),
    MG_END
};
