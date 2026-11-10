/*
 * OpenGL ES 1 on ANGLE itself: Megler's own ANGLE for this PC (tools/build-angle.py --arch x64, with angle/*.patch), through
 * the core's EGL and OpenGL ES (ndk/mg_egl.c) as an app's Java reaches them -- GL10, MeglerGL10 and GLES10's natives call
 * what mg_egl_host gives -- on a display of Direct3D feature level 9_3, as a Lumia 650's Adreno 304 and most Lumias have
 * (the flag egl-level-9_3), or of this PC's own level:
 *
 *     mg_tests --angle-windows <ANGLE's folder> <scratch folder> [9_3]
 *
 * A version-1 context on a 64x64 pbuffer draws what Google Maps' legacy renderer draws with GL10 (Transit's map): a tile, a
 * texture of four colours on client arrays of positions, texture coordinates and colours, its matrices loaded and multiplied
 * (glLoadMatrixf, glMultMatrixf) and modulated by the vertex colours; roads from buffer objects with glDrawElements; labels
 * as alpha textures blended over the map, or cut by the alpha test; matrices pushed, moved, turned and scaled; a texture
 * replacing the colour and blended half; a lit square; the combiner; glDrawTexOES; and the eight lights at once, which feature
 * level 9_3's short vertex programs may refuse -- refused, its draws fail (GL_INVALID_OPERATION) each time and the next
 * state draws, where ANGLE as gecko-w10m builds it ended the app at the second. Each picture is read back (glReadPixels) and
 * its pixels checked, and written as <scratch>/gles1-<level>-<scene>.bmp. At 9_3 the version-1 context must be of version
 * 1 (no fallback to 2: mg_egl.c, es1_on_es2) and ANGLE's renderer must say level_9_3.
 *
 * Only in the build tools/build-tests.sh win makes (MG_ANGLE_WINDOWS); elsewhere it says so and does nothing.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../include/megler.h"
#include "../src/host/mg_host.h"
#include "../src/mg_core.h"
#include "../src/ndk/mg_egl.h"
#include "mg_tests.h"

#ifdef MG_ANGLE_WINDOWS
#include <windows.h>

/* scenes: begin */
enum {
    GL_NO_ERROR_ = 0, GL_INVALID_OPERATION_ = 0x0502, GL_COLOR_BUFFER_BIT_ = 0x4000, GL_DEPTH_BUFFER_BIT_ = 0x0100,
    GL_PROJECTION_ = 0x1701, GL_MODELVIEW_ = 0x1700, GL_VERTEX_ARRAY_ = 0x8074, GL_NORMAL_ARRAY_ = 0x8075, GL_COLOR_ARRAY_ = 0x8076,
    GL_TEXTURE_COORD_ARRAY_ = 0x8078, GL_FLOAT_ = 0x1406, GL_UNSIGNED_BYTE_ = 0x1401, GL_UNSIGNED_SHORT_ = 0x1403, GL_SHORT_ = 0x1402,
    GL_TRIANGLES_ = 4, GL_TRIANGLE_STRIP_ = 5, GL_TRIANGLE_FAN_ = 6, GL_TEXTURE_2D_ = 0x0DE1, GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_MAG_FILTER_ = 0x2800, GL_NEAREST_ = 0x2600, GL_TEXTURE_WRAP_S_ = 0x2802, GL_TEXTURE_WRAP_T_ = 0x2803,
    GL_CLAMP_TO_EDGE_ = 0x812F, GL_RGBA_ = 0x1908, GL_ALPHA_ = 0x1906, GL_LUMINANCE_ = 0x1909, GL_TEXTURE_ENV_ = 0x2300,
    GL_TEXTURE_ENV_MODE_ = 0x2200, GL_TEXTURE_ENV_COLOR_ = 0x2201, GL_MODULATE_ = 0x2100, GL_REPLACE_ = 0x1E01, GL_BLEND_ = 0x0BE2,
    GL_SRC_ALPHA_ = 0x0302, GL_ONE_MINUS_SRC_ALPHA_ = 0x0303, GL_ALPHA_TEST_ = 0x0BC0, GL_GREATER_ = 0x0204, GL_ARRAY_BUFFER_ = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER_ = 0x8893, GL_STATIC_DRAW_ = 0x88E4, GL_LIGHTING_ = 0x0B50, GL_LIGHT0_ = 0x4000, GL_AMBIENT_ = 0x1200,
    GL_DIFFUSE_ = 0x1201, GL_SPECULAR_ = 0x1202, GL_POSITION_ = 0x1203, GL_LIGHT_MODEL_AMBIENT_ = 0x0B53, GL_FRONT_AND_BACK_ = 0x0408,
    GL_VERSION_ = 0x1F02, GL_RENDERER_ = 0x1F01, GL_TEXTURE_CROP_RECT_OES_ = 0x8B9D, GL_DEPTH_TEST_ = 0x0B71,
    GL_UNPACK_ALIGNMENT_ = 0x0CF5, GL_PACK_ALIGNMENT_ = 0x0D05, GL_COMBINE_ = 0x8570, GL_COMBINE_RGB_ = 0x8571,
    GL_COMBINE_ALPHA_ = 0x8572, GL_SRC0_RGB_ = 0x8580, GL_SRC1_RGB_ = 0x8581, GL_SRC0_ALPHA_ = 0x8588, GL_OPERAND0_RGB_ = 0x8590,
    GL_OPERAND1_RGB_ = 0x8591, GL_OPERAND0_ALPHA_ = 0x8598, GL_SRC_COLOR_ = 0x0300, GL_CONSTANT_ = 0x8576, GL_PRIMARY_COLOR_ = 0x8577,
    GL_TEXTURE_ = 0x1702, GL_ADD_ = 0x0104, GL_FOG_ = 0x0B60, GL_FOG_MODE_ = 0x0B65, GL_FOG_START_ = 0x0B63, GL_FOG_END_ = 0x0B64,
    GL_FOG_COLOR_ = 0x0B66, GL_LINEAR_ = 0x2601, GL_TEXTURE0_ = 0x84C0, GL_TEXTURE1_ = 0x84C1, GL_CLIP_PLANE0_ = 0x3000,
    GL_FLAT_ = 0x1D00, GL_SMOOTH_ = 0x1D01, GL_POINTS_ = 0, GL_LINES_ = 1, GL_LINE_LOOP_ = 2, GL_LINE_STRIP_ = 3,
    GL_SPOT_DIRECTION_ = 0x1204, GL_SPOT_EXPONENT_ = 0x1205, GL_SPOT_CUTOFF_ = 0x1206, GL_CONSTANT_ATTENUATION_ = 0x1207,
    GL_LINEAR_ATTENUATION_ = 0x1208, GL_SHININESS_ = 0x1601, GL_COLOR_MATERIAL_ = 0x0B57, GL_NORMALIZE_ = 0x0BA1,
    GL_RESCALE_NORMAL_ = 0x803A, GL_EXP_ = 0x0800, GL_EXP2_ = 0x0801, GL_FOG_DENSITY_ = 0x0B62, GL_RGB_ = 0x1907,
    GL_LUMINANCE_ALPHA_ = 0x190A, GL_DECAL_ = 0x2101, GL_RGB_SCALE_ = 0x8573, GL_ADD_SIGNED_ = 0x8574, GL_INTERPOLATE_ = 0x8575,
    GL_SUBTRACT_ = 0x84E7, GL_DOT3_RGB_ = 0x86AE, GL_DOT3_RGBA_ = 0x86AF, GL_SRC2_RGB_ = 0x8582, GL_SRC1_ALPHA_ = 0x8589,
    GL_SRC2_ALPHA_ = 0x858A, GL_OPERAND2_RGB_ = 0x8592, GL_OPERAND1_ALPHA_ = 0x8599, GL_OPERAND2_ALPHA_ = 0x859A,
    GL_ONE_MINUS_SRC_COLOR_ = 0x0301, GL_PREVIOUS_ = 0x8578, GL_TEXTURE_CUBE_MAP_ = 0x8513, GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515,
    GL_POINT_SPRITE_OES_ = 0x8861, GL_COORD_REPLACE_OES_ = 0x8862, GL_GENERATE_MIPMAP_ = 0x8191, GL_LINEAR_MIPMAP_NEAREST_ = 0x2701,
    GL_FIXED_ = 0x140C
};

