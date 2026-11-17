/*
 * The microphone (megler_microphone_host), for whatever records: Java's AudioRecord (drivers/mg_java.c) and OpenSL ES's
 * audio recorder (ndk/mg_opensles.c).
 *
 * The shell pushes what it hears into a ring of two seconds (megler_microphone_push); a reader takes it out, waiting for
 * it when asked to block. One recording at a time, as on Android: a second start takes the microphone over, and the
 * recording taken over from reads nothing more and stops nothing when it ends.
 */
#ifndef MG_MIC_H
#define MG_MIC_H

#include "../../include/megler.h"
#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The microphone asked of the shell (`h`: the run's megler_microphone_host), as 16-bit PCM at `rate` Hz (4000 to 192000)
 * in `channels` (1 or 2) -- Windows asks the user the first time, and the call waits for the answer. The recording's
 * token, for the two below, when the microphone hears; 0 when it does not (none there: h->start is zero; or refused).
 */
u32 mg_mic_start(const megler_microphone_host *h, int rate, int channels);
/* The microphone no longer wanted by the recording `token` (0, or one taken over from: nothing happens). */
void mg_mic_stop(u32 token);
/*
 * Up to `want` samples heard (frames times channels) for the recording `token`; how many. With `blocking` it waits for
 * all of them while the recording goes on; when nothing comes for a second (the microphone gone) the rest is silence, so
 * that the app does not hang. Without, and once the recording has stopped, what the ring holds. 0 for a recording taken
 * over from.
 */
int mg_mic_read(u32 token, i16 *out, int want, bool blocking);

#ifdef __cplusplus
}
#endif

#endif
