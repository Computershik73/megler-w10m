/*
 * The core's tests.
 *
 *   hk_tests <guest library dir> <scratch dir>
 *
 * Everything that needs no ARM code runs anywhere (the x64 build on this PC).
 * The rest loads the guest test libraries (tools/build-guest.sh) and calls
 * into them, which needs a 32-bit ARM host: the same tests run under qemu-arm
 * (tools/build-tests.sh arm) and, linked into the app, on the phone itself.
 *
 * A guest function with floating-point parameters is called here through a
 * pointer typed with u32/u64 -- the softfp convention, spelled out.
 */
#include "hk_gfx_tests.h"
#include "hk_storage_tests.h"
#include "hk_tests.h"

#include <math.h>
#include <stdio.h>

#include "../src/arm/hk_stubs.h"
#include "../src/bionic/hk_bionic.h"
#include "../src/bionic/hk_fd.h"
#include "../src/hk_core.h"
#include "../src/host/hk_host.h"
#include "../include/megler.h"
#include "../src/jni/hk_jni.h"
#include "../src/jni/hk_jni_loop.h"
#include "../src/ld/hk_ld.h"
#include "../src/ndk/hk_egl.h"
#include "../src/ndk/hk_media.h"
#include "../src/sqlite/hk_sqlite.h"
#if defined(HK_HAVE_SQLITE) && HK_HAVE_SQLITE
#include "sqlite3.h"
#endif
#include "../src/ndk/hk_ndk.h"
#include "../src/run/hk_run.h"
#include "../src/zip/hk_zip.h"