#define GL1_SIZE 64

/* The functions of OpenGL ES 1 the scenes call, as an app's GL10 calls them. */
typedef struct gl1 {
    void (*ClearColor)(float, float, float, float);
    void (*Clear)(u32);
    void (*Viewport)(i32, i32, i32, i32);
    void (*MatrixMode)(u32);
    void (*LoadIdentity)(void);
    void (*LoadMatrixf)(const float *);
    void (*MultMatrixf)(const float *);
    void (*PushMatrix)(void);
    void (*PopMatrix)(void);
    void (*Translatef)(float, float, float);
    void (*Rotatef)(float, float, float, float);
    void (*Scalef)(float, float, float);
    void (*EnableClientState)(u32);
    void (*DisableClientState)(u32);
    void (*VertexPointer)(i32, u32, i32, const void *);
    void (*ColorPointer)(i32, u32, i32, const void *);
    void (*TexCoordPointer)(i32, u32, i32, const void *);
    void (*NormalPointer)(u32, i32, const void *);
    void (*Color4f)(float, float, float, float);
    void (*Normal3f)(float, float, float);
    void (*Enable)(u32);
    void (*Disable)(u32);
    void (*GenTextures)(i32, u32 *);
    void (*DeleteTextures)(i32, const u32 *);
    void (*BindTexture)(u32, u32);
    void (*TexImage2D)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    void (*TexParameteri)(u32, u32, i32);
    void (*TexParameteriv)(u32, u32, const i32 *);
    void (*TexEnvi)(u32, u32, i32);
    void (*TexEnvfv)(u32, u32, const float *);
    void (*BlendFunc)(u32, u32);
    void (*AlphaFunc)(u32, float);
    void (*Lightfv)(u32, u32, const float *);
    void (*LightModelfv)(u32, const float *);
    void (*Materialfv)(u32, u32, const float *);
    void (*Fogf)(u32, float);
    void (*Fogfv)(u32, const float *);
    void (*GenBuffers)(i32, u32 *);
    void (*DeleteBuffers)(i32, const u32 *);
    void (*BindBuffer)(u32, u32);
    void (*BufferData)(u32, intptr_t, const void *, u32);
    void (*DrawArrays)(u32, i32, i32);
    void (*DrawElements)(u32, i32, u32, const void *);
    void (*DrawTexiOES)(i32, i32, i32, i32, i32);
    void (*PixelStorei)(u32, i32);
    void (*ReadPixels)(i32, i32, i32, i32, u32, u32, void *);
    u32 (*GetError)(void);
    const u8 *(*GetString)(u32);
    void (*ActiveTexture)(u32);
    void (*ClientActiveTexture)(u32);
    void (*ClipPlanef)(u32, const float *);
    void (*ShadeModel)(u32);
    void (*PointSize)(float);
    void (*Lightf)(u32, u32, float);
    void (*Materialf)(u32, u32, float);
    void (*TexEnvf)(u32, u32, float);
} gl1;

static bool gl1_resolve(gl1 *f, void *(*get)(const char *name))
{
    static const char *const names[] = {
        "glClearColor", "glClear", "glViewport", "glMatrixMode", "glLoadIdentity", "glLoadMatrixf", "glMultMatrixf", "glPushMatrix",
        "glPopMatrix", "glTranslatef", "glRotatef", "glScalef", "glEnableClientState", "glDisableClientState", "glVertexPointer",
        "glColorPointer", "glTexCoordPointer", "glNormalPointer", "glColor4f", "glNormal3f", "glEnable", "glDisable", "glGenTextures",
        "glDeleteTextures", "glBindTexture", "glTexImage2D", "glTexParameteri", "glTexParameteriv", "glTexEnvi", "glTexEnvfv",
        "glBlendFunc", "glAlphaFunc", "glLightfv", "glLightModelfv", "glMaterialfv", "glFogf", "glFogfv", "glGenBuffers",
        "glDeleteBuffers", "glBindBuffer", "glBufferData", "glDrawArrays", "glDrawElements", "glDrawTexiOES", "glPixelStorei",
        "glReadPixels", "glGetError", "glGetString", "glActiveTexture", "glClientActiveTexture", "glClipPlanef", "glShadeModel",
        "glPointSize", "glLightf", "glMaterialf", "glTexEnvf",
    };
    void **slots = (void **)f;
    size_t i;
    bool all = true;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        slots[i] = get(names[i]);
        if (!slots[i]) all = false;
    }
    return all;
}

typedef struct gl1_run {
    const gl1 *f;
    const char *level;                 /* "9_3", or this PC's own */
    const char *pictures;              /* where each scene's picture is written, or NULL */
    bool compare;                      /* each picture compared with the PC's own level's of that scene, where it is there */
    void (*say)(const char *line, void *user);
    void *user;
    int checks, failed, compared;
    char scene[40];
} gl1_run;

static void gl1_say(gl1_run *r, const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    r->say(line, r->user);
}

static void gl1_check(gl1_run *r, bool ok, const char *what)
{
    r->checks++;
    if (ok) return;
    r->failed++;
    gl1_say(r, "  FAIL OpenGL ES 1 at %s, %s: %s", r->level, r->scene, what);
}

