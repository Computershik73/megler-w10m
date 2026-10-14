/* libwebp's src/dec/webp_dec.c, compiled into the core where the decoder's sources are: see hk_webp.h. */
#if defined(HK_HAVE_WEBP) && HK_HAVE_WEBP
#include "hk_webp_config.h"
#include "src/dec/webp_dec.c"
#else
typedef int hk_webp_dec_webp_dec_not_built;
#endif
