/*
 * libandroid's hardware buffers: AHardwareBuffer (hk_ahb.h says what one is here).
 *
 * Chromium's GPU service allocates a page's canvases and WebGL's drawing buffers as hardware buffers once the platform
 * says Android 8 or later, and makes EGL images of them (EGL_ANDROID_image_native_buffer) to draw into and to read from.
 * With none to be had it lost each such canvas's GPU context: a 2D canvas went on in software and WebGL was not offered.
 *
 * Formats: R8G8B8A8, R8G8B8X8, R8G8B8 and R5G6B5, of one layer, which OpenGL ES 2.0 has textures of; BLOB (memory for the
 * CPU, `width` bytes, height 1). Others, and protected content, cube maps and buffers of data for the graphics processor,
 * are refused with BAD_VALUE, as a gralloc that cannot make them refuses them; AHardwareBuffer_isSupported says which.
 * Several layers, or planes (AHardwareBuffer_lockPlanes), are not there.
 *
 * A buffer's memory for the CPU is made at its first lock, and its texture at the first EGL image of it (ndk/hk_egl.c,
 * hk_egl_ahb_*): a lock for reading reads the texture back first when the graphics processor may have drawn into it, an
 * unlock after writing uploads what was written. Fences are not kept: one given to a lock is closed, an unlock gives -1,
 * as a buffer that is ready at once has it. Between threads of one process a buffer goes through a socket as its address
 * (AHardwareBuffer_sendHandleToUnixSocket), with a reference taken for the one who receives it.
 */
#include <stdlib.h>
#include <string.h>

#include "../bionic/hk_bionic.h"
#include "../bionic/hk_fd.h"
#include "../host/hk_host.h"
#include "hk_ahb.h"
#include "hk_egl.h"

#define AHB_MAGIC 0x41484221u          /* "AHB!" */
#define ANWB_MAGIC 0x5f626672           /* ANDROID_NATIVE_BUFFER_MAGIC: '_', 'b', 'f', 'r' */

#define E_BAD_VALUE (-22)
#define E_NO_MEMORY (-12)
#define E_INVALID_OPERATION (-38)

enum {
    F_R8G8B8A8 = 1, F_R8G8B8X8 = 2, F_R8G8B8 = 3, F_R5G6B5 = 4, F_BLOB = 0x21,
};
#define U_CPU_READ_MASK 0xFull
#define U_CPU_WRITE_MASK 0xF0ull
#define U_GPU_FRAMEBUFFER 0x200ull
#define U_PROTECTED 0x4000ull
#define U_GPU_DATA_BUFFER 0x1000000ull
#define U_GPU_CUBE_MAP 0x2000000ull

/* AHardwareBuffer_Desc as 32-bit ARM lays it out: 40 bytes. */
typedef struct g_desc {
    u32 width, height, layers, format;
    u64 usage;
    u32 stride, rfu0;
    u64 rfu1;
} g_desc;

/* The buffers there are, to know one when given one. */
static hk_lock g_list_lock = HK_LOCK_INIT;
static hk_ahb **g_list;
static int g_count, g_cap;
static u64 g_next_id;

static u32 bpp_of(u32 format)
{
    switch (format) {
    case F_R8G8B8A8: case F_R8G8B8X8: return 4;
    case F_R8G8B8: return 3;
    case F_R5G6B5: return 2;
    case F_BLOB: return 1;
    default: return 0;
    }
}

/* Whether a description is one this layer makes buffers of. */
static bool supported(const g_desc *d)
{
    u32 bpp;
    if (!d || !d->width || !d->height || d->layers != 1) return false;
    if (d->usage & (U_PROTECTED | U_GPU_CUBE_MAP)) return false;
    if (!(bpp = bpp_of(d->format))) return false;
    if (d->format == F_BLOB) return d->height == 1;
    if (d->usage & U_GPU_DATA_BUFFER) return false;
    return d->width <= 16384 && d->height <= 16384;
}

bool hk_ahb_gl_format(const hk_ahb *b, u32 *format, u32 *type)
{
    enum { RGBA = 0x1908, RGB = 0x1907, UNSIGNED_BYTE = 0x1401, UNSIGNED_SHORT_5_6_5 = 0x8363 };
    switch (b->format) {
    case F_R8G8B8A8: case F_R8G8B8X8: *format = RGBA; *type = UNSIGNED_BYTE; return true;
    case F_R8G8B8: *format = RGB; *type = UNSIGNED_BYTE; return true;
    case F_R5G6B5: *format = RGB; *type = UNSIGNED_SHORT_5_6_5; return true;
    default: return false;
    }
}