/* A pixel of the pbuffer, rows from the bottom as OpenGL has them, as 0xRRGGBBAA. */
static u32 gl1_pixel(gl1_run *r, int x, int y)
{
    u8 p[4] = { 0, 0, 0, 0 };
    r->f->ReadPixels(x, y, 1, 1, GL_RGBA_, GL_UNSIGNED_BYTE_, p);
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static bool gl1_near(u32 got, u32 want, int tolerance)
{
    int i;
    for (i = 0; i < 32; i += 8) {
        int a = (int)(got >> i & 255), b = (int)(want >> i & 255);
        if (a - b > tolerance || b - a > tolerance) return false;
    }
    return true;
}

static void gl1_expect(gl1_run *r, int x, int y, u32 want)
{
    char what[160];
    u32 got = gl1_pixel(r, x, y);
    snprintf(what, sizeof(what), "pixel %d,%d is %08x, expected %08x", x, y, (unsigned)got, (unsigned)want);
    gl1_check(r, gl1_near(got, want, 3), what);
}

/*
 * The scene's picture as a BMP (B, G, R, A rows from the bottom, as OpenGL reads them), and at 9_3 compared with the one the
 * PC's own level drew of the same scene (written before, by the run without the flag): every pixel the same, each channel
 * within 2 -- ANGLE's own uber shader there, the ESSL 1.00 shaders here.
 */
static void gl1_picture(gl1_run *r)
{
    u8 rgba[GL1_SIZE * GL1_SIZE * 4], file[54 + sizeof(rgba)], own[54 + sizeof(rgba)];
    char path[700], what[200];
    FILE *out, *in;
    int i, differ = 0, worst = 0, at = -1;
    u32 v;
    if (!r->pictures) return;
    r->f->PixelStorei(GL_PACK_ALIGNMENT_, 1);
    r->f->ReadPixels(0, 0, GL1_SIZE, GL1_SIZE, GL_RGBA_, GL_UNSIGNED_BYTE_, rgba);
    memset(file, 0, 54);
    file[0] = 'B';
    file[1] = 'M';
    v = (u32)sizeof(file);
    memcpy(file + 2, &v, 4);
    v = 54;
    memcpy(file + 10, &v, 4);
    v = 40;
    memcpy(file + 14, &v, 4);
    v = GL1_SIZE;
    memcpy(file + 18, &v, 4);
    memcpy(file + 22, &v, 4);
    file[26] = 1;
    file[28] = 32;
    for (i = 0; i < GL1_SIZE * GL1_SIZE; i++) {
        file[54 + i * 4] = rgba[i * 4 + 2];
        file[54 + i * 4 + 1] = rgba[i * 4 + 1];
        file[54 + i * 4 + 2] = rgba[i * 4];
        file[54 + i * 4 + 3] = rgba[i * 4 + 3];
    }
    snprintf(path, sizeof(path), "%s/gles1-%s-%s.bmp", r->pictures, r->level, r->scene);
    out = fopen(path, "wb");
    if (out) {
        fwrite(file, 1, sizeof(file), out);
        fclose(out);
    }
    if (!r->compare) return;
    snprintf(path, sizeof(path), "%s/gles1-own-%s.bmp", r->pictures, r->scene);
    in = fopen(path, "rb");
    if (!in) return;
    i = (int)fread(own, 1, sizeof(own), in);
    fclose(in);
    r->compared++;
    if (i != (int)sizeof(own)) {
        gl1_check(r, false, "the PC's own level's picture of this scene is not one of ours");
        return;
    }
    for (i = 54; i < (int)sizeof(own); i++) {
        int d = (int)file[i] - (int)own[i];
        if (d < 0) d = -d;
        if (d > 2 && (i - 54) / 4 != at) {
            differ++;
            at = (i - 54) / 4;
        }
        if (d > worst) worst = d;
    }
    snprintf(what, sizeof(what), "%d of its %d pixels differ from the PC's own level's (by %d at most)", differ, GL1_SIZE * GL1_SIZE, worst);
    gl1_check(r, differ == 0, what);
}

/* A scene begun: the background cleared, the projection a pixel to a unit with y up (glLoadMatrixf), the modelview none. */
static void gl1_begin(gl1_run *r, const char *scene)
{
    static const float ortho[16] = { 2.0f / GL1_SIZE, 0, 0, 0, 0, 2.0f / GL1_SIZE, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1 };
    const gl1 *f = r->f;
    snprintf(r->scene, sizeof(r->scene), "%s", scene);
    while (f->GetError()) {}
    f->Viewport(0, 0, GL1_SIZE, GL1_SIZE);
    f->ClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    f->Clear(GL_COLOR_BUFFER_BIT_ | GL_DEPTH_BUFFER_BIT_);
    f->MatrixMode(GL_PROJECTION_);
    f->LoadMatrixf(ortho);
    f->MatrixMode(GL_MODELVIEW_);
    f->LoadIdentity();
}

/* A scene ended: no error, its picture written, the state it set back as it was. */
static void gl1_end(gl1_run *r)
{
    const gl1 *f = r->f;
    u32 e = f->GetError();
    char what[80];
    snprintf(what, sizeof(what), "glGetError is 0x%x", (unsigned)e);
    gl1_check(r, e == GL_NO_ERROR_, what);
    gl1_picture(r);
    f->DisableClientState(GL_VERTEX_ARRAY_);
    f->DisableClientState(GL_COLOR_ARRAY_);
    f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
    f->DisableClientState(GL_NORMAL_ARRAY_);
    f->Disable(GL_TEXTURE_2D_);
    f->Disable(GL_BLEND_);
    f->Disable(GL_ALPHA_TEST_);
    f->Disable(GL_LIGHTING_);
    f->Disable(GL_FOG_);
    f->Disable(GL_DEPTH_TEST_);
    f->Disable(GL_CLIP_PLANE0_);
    f->Disable(GL_COLOR_MATERIAL_);
    f->Disable(GL_NORMALIZE_);
    f->Disable(GL_RESCALE_NORMAL_);
    f->Disable(GL_POINT_SPRITE_OES_);
    f->ShadeModel(GL_SMOOTH_);
    f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_MODULATE_);
    f->BindBuffer(GL_ARRAY_BUFFER_, 0);
    f->BindBuffer(GL_ELEMENT_ARRAY_BUFFER_, 0);
    f->Color4f(1, 1, 1, 1);
}

static const u32 RED = 0xFF0000FFu, GREEN = 0x00FF00FFu, BLUE = 0x0000FFFFu, YELLOW = 0xFFFF00FFu, BACKGROUND = 0x4080BFFFu;

/* A texture of NEAREST texels, clamped. */
static u32 gl1_texture(const gl1 *f, int width, int height, u32 format, const u8 *texels)
{
    u32 t = 0;
    f->GenTextures(1, &t);
    f->BindTexture(GL_TEXTURE_2D_, t);
    f->PixelStorei(GL_UNPACK_ALIGNMENT_, 1);
    f->TexImage2D(GL_TEXTURE_2D_, 0, (i32)format, width, height, 0, format, GL_UNSIGNED_BYTE_, texels);
    f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
    f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
    f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_S_, GL_CLAMP_TO_EDGE_);
    f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_WRAP_T_, GL_CLAMP_TO_EDGE_);
    return t;
}

/* The map's four-coloured tile: red, green (bottom row), blue, yellow (top row). */
static const u8 k_tile[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255 };

