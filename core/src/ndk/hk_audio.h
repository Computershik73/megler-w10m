/*
 * Sound: voices mixed into one stream for the shell to play.
 *
 * Whatever an app plays PCM through (OpenSL ES buffer queues, so far) becomes
 * a voice: a queue of the app's own buffers in the app's own format. One
 * thread here mixes the voices that are playing into 16-bit stereo, ten
 * milliseconds at a time, and hands that to the shell's audio output
 * (megler.h). With no output the thread still consumes the queues in real time,
 * so an app that paces itself by its audio callbacks keeps running, silently.
 */
#ifndef HK_AUDIO_H
#define HK_AUDIO_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_voice hk_voice;
/* A queued buffer has been played to its end. Called on the mixing thread, outside any lock. */
typedef void (*hk_voice_done_fn)(hk_voice *v, void *user);

/* `bits` 8, 16 or 32; `is_float` for 32-bit float samples. NULL for a format that cannot be mixed. */
hk_voice *hk_voice_create(int rate, int channels, int bits, bool is_float);
void hk_voice_destroy(hk_voice *v);
/* The bytes are the caller's and must stay as they are until played. False when the queue is full. */
bool hk_voice_enqueue(hk_voice *v, const void *data, u32 bytes);
void hk_voice_clear(hk_voice *v);
void hk_voice_set_playing(hk_voice *v, bool playing);
void hk_voice_set_gain(hk_voice *v, float gain);
void hk_voice_set_done(hk_voice *v, hk_voice_done_fn fn, void *user);
/* Buffers waiting (the one being played included), and how many have been played since the last clear. */
u32 hk_voice_queued(hk_voice *v);
u32 hk_voice_played(hk_voice *v);
/* Frames of the voice's own format played since it was created. */
u64 hk_voice_position(hk_voice *v);

/* A film's sound has begun: with the flag "record-film", the half minute the mixer records starts here. */
void hk_audio_film_begins(void);

#define HK_AUDIO_RATE 44100
#define HK_AUDIO_CHUNK 441             /* frames mixed at a time: 10 ms */

#ifdef __cplusplus
}
#endif

#endif