static hk_ahb *find(const void *p)
{
    hk_ahb *b = NULL;
    int i;
    if (!p) return NULL;
    hk_lock_acquire(&g_list_lock);
    for (i = 0; i < g_count; i++)
        if ((const void *)g_list[i] == p) {
            b = g_list[i];
            break;
        }
    hk_lock_release(&g_list_lock);
    return b;
}

hk_ahb *hk_ahb_of(const void *ahardwarebuffer) { return find(ahardwarebuffer); }

hk_ahb *hk_ahb_of_client_buffer(const void *client_buffer)
{
    return client_buffer ? find((const u8 *)client_buffer - HK_AHB_ANWB_OFFSET) : NULL;
}

static void g_acquire(void *buffer)
{
    hk_ahb *b = find(buffer);
    if (b) hk_atomic_add(&b->refs, 1);
}

static void g_release(void *buffer)
{
    hk_ahb *b = find(buffer);
    int i;
    if (!b || hk_atomic_add(&b->refs, -1) > 0) return;
    hk_lock_acquire(&g_list_lock);
    for (i = 0; i < g_count; i++)
        if (g_list[i] == b) {
            g_list[i] = g_list[--g_count];
            break;
        }
    hk_lock_release(&g_list_lock);
    if (b->texture) hk_egl_ahb_forget(b);
    b->magic = 0;
    free(b->pixels);
    free(b);
}

#if HK_HOST_ARM32
/* android_native_base_t's incRef and decRef, for code that counts references through the ANativeWindowBuffer. */
static void anwb_inc(void *base) { g_acquire((u8 *)base - HK_AHB_ANWB_OFFSET); }
static void anwb_dec(void *base) { g_release((u8 *)base - HK_AHB_ANWB_OFFSET); }
#endif

static int g_allocate(const g_desc *d, void **out)
{
    hk_ahb *b;
    if (!out) return E_BAD_VALUE;
    *out = NULL;
    if (!supported(d)) {
        hk_note_once("AHardwareBuffer_allocate: %ux%u of %u layers, format %u, usage 0x%llx is not one made here; BAD_VALUE",
                     d ? d->width : 0, d ? d->height : 0, d ? d->layers : 0, d ? d->format : 0, d ? (unsigned long long)d->usage : 0ull);
        return E_BAD_VALUE;
    }
    b = (hk_ahb *)calloc(1, sizeof(*b));
    if (!b) return E_NO_MEMORY;
    b->anwb.magic = ANWB_MAGIC;
    b->anwb.version = (i32)sizeof(hk_ahb_anwb);
#if HK_HOST_ARM32
    b->anwb.inc_ref = (u32)(hk_addr)anwb_inc;
    b->anwb.dec_ref = (u32)(hk_addr)anwb_dec;
#endif
    b->anwb.width = (i32)d->width;
    b->anwb.height = (i32)d->height;
    b->anwb.stride = (i32)d->width;
    b->anwb.format = (i32)d->format;
    b->anwb.usage_deprecated = (i32)(u32)d->usage;
    b->anwb.layer_count = 1;
    b->anwb.usage_lo = (u32)d->usage;
    b->anwb.usage_hi = (u32)(d->usage >> 32);
    b->magic = AHB_MAGIC;
    b->refs = 1;
    b->width = d->width;
    b->height = d->height;
    b->format = d->format;
    b->bpp = bpp_of(d->format);
    b->usage = d->usage;
    hk_lock_init(&b->lock);
    hk_lock_acquire(&g_list_lock);
    if (g_count == g_cap) {
        int cap = g_cap ? g_cap * 2 : 64;
        hk_ahb **list = (hk_ahb **)realloc(g_list, sizeof(*list) * (size_t)cap);
        if (!list) {
            hk_lock_release(&g_list_lock);
            free(b);
            return E_NO_MEMORY;
        }
        g_list = list;
        g_cap = cap;
    }
    b->id = ++g_next_id;
    g_list[g_count++] = b;
    hk_lock_release(&g_list_lock);
    hk_note_once("AHardwareBuffer_allocate: the first buffer: %ux%u, format %u, usage 0x%llx", d->width, d->height, d->format,
                 (unsigned long long)d->usage);
    *out = b;
    return 0;
}

static int g_is_supported(const g_desc *d) { return supported(d) ? 1 : 0; }

static void g_describe(const void *buffer, g_desc *out)
{
    hk_ahb *b = find(buffer);
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!b) return;
    out->width = b->width;
    out->height = b->height;
    out->layers = 1;
    out->format = b->format;
    out->usage = b->usage;
    out->stride = b->width;
}

