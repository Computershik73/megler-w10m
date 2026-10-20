/*
 * How libwebp's files are compiled here (mg_webp.h): included first by each
 * mg_webp_*.c that compiles one of them, and by nothing else.
 *
 * Plain C on every host. libwebp decides by itself which processor it is
 * built for and switches its NEON or SSE code on (for MSVC on ARM without
 * being asked), which would need those files compiled too, with the right
 * options for each compiler, and a question to the processor at run time.
 * The switches are its own macros, set by its src/dsp/cpu.h; they are taken
 * back here once that header has been read. Its include guard keeps them
 * that way for the file that follows.
 */
#ifndef MG_WEBP_CONFIG_H
#define MG_WEBP_CONFIG_H

/* Somebody else's code, with warnings of its own at the levels this core is built at. */
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include "src/dsp/cpu.h"

#undef WEBP_USE_SSE2
#undef WEBP_HAVE_SSE2
#undef WEBP_USE_SSE41
#undef WEBP_HAVE_SSE41
#undef WEBP_USE_AVX2
#undef WEBP_HAVE_AVX2
#undef WEBP_USE_NEON
#undef WEBP_HAVE_NEON
#undef WEBP_ANDROID_NEON
#undef WEBP_USE_INTRINSICS
#undef WEBP_USE_MIPS32
#undef WEBP_USE_MIPS32_R2
#undef WEBP_USE_MIPS_DSP_R2
#undef WEBP_USE_MSA
/* With NEON on, the C code that NEON replaces is left out; it must be there. */
#undef WEBP_NEON_OMIT_C_CODE
#define WEBP_NEON_OMIT_C_CODE 0

#endif
