/*
 * A shell's drawing, looked at and checked: scenes drawn through a
 * meglet_gfx_host and nothing else, as the core calls it (mg_gfx_scenes.c).
 *
 * This is for the real thing -- the phone's Direct2D, DirectWrite and WIC
 * (app/Meglet/player/GfxHost.cpp) -- where mg_gfx_tests.c is for the
 * stand-in. It is plain C over the struct, so it builds wherever the core
 * does; it only runs where someone has a host to hand it: the test program's
 * --gfx-windows mode on a PC (tools/build-tests.sh win).
 */
#ifndef MG_GFX_SCENES_H
#define MG_GFX_SCENES_H

#include "../include/meglet.h"
#include "mg_tests.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_gfx_scenes_config {
    const meglet_gfx_host *host;         /* the drawing to look at; every function of it is expected to be there */
    /* Where each scene goes as a PNG file, and sheet.png with all of them at half size. NULL: no file is written. */
    const char *out_dir;
    /*
     * Put before each file's name; NULL: nothing. And BMP files, not PNG (the colours as they would show, the alpha left
     * out). Both for a phone, whose pictures tools/phone.py fetches as LocalState's shot-*.bmp.
     */
    const char *file_prefix;
    bool bmp;
    /* A TrueType or OpenType file for typeface_from_data. NULL: a few of Windows's own are tried, and the scene says which. */
    const char *font_file;
    /* A JPEG to decode at an eighth of its size. NULL: one of Windows's own pictures is tried. */
    const char *jpeg_file;
} mg_gfx_scenes_config;

/*
 * Draws the scenes and checks what needs no eyes: the scenes the stand-in
 * can draw too compared with the stand-in's, pixel by pixel (the middles of
 * shapes equal, edges within a pixel); row and channel order; what the text
 * functions answer against each other and against the pixels drawn; image
 * files written and read back. Returns the number of failed checks and adds
 * the number made to *checks.
 */
int mg_gfx_scenes_run(const mg_gfx_scenes_config *config, mg_test_sink sink, void *user, int *checks);

#ifdef __cplusplus
}
#endif

#endif
