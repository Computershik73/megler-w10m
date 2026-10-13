/* libmediandk.so and the SurfaceTexture a decoder draws to: a game's films. See hk_media.c. */
#ifndef HK_MEDIA_H
#define HK_MEDIA_H

#include "../bionic/hk_bionic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libmediandk.so's functions and data, for the linker. */
const hk_sym *hk_media_table(void);
/* The Java half (SurfaceTexture, a Surface made of one); after a driver's own table. */
void hk_media_install(void);
/* For the tests: the decoder to use instead of the running app's shell's (NULL: that one again). */
struct megler_media_host;
void hk_media_set_host(const struct megler_media_host *host);

#ifdef __cplusplus
}
#endif

#endif
