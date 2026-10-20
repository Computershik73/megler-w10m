/* libmediandk.so and the SurfaceTexture a decoder draws to: a game's films. See mg_media.c. */
#ifndef MG_MEDIA_H
#define MG_MEDIA_H

#include "../bionic/mg_bionic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* libmediandk.so's functions and data, for the linker. */
const mg_sym *mg_media_table(void);
/* The Java half (SurfaceTexture, a Surface made of one); after a driver's own table. */
void mg_media_install(void);
/* For the tests: the decoder to use instead of the running app's shell's (NULL: that one again). */
struct meglet_media_host;
void mg_media_set_host(const struct meglet_media_host *host);

#ifdef __cplusplus
}
#endif

#endif
