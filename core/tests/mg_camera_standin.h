/*
 * A stand-in for the shell's cameras (megler_camera_host), for tests where there are none: portable C that makes pictures
 * whose every byte is known, on a thread of its own, as a camera's driver would hand them over.
 *
 * Two cameras, as a phone has them: 0 at the back, turned 90 degrees (a sensor laid along the phone's long side), with
 * focus (once, continuously, close up) and a torch, at 1280 x 720, 640 x 480 and 320 x 240; 1 at the front, turned 270,
 * fixed focus, no light, at 640 x 480 and 320 x 240. Each streams at 15 to 30 frames a second, at the rate it was started
 * with. A camera opens once at a time: a second open is MEGLER_CAMERA_BUSY; and every open is MEGLER_CAMERA_DENIED while
 * mg_camera_standin_refuse says so, as Windows answers when the user has said no.
 *
 * A picture is NV12 in four quarters of one colour each, with BT.601's video-range values: the top left red, the top right
 * green, the bottom left blue, the bottom right white (MG_CAMERA_STANDIN_* below, luma, U, V). The first four bytes of the
 * luma are the picture's number, counted from 1 since the start, low byte first. Rows are longer than the picture (16 more
 * bytes of luma, 32 more of chroma) and what is beyond the picture is 0xEE, so that a copy that takes the stride for the
 * width is seen.
 *
 * Focusing once ends 30 ms later, in focus; continuous focus says so at the next picture. The torch's state and every
 * count are there to be read.
 */
#ifndef MG_CAMERA_STANDIN_H
#define MG_CAMERA_STANDIN_H

#include "../include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MG_CAMERA_STANDIN_RED 81, 90, 240
#define MG_CAMERA_STANDIN_GREEN 145, 54, 34
#define MG_CAMERA_STANDIN_BLUE 41, 240, 110
#define MG_CAMERA_STANDIN_WHITE 235, 128, 128

const megler_camera_host *mg_camera_standin(void);
/* Every open refused (MEGLER_CAMERA_DENIED) from now on, or no longer. */
void mg_camera_standin_refuse(bool refuse);

typedef struct mg_camera_standin_counts {
    int opened, closed, started, stopped, frames, focused, cancelled;
    bool torch;                        /* on, now */
    int width, height, fps;            /* as last started */
} mg_camera_standin_counts;
void mg_camera_standin_counts_get(mg_camera_standin_counts *out);

#ifdef __cplusplus
}
#endif

#endif