static void gl1_scenes(gl1_run *r)
{
    const gl1 *f = r->f;
    /* A quad of x0,y0 to x1,y1 as a strip, with texture coordinates 0..1 over it. */
    float quad[8], uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
#define QUAD(x0, y0, x1, y1) (quad[0] = (x0), quad[1] = (y0), quad[2] = (x1), quad[3] = (y0), quad[4] = (x0), quad[5] = (y1), quad[6] = (x1), quad[7] = (y1))
    u32 tile = gl1_texture(f, 2, 2, GL_RGBA_, k_tile), alpha, half, buffers[2] = { 0, 0 };

    /* The map's tile: client arrays of positions, texture coordinates and colours (white and grey), the modelview multiplied
       by a move (glMultMatrixf), the texture modulated by the vertex colour. */
    {
        static const float move[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 4, 4, 0, 1 };
        static const u8 white[16] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
        gl1_begin(r, "tile");
        f->MultMatrixf(move);
        QUAD(4, 4, 36, 36);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tile);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_MODULATE_);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, 0, white);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 16, 16, RED);
        gl1_expect(r, 32, 16, GREEN);
        gl1_expect(r, 16, 32, BLUE);
        gl1_expect(r, 32, 32, YELLOW);
        gl1_expect(r, 50, 50, BACKGROUND);
        gl1_end(r);
    }
    {
        static const u8 grey[16] = { 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255 };
        gl1_begin(r, "tile-modulated");
        QUAD(8, 8, 40, 40);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tile);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, 0, grey);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 16, 16, 0x800000FFu);
        gl1_expect(r, 32, 32, 0x808000FFu);
        gl1_end(r);
    }

    /* Roads: positions and colours interleaved in a buffer object, triangles indexed from another (glDrawElements). */
    {
        static const struct { float x, y; u8 rgba[4]; } road[] = {
            { 4, 40, { 255, 0, 255, 255 } }, { 60, 40, { 255, 0, 255, 255 } }, { 4, 48, { 255, 0, 255, 255 } }, { 60, 48, { 255, 0, 255, 255 } },
            { 28, 4, { 255, 128, 0, 255 } }, { 36, 4, { 255, 128, 0, 255 } }, { 28, 60, { 255, 128, 0, 255 } }, { 36, 60, { 255, 128, 0, 255 } },
        };
        static const u16 indices[] = { 0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7 };
        gl1_begin(r, "roads");
        f->GenBuffers(2, buffers);
        f->BindBuffer(GL_ARRAY_BUFFER_, buffers[0]);
        f->BufferData(GL_ARRAY_BUFFER_, (intptr_t)sizeof(road), road, GL_STATIC_DRAW_);
        f->BindBuffer(GL_ELEMENT_ARRAY_BUFFER_, buffers[1]);
        f->BufferData(GL_ELEMENT_ARRAY_BUFFER_, (intptr_t)sizeof(indices), indices, GL_STATIC_DRAW_);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, (i32)sizeof(road[0]), (const void *)0);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, (i32)sizeof(road[0]), (const void *)8);
        f->DrawElements(GL_TRIANGLES_, 12, GL_UNSIGNED_SHORT_, (const void *)0);
        gl1_expect(r, 10, 44, 0xFF00FFFFu);
        gl1_expect(r, 32, 20, 0xFF8000FFu);
        gl1_expect(r, 10, 20, BACKGROUND);
        gl1_end(r);
    }

    /* The modelview pushed, moved, turned a quarter and scaled, popped: a 4x2 quad becomes x 46..48, y 40..48. */
    {
        static const float square[8] = { 0, 0, 4, 0, 0, 2, 4, 2 };
        gl1_begin(r, "matrices");
        f->PushMatrix();
        f->Translatef(48, 40, 0);
        f->Rotatef(90, 0, 0, 1);
        f->Scalef(2, 1, 1);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, square);
        f->Color4f(0, 1, 1, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        f->PopMatrix();
        f->Color4f(1, 0, 0, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 47, 44, 0x00FFFFFFu);
        gl1_expect(r, 49, 44, BACKGROUND);
        gl1_expect(r, 45, 44, BACKGROUND);
        gl1_expect(r, 2, 1, RED);
        gl1_end(r);
    }

    /* Labels: a glyph's coverage as an alpha texture, modulated by the label's colour and blended over the map; and the same
       cut by the alpha test instead. */
    {
        static const u8 coverage[2] = { 255, 0 };
        alpha = gl1_texture(f, 2, 1, GL_ALPHA_, coverage);
        gl1_begin(r, "label-blend");
        QUAD(8, 8, 40, 24);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, alpha);
        f->Enable(GL_BLEND_);
        f->BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->Color4f(1, 0, 0, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 16, 16, RED);
        gl1_expect(r, 32, 16, BACKGROUND);
        gl1_end(r);

        gl1_begin(r, "label-alpha-test");
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, alpha);
        f->Enable(GL_ALPHA_TEST_);
        f->AlphaFunc(GL_GREATER_, 0.5f);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->Color4f(0, 1, 0, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 16, 16, GREEN);
        gl1_expect(r, 32, 16, BACKGROUND);
        gl1_end(r);
    }

    /* A texture of white at half alpha replacing the colour (GL_REPLACE), blended over the background. */
    {
        static const u8 white_half[4] = { 255, 255, 255, 128 };
        half = gl1_texture(f, 1, 1, GL_RGBA_, white_half);
        gl1_begin(r, "replace-blend");
        QUAD(8, 8, 56, 56);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, half);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_REPLACE_);
        f->Enable(GL_BLEND_);
        f->BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->Color4f(0, 0, 0, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        /* 255 a + background (1 - a), a = 128/255; alpha 255 (a a + 1 - a). */
        gl1_expect(r, 32, 32, 0xA0C0DFBFu);
        gl1_end(r);
    }

    /* Lit: one light from the viewer, the material's diffuse 0.5, no ambient light: half grey. */
    {
        static const float dir[4] = { 0, 0, 1, 0 }, white[4] = { 1, 1, 1, 1 }, none[4] = { 0, 0, 0, 1 }, half_grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
        gl1_begin(r, "lit");
        QUAD(8, 8, 56, 56);
        f->Enable(GL_LIGHTING_);
        f->Enable(GL_LIGHT0_);
        f->Lightfv(GL_LIGHT0_, GL_POSITION_, dir);
        f->Lightfv(GL_LIGHT0_, GL_DIFFUSE_, white);
        f->Lightfv(GL_LIGHT0_, GL_AMBIENT_, none);
        f->Lightfv(GL_LIGHT0_, GL_SPECULAR_, none);
        f->LightModelfv(GL_LIGHT_MODEL_AMBIENT_, none);
        f->Materialfv(GL_FRONT_AND_BACK_, GL_DIFFUSE_, half_grey);
        f->Materialfv(GL_FRONT_AND_BACK_, GL_AMBIENT_, none);
        f->Normal3f(0, 0, 1);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 32, 32, 0x808080FFu);
        f->Disable(GL_LIGHT0_);
        gl1_end(r);
    }

    /* The combiner: the texture modulated by the environment's colour (GL_COMBINE, GL_MODULATE of GL_TEXTURE and GL_CONSTANT),
       its alpha the vertex colour's. */
    {
        static const float env[4] = { 0.5f, 1, 0.25f, 1 };
        gl1_begin(r, "combine");
        QUAD(8, 8, 40, 40);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tile);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_COMBINE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_COMBINE_RGB_, GL_MODULATE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC0_RGB_, GL_TEXTURE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND0_RGB_, GL_SRC_COLOR_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC1_RGB_, GL_CONSTANT_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND1_RGB_, GL_SRC_COLOR_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_COMBINE_ALPHA_, GL_REPLACE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC0_ALPHA_, GL_PRIMARY_COLOR_);
        f->TexEnvfv(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_COLOR_, env);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
        f->Color4f(1, 1, 1, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 16, 16, 0x800000FFu);
        gl1_expect(r, 32, 16, 0x00FF00FFu);
        gl1_expect(r, 32, 32, 0x80FF00FFu);
        gl1_end(r);
    }

    /* Fog, linear, from 0 to 2 in the eye's depth: the quad at depth 1 (z -1) takes half of the fog's colour. */
    {
        static const float fog_color[4] = { 1, 1, 1, 1 };
        static const float at_depth[12] = { 8, 8, -1, 56, 8, -1, 8, 56, -1, 56, 56, -1 };
        static const float far_ortho[16] = { 2.0f / GL1_SIZE, 0, 0, 0, 0, 2.0f / GL1_SIZE, 0, 0, 0, 0, -0.5f, 0, -1, -1, 0, 1 };
        gl1_begin(r, "fog");
        f->MatrixMode(GL_PROJECTION_);
        f->LoadMatrixf(far_ortho);
        f->MatrixMode(GL_MODELVIEW_);
        f->Enable(GL_FOG_);
        f->Fogf(GL_FOG_MODE_, (float)GL_LINEAR_);
        f->Fogf(GL_FOG_START_, 0);
        f->Fogf(GL_FOG_END_, 2);
        f->Fogfv(GL_FOG_COLOR_, fog_color);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->VertexPointer(3, GL_FLOAT_, 0, at_depth);
        f->Color4f(0, 0, 0, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 32, 32, 0x808080FFu);
        gl1_end(r);
    }

    /* glDrawTexOES: the tile drawn as a 16x16 rectangle at 40,40, its crop the whole texture. */
    {
        static const i32 crop[4] = { 0, 0, 2, 2 };
        gl1_begin(r, "draw-texture");
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tile);
        f->TexParameteriv(GL_TEXTURE_2D_, GL_TEXTURE_CROP_RECT_OES_, crop);
        f->Color4f(1, 1, 1, 1);
        f->DrawTexiOES(40, 40, 0, 16, 16);
        gl1_expect(r, 44, 44, RED);
        gl1_expect(r, 52, 44, GREEN);
        gl1_expect(r, 44, 52, BLUE);
        gl1_expect(r, 52, 52, YELLOW);
        gl1_expect(r, 36, 36, BACKGROUND);
        gl1_end(r);
    }

    /* The eight lights at once. A program feature level 9_3 refuses must have its draws refused, each time, and the next state
       draw: where ANGLE ended the app at the second. Drawn, it is white. */
    {
        static const float dir[4] = { 0, 0, 1, 0 }, white[4] = { 1, 1, 1, 1 }, none[4] = { 0, 0, 0, 1 };
        u32 first, second;
        int i;
        gl1_begin(r, "eight-lights");
        QUAD(8, 8, 56, 56);
        f->Enable(GL_LIGHTING_);
        for (i = 0; i < 8; i++) {
            f->Enable(GL_LIGHT0_ + (u32)i);
            f->Lightfv(GL_LIGHT0_ + (u32)i, GL_POSITION_, dir);
            f->Lightfv(GL_LIGHT0_ + (u32)i, GL_DIFFUSE_, white);
            f->Lightfv(GL_LIGHT0_ + (u32)i, GL_AMBIENT_, none);
        }
        f->Materialfv(GL_FRONT_AND_BACK_, GL_DIFFUSE_, white);
        f->Normal3f(0, 0, 1);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, quad);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        first = f->GetError();
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        second = f->GetError();
        if (first == GL_NO_ERROR_) {
            gl1_say(r, "  eight lights: drawn");
            gl1_check(r, second == GL_NO_ERROR_, "the second draw of eight lights failed where the first did not");
            gl1_expect(r, 32, 32, 0xFFFFFFFFu);
        } else {
            gl1_say(r, "  eight lights: refused (0x%x, then 0x%x), as this level's vertex programs are short", (unsigned)first, (unsigned)second);
            gl1_check(r, first == GL_INVALID_OPERATION_ && second == GL_INVALID_OPERATION_, "a refused state's draws are not refused with GL_INVALID_OPERATION");
        }
        for (i = 0; i < 8; i++) f->Disable(GL_LIGHT0_ + (u32)i);
        f->Disable(GL_LIGHTING_);
        f->Color4f(0, 0, 1, 1);
        f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        gl1_expect(r, 32, 32, BLUE);
        gl1_end(r);
    }
