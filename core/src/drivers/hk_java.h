/* The Java driver's own interface to the rest of the core and to the tests (hk_java.c). */
#ifndef HK_JAVA_H
#define HK_JAVA_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the running Java app has done so far: pictures handed to the shell, and turns of its main loop that asked for input. */
typedef struct hk_java_stats {
    volatile i32 frames;
    volatile i32 polls;
} hk_java_stats;
void hk_java_get_stats(hk_java_stats *out);
/*
 * Whether the app's main thread has ended, and how: the status and the
 * reason hk_run_ended was given. For a test that starts the driver itself, on
 * a host where megler_run refuses every app because guest code cannot run
 * there -- which a Java app without native libraries does not need.
 */
bool hk_java_ended(int *status, const char **reason);

#ifdef __cplusplus
}
#endif

#endif
