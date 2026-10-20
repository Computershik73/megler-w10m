/* libwebp's src/dsp/upsampling.c, compiled into the core where the decoder's sources are: see mg_webp.h. */
#if defined(MG_HAVE_WEBP) && MG_HAVE_WEBP
#include "mg_webp_config.h"
#include "src/dsp/upsampling.c"
#else
typedef int mg_webp_dsp_upsampling_not_built;
#endif