#undef QUAD
    f->DeleteBuffers(2, buffers);
    f->DeleteTextures(1, &tile);
    f->DeleteTextures(1, &alpha);
    f->DeleteTextures(1, &half);
}

/* A quad of x0,y0 to x1,y1 drawn as a strip, with texture coordinates 0..1 over it on unit 0 (and reversed on unit 1). */
static void gl1_quad(const gl1 *f, float x0, float y0, float x1, float y1)
{
    float quad[8], uv[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    quad[0] = x0, quad[1] = y0, quad[2] = x1, quad[3] = y0, quad[4] = x0, quad[5] = y1, quad[6] = x1, quad[7] = y1;
    f->EnableClientState(GL_VERTEX_ARRAY_);
    f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
    f->VertexPointer(2, GL_FLOAT_, 0, quad);
    f->TexCoordPointer(2, GL_FLOAT_, 0, uv);
    f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
}

/*
 * What a picture alone can tell between the two levels, each state a program of its own: every texture environment for every
 * base format, the combiner's every function, two texture units, a cube map, fog of each kind, a clip plane, lights of each
 * kind with the material from the vertex colours, normals scaled and normalised, points and point sprites, flat shading,
 * fixed-point and short arrays with a fan of byte indices, lines, mipmaps made by GL_GENERATE_MIPMAP, the texture matrix. At
 * 9_3 each picture is compared with the PC's own level's (gl1_picture); a few have pixels checked here as well.
 */
static void gl1_variants(gl1_run *r)
{
    static const u8 rgb[12] = { 200, 100, 50, 50, 200, 100, 100, 50, 200, 250, 250, 250 };
    static const u8 rgba[16] = { 200, 100, 50, 255, 50, 200, 100, 128, 100, 50, 200, 64, 250, 250, 250, 0 };
    static const u8 lum[4] = { 200, 100, 50, 250 }, lum_alpha[8] = { 200, 255, 100, 128, 50, 64, 250, 0 }, alpha[4] = { 255, 128, 64, 0 };
    static const u32 formats[5] = { GL_RGB_, GL_RGBA_, GL_LUMINANCE_, GL_LUMINANCE_ALPHA_, GL_ALPHA_ };
    static const struct { u32 mode; const char *name; } envs[5] = {
        { GL_MODULATE_, "env-modulate" }, { GL_REPLACE_, "env-replace" }, { GL_DECAL_, "env-decal" }, { GL_BLEND_, "env-blend" }, { GL_ADD_, "env-add" },
    };
    static const float env_color[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    const gl1 *f = r->f;
    const u8 *texels[5] = { rgb, rgba, lum, lum_alpha, alpha };
    u32 tex[5], cube = 0, mip = 0;
    int i, m;
    for (i = 0; i < 5; i++) tex[i] = gl1_texture(f, 2, 2, formats[i], texels[i]);

    /* The texture environments, each over the five base formats, blended. */
    for (m = 0; m < 5; m++) {
        gl1_begin(r, envs[m].name);
        f->Enable(GL_TEXTURE_2D_);
        f->Enable(GL_BLEND_);
        f->BlendFunc(GL_SRC_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, (i32)envs[m].mode);
        f->TexEnvfv(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_COLOR_, env_color);
        f->Color4f(0.5f, 0.75f, 1.0f, 0.5f);
        for (i = 0; i < 5; i++) {
            f->BindTexture(GL_TEXTURE_2D_, tex[i]);
            gl1_quad(f, 2.0f + 12 * i, 8, 12.0f + 12 * i, 56);
        }
        gl1_end(r);
    }

    /* The combiner: each function of its colour, and of its alpha, of sources and operands of each kind, scaled by 1 and 2. */
    {
        static const u32 rgb_fn[8] = { GL_REPLACE_, GL_MODULATE_, GL_ADD_, GL_ADD_SIGNED_, GL_INTERPOLATE_, GL_SUBTRACT_, GL_DOT3_RGB_, GL_DOT3_RGBA_ };
        static const u32 alpha_fn[8] = { GL_REPLACE_, GL_MODULATE_, GL_ADD_, GL_ADD_SIGNED_, GL_INTERPOLATE_, GL_SUBTRACT_, GL_REPLACE_, GL_MODULATE_ };
        static const float env[4] = { 0.25f, 0.5f, 0.75f, 0.6f };
        gl1_begin(r, "combine-each");
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[1]);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_COMBINE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC0_RGB_, GL_TEXTURE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND0_RGB_, GL_SRC_COLOR_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC1_RGB_, GL_CONSTANT_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND1_RGB_, GL_ONE_MINUS_SRC_COLOR_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC2_RGB_, GL_PRIMARY_COLOR_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND2_RGB_, GL_SRC_ALPHA_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC0_ALPHA_, GL_TEXTURE_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND0_ALPHA_, GL_SRC_ALPHA_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC1_ALPHA_, GL_PREVIOUS_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND1_ALPHA_, GL_ONE_MINUS_SRC_ALPHA_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_SRC2_ALPHA_, GL_CONSTANT_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_OPERAND2_ALPHA_, GL_SRC_ALPHA_);
        f->TexEnvfv(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_COLOR_, env);
        f->Color4f(0.9f, 0.6f, 0.3f, 0.7f);
        for (i = 0; i < 8; i++) {
            float x = 2.0f + 15 * (i % 4), y = i < 4 ? 4.0f : 34.0f;
            f->TexEnvi(GL_TEXTURE_ENV_, GL_COMBINE_RGB_, (i32)rgb_fn[i]);
            f->TexEnvi(GL_TEXTURE_ENV_, GL_COMBINE_ALPHA_, (i32)alpha_fn[i]);
            f->TexEnvf(GL_TEXTURE_ENV_, GL_RGB_SCALE_, i % 2 ? 2.0f : 1.0f);
            gl1_quad(f, x, y, x + 13, y + 26);
        }
        f->TexEnvf(GL_TEXTURE_ENV_, GL_RGB_SCALE_, 1.0f);
        gl1_end(r);
    }

    /* Two texture units: the colours modulated on unit 0, luminance added on unit 1, its coordinates reversed. */
    {
        static const float reversed[8] = { 1, 1, 0, 1, 1, 0, 0, 0 };
        gl1_begin(r, "two-units");
        f->ActiveTexture(GL_TEXTURE1_);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[2]);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_ADD_);
        f->ClientActiveTexture(GL_TEXTURE1_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->TexCoordPointer(2, GL_FLOAT_, 0, reversed);
        f->ClientActiveTexture(GL_TEXTURE0_);
        f->ActiveTexture(GL_TEXTURE0_);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[0]);
        f->Color4f(0.5f, 0.5f, 0.5f, 1);
        gl1_quad(f, 8, 8, 56, 56);
        f->ActiveTexture(GL_TEXTURE1_);
        f->Disable(GL_TEXTURE_2D_);
        f->TexEnvi(GL_TEXTURE_ENV_, GL_TEXTURE_ENV_MODE_, GL_MODULATE_);
        f->ClientActiveTexture(GL_TEXTURE1_);
        f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->ClientActiveTexture(GL_TEXTURE0_);
        f->ActiveTexture(GL_TEXTURE0_);
        gl1_end(r);
    }

    /* A cube map, its six faces of six colours, looked up along each axis. */
    {
        static const u8 faces[6][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 } };
        static const float dirs[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        gl1_begin(r, "cube-map");
        f->GenTextures(1, &cube);
        f->BindTexture(GL_TEXTURE_CUBE_MAP_, cube);
        for (i = 0; i < 6; i++) f->TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + (u32)i, 0, GL_RGBA_, 1, 1, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, faces[i]);
        f->TexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MIN_FILTER_, GL_NEAREST_);
        f->TexParameteri(GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
        f->Enable(GL_TEXTURE_CUBE_MAP_);
        for (i = 0; i < 6; i++) {
            float quad[8], dir[12];
            int k;
            quad[0] = 2.0f + 10 * i, quad[1] = 8, quad[2] = 10.0f + 10 * i, quad[3] = 8;
            quad[4] = 2.0f + 10 * i, quad[5] = 56, quad[6] = 10.0f + 10 * i, quad[7] = 56;
            for (k = 0; k < 4; k++) memcpy(dir + 3 * k, dirs[i], sizeof(dirs[i]));
            f->EnableClientState(GL_VERTEX_ARRAY_);
            f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
            f->VertexPointer(2, GL_FLOAT_, 0, quad);
            f->TexCoordPointer(3, GL_FLOAT_, 0, dir);
            f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        }
        f->Disable(GL_TEXTURE_CUBE_MAP_);
        gl1_end(r);
    }

    /* Fog, exponential and squared, over squares at depths 1 to 4 in front of the eye. */
    for (m = 0; m < 2; m++) {
        static const float fog_color[4] = { 1, 1, 1, 1 };
        static const float far_ortho[16] = { 2.0f / GL1_SIZE, 0, 0, 0, 0, 2.0f / GL1_SIZE, 0, 0, 0, 0, -0.2f, 0, -1, -1, 0, 1 };
        gl1_begin(r, m ? "fog-exp2" : "fog-exp");
        f->MatrixMode(GL_PROJECTION_);
        f->LoadMatrixf(far_ortho);
        f->MatrixMode(GL_MODELVIEW_);
        f->Enable(GL_FOG_);
        f->Fogf(GL_FOG_MODE_, (float)(m ? GL_EXP2_ : GL_EXP_));
        f->Fogf(GL_FOG_DENSITY_, 0.5f);
        f->Fogfv(GL_FOG_COLOR_, fog_color);
        f->Color4f(0, 0, 0, 1);
        f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
        for (i = 0; i < 4; i++) {
            float z = -1.0f - (float)i, square[12];
            square[0] = 4.0f + 15 * i, square[1] = 8, square[2] = z, square[3] = 16.0f + 15 * i, square[4] = 8, square[5] = z;
            square[6] = 4.0f + 15 * i, square[7] = 56, square[8] = z, square[9] = 16.0f + 15 * i, square[10] = 56, square[11] = z;
            f->EnableClientState(GL_VERTEX_ARRAY_);
            f->VertexPointer(3, GL_FLOAT_, 0, square);
            f->DrawArrays(GL_TRIANGLE_STRIP_, 0, 4);
        }
        /* exp(-0.5) of black and the rest of white; exp(-0.25) for the squared. */
        gl1_expect(r, 10, 32, m ? 0x383838FFu : 0x646464FFu);
        gl1_end(r);
    }

    /* A clip plane: x >= 32 kept. */
    {
        static const float plane[4] = { 1, 0, 0, -32 };
        gl1_begin(r, "clip-plane");
        f->ClipPlanef(GL_CLIP_PLANE0_, plane);
        f->Enable(GL_CLIP_PLANE0_);
        f->Color4f(0, 1, 0, 1);
        f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
        gl1_quad(f, 8, 8, 56, 56);
        gl1_expect(r, 16, 32, BACKGROUND);
        gl1_expect(r, 48, 32, GREEN);
        gl1_end(r);
    }

    /* Lights: a point light fading with distance and a spot light with a specular highlight, the material's colours the
       vertices' (GL_COLOR_MATERIAL), the normals from an array through a modelview scaled in z: normalised in the bottom
       half, rescaled in the top. */
    {
        static const float point_at[4] = { 32, 32, 20, 1 }, spot_at[4] = { 16, 48, 30, 1 }, down[3] = { 0, 0, -1 };
        static const float warm[4] = { 1, 0.5f, 0.5f, 1 }, cool[4] = { 0.5f, 0.5f, 1, 1 }, white[4] = { 1, 1, 1, 1 }, none[4] = { 0, 0, 0, 1 };
        static const float normals[12] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
        static const u8 colors[16] = { 255, 255, 255, 255, 255, 128, 0, 255, 0, 128, 255, 255, 128, 255, 128, 255 };
        gl1_begin(r, "lights");
        f->Enable(GL_LIGHTING_);
        f->Enable(GL_LIGHT0_);
        f->Enable(GL_LIGHT0_ + 1);
        f->Lightfv(GL_LIGHT0_, GL_POSITION_, point_at);
        f->Lightfv(GL_LIGHT0_, GL_DIFFUSE_, warm);
        f->Lightfv(GL_LIGHT0_, GL_SPECULAR_, none);
        f->Lightf(GL_LIGHT0_, GL_CONSTANT_ATTENUATION_, 1);
        f->Lightf(GL_LIGHT0_, GL_LINEAR_ATTENUATION_, 0.02f);
        f->Lightfv(GL_LIGHT0_ + 1, GL_POSITION_, spot_at);
        f->Lightfv(GL_LIGHT0_ + 1, GL_SPOT_DIRECTION_, down);
        f->Lightf(GL_LIGHT0_ + 1, GL_SPOT_CUTOFF_, 45);
        f->Lightf(GL_LIGHT0_ + 1, GL_SPOT_EXPONENT_, 4);
        f->Lightfv(GL_LIGHT0_ + 1, GL_DIFFUSE_, cool);
        f->Lightfv(GL_LIGHT0_ + 1, GL_SPECULAR_, white);
        f->Materialfv(GL_FRONT_AND_BACK_, GL_SPECULAR_, white);
        f->Materialf(GL_FRONT_AND_BACK_, GL_SHININESS_, 20);
        f->Enable(GL_COLOR_MATERIAL_);
        f->EnableClientState(GL_NORMAL_ARRAY_);
        f->NormalPointer(GL_FLOAT_, 0, normals);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, 0, colors);
        f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->PushMatrix();
        f->Scalef(1, 1, 2);
        f->Enable(GL_NORMALIZE_);
        gl1_quad(f, 4, 4, 60, 30);
        f->Disable(GL_NORMALIZE_);
        f->Enable(GL_RESCALE_NORMAL_);
        gl1_quad(f, 4, 34, 60, 60);
        f->PopMatrix();
        f->Disable(GL_LIGHT0_ + 1);
        f->Disable(GL_LIGHT0_);
        f->Materialfv(GL_FRONT_AND_BACK_, GL_SPECULAR_, none);
        f->Materialf(GL_FRONT_AND_BACK_, GL_SHININESS_, 0);
        f->Lightf(GL_LIGHT0_, GL_LINEAR_ATTENUATION_, 0);
        gl1_end(r);
    }

    /* Points one pixel wide, and as point sprites of the tile's texture. */
    {
        static const float points[8] = { 10.5f, 10.5f, 20.5f, 10.5f, 30.5f, 10.5f, 40.5f, 10.5f };
        static const float sprites[8] = { 10.5f, 40.5f, 20.5f, 40.5f, 30.5f, 40.5f, 40.5f, 40.5f };
        gl1_begin(r, "points");
        f->PointSize(1);
        f->Color4f(1, 1, 0, 1);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->VertexPointer(2, GL_FLOAT_, 0, points);
        f->DrawArrays(GL_POINTS_, 0, 4);
        gl1_expect(r, 20, 10, YELLOW);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[1]);
        f->Enable(GL_POINT_SPRITE_OES_);
        f->TexEnvi(GL_POINT_SPRITE_OES_, GL_COORD_REPLACE_OES_, 1);
        f->Color4f(1, 1, 1, 1);
        f->VertexPointer(2, GL_FLOAT_, 0, sprites);
        f->DrawArrays(GL_POINTS_, 0, 4);
        f->TexEnvi(GL_POINT_SPRITE_OES_, GL_COORD_REPLACE_OES_, 0);
        gl1_end(r);
    }

    /* The flat shade model: the vertex colours alike, so smooth and flat give one picture. */
    {
        static const u8 colors[16] = { 0, 128, 255, 255, 0, 128, 255, 255, 0, 128, 255, 255, 0, 128, 255, 255 };
        gl1_begin(r, "flat");
        f->ShadeModel(GL_FLAT_);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, 0, colors);
        f->DisableClientState(GL_TEXTURE_COORD_ARRAY_);
        gl1_quad(f, 8, 8, 56, 56);
        gl1_expect(r, 32, 32, 0x0080FFFFu);
        gl1_end(r);
    }

    /* Fixed-point texture coordinates, short positions, a fan of byte indices. */
    {
        static const i16 corners[8] = { 8, 8, 40, 8, 40, 40, 8, 40 };
        static const i32 fixed_uv[8] = { 0, 0, 65536, 0, 65536, 65536, 0, 65536 };
        static const u8 fan[4] = { 0, 1, 2, 3 };
        gl1_begin(r, "fixed-short-fan");
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[1]);
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_TEXTURE_COORD_ARRAY_);
        f->VertexPointer(2, GL_SHORT_, 0, corners);
        f->TexCoordPointer(2, GL_FIXED_, 0, fixed_uv);
        f->DrawElements(GL_TRIANGLE_FAN_, 4, GL_UNSIGNED_BYTE_, fan);
        gl1_expect(r, 16, 16, 0xC86432FFu);
        gl1_expect(r, 32, 16, 0x32C86480u);
        gl1_end(r);
    }

    /* Lines: separate, a strip and a loop, coloured by their vertices. */
    {
        static const float lines[8] = { 4.5f, 4.5f, 59.5f, 4.5f, 4.5f, 8.5f, 59.5f, 30.5f };
        static const float strip[8] = { 4.5f, 40.5f, 20.5f, 56.5f, 36.5f, 40.5f, 52.5f, 56.5f };
        static const float loop[8] = { 30.5f, 16.5f, 50.5f, 16.5f, 50.5f, 30.5f, 30.5f, 30.5f };
        static const u8 colors[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        gl1_begin(r, "lines");
        f->EnableClientState(GL_VERTEX_ARRAY_);
        f->EnableClientState(GL_COLOR_ARRAY_);
        f->ColorPointer(4, GL_UNSIGNED_BYTE_, 0, colors);
        f->VertexPointer(2, GL_FLOAT_, 0, lines);
        f->DrawArrays(GL_LINES_, 0, 4);
        f->VertexPointer(2, GL_FLOAT_, 0, strip);
        f->DrawArrays(GL_LINE_STRIP_, 0, 4);
        f->VertexPointer(2, GL_FLOAT_, 0, loop);
        f->DrawArrays(GL_LINE_LOOP_, 0, 4);
        gl1_end(r);
    }

    /* Mipmaps made by GL_GENERATE_MIPMAP, a 4x4 texture drawn at 8x8, 2x2 and 1x1. */
    {
        u8 big[64];
        for (i = 0; i < 16; i++) {
            big[i * 4] = (u8)(i * 16), big[i * 4 + 1] = (u8)(255 - i * 16), big[i * 4 + 2] = (u8)(i % 4 * 64), big[i * 4 + 3] = 255;
        }
        gl1_begin(r, "mipmaps");
        f->GenTextures(1, &mip);
        f->BindTexture(GL_TEXTURE_2D_, mip);
        f->TexParameteri(GL_TEXTURE_2D_, GL_GENERATE_MIPMAP_, 1);
        f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MIN_FILTER_, GL_LINEAR_MIPMAP_NEAREST_);
        f->TexParameteri(GL_TEXTURE_2D_, GL_TEXTURE_MAG_FILTER_, GL_NEAREST_);
        f->TexImage2D(GL_TEXTURE_2D_, 0, GL_RGBA_, 4, 4, 0, GL_RGBA_, GL_UNSIGNED_BYTE_, big);
        f->Enable(GL_TEXTURE_2D_);
        gl1_quad(f, 8, 8, 16, 16);
        gl1_quad(f, 24, 8, 26, 10);
        gl1_quad(f, 32, 8, 33, 9);
        gl1_end(r);
    }

    /* The texture matrix: coordinates moved half the texture right, clamped. */
    {
        gl1_begin(r, "texture-matrix");
        f->MatrixMode(GL_TEXTURE_);
        f->LoadIdentity();
        f->Translatef(0.5f, 0, 0);
        f->MatrixMode(GL_MODELVIEW_);
        f->Enable(GL_TEXTURE_2D_);
        f->BindTexture(GL_TEXTURE_2D_, tex[1]);
        gl1_quad(f, 8, 8, 40, 40);
        f->MatrixMode(GL_TEXTURE_);
        f->LoadIdentity();
        f->MatrixMode(GL_MODELVIEW_);
        gl1_expect(r, 16, 16, 0x32C86480u);
        gl1_end(r);
    }

    f->DeleteTextures(5, tex);
    f->DeleteTextures(1, &cube);
    f->DeleteTextures(1, &mip);
}
/* scenes: end */