static int g_checks, g_failed;
static hk_test_sink g_sink;
static void *g_sink_user;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (g_sink) g_sink(line, g_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

#define CHECK_INT(expr, want)                                                              \
    do {                                                                                   \
        long got_ = (long)(expr), want_ = (long)(want);                                    \
        g_checks++;                                                                        \
        if (got_ != want_) {                                                               \
            g_failed++;                                                                    \
            say("  FAIL %s:%d: %s = %ld, expected %ld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                  \
    } while (0)

static bool near(double a, double b) { return fabs(a - b) <= 1e-9 * (1.0 + fabs(b)); }

/* ------------------------------------------------------------------- host */

static volatile i32 g_futex_word;
static void futex_waker(void *arg)
{
    (void)arg;
    hk_sleep_ns(20000000ull);
    hk_atomic_xchg(&g_futex_word, 1);
    hk_futex_wake(&g_futex_word, true);
}

typedef struct list_seen {
    int count, files;
    char last[64];
} list_seen;

static void list_one(const char *name, bool is_dir, void *user)
{
    list_seen *s = (list_seen *)user;
    s->count++;
    if (!is_dir) s->files++;
    hk_strlcpy(s->last, name, sizeof(s->last));
}

static void test_host(const char *scratch)
{
    u8 *p;
    char *path, *sub;
    hk_file *f;
    hk_stat st;
    hk_thread *t;
    int err = 0;
    u64 t0;
    say("host (%s)", hk_host_name());

    p = (u8 *)hk_mem_reserve(1 << 20);
    CHECK(p != NULL);
    if (p) {
        CHECK(hk_mem_commit(p + 65536, 8192, HK_PROT_RW));
        p[65536] = 0x42;
        p[65536 + 8191] = 0x24;
        CHECK(p[65536] == 0x42 && p[65536 + 4096] == 0);
        CHECK(hk_mem_protect(p + 65536, 4096, HK_PROT_R));
        CHECK(p[65536] == 0x42);
        CHECK(hk_mem_decommit(p + 65536, 8192));
        hk_mem_release(p, 1 << 20);
    }

    t0 = hk_time_monotonic_ns();
    hk_sleep_ns(5000000ull);
    CHECK(hk_time_monotonic_ns() - t0 >= 4000000ull);
    CHECK(hk_time_realtime_ns() > 1600000000ull * 1000000000ull);

    g_futex_word = 0;
    t = hk_thread_start(futex_waker, NULL, 0, "waker");
    CHECK(t != NULL);
    while (!g_futex_word) hk_futex_wait(&g_futex_word, 0, 0);
    if (t) hk_thread_join(t);
    CHECK(!hk_futex_wait(&g_futex_word, 1, 2000000ull));       /* nothing wakes it: the time runs out */
    CHECK(hk_futex_wait(&g_futex_word, 0, 2000000ull));        /* the value differs: returns at once */

    sub = hk_sprintf("%s/host/a/b", scratch);
    CHECK(hk_path_mkdirs(sub));
    path = hk_sprintf("%s/file.bin", sub);
    f = hk_file_open(path, HK_O_WRITE | HK_O_CREATE | HK_O_TRUNC, &err);
    CHECK(f != NULL);
    if (f) {
        char buf[16];
        CHECK_INT(hk_file_write(f, "0123456789", 10, &err), 10);
        hk_file_close(f);
        f = hk_file_open(path, HK_O_READ | HK_O_WRITE, &err);
        CHECK(f != NULL);
        CHECK_INT(hk_file_pread(f, buf, 4, 3, &err), 4);
        CHECK(!memcmp(buf, "3456", 4));
        CHECK_INT(hk_file_read(f, buf, 2, &err), 2);       /* the positioned read left the position alone */
        CHECK(!memcmp(buf, "01", 2));
        CHECK_INT(hk_file_seek(f, -2, 2, &err), 8);
        CHECK_INT(hk_file_read(f, buf, 10, &err), 2);
        CHECK(hk_file_truncate(f, 4, &err));
        CHECK(hk_file_stat(f, &st) && st.size == 4 && !st.is_dir);
        hk_file_close(f);
    }
    CHECK(hk_path_stat(sub, &st, &err) && st.is_dir);
    {
        /* Listing, renaming, and the whole-file helpers. */
        char *other = hk_sprintf("%s/renamed.bin", sub);
        list_seen seen;
        size_t n = 0;
        u8 *all;
        bool dst = false;
        i32 offset = hk_time_utc_offset((i64)(hk_time_realtime_ns() / 1000000000ull), &dst);
        memset(&seen, 0, sizeof(seen));
        CHECK(hk_path_list(sub, list_one, &seen, &err));
        CHECK(seen.count == 1 && seen.files == 1 && !strcmp(seen.last, "file.bin"));
        CHECK(hk_path_rename(path, other, &err));
        CHECK(!hk_path_stat(path, &st, &err) && hk_path_stat(other, &st, &err) && st.size == 4);
        all = hk_file_read_all(other, &n);
        CHECK(all && n == 4 && !memcmp(all, "0123", 4));
        free(all);
        CHECK(hk_file_write_all(path, "back", 4));
        CHECK(hk_path_remove(other, &err));
        free(other);
        CHECK(offset > -16 * 3600 && offset < 16 * 3600);
    }
    CHECK(hk_file_open(sub, HK_O_READ, &err) == NULL && err == HK_E_ISDIR);
    CHECK(!hk_path_stat("/no/such/thing/at/all", &st, &err) && err == HK_E_NOENT);
    CHECK(!hk_path_rmdir(sub, &err) && err == HK_E_NOTEMPTY);
    CHECK(hk_path_remove(path, &err));
    CHECK(hk_path_rmdir(sub, &err));
    free(path);
    free(sub);
}

/* -------------------------------------------------------------------- zip */

/* A ZIP with one stored and one deflated entry, written by hand so the reader is tested against the format, not against itself. */
static void test_zip(const char *scratch)
{
    static const u8 stored[] = "stored entry";
    /* "aaaaaaaaaaaaaaaaaaaa" (20 bytes) as a raw deflate stream */
    static const u8 deflated[] = { 0x4B, 0x4C, 0xC4, 0x04, 0x00 };
    u8 zip[512];
    size_t n = 0, cd, i;
    char *path = hk_sprintf("%s/test.zip", scratch), err[80];
    hk_zip z;
    struct { const char *name; const u8 *data; u32 csize, usize, crc; u16 method; u32 offset; } e[2] = {
        { "lib/armeabi-v7a/libone.so", stored, sizeof(stored) - 1, sizeof(stored) - 1, 0, 0, 0 },
        { "assets/a.txt", deflated, sizeof(deflated), 20, 0, 8, 0 },
    };
    say("zip");
    for (i = 0; i < 2; i++) {
        u16 nl = (u16)strlen(e[i].name);
        e[i].offset = (u32)n;
        hk_wr32(zip + n, 0x04034b50u); hk_wr16(zip + n + 4, 20); hk_wr16(zip + n + 6, 0); hk_wr16(zip + n + 8, e[i].method);
        hk_wr32(zip + n + 10, 0); hk_wr32(zip + n + 14, e[i].crc); hk_wr32(zip + n + 18, e[i].csize); hk_wr32(zip + n + 22, e[i].usize);
        hk_wr16(zip + n + 26, nl); hk_wr16(zip + n + 28, 0);
        memcpy(zip + n + 30, e[i].name, nl);
        memcpy(zip + n + 30 + nl, e[i].data, e[i].csize);
        n += 30 + nl + e[i].csize;
    }
    cd = n;
    for (i = 0; i < 2; i++) {
        u16 nl = (u16)strlen(e[i].name);
        memset(zip + n, 0, 46);
        hk_wr32(zip + n, 0x02014b50u); hk_wr16(zip + n + 10, e[i].method); hk_wr32(zip + n + 20, e[i].csize);
        hk_wr32(zip + n + 24, e[i].usize); hk_wr16(zip + n + 28, nl); hk_wr32(zip + n + 42, e[i].offset);
        memcpy(zip + n + 46, e[i].name, nl);
        n += 46 + nl;
    }
    memset(zip + n, 0, 22);
    hk_wr32(zip + n, 0x06054b50u); hk_wr16(zip + n + 8, 2); hk_wr16(zip + n + 10, 2); hk_wr32(zip + n + 12, (u32)(n - cd)); hk_wr32(zip + n + 16, (u32)cd);
    n += 22;
    CHECK(hk_file_write_all(path, zip, n));
    CHECK(hk_zip_open(&z, path, err, sizeof(err)));
    if (z.count) {
        const hk_zip_entry *a = hk_zip_find(&z, "assets/a.txt"), *s = hk_zip_find(&z, "lib/armeabi-v7a/libone.so");
        size_t size = 0;
        u8 *data;
        CHECK_INT(z.count, 2);
        CHECK(a && s && !hk_zip_find(&z, "assets/b.txt"));
        if (a && s) {
            {
                u64 at = 0;
                char back[sizeof(stored)];
                memset(back, 0, sizeof(back));
                CHECK(hk_zip_stored_at(&z, s, &at) && hk_zip_pread(&z, at, back, sizeof(stored) - 1) && !memcmp(back, stored, sizeof(stored) - 1));
                CHECK(!hk_zip_stored_at(&z, a, &at));
                CHECK(!hk_zip_pread(&z, z.size - 2, back, 4));       /* past the end of the file */
            }
            data = hk_zip_read(&z, a, 1000, &size);
            CHECK(data && size == 20 && data[0] == 'a' && data[19] == 'a');
            free(data);
            CHECK(!hk_zip_read(&z, a, 10, &size));          /* over the caller's limit */
        }
        hk_zip_close(&z);
    }
    /* Damaged archives are refused, not followed. */
    hk_wr32(zip + n - 22 + 16, 0x7FFFFFF0u);
    CHECK(hk_file_write_all(path, zip, n));
    CHECK(!hk_zip_open(&z, path, err, sizeof(err)));
    CHECK(hk_file_write_all(path, "not a zip at all, just text", 27));
    CHECK(!hk_zip_open(&z, path, err, sizeof(err)));
    hk_path_remove(path, NULL);
    free(path);
}

/* ------------------------------------------------------------- guest code */

/* ------------------------------------------------------------------ input */

/*
 * An app's thread waits on a looper for its input queue, and what the looper
 * says must be true: something to read exactly while events are queued. Said
 * while there are none, an app that polls without waiting (every game, each
 * frame) goes round its event loop for ever and never draws: that was seen on
 * a phone, where touches arrive from another thread on another core. So one
 * thread touches as fast as it can here while another takes the events the
 * way an app does, and at the end the looper must have nothing to say.
 */
static struct {
    hk_input_queue *queue;
    volatile i32 downs, ups, stop, idle_wakes;
    int left_readable;
} G_INPUT;

static void input_taker(void *arg)
{
    void *(*prepare)(int) = (void *(*)(int))hk_bionic_find("ALooper_prepare");
    void (*attach)(hk_input_queue *, void *, int, hk_addr, void *) = (void (*)(hk_input_queue *, void *, int, hk_addr, void *))hk_bionic_find("AInputQueue_attachLooper");
    void (*detach)(hk_input_queue *) = (void (*)(hk_input_queue *))hk_bionic_find("AInputQueue_detachLooper");
    int (*poll_all)(int, int *, int *, void **) = (int (*)(int, int *, int *, void **))hk_bionic_find("ALooper_pollAll");
    int (*get)(hk_input_queue *, void **) = (int (*)(hk_input_queue *, void **))hk_bionic_find("AInputQueue_getEvent");
    void (*finish)(hk_input_queue *, void *, int) = (void (*)(hk_input_queue *, void *, int))hk_bionic_find("AInputQueue_finishEvent");
    int (*action_of)(const void *) = (int (*)(const void *))hk_bionic_find("AMotionEvent_getAction");
    int rounds = 0, ident;
    (void)arg;
    attach(G_INPUT.queue, prepare(0), 2, 0, NULL);
    /* Without waiting, as a game's loop polls between frames; and a few rounds more once told to stop, to take what is left. */
    while (rounds < 3) {
        void *event;
        int taken = 0;
        ident = poll_all(0, NULL, NULL, NULL);
        if (ident == 2) {
            while (get(G_INPUT.queue, &event) >= 0) {
                int action = action_of(event) & 0xFF;
                if (action == 0 || action == 5) hk_atomic_add(&G_INPUT.downs, 1);
                if (action == 1 || action == 6) hk_atomic_add(&G_INPUT.ups, 1);
                finish(G_INPUT.queue, event, 1);
                taken++;
            }
            if (!taken) hk_atomic_add(&G_INPUT.idle_wakes, 1);
        }
        if (G_INPUT.stop && !taken) rounds++;
    }
    /* Everything has been taken: the looper must say so. */
    G_INPUT.left_readable = poll_all(0, NULL, NULL, NULL) == 2;
    detach(G_INPUT.queue);
}

/* A DXT5 block back to sixteen RGBA pixels, by the format's definition. */
static void dxt5_decode(const u8 *b, u8 out[16][4])
{
    u32 a[8], c0 = (u32)b[8] | ((u32)b[9] << 8), c1 = (u32)b[10] | ((u32)b[11] << 8), bits = hk_rd32(b + 12);
    u8 pal[4][3];
    u64 abits = 0;
    int i, k;
    a[0] = b[0];
    a[1] = b[1];
    for (i = 2; i < 8; i++) a[i] = a[0] > a[1] ? ((8 - i) * a[0] + (i - 1) * a[1]) / 7 : i < 6 ? ((6 - i) * a[0] + (i - 1) * a[1]) / 5 : i == 6 ? 0 : 255;
    for (i = 0; i < 6; i++) abits |= (u64)b[2 + i] << (8 * i);
    for (k = 0; k < 2; k++) {
        u32 c = k ? c1 : c0, r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
        pal[k][0] = (u8)((r << 3) | (r >> 2));
        pal[k][1] = (u8)((g << 2) | (g >> 4));
        pal[k][2] = (u8)((bl << 3) | (bl >> 2));
    }
    for (k = 0; k < 3; k++) {
        pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
        pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    for (i = 0; i < 16; i++) {
        memcpy(out[i], pal[(bits >> (2 * i)) & 3], 3);
        out[i][3] = (u8)a[(abits >> (3 * i)) & 7];
    }
}

static int far_apart(int a, int b) { return a > b ? a - b : b - a; }

/* Textures packed on their way in (hk_gltex.c): what comes out of the DXT5 blocks is what went in, as near as the format allows. */
static void test_textures(void)
{
    u8 img[8 * 8 * 4], packed[64], back[16][4];
    int i, worst;
    say("textures");
    /* One colour: exact but for the five and six bits a channel has. */
    for (i = 0; i < 64; i++) {
        img[i * 4] = 200;
        img[i * 4 + 1] = 100;
        img[i * 4 + 2] = 50;
        img[i * 4 + 3] = 128;
    }
    CHECK(hk_gltex_dxt5_size(8, 8) == 64 && hk_gltex_dxt5_size(6, 2) == 32 && hk_gltex_dxt5_size(1, 1) == 16);
    hk_gltex_dxt5(img, 8, 8, 32, packed);
    dxt5_decode(packed + 48, back);
    worst = 0;
    for (i = 0; i < 16; i++) {
        if (far_apart(back[i][0], 200) > worst) worst = far_apart(back[i][0], 200);
        if (far_apart(back[i][1], 100) > worst) worst = far_apart(back[i][1], 100);
        if (far_apart(back[i][2], 50) > worst) worst = far_apart(back[i][2], 50);
        if (back[i][3] != 128) worst = 255;
    }
    CHECK(worst <= 7);
    /* Two colours side by side, opaque and clear: each pixel comes back on its own side. */
    for (i = 0; i < 16; i++) {
        bool left = (i & 3) < 2;
        img[i * 4] = left ? 255 : 0;
        img[i * 4 + 1] = 0;
        img[i * 4 + 2] = left ? 0 : 255;
        img[i * 4 + 3] = left ? 255 : 0;
    }
    hk_gltex_dxt5(img, 4, 4, 16, packed);
    dxt5_decode(packed, back);
    worst = 0;
    for (i = 0; i < 16; i++) {
        bool left = (i & 3) < 2;
        if (left ? (back[i][0] < 200 || back[i][2] > 55 || back[i][3] != 255) : (back[i][2] < 200 || back[i][0] > 55 || back[i][3] != 0)) worst++;
    }
    CHECK_INT(worst, 0);
    /* A ramp of alpha: within half a step of the eight the format has. */
    for (i = 0; i < 16; i++) {
        img[i * 4] = img[i * 4 + 1] = img[i * 4 + 2] = 90;
        img[i * 4 + 3] = (u8)(i * 17);
    }
    hk_gltex_dxt5(img, 4, 4, 16, packed);
    dxt5_decode(packed, back);
    worst = 0;
    for (i = 0; i < 16; i++)
        if (far_apart(back[i][3], i * 17) > worst) worst = far_apart(back[i][3], i * 17);
    CHECK(worst <= 19);
    /* A size that is not whole blocks: the edge is repeated, nothing past it is read (the picture is at the very end of its memory). */
    {
        u8 *edge = (u8 *)malloc(6 * 2 * 4);
        for (i = 0; i < 12; i++) {
            edge[i * 4] = edge[i * 4 + 1] = edge[i * 4 + 2] = (u8)(i < 6 ? 20 : 220);
            edge[i * 4 + 3] = 255;
        }
        hk_gltex_dxt5(edge, 6, 2, 24, packed);
        dxt5_decode(packed + 16, back);
        CHECK(far_apart(back[0][0], 20) <= 20 && far_apart(back[4][0], 220) <= 20 && back[0][3] == 255);
        free(edge);
    }
}

/*
 * A film through libmediandk (ndk/hk_media.c), with a decoder that makes its
 * pictures up: the samples an extractor hands out, a codec fed with them, and
 * what comes out, in the order and with the notices an engine counts on.
 */
static int g_fake_video, g_fake_audio;
static u64 g_fake_size;

static void *fake_open(void *user, const char *path, megler_media_info *info)
{
    hk_stat st;
    (void)user;
    g_fake_size = hk_path_stat(path, &st, NULL) ? st.size : 0;
    info->width = 4;
    info->height = 2;
    info->frame_rate_num = 30;
    info->frame_rate_den = 1;
    info->sample_rate = 48000;
    info->channels = 2;
    info->duration_us = 100000;
    g_fake_video = g_fake_audio = 0;
    return &g_fake_size;
}
static int fake_video(void *user, void *media, uint8_t *rgba, int64_t *time_us)
{
    (void)user; (void)media;
    if (g_fake_video >= 3) return 0;
    memset(rgba, 10 + g_fake_video, 4 * 2 * 4);
    *time_us = g_fake_video * 33333;
    g_fake_video++;
    return 1;
}
static int fake_audio(void *user, void *media, int16_t *samples, int max_frames, int64_t *time_us)
{
    int n = 4800 - g_fake_audio < max_frames ? 4800 - g_fake_audio : max_frames;
    (void)user; (void)media;
    memset(samples, 1, (size_t)n * 4);
    *time_us = (int64_t)g_fake_audio * 1000000 / 48000;
    g_fake_audio += n;
    return n;
}
static bool fake_seek(void *user, void *media, int64_t time_us)
{
    (void)user; (void)media;
    g_fake_video = (int)(time_us * 30 / 1000000);
    g_fake_audio = (int)(time_us * 48000 / 1000000);
    return true;
}
static void fake_close(void *user, void *media) { (void)user; (void)media; }

static void test_media(const char *scratch)
{
    typedef struct { i32 offset, size; i64 time_us; u32 flags; } buffer_info;
    static const megler_media_host fake = { NULL, fake_open, fake_video, fake_audio, fake_seek, fake_close };
    void *(*ex_new)(void) = (void *(*)(void))hk_bionic_find("AMediaExtractor_new");
    int (*ex_fd)(void *, int, i64, i64) = (int (*)(void *, int, i64, i64))hk_bionic_find("AMediaExtractor_setDataSourceFd");
    u32 (*ex_count)(void *) = (u32 (*)(void *))hk_bionic_find("AMediaExtractor_getTrackCount");
    void *(*ex_format)(void *, u32) = (void *(*)(void *, u32))hk_bionic_find("AMediaExtractor_getTrackFormat");
    int (*ex_select)(void *, u32) = (int (*)(void *, u32))hk_bionic_find("AMediaExtractor_selectTrack");
    int (*ex_unselect)(void *, u32) = (int (*)(void *, u32))hk_bionic_find("AMediaExtractor_unselectTrack");
    i32 (*ex_read)(void *, u8 *, u32) = (i32 (*)(void *, u8 *, u32))hk_bionic_find("AMediaExtractor_readSampleData");
    i64 (*ex_time)(void *) = (i64 (*)(void *))hk_bionic_find("AMediaExtractor_getSampleTime");
    int (*ex_track)(void *) = (int (*)(void *))hk_bionic_find("AMediaExtractor_getSampleTrackIndex");
    u8 (*ex_advance)(void *) = (u8 (*)(void *))hk_bionic_find("AMediaExtractor_advance");
    int (*ex_seek)(void *, i64, int) = (int (*)(void *, i64, int))hk_bionic_find("AMediaExtractor_seekTo");
    int (*ex_delete)(void *) = (int (*)(void *))hk_bionic_find("AMediaExtractor_delete");
    u8 (*fmt_string)(void *, const char *, const char **) = (u8 (*)(void *, const char *, const char **))hk_bionic_find("AMediaFormat_getString");
    u8 (*fmt_i32)(void *, const char *, i32 *) = (u8 (*)(void *, const char *, i32 *))hk_bionic_find("AMediaFormat_getInt32");
    int (*fmt_delete)(void *) = (int (*)(void *))hk_bionic_find("AMediaFormat_delete");
    void *(*codec_new)(const char *) = (void *(*)(const char *))hk_bionic_find("AMediaCodec_createDecoderByType");
    int (*codec_configure)(void *, void *, void *, void *, u32) = (int (*)(void *, void *, void *, void *, u32))hk_bionic_find("AMediaCodec_configure");
    int (*codec_start)(void *) = (int (*)(void *))hk_bionic_find("AMediaCodec_start");
    i32 (*codec_in)(void *, i64) = (i32 (*)(void *, i64))hk_bionic_find("AMediaCodec_dequeueInputBuffer");
    u8 *(*codec_in_buffer)(void *, u32, u32 *) = (u8 *(*)(void *, u32, u32 *))hk_bionic_find("AMediaCodec_getInputBuffer");
    int (*codec_queue)(void *, u32, i32, u32, u64, u32) = (int (*)(void *, u32, i32, u32, u64, u32))hk_bionic_find("AMediaCodec_queueInputBuffer");
    i32 (*codec_out)(void *, buffer_info *, i64) = (i32 (*)(void *, buffer_info *, i64))hk_bionic_find("AMediaCodec_dequeueOutputBuffer");
    u8 *(*codec_out_buffer)(void *, u32, u32 *) = (u8 *(*)(void *, u32, u32 *))hk_bionic_find("AMediaCodec_getOutputBuffer");
    int (*codec_release)(void *, u32, u8) = (int (*)(void *, u32, u8))hk_bionic_find("AMediaCodec_releaseOutputBuffer");
    int (*codec_delete)(void *) = (int (*)(void *))hk_bionic_find("AMediaCodec_delete");
    const char **key_mime = (const char **)hk_bionic_find("AMEDIAFORMAT_KEY_MIME");
    char *path = hk_sprintf("%s/film-source.bin", scratch);
    u8 bytes[1000], sample[64];
    void *ex, *format, *codec;
    const char *mime = NULL;
    i32 width = 0;
    int fd, video = 0, audio = 0, i;
    i64 last = -1;
    bool ordered = true;
    buffer_info info;
    say("films");
    CHECK(ex_new && ex_fd && codec_new && key_mime && !strcmp(*key_mime, "mime"));
    if (!ex_new || !codec_new) return;
    hk_media_set_host(&fake);
    for (i = 0; i < 1000; i++) bytes[i] = (u8)i;
    CHECK(hk_file_write_all(path, bytes, sizeof(bytes)));
    fd = hk_fd_open_host(path, HK_O_READ);
    ex = ex_new();
    /* A range of a larger file, as a film inside an APK is: that many bytes reach the decoder's own copy. */
    CHECK_INT(ex_fd(ex, fd, 100, 800), 0);
    CHECK_INT((int)g_fake_size, 800);
    CHECK_INT((int)ex_count(ex), 2);
    format = ex_format(ex, 0);
    CHECK(fmt_string(format, "mime", &mime) && mime && !strcmp(mime, "video/avc") && fmt_i32(format, "width", &width) && width == 4);
    fmt_delete(format);
    /* Both tracks, in order of time: three pictures, and five blocks of sound for a tenth of a second at 48 kHz. */
    ex_select(ex, 0);
    ex_select(ex, 1);
    do {
        i64 t = ex_time(ex);
        if (ex_read(ex, sample, sizeof(sample)) <= 0) break;
        if (t < last) ordered = false;
        last = t;
        if (ex_track(ex) == 0) video++;
        else audio++;
    } while (ex_advance(ex));
    CHECK(ordered);
    CHECK_INT(video, 3);
    CHECK_INT(audio, 5);
    CHECK(ex_read(ex, sample, sizeof(sample)) < 0);
    /* The pictures again through a decoder: a notice of the format first, each frame with its time, then the end. */
    ex_seek(ex, 0, 0);
    ex_unselect(ex, 1);
    codec = codec_new("video/avc");
    CHECK(codec != NULL);
    CHECK_INT(codec_configure(codec, NULL, NULL, NULL, 0), 0);
    codec_start(codec);
    CHECK_INT(codec_out(codec, &info, 0), -1);              /* nothing queued yet */
    for (;;) {
        u32 size = 0;
        i32 slot = codec_in(codec, 0), n;
        u8 *buffer = codec_in_buffer(codec, (u32)slot, &size);
        n = ex_read(ex, buffer, size);
        if (n < 0) {
            codec_queue(codec, (u32)slot, 0, 0, 0, 4);
            break;
        }
        codec_queue(codec, (u32)slot, 0, (u32)n, (u64)ex_time(ex), 0);
        ex_advance(ex);
    }
    CHECK_INT(codec_out(codec, &info, 0), -2);
    for (i = 0; i < 3; i++) {
        u32 size = 0;
        i32 slot = codec_out(codec, &info, 0);
        const u8 *frame = slot >= 0 ? codec_out_buffer(codec, (u32)slot, &size) : NULL;
        CHECK(slot >= 0 && frame && size == 32 && frame[0] == 10 + i && info.size == 32 && info.time_us == (i64)i * 33333 && !info.flags);
        if (slot >= 0) codec_release(codec, (u32)slot, 1);
    }
    {
        i32 slot = codec_out(codec, &info, 0);
        CHECK(slot >= 0 && (info.flags & 4) && !info.size);
        if (slot >= 0) codec_release(codec, (u32)slot, 0);
    }
    CHECK_INT(codec_out(codec, &info, 0), -1);
    codec_delete(codec);
    /* A decoder is full after a handful of samples nobody has asked the output for, and has room again once one is. */
    ex_seek(ex, 0, 0);
    codec = codec_new("video/avc");
    codec_configure(codec, NULL, NULL, NULL, 0);
    codec_start(codec);
    for (i = 0; i < 64 && codec_in(codec, 0) == 0; i++) {
        u32 size = 0;
        u8 *buffer = codec_in_buffer(codec, 0, &size);
        codec_queue(codec, 0, 0, (u32)ex_read(ex, buffer, size), 0, 0);
    }
    CHECK(i >= 2 && i <= 16);
    CHECK_INT(codec_in(codec, 0), -1);
    CHECK_INT(codec_out(codec, &info, 0), -2);
    CHECK(codec_out(codec, &info, 0) >= 0);
    CHECK_INT(codec_in(codec, 0), 0);
    codec_delete(codec);
    ex_delete(ex);
    hk_fd_close(fd);
    hk_media_set_host(NULL);
    free(path);
}

/* The battery the core answers for (run/hk_run.h): a full one on mains until one is set, then the one set, made safe to divide by and to read as text. */
static void test_battery(void)
{
    hk_battery b, set;
    say("the battery");
    memset(&b, 0, sizeof(b));
    hk_battery_read(&b);
    CHECK(b.level == 100 && b.scale == 100 && b.status == 5 && b.plugged == 1 && b.health == 2 && b.voltage_mv == 4200 && b.temperature == 250 && b.present && !strcmp(b.technology, "Li-ion"));
    memset(&set, 0, sizeof(set));
    set.level = 12;
    set.status = 3;
    set.health = 2;
    set.voltage_mv = 3600;
    set.temperature = -52;
    set.present = true;
    memset(set.technology, 'x', sizeof(set.technology));
    hk_battery_set(&set);
    hk_battery_read(&b);
    CHECK(b.level == 12 && b.scale == 100 && b.status == 3 && b.plugged == 0 && b.voltage_mv == 3600 && b.temperature == -52 && b.present);
    CHECK(strlen(b.technology) == sizeof(b.technology) - 1);
    hk_battery_set(NULL);
    hk_battery_read(&b);
    CHECK(b.level == 100 && b.plugged == 1 && !strcmp(b.technology, "Li-ion"));
    {
        /*
         * The shell's battery (megler.h): what a phone with Windows says, and the rest filled in. And the count of changes an app is
         * told of: one for each time the level, the status or the plug became another, none for the same again or for the
         * voltage alone.
         */
        megler_battery phone;
        u32 changes = hk_battery_changes();
        memset(&phone, 0, sizeof(phone));
        phone.level = 50;
        phone.status = 3;
        phone.temperature = INT_MIN;
        phone.present = true;
        megler_set_battery(&phone);
        hk_battery_read(&b);
        CHECK(b.level == 50 && b.scale == 100 && b.status == 3 && b.plugged == 0 && b.health == 2 && b.voltage_mv == 3800 && b.temperature == 250 && b.present
              && !strcmp(b.technology, "Li-ion"));
        CHECK(hk_battery_changes() == changes + 1);
        megler_set_battery(&phone);
        CHECK(hk_battery_changes() == changes + 1);
        phone.voltage_mv = 3777;
        phone.temperature = -15;
        megler_set_battery(&phone);
        hk_battery_read(&b);
        CHECK(b.voltage_mv == 3777 && b.temperature == -15 && hk_battery_changes() == changes + 1);
        phone.plugged = 2;
        phone.status = 2;
        megler_set_battery(&phone);
        CHECK(hk_battery_changes() == changes + 2);
        /* What cannot be is brought to what can. */
        phone.level = 140;
        phone.status = 9;
        phone.plugged = -1;
        phone.present = false;
        megler_set_battery(&phone);
        hk_battery_read(&b);
        CHECK(b.level == 100 && b.status == 1 && b.plugged == 0 && b.health == 1 && !b.present);
        megler_set_battery(NULL);
        hk_battery_read(&b);
        CHECK(b.level == 100 && b.status == 5 && b.plugged == 1 && b.voltage_mv == 4200);
    }
}

static void test_input(void)
{
    enum { TAPS = 60000 };
    hk_thread *taker;
    int i, waited;
    say("input queue");
    if (!hk_bionic_find("AInputQueue_getEvent") || !hk_bionic_find("ALooper_pollAll")) {
        CHECK(!"the NDK's functions are registered");
        return;
    }
    memset(&G_INPUT, 0, sizeof(G_INPUT));
    G_INPUT.queue = hk_input_queue_create();
    taker = hk_thread_start(input_taker, NULL, 256 * 1024, "test-input");
    for (i = 0; i < TAPS; i++) {
        hk_input_queue_touch(G_INPUT.queue, 0, 1, (float)i, 10.0f);
        hk_input_queue_touch(G_INPUT.queue, 1, 1, (float)i, 11.0f);
        hk_input_queue_touch(G_INPUT.queue, 2, 1, (float)i, 12.0f);
        /* The queue is not endless; a touch is not put in faster than it is taken for long. */
        for (waited = 0; waited < 2000000 && i - G_INPUT.ups > 40; waited++) hk_thread_yield();
    }
    for (waited = 0; waited < 2000 && G_INPUT.ups < TAPS; waited++) hk_sleep_ns(1000000ull);
    hk_atomic_xchg(&G_INPUT.stop, 1);
    hk_thread_join(taker);
    /* No touch lost, however they were put in... */
    CHECK_INT(G_INPUT.downs, TAPS);
    CHECK_INT(G_INPUT.ups, TAPS);
    /* ...and nothing said to be there once there is not. */
    CHECK_INT(G_INPUT.left_readable, 0);
    say("  wakes with nothing to take: %d", (int)G_INPUT.idle_wakes);
}

#if HK_HOST_ARM32

/*
 * An interface proxy of Unity's (jni/hk_jni_loop.c): a call through it and its
 * being disabled exclude each other, as they do under the proxy's lock in the
 * engine's own Java. One thread calls through a proxy as fast as it can while
 * this one disables the proxy and then "deletes" what was behind it; no call
 * may find it deleted. On a phone a film skipped with a tap did exactly that
 * to its decoder's thread before there was a lock. (Here, where a native
 * method can be a function of this program: it is called as 32-bit ARM code.)
 */
typedef struct proxied {
    volatile i32 alive, stop, calls, dead;
    volatile i32 quits, done;           /* a handler that quits the loop it runs on, and when it has returned */
} proxied;

/* The JNIEnv's own functions, by their places in its table, as an app's library calls them. */
typedef hk_jobj *(*jni_find_class_fn)(void *, const char *);
typedef void *(*jni_id_fn)(void *, hk_jobj *, const char *, const char *);
typedef hk_jobj *(*jni_object_a_fn)(void *, hk_jobj *, void *, const hk_jvalue *);
typedef void (*jni_void_a_fn)(void *, hk_jobj *, void *, const hk_jvalue *);
typedef u8 (*jni_boolean_a_fn)(void *, hk_jobj *, void *, const hk_jvalue *);
typedef hk_jobj *(*jni_string_fn)(void *, const char *);
typedef hk_jobj *(*jni_element_fn)(void *, hk_jobj *, i32);
enum { JNI_FIND_CLASS = 6, JNI_NEW_OBJECT_A = 30, JNI_METHOD_ID = 33, JNI_OBJECT_A = 36, JNI_BOOLEAN_A = 39, JNI_VOID_A = 63, JNI_STATIC_ID = 113,
       JNI_STATIC_OBJECT_A = 116, JNI_STATIC_VOID_A = 143, JNI_NEW_STRING_UTF = 167, JNI_ARRAY_ELEMENT = 173, JNI_REGISTER = 215 };

static void *proxied_invoke(void *env, void *cls, i64 handle, void *iface, void *method, void *args)
{
    proxied *p = (proxied *)(hk_addr)handle;
    int i;
    (void)cls; (void)iface; (void)method;
    if (p->quits) {
        /*
         * A last message to a thread, whose handler ends the thread's loop from inside. 1: Looper.myLooper().quit().
         * 2: message.getTarget().getLooper().quit(), which is how Unity 2020 ends a decoder's thread. 3: it forgets to.
         */
        void **table = *(void ***)env;
        jni_id_fn id = (jni_id_fn)table[JNI_METHOD_ID];
        hk_jobj *looper_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Looper");
        hk_jobj *looper = NULL;
        if (p->quits == 1) {
            void *mine = ((jni_id_fn)table[JNI_STATIC_ID])(env, looper_cls, "myLooper", "()Landroid/os/Looper;");
            looper = ((jni_object_a_fn)table[JNI_STATIC_OBJECT_A])(env, looper_cls, mine, NULL);
        } else if (p->quits == 2) {
            hk_jobj *message_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Message");
            hk_jobj *handler_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Handler");
            hk_jobj *message = ((jni_element_fn)table[JNI_ARRAY_ELEMENT])(env, (hk_jobj *)args, 0);
            hk_jobj *target = message ? ((jni_object_a_fn)table[JNI_OBJECT_A])(env, message, id(env, message_cls, "getTarget", "()Landroid/os/Handler;"), NULL) : NULL;
            looper = target ? ((jni_object_a_fn)table[JNI_OBJECT_A])(env, target, id(env, handler_cls, "getLooper", "()Landroid/os/Looper;"), NULL) : NULL;
        }
        if (looper) ((jni_void_a_fn)table[JNI_VOID_A])(env, looper, id(env, looper_cls, "quit", "()V"), NULL);
        hk_sleep_ns(40000000ull);
        hk_atomic_xchg(&p->done, looper || p->quits == 3 ? 1 : -1);
        return NULL;
    }
    if (!p->alive) hk_atomic_add(&p->dead, 1);
    for (i = 0; i < 20; i++) hk_thread_yield();
    if (!p->alive) hk_atomic_add(&p->dead, 1);
    hk_atomic_add(&p->calls, 1);
    return NULL;
}

typedef struct proxy_caller {
    hk_jobj *proxy;
    proxied *p;
} proxy_caller;

static void proxy_caller_thread(void *arg)
{
    proxy_caller *c = (proxy_caller *)arg;
    while (!c->p->stop) hk_jni_unref(hk_jni_proxy_call(c->proxy, "java/lang/Runnable", "run", "()V", NULL, 0));
}

static void test_proxies(void)
{
    enum { ROUNDS = 200 };
    /* The JNIEnv's own functions, by their places in its table, as an app's library calls them. */
    typedef struct { u32 name, sig, fn; } native_method;
    typedef hk_jobj *(*find_class_fn)(void *, const char *);
    typedef i32 (*register_fn)(void *, hk_jobj *, const native_method *, i32);
    typedef void *(*static_id_fn)(void *, hk_jobj *, const char *, const char *);
    typedef hk_jobj *(*call_object_fn)(void *, hk_jobj *, void *, const hk_jvalue *);
    typedef void (*call_void_fn)(void *, hk_jobj *, void *, const hk_jvalue *);
    static const char bridge[] = "bitter/jnibridge/JNIBridge";
    static const char invoke_sig[] = "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;";
    void *env;
    void **table;
    native_method m;
    hk_jobj *cls;
    void *new_id, *off_id;
    i32 dead = 0, calls = 0;
    int round, waited;
    say("interface proxies: a call and a disable keep out of each other's way");
    hk_jni_init();
    hk_jni_loop_install();
    env = hk_jni_env();
    table = *(void ***)env;
    cls = ((find_class_fn)table[6])(env, bridge);
    m.name = (u32)(hk_addr)"invoke";
    m.sig = (u32)(hk_addr)invoke_sig;
    m.fn = (u32)(hk_addr)proxied_invoke;
    ((register_fn)table[215])(env, cls, &m, 1);
    CHECK(hk_jni_native(bridge, "invoke", invoke_sig) == (void *)proxied_invoke);
    new_id = ((static_id_fn)table[113])(env, cls, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;");
    off_id = ((static_id_fn)table[113])(env, cls, "disableInterfaceProxy", "(Ljava/lang/Object;)V");
    CHECK(new_id && off_id);
    if (!new_id || !off_id) return;
    for (round = 0; round < ROUNDS; round++) {
        proxied p;
        proxy_caller caller;
        hk_jvalue a[2];
        hk_thread *t;
        memset(&p, 0, sizeof(p));
        p.alive = 1;
        memset(a, 0, sizeof(a));
        a[0].j = (i64)(hk_addr)&p;
        caller.proxy = ((call_object_fn)table[116])(env, cls, new_id, a);
        caller.p = &p;
        if (!caller.proxy) break;
        t = hk_thread_start(proxy_caller_thread, &caller, 256 * 1024, "test-proxy");
        for (waited = 0; waited < 2000000 && p.calls < 3; waited++) hk_thread_yield();
        memset(a, 0, sizeof(a));
        a[0].l = caller.proxy;
        ((call_void_fn)table[143])(env, cls, off_id, a);
        /* What the engine does next: the object behind the proxy goes. */
        hk_atomic_xchg(&p.alive, 0);
        for (waited = 0; waited < 50; waited++) hk_thread_yield();
        hk_atomic_xchg(&p.stop, 1);
        hk_thread_join(t);
        dead += p.dead;
        calls += p.calls;
    }
    CHECK_INT(round, ROUNDS);
    CHECK_INT(dead, 0);
    CHECK(calls >= ROUNDS * 3);

    /*
     * A HandlerThread ended the way Unity ends a decoder's: a last message whose handler quits the loop from inside, then
     * join -- which must not return before that handler has, because what the handler runs on is deleted next.
     */
    say("a HandlerThread: join waits for a loop that quits from inside, and does not wait for ever for one that never does");
    for (round = 1; round <= 3; round++) {
        proxied p;
        hk_jvalue a[2];
        hk_jobj *ht_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/HandlerThread");
        hk_jobj *h_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Handler");
        hk_jobj *m_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Message");
        jni_id_fn id = (jni_id_fn)table[JNI_METHOD_ID];
        jni_object_a_fn object_a = (jni_object_a_fn)table[JNI_OBJECT_A];
        jni_void_a_fn void_a = (jni_void_a_fn)table[JNI_VOID_A];
        hk_jobj *ht, *looper, *proxy, *handler, *msg;
        memset(&p, 0, sizeof(p));
        p.alive = 1;
        p.quits = round;
        memset(a, 0, sizeof(a));
        a[0].l = ((jni_string_fn)table[JNI_NEW_STRING_UTF])(env, "test-decoder");
        ht = ((jni_object_a_fn)table[JNI_NEW_OBJECT_A])(env, ht_cls, id(env, ht_cls, "<init>", "(Ljava/lang/String;)V"), a);
        void_a(env, ht, id(env, ht_cls, "start", "()V"), NULL);
        looper = object_a(env, ht, id(env, ht_cls, "getLooper", "()Landroid/os/Looper;"), NULL);
        CHECK(ht && looper);
        CHECK(((jni_boolean_a_fn)table[JNI_BOOLEAN_A])(env, ht, id(env, ht_cls, "isAlive", "()Z"), NULL) != 0);
        memset(a, 0, sizeof(a));
        a[0].j = (i64)(hk_addr)&p;
        proxy = ((jni_object_a_fn)table[JNI_STATIC_OBJECT_A])(env, cls, new_id, a);
        memset(a, 0, sizeof(a));
        a[0].l = looper;
        a[1].l = proxy;
        handler = ((jni_object_a_fn)table[JNI_NEW_OBJECT_A])(env, h_cls, id(env, h_cls, "<init>", "(Landroid/os/Looper;Landroid/os/Handler$Callback;)V"), a);
        memset(a, 0, sizeof(a));
        a[0].i = 1;
        msg = object_a(env, handler, id(env, h_cls, "obtainMessage", "(I)Landroid/os/Message;"), a);
        CHECK(proxy && handler && msg);
        if (ht && looper && proxy && handler && msg) {
            void_a(env, msg, id(env, m_cls, "sendToTarget", "()V"), NULL);
            void_a(env, ht, id(env, ht_cls, "join", "()V"), NULL);
            CHECK_INT(p.done, 1);
            CHECK(((jni_boolean_a_fn)table[JNI_BOOLEAN_A])(env, ht, id(env, ht_cls, "isAlive", "()Z"), NULL) == 0);
            memset(a, 0, sizeof(a));
            a[0].l = proxy;
            ((jni_void_a_fn)table[JNI_STATIC_VOID_A])(env, cls, off_id, a);
        }
    }
}

static int host_times_three(int v) { return v * 3; }
/* Called by guest code with softfp arguments: bits in, bits out. */
static u64 host_hypot(u64 a, u64 b) { return hk_db(sqrt(hk_d(a) * hk_d(a) + hk_d(b) * hk_d(b))); }

#define SYM(lib, name) hk_ld_sym(lib, name)
#define FN(lib, type, name) HK_GFN(type, SYM(lib, name))

static void test_library(hk_lib *lib, const char *what, const char *scratch)
{
    char buf[128], word[16];
    int a = 0, *counter;
    float f = 0;
    double ip = 0;
    hk_ld_stats st;
    say("guest library: %s", what);
    hk_ld_get_stats(lib, &st);
    CHECK_INT(st.unresolved, 0);
    CHECK_INT(st.tp_reads_failed, 0);

    /* Relocations, data, constructors. */
    CHECK_INT(FN(lib, int (*)(void), "t_ctor_ran")(), 1234);
    CHECK_INT(FN(lib, int (*)(int, int), "t_add")(40, 2), 42);
    counter = FN(lib, int *(*)(void), "t_counter_ptr")();
    CHECK(counter && *counter == 5);
    CHECK(!strcmp(FN(lib, const char *(*)(int), "t_name")(2), "two"));
    CHECK(!strcmp(FN(lib, const char *(*)(void), "t_hello")(), "hello from guest"));
    CHECK(SYM(lib, "t_exported_value") && *(int *)SYM(lib, "t_exported_value") == 0x1234);

    /* Floating point across the boundary, both ways. */
    CHECK(near(hk_d(FN(lib, u64 (*)(u64, u32), "t_fmul")(hk_db(1.5), hk_fb(4.0f))), 6.0));
    /* (int, float, double, float, int, double): r0, r1, [r2:r3], then the stack -- float, int, pad, double. */
    CHECK(near(hk_f(FN(lib, u32 (*)(int, u32, u64, u32, int, u64), "t_mixed")(1, hk_fb(2.0f), hk_db(3.0), hk_fb(4.0f), 5, hk_db(6.5))), 21.5));
    CHECK(near(hk_f(FN(lib, u32 (*)(u32), "t_sqrt")(hk_fb(2.25f))), 1.5));
    CHECK(near(hk_d(FN(lib, u64 (*)(u64), "t_sin")(hk_db(0.5))), sin(0.5)));
    CHECK(near(hk_d(FN(lib, u64 (*)(u64, u64), "t_pow")(hk_db(2.0), hk_db(10.0))), 1024.0));
    CHECK_INT(FN(lib, long (*)(u64), "t_lround")(hk_db(-2.5)), -3);
    CHECK(near(hk_d(FN(lib, u64 (*)(u64, double *), "t_modf")(hk_db(3.75), &ip)), 0.75) && near(ip, 3.0));
    CHECK(near(hk_d(FN(lib, u64 (*)(const char *), "t_strtod")("-12.5e2")), -1250.0));
    CHECK(near(hk_d(FN(lib, u64 (*)(void *, u64, u64), "t_call_d")((void *)host_hypot, hk_db(3.0), hk_db(4.0))), 5.0));

    /* Variadics. */
    CHECK_INT(FN(lib, int (*)(char *, int), "t_format")(buf, sizeof(buf)), (int)strlen("42 str 3.142 1234567890123 z beef"));
    CHECK(!strcmp(buf, "42 str 3.142 1234567890123 z beef"));
    CHECK_INT(FN(lib, int (*)(const char *, int *, float *, char *), "t_scan")("17 2.5 word", &a, &f, word), 3);
    CHECK(a == 17 && near(f, 2.5) && !strcmp(word, "word"));
    CHECK_INT(FN(lib, int (*)(void), "t_sum")(), 21);
    FN(lib, void (*)(void), "t_log")();

    /* Integer helpers the compiler calls. */
    CHECK(FN(lib, i64 (*)(i64, i64), "t_div64")(-1234567890123ll, 1000) == -1234567890ll);
    CHECK(FN(lib, i64 (*)(i64, i64), "t_mod64")(-1234567890123ll, 1000) == -123);
    CHECK(FN(lib, u64 (*)(u64, u64), "t_udiv64")(0xFFFFFFFFFFFFFFFFull, 0x10000) == 0xFFFFFFFFFFFFull);
    CHECK_INT(FN(lib, unsigned (*)(unsigned, unsigned), "t_udiv")(1000000, 7), 142857);
    CHECK_INT(FN(lib, int (*)(int, int), "t_mod")(-17, 5), -2);

    /* The heap, callbacks, setjmp, errno. */
    CHECK_INT(FN(lib, int (*)(void), "t_heap")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_sort")(), 1);
    CHECK_INT(FN(lib, int (*)(void *, int), "t_callback")((void *)host_times_three, 5), 16);
    CHECK_INT(FN(lib, int (*)(void), "t_setjmp")(), 77);
    CHECK_INT(FN(lib, int (*)(void), "t_errno")(), GE_NOENT);

    /* Thread-local storage through the thread pointer, and threads. */
    CHECK_INT(FN(lib, int (*)(void), "t_tls_get")(), 100);
    FN(lib, void (*)(int), "t_tls_set")(555);
    CHECK_INT(FN(lib, int (*)(void), "t_tls_get")(), 555);
    CHECK(*FN(lib, int *(*)(void), "t_tls_addr")() == 555);
    FN(lib, void (*)(int), "t_tls_set")(100);
    CHECK_INT(FN(lib, int (*)(int, int), "t_threads")(8, 2000), 16000);
    /* Four thousand megabytes mapped and unmapped in two pieces each: the addresses come back. */
    CHECK_INT(FN(lib, int (*)(int), "t_map_pieces")(4096), 0);
    /* A 300 KB frame written bottom first, on this thread and on a new one. */
    CHECK_INT(FN(lib, int (*)(int), "t_big_frame")(10), 56);
    /* Signals between threads: a thread stopped and let go twenty times, wherever it was found. */
    CHECK_INT(FN(lib, int (*)(int), "t_signals")(20), 0);
    {
        char report[200];
        hk_thread_interrupt_report(report, sizeof(report));
        say("  threads interrupted in guest code: %s", report);
    }

    /* Files, the made-up /proc, properties, dlopen. */
    snprintf(buf, sizeof(buf), "/scratch");
    (void)scratch;
    CHECK_INT(FN(lib, int (*)(const char *), "t_files")(buf), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_proc")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_props")(), 28);
    CHECK_INT(FN(lib, int (*)(void), "t_dl")(), 4);
    CHECK_INT(FN(lib, int (*)(const char *), "t_stack")("a string that fits"), 18);
}

/*
 * The app's libraries and the runtime's, kept apart (ld/hk_ld.h): there is a
 * libt_same.so of each kind, and a library of each kind that needs
 * "libt_same.so". The runtime's are loaded first, so that anything that
 * looked for a library or a symbol by name alone would find the wrong one.
 */
static void test_guest_kinds(const char *libdir)
{
    char *runtime_dir = hk_sprintf("%s/runtime", libdir);
    hk_lib *ruser, *user, *rsame, *same, *in = NULL;
    say("guest libraries: the app's and the runtime's of one name");
    hk_ld_add_runtime_dir(runtime_dir);
    free(runtime_dir);
    /* Each kind is looked for only where it is kept. */
    CHECK(hk_ld_load_runtime("libt_basic.so") == NULL && hk_ld_load("libt_ruser.so") == NULL);
    ruser = hk_ld_load_runtime("libt_ruser.so");
    CHECK(ruser && hk_ld_init(ruser) && hk_ld_lib_is_runtime(ruser));
    /* What it needs was loaded with it, as the runtime's; the app has no library of that name yet. */
    rsame = hk_ld_find_runtime_lib("libt_same.so");
    CHECK(rsame && hk_ld_lib_is_runtime(rsame) && hk_ld_find_lib("libt_same.so") == NULL);
    user = hk_ld_load("libt_user.so");
    CHECK(user && hk_ld_init(user) && !hk_ld_lib_is_runtime(user));
    same = hk_ld_find_lib("libt_same.so");
    CHECK(same && same != rsame && !hk_ld_lib_is_runtime(same) && hk_ld_find_runtime_lib("libt_same.so") == rsame);
    if (!ruser || !user || !same || !rsame) return;
    /* Each got the libt_same.so of its own kind. */
    CHECK_INT(FN(ruser, int (*)(void), "t_ruser_which")(), 202);
    CHECK_INT(FN(user, int (*)(void), "t_user_which")(), 101);
    /* A name only the other kind exports is not bound across: the weak import stays zero on both sides. */
    CHECK_INT(FN(ruser, int (*)(void), "t_ruser_stray")(), -1);
    CHECK_INT(FN(user, int (*)(void), "t_user_stray")(), -1);
    /* dlsym(RTLD_DEFAULT), which is the app's: its own libraries, never the runtime's. */
    CHECK(hk_ld_sym(NULL, "t_same_app_only") != NULL && hk_ld_sym(NULL, "t_same_runtime_only") == NULL && hk_ld_sym(NULL, "t_ruser_which") == NULL);
    CHECK(hk_ld_lib_of(hk_ld_sym(NULL, "t_same_which")) == same);
    /* And by kind: where the runtime looks for its classes' native methods, and the app for its own. */
    CHECK(hk_ld_sym_among(true, "t_same_which", &in) != NULL && in == rsame);
    CHECK(hk_ld_sym_among(false, "t_same_which", &in) != NULL && in == same);
    CHECK(hk_ld_sym_among(true, "t_user_which", &in) == NULL && in == NULL && hk_ld_sym_among(false, "t_ruser_which", NULL) == NULL);
    /* Loading either again by name gives the one already there. */
    CHECK(hk_ld_load("libt_same.so") == same && hk_ld_load_runtime("libt_same.so") == rsame);
}

static void test_guest(const char *libdir, const char *scratch)
{
    hk_lib *basic, *dep;
    hk_ld_stats st;
    hk_ld_add_search_dir(libdir);
    hk_fs_mount("/scratch", scratch);

    basic = hk_ld_load("libt_basic.so");
    CHECK(basic != NULL);
    if (!basic) return;
    CHECK(hk_ld_init(basic));
    hk_ld_get_stats(basic, &st);
    CHECK(st.tp_reads >= 4);                        /* the initial-exec TLS accesses */
    CHECK(st.thumb_functions > 20 && st.arm_functions == 0);
    test_library(basic, "Thumb-2", scratch);

    dep = hk_ld_load("libt_dep.so");
    CHECK(dep != NULL);
    if (dep) {
        u32 before = hk_core_plt_rescues();
        say("guest library: calls through a rewritten PLT");
        CHECK(hk_ld_init(dep));
        hk_ld_get_stats(dep, &st);
        CHECK_INT(st.plt_entries, 5);
        CHECK_INT(FN(dep, int (*)(int, int), "d_add_twice")(1, 2), 5);
        CHECK_INT(FN(dep, int (*)(void), "d_bump")(), 6);
        CHECK_INT(*FN(basic, int *(*)(void), "t_counter_ptr")(), 6);
        CHECK_INT(FN(dep, int (*)(void), "d_hello_len")(), 16);
        CHECK(near(hk_d(FN(dep, u64 (*)(u64, u32), "d_fmul")(hk_db(2.0), hk_fb(3.0f))), 7.0));
        CHECK_INT(FN(dep, int (*)(void), "d_value")(), 0x1234);
        CHECK(FN(dep, void *(*)(void), "d_add_ptr")() == SYM(basic, "t_add"));
        CHECK_INT(FN(dep, int (*)(int), "d_spin")(2000000), 2000000);
        say("  PLT entries rescued after an interrupt: %u", hk_core_plt_rescues() - before);
    }

    CHECK(hk_ld_find_lib("libt_basic.so") == basic && hk_ld_lib_of(SYM(basic, "t_add")) == basic);
    CHECK(hk_ld_load("libc.so") == NULL && hk_ld_load("libnothere.so") == NULL);
    test_guest_kinds(libdir);
}

/*
 * The same library built in ARM mode, run as it is. Last of all: on a host
 * that does not keep a thread in ARM state this is where the process ends,
 * and everything else has been said by then. When the probe has already found
 * that out (hk_core_arm_state), it is not attempted.
 */
static void test_guest_arm(const char *scratch)
{
    hk_lib *arm;
    hk_ld_stats st;
    int state = hk_core_arm_state();
    if (state < 0) {
        say("guest library: ARM mode -- not run: the probe found that this device does not keep a thread in ARM state");
        return;
    }
    if (state == 0) say("(the probe has not been run here: if the log ends in the next section, ARM-mode code does not survive on this device)");
    arm = hk_ld_load("libt_basic_arm.so");
    CHECK(arm != NULL);
    if (arm) {
        CHECK(hk_ld_init(arm));
        hk_ld_get_stats(arm, &st);
        CHECK(st.arm_functions > 20);
        test_library(arm, "ARM mode, packed relocations", scratch);
    }
}

/* The mechanisms the port rests on, tried directly. */
static void test_mechanisms(void)
{
    u32 before = hk_core_plt_rescues();
    void *entry = hk_stub(HK_STUB_ARM_ONCE_ENTRY);
    say("mechanisms");
    /* One ARM instruction and back, as a rewritten PLT entry is. */
    CHECK_INT(((int (*)(int))hk_stub(HK_STUB_ARM_ONCE))(41), 42);
    /* The same four bytes entered in Thumb state -- what Windows does to a thread interrupted there -- must fault and be rescued. */
    CHECK_INT(((int (*)(int))((hk_addr)entry | 1u))(7), 7);
    CHECK_INT(hk_core_plt_rescues() - before, 1);
}

#include "hk_tests_app.inc"

#endif /* HK_HOST_ARM32 */

/*
 * The Dalvik runtime's tests (hk_dvm_tests.c). They start the runtime, which
 * from then on is part of the Java world native code sees: so they come after
 * everything that tests that world as it is without one.
 */
static const char *g_dvm_fixtures, *g_dvm_apk, *g_dvm_android;
static int g_dvm_trace;

void hk_tests_set_dvm_android(const char *root) { g_dvm_android = root; }

void hk_tests_set_dvm(const char *fixtures, const char *apk, int trace)
{
    g_dvm_fixtures = fixtures;
    g_dvm_apk = apk;
    g_dvm_trace = trace;
}

/* A path beside a folder: its last part replaced (not ".." on the end, which not every way of opening a file here resolves). */
static char *beside_folder(const char *dir, const char *name)
{
    char *parent = hk_strdup(dir), *out;
    size_t n = strlen(parent);
    while (n && (parent[n - 1] == '/' || parent[n - 1] == '\\')) parent[--n] = 0;
    while (n && parent[n - 1] != '/' && parent[n - 1] != '\\') n--;
    parent[n] = 0;
    out = hk_sprintf("%s%s", n ? parent : "./", name);
    free(parent);
    return out;
}

static void test_dvm(const char *libdir, const char *scratch, bool guest_code)
{
    hk_dvm_test_config config;
    char *beside = !g_dvm_fixtures && libdir ? hk_sprintf("%s/dvm", libdir) : NULL;
    /* The Android root: in the app's package it is the folder `android` beside the one the guest libraries are in. */
    char *android = !g_dvm_android && libdir ? beside_folder(libdir, "android/framework/core-oj.dex") : NULL;
    char *java_scratch = hk_sprintf("%s/java", scratch);
    hk_stat st;
    int checks = 0;
    if (!g_dvm_fixtures && !beside) {
        say("(no folder of DEX fixtures was named: the Dalvik runtime's tests are skipped)");
        return;
    }
    memset(&config, 0, sizeof(config));
    config.fixtures = g_dvm_fixtures ? g_dvm_fixtures : beside;
    config.apk = g_dvm_apk;
    config.guest_dir = guest_code ? libdir : NULL;
    config.trace = g_dvm_trace;
    config.scratch = java_scratch;
    /* Android's libcore, where it has been put beside the guest libraries (the app's package) or named. */
    if (g_dvm_android && g_dvm_android[0]) config.android_root = g_dvm_android;
    else if (android && hk_path_stat(android, &st, NULL)) {
        android[strlen(android) - strlen("/framework/core-oj.dex")] = 0;
        config.android_root = android;
    }
    g_failed += hk_dvm_tests_run(&config, g_sink, g_sink_user, &checks);
    g_checks += checks;
    /* megler.Gfx's native methods make their arrays and strings with the runtime, which is running from here on. */
    checks = 0;
    g_failed += hk_gfx_natives_tests_run(g_sink, g_sink_user, &checks);
    g_checks += checks;
    free(android);
    free(java_scratch);
    free(beside);
}

/*
 * SQLite as the core has it (sqlite/hk_sqlite.c): its files through the host
 * layer, its locks kept in the process, its mutexes the host's. On every host,
 * since the file layer under it is each host's own. Where the core was built
 * without SQLite this says so and checks only that it knows.
 */
#if defined(HK_HAVE_SQLITE) && HK_HAVE_SQLITE
static int sq_int(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int v = -999;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void test_sqlite(const char *scratch)
{
    char *path = hk_sprintf("%s/sqlite-test.db", scratch), *journal = hk_sprintf("%s/sqlite-test.db-journal", scratch);
    sqlite3 *a = NULL, *b = NULL, *m = NULL;
    sqlite3_stmt *st = NULL;
    hk_stat fs;
    say("SQLite %s, compiled into the core, on the host layer's files", sqlite3_libversion());
    CHECK(hk_sqlite_available() && hk_sqlite_start() && hk_sqlite_start());
    CHECK(sqlite3_vfs_find(NULL) && !strcmp(sqlite3_vfs_find(NULL)->zName, "megler") && sqlite3_threadsafe() == 1);
    hk_path_remove(path, NULL);
    hk_path_remove(journal, NULL);
    /* A database that is not there is not made unless asked. */
    CHECK(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE, NULL) == SQLITE_CANTOPEN);
    sqlite3_close(a);
    a = NULL;
    CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, v REAL); INSERT INTO t(name, v) VALUES('one', 1.5), ('two', 2.5), ('three', 3.5);", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_changes(a), 3);
    CHECK(sqlite3_last_insert_rowid(a) == 3);
    /* It is a file of the host's, with SQLite's header, and no journal is left beside it. */
    CHECK(hk_path_stat(path, &fs, NULL) && fs.size >= 1024 && !hk_path_stat(journal, &fs, NULL));
    {
        size_t n = 0;
        u8 *head = hk_file_read_all(path, &n);
        CHECK(head && n >= 16 && !memcmp(head, "SQLite format 3", 16));
        free(head);
    }
    /* A statement with an argument, stepped. */
    CHECK_INT(sqlite3_prepare_v2(a, "SELECT name, v FROM t WHERE v > ?1 ORDER BY id", -1, &st, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_bind_double(st, 1, 2.0), SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "two") && sqlite3_column_double(st, 1) == 2.5);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "three") && sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    /* A transaction rolled back: its journal is there while it is open and gone after, and so is what it did. */
    CHECK_INT(sqlite3_exec(a, "BEGIN; DELETE FROM t WHERE id > 1;", NULL, NULL, NULL), SQLITE_OK);
    CHECK(sq_int(a, "SELECT count(*) FROM t") == 1 && hk_path_stat(journal, &fs, NULL));
    CHECK_INT(sqlite3_exec(a, "ROLLBACK", NULL, NULL, NULL), SQLITE_OK);
    CHECK(sq_int(a, "SELECT count(*) FROM t") == 3 && !hk_path_stat(journal, &fs, NULL));
    /* A second connection to the same file: it reads beside a writer that has not written yet, cannot write while that one means to, and can after. */
    CHECK_INT(sqlite3_open_v2(path, &b, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
    sqlite3_busy_timeout(b, 0);
    CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sq_int(b, "SELECT count(*) FROM t"), 3);
    CHECK_INT(sqlite3_exec(b, "INSERT INTO t(name) VALUES('refused')", NULL, NULL, NULL), SQLITE_BUSY);
    CHECK_INT(sqlite3_exec(a, "INSERT INTO t(name, v) VALUES('four', 4.5); COMMIT", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_exec(b, "INSERT INTO t(name) VALUES('five')", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), 5);
    /* A reader in the middle of its rows keeps a writer from committing, and lets it once it is done. */
    CHECK_INT(sqlite3_prepare_v2(b, "SELECT id FROM t", -1, &st, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_step(st), SQLITE_ROW);
    sqlite3_busy_timeout(a, 0);
    CHECK_INT(sqlite3_exec(a, "UPDATE t SET v = 0 WHERE id = 1", NULL, NULL, NULL), SQLITE_BUSY);
    sqlite3_finalize(st);
    CHECK_INT(sqlite3_exec(a, "UPDATE t SET v = 0 WHERE id = 1", NULL, NULL, NULL), SQLITE_OK);
    /* No write-ahead log (it needs memory shared between connections): asked for, the journal stays what it was. */
    CHECK_INT(sqlite3_prepare_v2(a, "PRAGMA journal_mode=WAL", -1, &st, NULL), SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "delete"));
    sqlite3_finalize(st);
    CHECK(sqlite3_close(a) == SQLITE_OK && sqlite3_close(b) == SQLITE_OK);
    /* Closed and opened again: what was committed is there. */
    a = NULL;
    CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    CHECK(sq_int(a, "SELECT count(*) FROM t") == 5 && sq_int(a, "SELECT v FROM t WHERE id = 1") == 0);
    CHECK_INT(sqlite3_exec(a, "DELETE FROM t", NULL, NULL, NULL), SQLITE_READONLY);
    sqlite3_close(a);
    /* A database in memory, with a full-text table, as Android's build has them; and the clock. */
    CHECK_INT(sqlite3_open_v2(":memory:", &m, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_exec(m, "CREATE VIRTUAL TABLE f USING fts4(body); INSERT INTO f VALUES('a quick brown fox'), ('a lazy dog'), ('quick, quick');", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sq_int(m, "SELECT count(*) FROM f WHERE body MATCH 'quick'"), 2);
    CHECK(sq_int(m, "SELECT CAST(strftime('%Y', 'now') AS INTEGER)") >= 2024);
    CHECK(sq_int(m, "SELECT abs(random()) >= 0") == 1);
    sqlite3_close(m);
    free(path);
    free(journal);
}
#else
static void test_sqlite(const char *scratch)
{
    (void)scratch;
    say("(this build has no SQLite in it: build/sqlite/sqlite3.c was not there)");
    CHECK(!hk_sqlite_available() && !hk_sqlite_start());
}
#endif

int hk_tests_run(const char *libdir, const char *scratch, hk_test_sink sink, void *user)
{
    bool can_run;
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    can_run = hk_core_init();
    hk_path_mkdirs(scratch);
    test_host(scratch);
    g_failed += hk_storage_tests_run(scratch, g_sink, g_sink_user, &g_checks);
    test_zip(scratch);
    test_sqlite(scratch);
    test_textures();
    test_media(scratch);
    test_battery();
    test_input();
    {
        /* The stand-in for the shell's drawing under a Java app: it is what the Java driver's tests see pictures through. */
        int checks = 0;
        hk_gfx_tests_set_scratch(scratch);
        g_failed += hk_gfx_tests_run(g_sink, g_sink_user, &checks);
        g_checks += checks;
    }
#if HK_HOST_ARM32
    test_proxies();
    if (can_run) {
        test_mechanisms();
        if (libdir) {
            test_guest(libdir, scratch);
            test_app(libdir);
            test_dvm(libdir, scratch, true);
            test_guest_arm(scratch);
        } else {
            test_dvm(NULL, scratch, false);
        }
    } else {
        g_failed++;
        say("FAIL: executable memory was refused; guest code cannot run");
        test_dvm(libdir, scratch, false);
    }
#else
    (void)can_run;
    say("(not a 32-bit ARM host: the guest-code tests are skipped)");
    test_dvm(libdir, scratch, false);
#endif
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
