/* libwebp's src/dsp/cpu.c, compiled into the core where the decoder's sources are: see hk_webp.h. */
#if defined(HK_HAVE_WEBP) && HK_HAVE_WEBP
#include "hk_webp_config.h"
#include "src/dsp/cpu.c"
#else
typedef int hk_webp_dsp_cpu_not_built;
#endif