/* The CPU's copy, read back first when the graphics processor may have drawn into the texture and the lock reads. */
static int lock_common(void *buffer, u64 usage, i32 fence, void **out)
{
    hk_ahb *b = find(buffer);
    if (out) *out = NULL;
    if (fence >= 0) hk_fd_close(fence);
    if (!b || !out) return E_BAD_VALUE;
    if (!(usage & (U_CPU_READ_MASK | U_CPU_WRITE_MASK))) return E_BAD_VALUE;
    hk_lock_acquire(&b->lock);
    if (!b->pixels) {
        b->pixels = (u8 *)calloc((size_t)b->width * b->height, b->bpp);
        if (!b->pixels) {
            hk_lock_release(&b->lock);
            return E_NO_MEMORY;
        }
    }
    /* What the graphics processor may have drawn: a buffer it can draw into is read back for every lock that reads. */
    if ((usage & U_CPU_READ_MASK) && b->texture && (b->usage & U_GPU_FRAMEBUFFER) && !b->cpu_newer) hk_egl_ahb_read(b);
    b->locked_usage |= usage;
    b->locks++;
    *out = b->pixels;
    hk_lock_release(&b->lock);
    return 0;
}

static int g_lock(void *buffer, u64 usage, i32 fence, const void *rect, void **out)
{
    (void)rect;
    return lock_common(buffer, usage, fence, out);
}

static int g_lock_and_get_info(void *buffer, u64 usage, i32 fence, const void *rect, void **out, i32 *bytes_per_pixel, i32 *bytes_per_stride)
{
    hk_ahb *b = find(buffer);
    int r;
    (void)rect;
    r = lock_common(buffer, usage, fence, out);
    if (r == 0 && b) {
        if (bytes_per_pixel) *bytes_per_pixel = (i32)b->bpp;
        if (bytes_per_stride) *bytes_per_stride = (i32)(b->width * b->bpp);
    }
    return r;
}

/* What was written goes to the texture, if there is one yet; else the texture is made with it. */
static int g_unlock(void *buffer, i32 *fence)
{
    hk_ahb *b = find(buffer);
    if (fence) *fence = -1;
    if (!b) return E_BAD_VALUE;
    hk_lock_acquire(&b->lock);
    if (!b->locks) {
        hk_lock_release(&b->lock);
        return E_INVALID_OPERATION;
    }
    if (--b->locks == 0) {
        if (b->locked_usage & U_CPU_WRITE_MASK) {
            b->cpu_newer = true;
            if (b->texture && hk_egl_ahb_upload(b)) b->cpu_newer = false;
        }
        b->locked_usage = 0;
    }
    hk_lock_release(&b->lock);
    return 0;
}

static int g_get_id(const void *buffer, u64 *out)
{
    hk_ahb *b = find(buffer);
    if (!b || !out) return E_BAD_VALUE;
    *out = b->id;
    return 0;
}

/* One process: the buffer's address, with a reference for the receiver. */
static int g_send_handle(const void *buffer, int fd)
{
    typedef i32 (*write_fn)(int, const void *, u32);
    write_fn wr = (write_fn)hk_bionic_find("write");
    hk_ahb *b = find(buffer);
    hk_addr word;
    if (!b || !wr) return E_BAD_VALUE;
    word = (hk_addr)b;
    hk_atomic_add(&b->refs, 1);
    if (wr(fd, &word, sizeof(word)) != (i32)sizeof(word)) {
        g_release(b);
        return E_INVALID_OPERATION;
    }
    return 0;
}

static int g_recv_handle(int fd, void **out)
{
    typedef i32 (*read_fn)(int, void *, u32);
    read_fn rd = (read_fn)hk_bionic_find("read");
    hk_addr word = 0;
    hk_ahb *b;
    if (!out) return E_BAD_VALUE;
    *out = NULL;
    if (!rd || rd(fd, &word, sizeof(word)) != (i32)sizeof(word)) return E_INVALID_OPERATION;
    b = find((const void *)word);
    if (!b) return E_BAD_VALUE;
    *out = b;
    return 0;
}

static const hk_sym g_tab[] = {
    HK_WRAP("AHardwareBuffer_allocate", g_allocate),
    HK_WRAP("AHardwareBuffer_acquire", g_acquire),
    HK_WRAP("AHardwareBuffer_release", g_release),
    HK_WRAP("AHardwareBuffer_describe", g_describe),
    HK_WRAP("AHardwareBuffer_isSupported", g_is_supported),
    HK_WRAP("AHardwareBuffer_lock", g_lock),
    HK_WRAP("AHardwareBuffer_lockAndGetInfo", g_lock_and_get_info),
    HK_WRAP("AHardwareBuffer_unlock", g_unlock),
    HK_WRAP("AHardwareBuffer_getId", g_get_id),
    HK_WRAP("AHardwareBuffer_sendHandleToUnixSocket", g_send_handle),
    HK_WRAP("AHardwareBuffer_recvHandleFromUnixSocket", g_recv_handle),
    HK_END
};

const hk_sym *hk_ahb_table(void) { return g_tab; }
