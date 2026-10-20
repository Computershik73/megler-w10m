/* libwebp's src/dec/webp_dec.c, compiled into the core where the decoder's sources are: see mg_webp.h. */
#if defined(MG_HAVE_WEBP) && MG_HAVE_WEBP
#include "mg_webp_config.h"
#include "src/dec/webp_dec.c"
#else
typedef int mg_webp_dec_webp_dec_not_built;
#endif
