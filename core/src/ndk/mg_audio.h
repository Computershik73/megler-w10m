/*
 * Sound: voices mixed into one stream for the shell to play.
 *
 * Whatever an app plays PCM through (OpenSL ES buffer queues, AAudio streams,
 * and from Java AudioTrack, MediaPlayer and SoundPool) becomes
 * a voice: a queue of the app's own buffers in the app's own format. One
 * thread here mixes the voices that are playing into 16-bit stereo, ten
 * milliseconds at a time, and hands that to the shell's audio output
 * (megler.h). With no output the thread still consumes the queues in real time,
 * so an app that paces itself by its audio callbacks keeps running, silently.
 */
#ifndef MG_AUDIO_H
#define MG_AUDIO_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_voice mg_voice;
/* A queued buffer has been played to its end. Called on the mixing thread, outside any lock. */
typedef void (*mg_voice_done_fn)(mg_voice *v, void *user);

/* `bits` 8, 16 or 32; `is_float` for 32-bit float samples. NULL for a format that cannot be mixed. */
mg_voice *mg_voice_create(int rate, int channels, int bits, bool is_float);
void mg_voice_destroy(mg_voice *v);
/* The bytes are the caller's and must stay as they are until played. False when the queue is full. */
bool mg_voice_enqueue(mg_voice *v, const void *data, u32 bytes);
void mg_voice_clear(mg_voice *v);
void mg_voice_set_playing(mg_voice *v, bool playing);
void mg_voice_set_gain(mg_voice *v, float gain);
/* Left and right, besides the gain: SoundPool's and AudioTrack's two volumes. Both 1 when made. */
void mg_voice_set_gains(mg_voice *v, float left, float right);
/* The rate its frames are played at from now on: SoundPool's playback rate. */
void mg_voice_set_rate(mg_voice *v, int rate);
void mg_voice_set_done(mg_voice *v, mg_voice_done_fn fn, void *user);
/* Buffers waiting (the one being played included), and how many have been played since the last clear. */
u32 mg_voice_queued(mg_voice *v);
u32 mg_voice_played(mg_voice *v);
/* Frames of the voice's own format played since it was created. */
u64 mg_voice_position(mg_voice *v);

/* A film's sound has begun: with the flag "record-film", the half minute the mixer records starts here. */
void mg_audio_film_begins(void);

/*
 * PCM written into a stream, which waits while the stream's buffers are full: AAudio's written streams
 * (ndk/mg_aaudio.c), for Java's AudioTrack and the music MediaPlayer decodes. `format` 1 for 16-bit, 2 for float.
 * mg_pcm_write takes up to `frames` frames and returns how many it took; with `wait`, it waits while the stream is full,
 * playing or not, as AudioTrack's blocking write does, until all are taken or the stream is flushed or closed.
 */
typedef struct aa_stream mg_pcm;
mg_pcm *mg_pcm_open(int rate, int channels, int format, int buffer_ms);
int mg_pcm_write(mg_pcm *p, const void *frames, int count, bool wait);
void mg_pcm_play(mg_pcm *p, bool play);
void mg_pcm_flush(mg_pcm *p);
void mg_pcm_volume(mg_pcm *p, float left, float right);
u64 mg_pcm_position(mg_pcm *p);           /* frames played */
int mg_pcm_pending(mg_pcm *p);            /* frames written and not yet played */
void mg_pcm_close(mg_pcm *p);

#define MG_AUDIO_RATE 44100
#define MG_AUDIO_CHUNK 441             /* frames mixed at a time: 10 ms */

#ifdef __cplusplus
}
#endif

#endif
