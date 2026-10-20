/* libwebp's src/dsp/cpu.c, compiled into the core where the decoder's sources are: see mg_webp.h. */
#if defined(MG_HAVE_WEBP) && MG_HAVE_WEBP
#include "mg_webp_config.h"
#include "src/dsp/cpu.c"
#else
typedef int mg_webp_dsp_cpu_not_built;
#endif
