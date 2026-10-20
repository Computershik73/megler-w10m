/*
 * The ARM run-time ABI helpers that bionic's libc.so exports.
 *
 * Compilers call these for memory copies, for integer division on CPUs
 * without a divide instruction, and -- in armeabi (ARMv5) code, which has no
 * FPU to count on -- for every floating-point operation. A library normally
 * carries its own copies from libgcc, but libc.so exports them too and older
 * libraries import them from there.
 *
 * The floating-point ones take and return bits, like everything else that
 * crosses the guest boundary with a float in it. The few whose results do not
 * fit C's return conventions (64-bit division, which returns a quotient and a
 * remainder in four registers) are in arm/mg_stubs.S.
 */
#include "mg_bionic.h"

/* __aeabi_memset's arguments are (dest, n, c): not memset's order. */
static void g_aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static void g_aeabi_memmove(void *d, const void *s, size_t n) { memmove(d, s, n); }
static void g_aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
static void g_aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }

static int g_aeabi_idiv(int a, int b) { return b ? (a == (int)0x80000000 && b == -1 ? a : a / b) : 0; }
static unsigned g_aeabi_uidiv(unsigned a, unsigned b) { return b ? a / b : 0; }
/* {quotient, remainder} in r0, r1: a 64-bit result's two halves. */
static u64 g_aeabi_idivmod(int a, int b)
{
    int q = g_aeabi_idiv(a, b), r = b ? a - q * b : 0;
    return (u64)(u32)q | ((u64)(u32)r << 32);
}
static u64 g_aeabi_uidivmod(unsigned a, unsigned b)
{
    unsigned q = g_aeabi_uidiv(a, b), r = b ? a - q * b : 0;
    return (u64)q | ((u64)r << 32);
}

/* What arm/mg_stubs.S calls for __aeabi_ldivmod and __aeabi_uldivmod. */
u64 mg_aeabi_uldivmod(u64 n, u64 d, u64 *rem)
{
    if (!d) {
        *rem = 0;
        return 0;
    }
    *rem = n % d;
    return n / d;
}

i64 mg_aeabi_ldivmod(i64 n, i64 d, i64 *rem)
{
    if (!d || (n == (i64)0x8000000000000000ull && d == -1)) {
        *rem = 0;
        return d ? n : 0;
    }
    *rem = n % d;
    return n / d;
}

static i64 g_aeabi_lmul(i64 a, i64 b) { return (i64)((u64)a * (u64)b); }
static i64 g_aeabi_llsl(i64 a, int n) { return (i64)((u64)a << (n & 63)); }
static i64 g_aeabi_llsr(i64 a, int n) { return (i64)((u64)a >> (n & 63)); }
static i64 g_aeabi_lasr(i64 a, int n) { return a >> (n & 63); }
static int g_aeabi_lcmp(i64 a, i64 b) { return a < b ? -1 : a > b; }
static int g_aeabi_ulcmp(u64 a, u64 b) { return a < b ? -1 : a > b; }

/* Double precision. */
static u64 g_dadd(u64 a, u64 b) { return mg_db(mg_d(a) + mg_d(b)); }
static u64 g_dsub(u64 a, u64 b) { return mg_db(mg_d(a) - mg_d(b)); }
static u64 g_drsub(u64 a, u64 b) { return mg_db(mg_d(b) - mg_d(a)); }
static u64 g_dmul(u64 a, u64 b) { return mg_db(mg_d(a) * mg_d(b)); }
static u64 g_ddiv(u64 a, u64 b) { return mg_db(mg_d(a) / mg_d(b)); }
static int g_dcmpeq(u64 a, u64 b) { return mg_d(a) == mg_d(b); }
static int g_dcmplt(u64 a, u64 b) { return mg_d(a) < mg_d(b); }
static int g_dcmple(u64 a, u64 b) { return mg_d(a) <= mg_d(b); }
static int g_dcmpge(u64 a, u64 b) { return mg_d(a) >= mg_d(b); }
static int g_dcmpgt(u64 a, u64 b) { return mg_d(a) > mg_d(b); }
static int g_dcmpun(u64 a, u64 b) { return mg_d(a) != mg_d(a) || mg_d(b) != mg_d(b); }
static int g_d2iz(u64 a) { return (int)mg_d(a); }
static unsigned g_d2uiz(u64 a) { return (unsigned)mg_d(a); }
static i64 g_d2lz(u64 a) { return (i64)mg_d(a); }
static u64 g_d2ulz(u64 a) { return (u64)mg_d(a); }
static u32 g_d2f(u64 a) { return mg_fb((float)mg_d(a)); }
static u64 g_i2d(int a) { return mg_db((double)a); }
static u64 g_ui2d(unsigned a) { return mg_db((double)a); }
static u64 g_l2d(i64 a) { return mg_db((double)a); }
static u64 g_ul2d(u64 a) { return mg_db((double)a); }

