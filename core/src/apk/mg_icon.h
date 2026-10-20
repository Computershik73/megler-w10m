/*
 * An app's icon drawn here, for an APK that has no picture of it: since Android 8 an app may give only an adaptive icon
 * (res/mipmap-anydpi-v26/ic_launcher.xml: a background and a foreground layer, each a drawable), and its layers are often
 * vector drawables -- paths in SVG's syntax, groups, clip paths, gradients -- which no picture decoder reads. The
 * drawables an icon is made of are drawn into a PNG: vector, adaptive-icon, layer-list, inset, shape, bitmap, colours
 * and colour selectors, and pictures (PNG, WebP). An adaptive icon is cut to the 72 of its 108 dp a launcher shows and
 * rounded, as a launcher masks it.
 */
#ifndef MG_ICON_H
#define MG_ICON_H

#include "../mg_base.h"

/* Where the drawables come from: the APK's (and its splits') files and resource tables. */
typedef struct mg_icon_src {
    /* A file by its path in the APK ("res/drawable/x.xml"), malloc'd, or NULL. */
    u8 *(*read)(void *user, const char *path, size_t *size);
    /* Resource `id`'s value, of its configuration best for an icon (the default, else the densest): Res_value's type and
       data, and for a string (a file's path) its text. A reference comes back as one (type 0x01). */
    bool (*value)(void *user, u32 id, u8 *type, u32 *data, char *text, size_t cap);
    void *user;
} mg_icon_src;

/* The drawable at `path` (an XML of the APK's, a PNG or a WebP) drawn as a PNG of `size` x `size`, malloc'd; NULL when
   it cannot be drawn. */
u8 *mg_icon_png(const mg_icon_src *src, const char *path, int size, size_t *png_size);

/* The same for drawn pixels: R, G, B, A straight, `size` x `size`, malloc'd (for the tests). */
u8 *mg_icon_rgba(const mg_icon_src *src, const char *path, int size);

/* A PNG of R, G, B, A straight pixels, malloc'd. */
u8 *mg_png_encode(const u8 *rgba, int width, int height, size_t *png_size);

#endif
