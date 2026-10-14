/* libwebp's src/dsp/dec.c, compiled into the core where the decoder's sources are: see hk_webp.h. */
#if defined(HK_HAVE_WEBP) && HK_HAVE_WEBP
#include "hk_webp_config.h"
#include "src/dsp/dec.c"
#else
typedef int hk_webp_dsp_dec_not_built;
#endif
