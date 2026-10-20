/* libwebp's src/dec/frame_dec.c, compiled into the core where the decoder's sources are: see mg_webp.h. */
#if defined(MG_HAVE_WEBP) && MG_HAVE_WEBP
#include "mg_webp_config.h"
#include "src/dec/frame_dec.c"
#else
typedef int mg_webp_dec_frame_dec_not_built;
#endif
