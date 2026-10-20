/* libwebp's src/utils/thread_utils.c, compiled into the core where the decoder's sources are: see mg_webp.h. */
#if defined(MG_HAVE_WEBP) && MG_HAVE_WEBP
#include "mg_webp_config.h"
#include "src/utils/thread_utils.c"
#else
typedef int mg_webp_utils_thread_utils_not_built;
#endif
