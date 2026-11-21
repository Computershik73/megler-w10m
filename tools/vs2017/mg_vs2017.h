/* Force-included into the core's C when the toolset is VS 2017's MSVC 14.16 (tools/build-appx.sh, MEGLER_VC), the last
   with ARM32 tools besides VS 2022 17.14's: its C compiler has C99 and most of C11, but not _Static_assert. */
#ifndef MG_VS2017_H
#define MG_VS2017_H
#if defined(_MSC_VER) && _MSC_VER < 1920 && !defined(__cplusplus) && !defined(__clang__)
#define MG_SA_CAT2(a, b) a##b
#define MG_SA_CAT(a, b) MG_SA_CAT2(a, b)
#define _Static_assert(e, m) typedef char MG_SA_CAT(mg_static_assert_, __COUNTER__)[(e) ? 1 : -1]
#endif
#endif