/* Single precision. */
static u32 g_fadd(u32 a, u32 b) { return mg_fb(mg_f(a) + mg_f(b)); }
static u32 g_fsub(u32 a, u32 b) { return mg_fb(mg_f(a) - mg_f(b)); }
static u32 g_frsub(u32 a, u32 b) { return mg_fb(mg_f(b) - mg_f(a)); }
static u32 g_fmul(u32 a, u32 b) { return mg_fb(mg_f(a) * mg_f(b)); }
static u32 g_fdiv(u32 a, u32 b) { return mg_fb(mg_f(a) / mg_f(b)); }
static int g_fcmpeq(u32 a, u32 b) { return mg_f(a) == mg_f(b); }
static int g_fcmplt(u32 a, u32 b) { return mg_f(a) < mg_f(b); }
static int g_fcmple(u32 a, u32 b) { return mg_f(a) <= mg_f(b); }
static int g_fcmpge(u32 a, u32 b) { return mg_f(a) >= mg_f(b); }
static int g_fcmpgt(u32 a, u32 b) { return mg_f(a) > mg_f(b); }
static int g_fcmpun(u32 a, u32 b) { return mg_f(a) != mg_f(a) || mg_f(b) != mg_f(b); }
static int g_f2iz(u32 a) { return (int)mg_f(a); }
static unsigned g_f2uiz(u32 a) { return (unsigned)mg_f(a); }
static i64 g_f2lz(u32 a) { return (i64)mg_f(a); }
static u64 g_f2ulz(u32 a) { return (u64)mg_f(a); }
static u64 g_f2d(u32 a) { return mg_db((double)mg_f(a)); }
static u32 g_i2f(int a) { return mg_fb((float)a); }
static u32 g_ui2f(unsigned a) { return mg_fb((float)a); }
static u32 g_l2f(i64 a) { return mg_fb((float)a); }
static u32 g_ul2f(u64 a) { return mg_fb((float)a); }

