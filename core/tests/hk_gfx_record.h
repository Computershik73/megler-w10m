/*
 * A Java app's drawing kept in a file, and drawn again from it by another
 * host (hk_gfx_record.c).
 *
 * A Java app runs where guest code runs: under qemu-arm, where the shell's
 * drawing is the stand-in, or on a phone. The shell's real drawing runs on
 * Windows. On one PC the two do not meet, so the app's calls through
 * megler_gfx_host are written down under qemu -- every call that draws, with
 * the pixels of every bitmap it draws from, and each picture handed to the
 * window -- and played back on Windows through the real host: the app's
 * views as Direct2D and DirectWrite draw them, laid out as the stand-in's
 * measures of text had the framework lay them out.
 */
#ifndef HK_GFX_RECORD_H
#define HK_GFX_RECORD_H

#include "../include/megler.h"
#include "hk_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts one of the Java test apps (`which`: "hello" or "demos") through
 * megler_run with the stand-in as the shell's drawing, and writes what it
 * asks of the drawing to `record_file`; "hello" has its button pressed once.
 * megler_init first, with hk_gfx_record_log as the log's sink. Only where
 * guest code can run. Returns 0 when the app started, showed a picture and
 * ended.
 */
int hk_gfx_record_run(const char *which, const char *apk, const char *android_root, const char *record_file);
void hk_gfx_record_log(int level, const char *line, void *user);

/*
 * Plays a record back through `host` and writes each picture the app
 * showed as <out_dir>/<name>-N.png (out_dir NULL: no files). `same`: the
 * host is the one the record was made with (the stand-in), so every picture
 * must come out as it was recorded, pixel for pixel, which is the test of
 * the record itself; otherwise the pictures are compared with the recorded
 * ones and the difference is said, not judged. Image files the app decoded
 * are decoded again by `host` and compared with what the stand-in made of
 * them. Returns the number of failed checks and adds the number made to
 * *checks.
 */
int hk_gfx_replay_run(const char *record_file, const megler_gfx_host *host, bool same, const char *out_dir, const char *name, hk_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