/* --------------------------------------------------------- through the core */

static void *host_gl(const char *name) { return mg_egl_host(name); }

int mg_angle_windows_run(const char *dir, const char *scratch, bool level_9_3, mg_test_sink sink, void *user, int *checks)
{
    enum { EGL_NONE_ = 0x3038, EGL_SURFACE_TYPE_ = 0x3033, EGL_PBUFFER_BIT_ = 1, EGL_RED_SIZE_ = 0x3024, EGL_GREEN_SIZE_ = 0x3023,
           EGL_BLUE_SIZE_ = 0x3022, EGL_ALPHA_SIZE_ = 0x3021, EGL_DEPTH_SIZE_ = 0x3025, EGL_WIDTH_ = 0x3057, EGL_HEIGHT_ = 0x3056,
           EGL_CONTEXT_CLIENT_VERSION_ = 0x3098 };
    typedef void *(*fn_get_display)(void *);
    typedef unsigned (*fn_initialize)(void *, int *, int *);
    typedef unsigned (*fn_choose)(void *, const int *, void **, int, int *);
    typedef void *(*fn_pbuffer)(void *, void *, const int *);
    typedef void *(*fn_create)(void *, void *, void *, const int *);
    typedef unsigned (*fn_make_current)(void *, void *, void *, void *);
    typedef unsigned (*fn_destroy)(void *, void *);
    typedef unsigned (*fn_query_context)(void *, void *, int, int *);
    static const int want[] = { EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_, EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8,
                                EGL_ALPHA_SIZE_, 8, EGL_DEPTH_SIZE_, 16, EGL_NONE_ };
    static const int size[] = { EGL_WIDTH_, GL1_SIZE, EGL_HEIGHT_, GL1_SIZE, EGL_NONE_ };
    static const int es1[] = { EGL_CONTEXT_CLIENT_VERSION_, 1, EGL_NONE_ };
    gl1_run r;
    gl1 f;
    mg_stat st;
    char path[700], flag[700];
    wchar_t wdir[600], full[600];
    void *d, *config = NULL, *pb = NULL, *ctx = NULL;
    int major = 0, minor = 0, n = 0, version = 0;
    const char *renderer, *gl_version;
    fn_get_display get_display;
    fn_initialize initialize;
    fn_choose choose;
    fn_pbuffer pbuffer;
    fn_create create;
    fn_make_current make_current;
    fn_destroy destroy_context, destroy_surface;
    fn_query_context query_context;
    memset(&r, 0, sizeof(r));
    r.f = &f;
    r.level = level_9_3 ? "9_3" : "own";
    r.pictures = scratch;
    r.compare = level_9_3;
    r.say = sink;
    r.user = user;
    snprintf(r.scene, sizeof(r.scene), "setting up");
    /* ANGLE from that folder: the core loads libGLESv2.dll and libEGL.dll by name (mg_egl.c, angle), as the package's. */
    snprintf(path, sizeof(path), "%s/libGLESv2.dll", dir);
    if (!mg_path_stat(path, &st, NULL)) {
        gl1_say(&r, "  FAIL no ANGLE in %s (python tools/build-angle.py --arch x64 makes one)", dir);
        *checks += 1;
        return 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, (int)(sizeof(wdir) / sizeof(wdir[0])));
    if (!GetFullPathNameW(wdir, (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL)) wcscpy(full, wdir);
    SetDllDirectoryW(full);
    get_display = (fn_get_display)mg_egl_host("eglGetDisplay");
    initialize = (fn_initialize)mg_egl_host("eglInitialize");
    choose = (fn_choose)mg_egl_host("eglChooseConfig");
    pbuffer = (fn_pbuffer)mg_egl_host("eglCreatePbufferSurface");
    create = (fn_create)mg_egl_host("eglCreateContext");
    make_current = (fn_make_current)mg_egl_host("eglMakeCurrent");
    destroy_context = (fn_destroy)mg_egl_host("eglDestroyContext");
    destroy_surface = (fn_destroy)mg_egl_host("eglDestroySurface");
    query_context = (fn_query_context)mg_egl_host("eglQueryContext");
    /* The flag egl-level-9_3 makes the display at feature level 9_3 whatever this PC's graphics processor has. */
    snprintf(flag, sizeof(flag), "%s/flags", scratch);
    mg_path_mkdirs(flag);
    snprintf(flag, sizeof(flag), "%s/flags/egl-level-9_3", scratch);
    if (level_9_3) mg_file_write_all(flag, "", 0);
    else mg_path_remove(flag, NULL);
    gl1_say(&r, "OpenGL ES 1 on ANGLE (%s), Direct3D 11 at %s", dir, level_9_3 ? "feature level 9_3 (the flag egl-level-9_3)" : "this PC's own feature level");
    if (!get_display || !initialize || !choose || !pbuffer || !create || !make_current || !destroy_context || !destroy_surface || !query_context
        || !gl1_resolve(&f, host_gl)) {
        gl1_check(&r, false, "a function of EGL's or OpenGL ES 1's is missing");
        *checks += r.checks;
        return r.failed;
    }
    d = get_display(NULL);
    gl1_check(&r, d && initialize(d, &major, &minor), "no display");
    gl1_check(&r, d && choose(d, want, &config, 1, &n) && n == 1 && config, "no configuration of 8888 with depth for a pbuffer");
    if (r.failed) goto done;
    pb = pbuffer(d, config, size);
    ctx = create(d, config, NULL, es1);
    gl1_check(&r, pb && ctx, "no pbuffer or no context of version 1");
    if (r.failed) goto done;
    query_context(d, ctx, EGL_CONTEXT_CLIENT_VERSION_, &version);
    gl1_say(&r, "  the context asked for as version 1 is of version %d", version);
    gl1_check(&r, version == 1, "the context asked for as version 1 is not: made of version 2, as where ANGLE does not draw version 1 "
                                "(an ANGLE built before angle/0004? python tools/build-angle.py --arch x64)");
    gl1_check(&r, make_current(d, pb, pb, ctx) != 0, "the context could not be made current");
    if (r.failed) goto done;
    renderer = (const char *)f.GetString(GL_RENDERER_);
    gl_version = (const char *)f.GetString(GL_VERSION_);
    gl1_say(&r, "  EGL %d.%d, context version %d: %s; %s", major, minor, version, gl_version ? gl_version : "(no version)",
            renderer ? renderer : "(no renderer)");
    /* (ANGLE says "OpenGL ES 1.1 (ANGLE ...)", Android's own "OpenGL ES-CM 1.1".) */
    gl1_check(&r, gl_version && (!strncmp(gl_version, "OpenGL ES 1.", 12) || !strncmp(gl_version, "OpenGL ES-CM 1.", 15)), "GL_VERSION is not OpenGL ES 1.x");
    if (level_9_3) gl1_check(&r, renderer && strstr(renderer, "level_9_3") != NULL, "ANGLE's renderer is not of feature level 9_3");
    else gl1_check(&r, renderer && strstr(renderer, "level_9") == NULL, "ANGLE's renderer is of feature level 9_x without the flag");
    gl1_scenes(&r);
    gl1_variants(&r);
    if (level_9_3)
        gl1_say(&r, "  %d of its pictures compared with the PC's own level's%s", r.compared,
                r.compared ? "" : " (there were none: run it without 9_3 first, into the same folder)");
done:
    if (ctx) make_current(d, NULL, NULL, NULL);
    if (ctx) destroy_context(d, ctx);
    if (pb) destroy_surface(d, pb);
    mg_path_remove(flag, NULL);
    gl1_say(&r, "  OpenGL ES 1 at %s: %d checks, %d failed (pictures: %s/gles1-%s-*.bmp)", r.level, r.checks, r.failed, scratch, r.level);
    *checks += r.checks;
    return r.failed;
}

#else

int mg_angle_windows_run(const char *dir, const char *scratch, bool level_9_3, mg_test_sink sink, void *user, int *checks)
{
    (void)dir;
    (void)scratch;
    (void)level_9_3;
    (void)checks;
    if (sink) sink("(ANGLE itself is not in this build of the tests: --angle-windows does nothing here; tools/build-tests.sh win runs it)", user);
    return 0;
}

#endif