const mg_sym mg_tab_aeabi[] = {
    MG_WRAP("__aeabi_memcpy", g_aeabi_memcpy), MG_WRAP("__aeabi_memcpy4", g_aeabi_memcpy), MG_WRAP("__aeabi_memcpy8", g_aeabi_memcpy),
    MG_WRAP("__aeabi_memmove", g_aeabi_memmove), MG_WRAP("__aeabi_memmove4", g_aeabi_memmove), MG_WRAP("__aeabi_memmove8", g_aeabi_memmove),
    MG_WRAP("__aeabi_memset", g_aeabi_memset), MG_WRAP("__aeabi_memset4", g_aeabi_memset), MG_WRAP("__aeabi_memset8", g_aeabi_memset),
    MG_WRAP("__aeabi_memclr", g_aeabi_memclr), MG_WRAP("__aeabi_memclr4", g_aeabi_memclr), MG_WRAP("__aeabi_memclr8", g_aeabi_memclr),
    MG_WRAP("__aeabi_idiv", g_aeabi_idiv), MG_WRAP("__aeabi_uidiv", g_aeabi_uidiv), MG_WRAP("__aeabi_idivmod", g_aeabi_idivmod),
    MG_WRAP("__aeabi_uidivmod", g_aeabi_uidivmod), MG_WRAP("__divsi3", g_aeabi_idiv), MG_WRAP("__udivsi3", g_aeabi_uidiv),
    MG_WRAP("__aeabi_lmul", g_aeabi_lmul), MG_WRAP("__aeabi_llsl", g_aeabi_llsl), MG_WRAP("__aeabi_llsr", g_aeabi_llsr),
    MG_WRAP("__aeabi_lasr", g_aeabi_lasr), MG_WRAP("__aeabi_lcmp", g_aeabi_lcmp), MG_WRAP("__aeabi_ulcmp", g_aeabi_ulcmp),
    MG_WRAP("__muldi3", g_aeabi_lmul), MG_WRAP("__ashldi3", g_aeabi_llsl), MG_WRAP("__lshrdi3", g_aeabi_llsr), MG_WRAP("__ashrdi3", g_aeabi_lasr),
    MG_WRAP("__aeabi_dadd", g_dadd), MG_WRAP("__aeabi_dsub", g_dsub), MG_WRAP("__aeabi_drsub", g_drsub), MG_WRAP("__aeabi_dmul", g_dmul),
    MG_WRAP("__aeabi_ddiv", g_ddiv), MG_WRAP("__aeabi_dcmpeq", g_dcmpeq), MG_WRAP("__aeabi_dcmplt", g_dcmplt),
    MG_WRAP("__aeabi_dcmple", g_dcmple), MG_WRAP("__aeabi_dcmpge", g_dcmpge), MG_WRAP("__aeabi_dcmpgt", g_dcmpgt),
    MG_WRAP("__aeabi_dcmpun", g_dcmpun), MG_WRAP("__aeabi_d2iz", g_d2iz), MG_WRAP("__aeabi_d2uiz", g_d2uiz),
    MG_WRAP("__aeabi_d2lz", g_d2lz), MG_WRAP("__aeabi_d2ulz", g_d2ulz), MG_WRAP("__aeabi_d2f", g_d2f), MG_WRAP("__aeabi_i2d", g_i2d),
    MG_WRAP("__aeabi_ui2d", g_ui2d), MG_WRAP("__aeabi_l2d", g_l2d), MG_WRAP("__aeabi_ul2d", g_ul2d),
    MG_WRAP("__adddf3", g_dadd), MG_WRAP("__subdf3", g_dsub), MG_WRAP("__muldf3", g_dmul), MG_WRAP("__divdf3", g_ddiv),
    MG_WRAP("__fixdfsi", g_d2iz), MG_WRAP("__fixunsdfsi", g_d2uiz), MG_WRAP("__fixdfdi", g_d2lz), MG_WRAP("__fixunsdfdi", g_d2ulz),
    MG_WRAP("__floatsidf", g_i2d), MG_WRAP("__floatunsidf", g_ui2d), MG_WRAP("__floatdidf", g_l2d), MG_WRAP("__floatundidf", g_ul2d),
    MG_WRAP("__truncdfsf2", g_d2f), MG_WRAP("__extendsfdf2", g_f2d),
    MG_WRAP("__aeabi_fadd", g_fadd), MG_WRAP("__aeabi_fsub", g_fsub), MG_WRAP("__aeabi_frsub", g_frsub), MG_WRAP("__aeabi_fmul", g_fmul),
    MG_WRAP("__aeabi_fdiv", g_fdiv), MG_WRAP("__aeabi_fcmpeq", g_fcmpeq), MG_WRAP("__aeabi_fcmplt", g_fcmplt),
    MG_WRAP("__aeabi_fcmple", g_fcmple), MG_WRAP("__aeabi_fcmpge", g_fcmpge), MG_WRAP("__aeabi_fcmpgt", g_fcmpgt),
    MG_WRAP("__aeabi_fcmpun", g_fcmpun), MG_WRAP("__aeabi_f2iz", g_f2iz), MG_WRAP("__aeabi_f2uiz", g_f2uiz),
    MG_WRAP("__aeabi_f2lz", g_f2lz), MG_WRAP("__aeabi_f2ulz", g_f2ulz), MG_WRAP("__aeabi_f2d", g_f2d), MG_WRAP("__aeabi_i2f", g_i2f),
    MG_WRAP("__aeabi_ui2f", g_ui2f), MG_WRAP("__aeabi_l2f", g_l2f), MG_WRAP("__aeabi_ul2f", g_ul2f),
    MG_WRAP("__addsf3", g_fadd), MG_WRAP("__subsf3", g_fsub), MG_WRAP("__mulsf3", g_fmul), MG_WRAP("__divsf3", g_fdiv),
    MG_WRAP("__fixsfsi", g_f2iz), MG_WRAP("__fixunssfsi", g_f2uiz), MG_WRAP("__fixsfdi", g_f2lz), MG_WRAP("__fixunssfdi", g_f2ulz),
    MG_WRAP("__floatsisf", g_i2f), MG_WRAP("__floatunsisf", g_ui2f), MG_WRAP("__floatdisf", g_l2f), MG_WRAP("__floatundisf", g_ul2f),
    MG_END
};
