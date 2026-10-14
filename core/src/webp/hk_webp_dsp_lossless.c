/* libwebp's src/dsp/lossless.c, compiled into the core where the decoder's sources are: see hk_webp.h. */
#if defined(HK_HAVE_WEBP) && HK_HAVE_WEBP
#include "hk_webp_config.h"
#include "src/dsp/lossless.c"
#else
typedef int hk_webp_dsp_lossless_not_built;
#endif
