/*
 * The core's tests.
 *
 *   mg_tests <guest library dir> <scratch dir>
 *
 * Everything that needs no ARM code runs anywhere (the x64 build on this PC).
 * The rest loads the guest test libraries (tools/build-guest.sh) and calls
 * into them, which needs a 32-bit ARM host: the same tests run under qemu-arm
 * (tools/build-tests.sh arm) and, linked into the app, on the phone itself.
 *
 * A guest function with floating-point parameters is called here through a
 * pointer typed with u32/u64 -- the softfp convention, spelled out.
 */
#include "mg_camera_standin.h"
#include "mg_codec_standin.h"
#include "mg_gfx_tests.h"
#include "mg_net_tests.h"
#include "mg_storage_tests.h"
#include "mg_tests.h"

#include <math.h>
#include <stdio.h>

#include "../src/arm/mg_kuser.h"
#include "../src/arm/mg_stubs.h"
#include "../src/bionic/mg_bionic.h"
#include "../src/bionic/mg_fd.h"
#include "../src/drivers/mg_java.h"
#include "../src/mg_core.h"
#include "../src/host/mg_host.h"
#include "../include/megler.h"
#include "../src/jni/mg_jni.h"
#include "../src/jni/mg_jni_loop.h"
#include "../src/ld/mg_ld.h"
#include "../src/ndk/mg_egl.h"
#include "../src/ndk/mg_media.h"
#include "../src/sqlite/mg_sqlite.h"
#if defined(MG_HAVE_SQLITE) && MG_HAVE_SQLITE
#include "sqlite3.h"
#endif
#include "../src/ndk/mg_ndk.h"
#include "../src/run/mg_run.h"
#include "../src/appx/mg_appx.h"
#include "../src/apk/mg_arsc.h"
#include "zlib.h"
#include "../src/zip/mg_zip.h"

static int g_checks, g_failed;
static mg_test_sink g_sink;
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
    mg_sleep_ns(20000000ull);
    mg_atomic_xchg(&g_futex_word, 1);
    mg_futex_wake(&g_futex_word, true);
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
    mg_strlcpy(s->last, name, sizeof(s->last));
}

/*
 * Positioned reads of one file from two threads at once, each at offsets of its own: every word of the file is its own
 * offset, and each read has to see its own (mg_file_pread's offset goes with the call; seeking first, the two took each
 * other's offsets).
 */
typedef struct pread_race {
    mg_file *f;
    u32 seed, bad, done;
} pread_race;

static void pread_racer(void *arg)
{
    pread_race *r = (pread_race *)arg;
    u32 i, x = r->seed, word = 0;
    int err = 0;
    for (i = 0; i < 3000; i++) {
        u32 at;
        x = x * 1103515245u + 12345u;
        at = ((x >> 8) % (256u * 1024u)) * 4u;
        if (mg_file_pread(r->f, &word, 4, at, &err) != 4 || word != at) r->bad++;
        r->done++;
    }
}

static void test_pread_threads(const char *scratch)
{
    char *path = mg_sprintf("%s/pread-race.bin", scratch);
    u32 *words = (u32 *)mg_alloc(1024u * 1024u), i;
    pread_race a, b;
    mg_thread *ta, *tb;
    mg_file *f;
    int err = 0;
    say("files: positioned reads from two threads at once, each at its own offsets");
    for (i = 0; i < 256u * 1024u; i++) words[i] = i * 4u;
    f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    CHECK(f && mg_file_write(f, words, 1024u * 1024u, &err) == 1024 * 1024);
    if (f) mg_file_close(f);
    f = mg_file_open(path, MG_O_READ, &err);
    CHECK(f != NULL);
    if (f) {
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        a.f = b.f = f;
        a.seed = 1;
        b.seed = 77;
        ta = mg_thread_start(pread_racer, &a, 256u * 1024u, "pread-a");
        tb = mg_thread_start(pread_racer, &b, 256u * 1024u, "pread-b");
        if (ta) mg_thread_join(ta);
        if (tb) mg_thread_join(tb);
        say("  %u and %u reads, %u and %u of them not at their offset", a.done, b.done, a.bad, b.bad);
        CHECK(ta && tb && a.done == 3000 && b.done == 3000 && a.bad == 0 && b.bad == 0);
        mg_file_close(f);
    }
    free(words);
    free(path);
}

static void test_host(const char *scratch)
{
    u8 *p;
    char *path, *sub;
    mg_file *f;
    mg_stat st;
    mg_thread *t;
    int err = 0;
    u64 t0;
    say("host (%s)", mg_host_name());

    p = (u8 *)mg_mem_reserve(1 << 20);
    CHECK(p != NULL);
    if (p) {
        CHECK(mg_mem_commit(p + 65536, 8192, MG_PROT_RW));
        p[65536] = 0x42;
        p[65536 + 8191] = 0x24;
        CHECK(p[65536] == 0x42 && p[65536 + 4096] == 0);
        CHECK(mg_mem_protect(p + 65536, 4096, MG_PROT_R));
        CHECK(p[65536] == 0x42);
        CHECK(mg_mem_decommit(p + 65536, 8192));
        mg_mem_release(p, 1 << 20);
    }

    /* A section: two views of the same pages at addresses of their own; a view outlives the other and the section's close. */
    {
        mg_section *s = mg_section_create(3 * 65536);
        void *base_a = NULL, *base_b = NULL;
        u8 *a = s ? (u8 *)mg_section_map(s, 0, 3 * 65536, true, &base_a) : NULL;
        u8 *b = s ? (u8 *)mg_section_map(s, 65536, 2 * 65536, false, &base_b) : NULL;
        CHECK(s != NULL && a != NULL && b != NULL && a != b);
        if (a && b) {
            CHECK(a[65536 + 7] == 0);
            a[65536 + 7] = 0x5A;
            a[3 * 65536 - 1] = 0xA5;
            CHECK(b[7] == 0x5A && b[2 * 65536 - 1] == 0xA5);
            mg_section_unmap(base_a, 3 * 65536);
            mg_section_close(s);
            CHECK(b[7] == 0x5A);
            mg_section_unmap(base_b, 2 * 65536 + (size_t)(b - (u8 *)base_b));
        }
    }
#if defined(__arm__) || defined(_M_ARM)
    /* Code written through one view of a section and run through another, which may be run (mg_section_map_code): a hook's
       two mappings of one memfd (Facebook's). Thumb: movs r0, #42; bx lr. */
    {
        mg_section *s = mg_section_create(65536);
        void *base_w = NULL, *base_x = NULL;
        u16 *w = s ? (u16 *)mg_section_map(s, 0, 65536, true, &base_w) : NULL;
        u8 *x = s ? (u8 *)mg_section_map_code(s, 0, 65536, &base_x) : NULL;
        CHECK(s != NULL && w != NULL && x != NULL);
        if (w && x) {
            w[0] = 0x202A;
            w[1] = 0x4770;
            mg_icache_flush(x, 4);
            CHECK_INT(((int (*)(void))((mg_addr)x | 1))(), 42);
        }
        if (w) mg_section_unmap(base_w, 65536);
        if (x) mg_section_unmap(base_x, 65536 + (size_t)(x - (u8 *)base_x));
        if (s) mg_section_close(s);
    }
#endif

    t0 = mg_time_monotonic_ns();
    mg_sleep_ns(5000000ull);
    CHECK(mg_time_monotonic_ns() - t0 >= 4000000ull);
    CHECK(mg_time_realtime_ns() > 1600000000ull * 1000000000ull);

    g_futex_word = 0;
    t = mg_thread_start(futex_waker, NULL, 0, "waker");
    CHECK(t != NULL);
    while (!g_futex_word) mg_futex_wait(&g_futex_word, 0, 0);
    if (t) mg_thread_join(t);
    CHECK(!mg_futex_wait(&g_futex_word, 1, 2000000ull));       /* nothing wakes it: the time runs out */
    CHECK(mg_futex_wait(&g_futex_word, 0, 2000000ull));        /* the value differs: returns at once */

    sub = mg_sprintf("%s/host/a/b", scratch);
    CHECK(mg_path_mkdirs(sub));
    path = mg_sprintf("%s/file.bin", sub);
    f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    CHECK(f != NULL);
    if (f) {
        char buf[16];
        CHECK_INT(mg_file_write(f, "0123456789", 10, &err), 10);
        mg_file_close(f);
        f = mg_file_open(path, MG_O_READ | MG_O_WRITE, &err);
        CHECK(f != NULL);
        CHECK_INT(mg_file_pread(f, buf, 4, 3, &err), 4);
        CHECK(!memcmp(buf, "3456", 4));
        CHECK_INT(mg_file_read(f, buf, 2, &err), 2);       /* the positioned read left the position alone */
        CHECK(!memcmp(buf, "01", 2));
        CHECK_INT(mg_file_seek(f, -2, 2, &err), 8);
        CHECK_INT(mg_file_read(f, buf, 10, &err), 2);
        CHECK(mg_file_truncate(f, 4, &err));
        CHECK(mg_file_stat(f, &st) && st.size == 4 && !st.is_dir);
        mg_file_close(f);
    }
    CHECK(mg_path_stat(sub, &st, &err) && st.is_dir);
    {
        /* Listing, renaming, and the whole-file helpers. */
        char *other = mg_sprintf("%s/renamed.bin", sub);
        list_seen seen;
        size_t n = 0;
        u8 *all;
        bool dst = false;
        i32 offset = mg_time_utc_offset((i64)(mg_time_realtime_ns() / 1000000000ull), &dst);
        memset(&seen, 0, sizeof(seen));
        CHECK(mg_path_list(sub, list_one, &seen, &err));
        CHECK(seen.count == 1 && seen.files == 1 && !strcmp(seen.last, "file.bin"));
        CHECK(mg_path_rename(path, other, &err));
        CHECK(!mg_path_stat(path, &st, &err) && mg_path_stat(other, &st, &err) && st.size == 4);
        all = mg_file_read_all(other, &n);
        CHECK(all && n == 4 && !memcmp(all, "0123", 4));
        free(all);
        CHECK(mg_file_write_all(path, "back", 4));
        CHECK(mg_path_remove(other, &err));
        free(other);
        CHECK(offset > -16 * 3600 && offset < 16 * 3600);
    }
    {
        /* Paths longer than Win32's MAX_PATH, as an app's own folders and names make them (Instagram's unpacked libraries). */
        char name[121];
        char *top, *deep, *file, *dotted, *moved;
        list_seen seen;
        size_t n = 0;
        u8 *all;
        memset(name, 'n', 120);
        name[120] = 0;
        top = mg_sprintf("%s/host/long-%s", scratch, name);
        deep = mg_sprintf("%s/%s", top, name);
        file = mg_sprintf("%s/lib-%s.so", deep, name);
        dotted = mg_sprintf("%s/./%s/../%s/lib-%s.so", top, name, name, name);
        moved = mg_sprintf("%s/moved.so", deep);
        CHECK(strlen(file) > 300);
        CHECK(mg_path_mkdirs(deep));
        CHECK(mg_file_write_all(file, "long", 4));
        CHECK(mg_path_stat(file, &st, &err) && st.size == 4 && !st.is_dir);
        all = mg_file_read_all(dotted, &n);
        CHECK(all && n == 4 && !memcmp(all, "long", 4));
        free(all);
        memset(&seen, 0, sizeof(seen));
        CHECK(mg_path_list(deep, list_one, &seen, &err) && seen.count == 1 && seen.files == 1);
        CHECK(mg_path_rename(file, moved, &err) && mg_path_stat(moved, &st, &err) && st.size == 4);
        CHECK(mg_path_remove(moved, &err));
        CHECK(mg_path_rmdir(deep, &err) && mg_path_rmdir(top, &err));
        free(top);
        free(deep);
        free(file);
        free(dotted);
        free(moved);
    }
    CHECK(mg_file_open(sub, MG_O_READ, &err) == NULL && err == MG_E_ISDIR);
    CHECK(!mg_path_stat("/no/such/thing/at/all", &st, &err) && err == MG_E_NOENT);
    CHECK(!mg_path_rmdir(sub, &err) && err == MG_E_NOTEMPTY);
    CHECK(mg_path_remove(path, &err));
    CHECK(mg_path_rmdir(sub, &err));
    free(path);
    free(sub);
}

/* -------------------------------------------------------------------- zip */

/* A ZIP with one stored and one deflated entry, written by hand so the reader is tested against the format, not against itself. */
#ifdef _WIN32
/* (Declared by hand: this file has no windows.h.) */
__declspec(dllimport) void *__stdcall CreateFileA(const char *name, unsigned long access, unsigned long share, void *security,
                                                   unsigned long disposition, unsigned long attributes, void *templ);

/* A file by handle's opener: the file of the path `user`, read, a handle of its own each time. */
static void *open_by_path(void *user) { return CreateFileA((const char *)user, 0x80000000ul, 1ul | 4ul, NULL, 3ul, 0x80ul, NULL); }
#endif

static void test_zip(const char *scratch)
{
    static const u8 stored[] = "stored entry";
    /* "aaaaaaaaaaaaaaaaaaaa" (20 bytes) as a raw deflate stream */
    static const u8 deflated[] = { 0x4B, 0x4C, 0xC4, 0x04, 0x00 };
    u8 zip[512];
    size_t n = 0, cd, i;
    char *path = mg_sprintf("%s/test.zip", scratch), err[80];
    mg_zip z;
    struct { const char *name; const u8 *data; u32 csize, usize, crc; u16 method; u32 offset; } e[2] = {
        { "lib/armeabi-v7a/libone.so", stored, sizeof(stored) - 1, sizeof(stored) - 1, 0, 0, 0 },
        { "assets/a.txt", deflated, sizeof(deflated), 20, 0, 8, 0 },
    };
    say("zip");
    for (i = 0; i < 2; i++) {
        u16 nl = (u16)strlen(e[i].name);
        e[i].offset = (u32)n;
        mg_wr32(zip + n, 0x04034b50u); mg_wr16(zip + n + 4, 20); mg_wr16(zip + n + 6, 0); mg_wr16(zip + n + 8, e[i].method);
        mg_wr32(zip + n + 10, 0); mg_wr32(zip + n + 14, e[i].crc); mg_wr32(zip + n + 18, e[i].csize); mg_wr32(zip + n + 22, e[i].usize);
        mg_wr16(zip + n + 26, nl); mg_wr16(zip + n + 28, 0);
        memcpy(zip + n + 30, e[i].name, nl);
        memcpy(zip + n + 30 + nl, e[i].data, e[i].csize);
        n += 30 + nl + e[i].csize;
    }
    cd = n;
    for (i = 0; i < 2; i++) {
        u16 nl = (u16)strlen(e[i].name);
        memset(zip + n, 0, 46);
        mg_wr32(zip + n, 0x02014b50u); mg_wr16(zip + n + 10, e[i].method); mg_wr32(zip + n + 20, e[i].csize);
        mg_wr32(zip + n + 24, e[i].usize); mg_wr16(zip + n + 28, nl); mg_wr32(zip + n + 42, e[i].offset);
        memcpy(zip + n + 46, e[i].name, nl);
        n += 46 + nl;
    }
    memset(zip + n, 0, 22);
    mg_wr32(zip + n, 0x06054b50u); mg_wr16(zip + n + 8, 2); mg_wr16(zip + n + 10, 2); mg_wr32(zip + n + 12, (u32)(n - cd)); mg_wr32(zip + n + 16, (u32)cd);
    n += 22;
    CHECK(mg_file_write_all(path, zip, n));
    CHECK(mg_zip_open(&z, path, err, sizeof(err)));
    if (z.count) {
        const mg_zip_entry *a = mg_zip_find(&z, "assets/a.txt"), *s = mg_zip_find(&z, "lib/armeabi-v7a/libone.so");
        size_t size = 0;
        u8 *data;
        CHECK_INT(z.count, 2);
        CHECK(a && s && !mg_zip_find(&z, "assets/b.txt"));
        if (a && s) {
            {
                u64 at = 0;
                char back[sizeof(stored)];
                memset(back, 0, sizeof(back));
                CHECK(mg_zip_stored_at(&z, s, &at) && mg_zip_pread(&z, at, back, sizeof(stored) - 1) && !memcmp(back, stored, sizeof(stored) - 1));
                CHECK(!mg_zip_stored_at(&z, a, &at));
                CHECK(!mg_zip_pread(&z, z.size - 2, back, 4));       /* past the end of the file */
            }
            data = mg_zip_read(&z, a, 1000, &size);
            CHECK(data && size == 20 && data[0] == 'a' && data[19] == 'a');
            free(data);
            CHECK(!mg_zip_read(&z, a, 10, &size));          /* over the caller's limit */
        }
        mg_zip_close(&z);
    }
#ifdef _WIN32
    /* The same archive as a file by handle (mg_host.h), as the shell gives the core one the user picked: under a path
       where nothing is, each open a handle of its own. */
    {
        static const char alias[] = "C:/nowhere-at-all/Picked.APK";
        mg_file *f1, *f2;
        u8 a4[4], b4[4];
        int ferr = 0;
        say("zip: a file by handle");
        CHECK(mg_file_by_handle("c:\\NOWHERE-at-all\\picked.apk", open_by_path, path));
        CHECK(mg_zip_open(&z, alias, err, sizeof(err)) && z.count == 2 && mg_zip_find(&z, "assets/a.txt") != NULL);
        if (z.count) mg_zip_close(&z);
        f1 = mg_file_open(alias, MG_O_READ, &ferr);
        f2 = mg_file_open(alias, MG_O_READ, &ferr);
        CHECK(f1 && f2 && f1 != f2);
        if (f1 && f2) {
            CHECK(mg_file_read(f1, a4, 4, &ferr) == 4 && !memcmp(a4, "PK\3\4", 4));
            CHECK(mg_file_read(f2, b4, 4, &ferr) == 4 && !memcmp(b4, "PK\3\4", 4));   /* a position of its own */
        }
        mg_file_close(f1);
        mg_file_close(f2);
        CHECK(!mg_file_open(alias, MG_O_READ | MG_O_WRITE, &ferr));   /* never written to */
        CHECK(mg_file_by_handle(alias, NULL, NULL));
        CHECK(!mg_file_open(alias, MG_O_READ, &ferr));
    }
#endif
    /* Damaged archives are refused, not followed. */
    mg_wr32(zip + n - 22 + 16, 0x7FFFFFF0u);
    CHECK(mg_file_write_all(path, zip, n));
    CHECK(!mg_zip_open(&z, path, err, sizeof(err)));
    CHECK(mg_file_write_all(path, "not a zip at all, just text", 27));
    CHECK(!mg_zip_open(&z, path, err, sizeof(err)));
    mg_path_remove(path, NULL);
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
    mg_input_queue *queue;
    volatile i32 downs, ups, stop, idle_wakes;
    int left_readable;
} G_INPUT;

static void input_taker(void *arg)
{
    void *(*prepare)(int) = (void *(*)(int))mg_bionic_find("ALooper_prepare");
    void (*attach)(mg_input_queue *, void *, int, mg_addr, void *) = (void (*)(mg_input_queue *, void *, int, mg_addr, void *))mg_bionic_find("AInputQueue_attachLooper");
    void (*detach)(mg_input_queue *) = (void (*)(mg_input_queue *))mg_bionic_find("AInputQueue_detachLooper");
    int (*poll_all)(int, int *, int *, void **) = (int (*)(int, int *, int *, void **))mg_bionic_find("ALooper_pollAll");
    int (*get)(mg_input_queue *, void **) = (int (*)(mg_input_queue *, void **))mg_bionic_find("AInputQueue_getEvent");
    void (*finish)(mg_input_queue *, void *, int) = (void (*)(mg_input_queue *, void *, int))mg_bionic_find("AInputQueue_finishEvent");
    int (*action_of)(const void *) = (int (*)(const void *))mg_bionic_find("AMotionEvent_getAction");
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
                if (action == 0 || action == 5) mg_atomic_add(&G_INPUT.downs, 1);
                if (action == 1 || action == 6) mg_atomic_add(&G_INPUT.ups, 1);
                finish(G_INPUT.queue, event, 1);
                taken++;
            }
            if (!taken) mg_atomic_add(&G_INPUT.idle_wakes, 1);
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
    u32 a[8], c0 = (u32)b[8] | ((u32)b[9] << 8), c1 = (u32)b[10] | ((u32)b[11] << 8), bits = mg_rd32(b + 12);
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

/* A stand-in for the few OpenGL functions mg_gltex.c's ETC path calls: what the last upload or storage was given. */
static struct {
    u32 internal;
    i32 level, w, h, size;
    u8 first[16];
    bool data;
} g_tex_got;
static const char *g_tex_ext;

static void tex_got(u32 internal, i32 level, i32 w, i32 h, i32 size, const void *data)
{
    memset(&g_tex_got, 0, sizeof(g_tex_got));
    g_tex_got.internal = internal;
    g_tex_got.level = level;
    g_tex_got.w = w;
    g_tex_got.h = h;
    g_tex_got.size = size;
    g_tex_got.data = data != NULL;
    if (data && size >= 16) memcpy(g_tex_got.first, data, 16);
}
static void f_compressed_2d(u32 t, i32 level, u32 internal, i32 w, i32 h, i32 border, i32 size, const void *data) { (void)t; (void)border; tex_got(internal, level, w, h, size, data); }
static void f_compressed_sub_2d(u32 t, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, i32 size, const void *data) { (void)t; (void)x; (void)y; tex_got(format, level, w, h, size, data); }
static void f_storage_2d(u32 t, i32 levels, u32 internal, i32 w, i32 h) { (void)t; tex_got(internal, levels, w, h, 0, NULL); }
static void f_bind_texture(u32 t, u32 name) { (void)t; (void)name; }
static void f_delete_textures(i32 n, const u32 *names) { (void)n; (void)names; }
static const u8 *f_get_string(u32 name) { return name == 0x1F03 ? (const u8 *)g_tex_ext : NULL; }
static void *f_tex_resolve(const char *name)
{
    if (!strcmp(name, "glCompressedTexSubImage2D")) return (void *)f_compressed_sub_2d;
    if (!strcmp(name, "glBindTexture")) return (void *)f_bind_texture;
    if (!strcmp(name, "glGetString")) return (void *)f_get_string;
    if (!strcmp(name, "glTexStorage2D")) return (void *)f_storage_2d;
    return NULL;
}

/*
 * ETC textures given to ANGLE in a format Direct3D has (mg_gltex.c). The decoder of RGBA8 ETC2 EAC blocks against ANGLE's
 * own (image_util/loadimage_etc.cpp, LoadETC2RGBA8ToRGBA8, built apart over the same 65536 blocks of a xorshift's bytes:
 * every mode -- individual, differential, T, H, planar -- and every EAC table comes up): the FNV-1a hash of all the pixels.
 * A block packed again is the DXT5 of what it decodes to. Then the uploads, through the wrappers, over a stand-in that
 * says what it was given: RGBA8 ETC2 EAC as DXT5, its blocks transcoded, at the first level and the next; ETC2 RGB8 as
 * ANGLE's lossy-decode format, the data as it was; a texture whose base level is not whole blocks as the app gave it, all
 * its levels; storage of sRGB RGBA8 ETC2 EAC as sRGB DXT5 and a part of a level uploaded into it so; without ANGLE's
 * lossy decoding, the RGB ones as they are. And what the textures held come to: the levels made as ANGLE keeps them (four
 * bytes a pixel for the ETC it decodes), a texture's storage, less a texture deleted. And the RGB decoders (ETC2 RGB8, its
 * punch-through kind) against ANGLE's own (LoadETC2RGB8ToRGBA8, LoadETC2RGB8A1ToRGBA8, the same way); a level halved, the
 * encoding of the mean of its decoding; and textures from 1024 pixels a side given at half their size: storage, with a
 * level fewer, a part of a level on whole blocks of the half and one that is not (not given), two levels, and a texture
 * of 512 left whole.
 */
static void test_etc(void)
{
    typedef void (*compressed_fn)(u32, i32, u32, i32, i32, i32, i32, const void *);
    typedef void (*sub_fn)(u32, i32, i32, i32, i32, i32, u32, i32, const void *);
    typedef void (*storage_fn)(u32, i32, u32, i32, i32);
    typedef void (*bind_fn)(u32, u32);
    typedef void (*delete_fn)(i32, const u32 *);
    u32 s = 0x12345678u, hash = 2166136261u;
    u8 block[64], px[16][4], packed[64], again[16];
    char report[600];
    compressed_fn compressed;
    sub_fn sub;
    storage_fn storage;
    bind_fn bind;
    delete_fn del;
    u32 eight = 8;
    int n, i;
    say("textures: ETC2 given to ANGLE as BC1 and DXT5");
    for (n = 0; n < 65536; n++) {
        for (i = 0; i < 16; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            block[i] = (u8)(s >> 24);
        }
        mg_gltex_etc2_eac_decode(block, px);
        for (i = 0; i < 64; i++) {
            hash ^= ((const u8 *)px)[i];
            hash *= 16777619u;
        }
    }
    CHECK_INT((long long)hash, 0x4e25546fLL);
    /* Four blocks to upload (an 8x8 level): the last random ones and three more. */
    for (i = 16; i < 64; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        block[i] = (u8)(s >> 24);
    }
    mg_gltex_etc2_eac_to_dxt5(block, 4, packed);
    mg_gltex_etc2_eac_decode(block + 16, px);
    mg_gltex_dxt5((const u8 *)px, 4, 4, 16, again);
    CHECK(!memcmp(packed + 16, again, 16));

    mg_gltex_reset();
    g_tex_ext = "GL_EXT_texture_compression_s3tc GL_EXT_texture_compression_s3tc_srgb GL_ANGLE_lossy_etc_decode";
    mg_gltex_set_host(f_tex_resolve);
    compressed = (compressed_fn)mg_gltex_wrap("glCompressedTexImage2D", (void *)f_compressed_2d);
    sub = (sub_fn)mg_gltex_wrap("glCompressedTexSubImage2D", (void *)f_compressed_sub_2d);
    storage = (storage_fn)mg_gltex_wrap("glTexStorage2D", (void *)f_storage_2d);
    bind = (bind_fn)mg_gltex_wrap("glBindTexture", (void *)f_bind_texture);
    CHECK(compressed != (compressed_fn)f_compressed_2d && sub != (sub_fn)f_compressed_sub_2d && storage != (storage_fn)f_storage_2d);
    bind(0x0DE1, 5);
    compressed(0x0DE1, 0, 0x9278, 8, 8, 0, 64, block);
    CHECK(g_tex_got.internal == 0x83F3 && g_tex_got.size == 64 && !memcmp(g_tex_got.first, packed, 16));
    compressed(0x0DE1, 1, 0x9278, 4, 4, 0, 16, block);
    CHECK(g_tex_got.internal == 0x83F3 && g_tex_got.level == 1 && !memcmp(g_tex_got.first, packed, 16));
    bind(0x0DE1, 6);
    compressed(0x0DE1, 0, 0x9274, 8, 8, 0, 32, block);
    CHECK(g_tex_got.internal == 0x9695 && !memcmp(g_tex_got.first, block, 16));
    bind(0x0DE1, 7);
    compressed(0x0DE1, 0, 0x9278, 6, 6, 0, 64, block);
    CHECK(g_tex_got.internal == 0x9278 && !memcmp(g_tex_got.first, block, 16));
    compressed(0x0DE1, 1, 0x9278, 3, 3, 0, 16, block);
    CHECK(g_tex_got.internal == 0x9278);
    bind(0x0DE1, 8);
    storage(0x0DE1, 4, 0x9279, 16, 16);
    CHECK(g_tex_got.internal == 0x8C4F);
    sub(0x0DE1, 0, 4, 4, 8, 8, 0x9279, 64, block);
    CHECK(g_tex_got.internal == 0x8C4F && !memcmp(g_tex_got.first, packed, 16));
    mg_gltex_report(report, sizeof(report));
    CHECK(strstr(report, "ETC2/EAC in 6 (0 MB of it in 4 given to ANGLE as BC1 or DXT5") != NULL);
    /* Held: 64 + 16 (texture 5), 32 (6), 256 + 64 (7, decoded by ANGLE), 352 (8's storage); and without 8 once it is deleted. */
    CHECK_INT((long)mg_gltex_held_bytes(), 784);
    del = (delete_fn)mg_gltex_wrap("glDeleteTextures", (void *)f_delete_textures);
    del(1, &eight);
    CHECK_INT((long)mg_gltex_held_bytes(), 432);

    /* The RGB decoders against ANGLE's own over 65536 eight-byte blocks each: ETC2 RGB8 and its punch-through kind. */
    {
        static const struct { u32 format, hash; } k_ref[] = { { 0x9274, 0x52d1ffa6u }, { 0x9276, 0x2877c191u } };
        int r;
        for (r = 0; r < 2; r++) {
            u32 hs = 0x12345678u, h = 2166136261u;
            for (n = 0; n < 65536; n++) {
                for (i = 0; i < 8; i++) {
                    hs ^= hs << 13;
                    hs ^= hs >> 17;
                    hs ^= hs << 5;
                    again[i] = (u8)(hs >> 24);
                }
                mg_gltex_etc_decode(k_ref[r].format, again, px);
                for (i = 0; i < 64; i++) {
                    h ^= ((const u8 *)px)[i];
                    h *= 16777619u;
                }
            }
            CHECK_INT((long)h, (long)k_ref[r].hash);
        }
    }
    /* A level halved is the DXT5 (with alpha) or BC1 (without) of the mean of each 2x2 of what it decodes to. */
    {
        u8 big[8 * 8 * 4], small[4 * 4 * 4], want[16], got[16];
        int f, bx, by, x, y, k;
        for (f = 0; f < 2; f++) {
            u32 format = f ? 0x9274 : 0x9278, bytes = f ? 8 : 16;
            for (by = 0; by < 2; by++)
                for (bx = 0; bx < 2; bx++) {
                    mg_gltex_etc_decode(format, block + (by * 2 + bx) * bytes, px);
                    for (y = 0; y < 4; y++) memcpy(big + ((by * 4 + y) * 8 + bx * 4) * 4, px[y * 4], 16);
                }
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    for (k = 0; k < 4; k++)
                        small[(y * 4 + x) * 4 + k] = (u8)((big[((2 * y) * 8 + 2 * x) * 4 + k] + big[((2 * y) * 8 + 2 * x + 1) * 4 + k] +
                                                           big[((2 * y + 1) * 8 + 2 * x) * 4 + k] + big[((2 * y + 1) * 8 + 2 * x + 1) * 4 + k] + 2) / 4);
            if (f) mg_gltex_bc1(small, 4, 4, 16, want);
            else mg_gltex_dxt5(small, 4, 4, 16, want);
            CHECK(mg_gltex_etc_halve(format, block, 8, 8, got) == (f ? 8u : 16u) && !memcmp(got, want, f ? 8 : 16));
        }
    }

    /* Textures from 1024 pixels a side, halved: storage, a part of a level, a level and the next, and one below the size left whole. */
    {
        u8 *level = (u8 *)malloc(1024 * 1024);
        u8 sub_px[256];
        if (level) {
            for (i = 0; i < 1024 * 1024; i++) level[i] = block[i & 63];
            memcpy(sub_px, level, sizeof(sub_px));
            bind(0x0DE1, 11);
            storage(0x0DE1, 11, 0x9278, 1024, 1024);
            CHECK(g_tex_got.internal == 0x83F3 && g_tex_got.w == 512 && g_tex_got.h == 512 && g_tex_got.level == 10);
            sub(0x0DE1, 0, 0, 0, 16, 16, 0x9278, 256, sub_px);
            CHECK(g_tex_got.internal == 0x83F3 && g_tex_got.w == 8 && g_tex_got.h == 8 && g_tex_got.size == 64);
            g_tex_got.internal = 0;
            sub(0x0DE1, 0, 4, 0, 16, 16, 0x9278, 256, sub_px);
            CHECK(g_tex_got.internal == 0);
            bind(0x0DE1, 12);
            compressed(0x0DE1, 0, 0x9274, 1024, 512, 0, 1024 * 512 / 2, level);
            CHECK(g_tex_got.internal == 0x83F0 && g_tex_got.w == 512 && g_tex_got.h == 256 && g_tex_got.size == 512 * 256 / 2 && g_tex_got.data);
            compressed(0x0DE1, 1, 0x9274, 512, 256, 0, 512 * 256 / 2, level);
            CHECK(g_tex_got.internal == 0x83F0 && g_tex_got.level == 1 && g_tex_got.w == 256 && g_tex_got.h == 128 && g_tex_got.size == 256 * 128 / 2);
            bind(0x0DE1, 13);
            compressed(0x0DE1, 0, 0x9278, 512, 512, 0, 512 * 512, level);
            CHECK(g_tex_got.internal == 0x83F3 && g_tex_got.w == 512 && g_tex_got.h == 512);
            mg_gltex_report(report, sizeof(report));
            CHECK(strstr(report, "halved: 3, ") != NULL);
            free(level);
        }
    }

    mg_gltex_reset();
    g_tex_ext = "GL_EXT_texture_compression_s3tc";
    mg_gltex_set_host(f_tex_resolve);
    compressed = (compressed_fn)mg_gltex_wrap("glCompressedTexImage2D", (void *)f_compressed_2d);
    bind = (bind_fn)mg_gltex_wrap("glBindTexture", (void *)f_bind_texture);
    bind(0x0DE1, 9);
    compressed(0x0DE1, 0, 0x9274, 8, 8, 0, 32, block);
    CHECK(g_tex_got.internal == 0x9274);
    bind(0x0DE1, 10);
    compressed(0x0DE1, 0, 0x9278, 8, 8, 0, 64, block);
    CHECK(g_tex_got.internal == 0x83F3);
    mg_gltex_reset();
}

/*
 * Large plain textures packed while the process is short of addresses (mg_gltex.c), whatever was asked for before: one
 * of glTexImage2D's, opaque, as BC1, its mipmaps made down to 1x1; one of glTexStorage2D's, with alpha, held back until its
 * first level comes whole, then DXT5 with its mipmaps, and glGenerateMipmap left with nothing to do; one of 512 left plain;
 * none packed while there are addresses enough.
 */
static int g_mipmaps_asked, g_plain_images;
static void f_image_2d(u32 t, i32 level, i32 internal, i32 w, i32 h, i32 border, u32 format, u32 type, const void *px)
{
    (void)t; (void)border; (void)format; (void)type; (void)px;
    g_plain_images++;
    tex_got((u32)internal, level, w, h, 0, NULL);
}
static void f_sub_image_2d(u32 t, i32 level, i32 x, i32 y, i32 w, i32 h, u32 format, u32 type, const void *px)
{
    (void)t; (void)x; (void)y; (void)type; (void)px;
    g_plain_images++;
    tex_got(format, level, w, h, 0, NULL);
}
static void f_generate_mipmap(u32 t) { (void)t; g_mipmaps_asked++; }

static void test_textures_pressed(void)
{
    typedef void (*image_fn)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void (*sub_image_fn)(u32, i32, i32, i32, i32, i32, u32, u32, const void *);
    typedef void (*storage_fn)(u32, i32, u32, i32, i32);
    typedef void (*bind_fn)(u32, u32);
    typedef void (*mipmap_fn)(u32);
    u8 *px = (u8 *)malloc(1024 * 1024 * 4);
    image_fn image;
    sub_image_fn sub_image;
    storage_fn storage;
    bind_fn bind;
    mipmap_fn mipmap;
    char report[600];
    int i;
    say("textures: large ones packed while addresses are short");
    if (!px) return;
    for (i = 0; i < 1024 * 1024; i++) {
        px[i * 4] = (u8)i;
        px[i * 4 + 1] = (u8)(i >> 8);
        px[i * 4 + 2] = (u8)(i >> 3);
        px[i * 4 + 3] = 255;
    }
    mg_gltex_reset();
    g_tex_ext = "GL_EXT_texture_compression_s3tc";
    mg_gltex_set_host(f_tex_resolve);
    (void)mg_gltex_wrap("glCompressedTexImage2D", (void *)f_compressed_2d);
    image = (image_fn)mg_gltex_wrap("glTexImage2D", (void *)f_image_2d);
    sub_image = (sub_image_fn)mg_gltex_wrap("glTexSubImage2D", (void *)f_sub_image_2d);
    storage = (storage_fn)mg_gltex_wrap("glTexStorage2D", (void *)f_storage_2d);
    bind = (bind_fn)mg_gltex_wrap("glBindTexture", (void *)f_bind_texture);
    mipmap = (mipmap_fn)mg_gltex_wrap("glGenerateMipmap", (void *)f_generate_mipmap);
    g_plain_images = g_mipmaps_asked = 0;

    /* Addresses enough: as the app gave it. */
    mg_gltex_short_of_addresses(0);
    bind(0x0DE1, 30);
    image(0x0DE1, 0, 0x1908, 1024, 1024, 0, 0x1908, 0x1401, px);
    CHECK(g_plain_images == 1 && g_tex_got.internal == 0x1908);

    mg_gltex_short_of_addresses(1);
    /* glTexImage2D's, opaque: BC1, and its last mipmap 1x1 made here, the size of a block. */
    bind(0x0DE1, 31);
    image(0x0DE1, 0, 0x8058, 1024, 1024, 0, 0x1908, 0x1401, px);
    CHECK(g_plain_images == 1 && g_tex_got.internal == 0x83F0 && g_tex_got.level == 10 && g_tex_got.w == 1 && g_tex_got.h == 1 && g_tex_got.size == 8);
    mipmap(0x0DE1);
    CHECK(g_mipmaps_asked == 0);
    /* A level of its own given whole is packed in place of the one made here. */
    image(0x0DE1, 1, 0x1908, 512, 512, 0, 0x1908, 0x1401, px);
    CHECK(g_plain_images == 1 && g_tex_got.internal == 0x83F0 && g_tex_got.level == 1 && g_tex_got.w == 512 && g_tex_got.size == 512 * 512 / 2);

    /* glTexStorage2D's, with alpha: held back, then DXT5 when its first level comes whole, its mipmaps made here. */
    for (i = 0; i < 1024 * 1024; i++) px[i * 4 + 3] = (u8)(i * 7);
    bind(0x0DE1, 32);
    g_tex_got.internal = 0;
    storage(0x0DE1, 11, 0x8058, 1024, 1024);
    CHECK(g_tex_got.internal == 0);
    sub_image(0x0DE1, 0, 0, 0, 1024, 1024, 0x1908, 0x1401, px);
    CHECK(g_plain_images == 1 && g_tex_got.internal == 0x83F3 && g_tex_got.level == 10 && g_tex_got.w == 1 && g_tex_got.size == 16);
    mipmap(0x0DE1);
    CHECK(g_mipmaps_asked == 0);

    /* 512 a side is not packed for the addresses alone. */
    bind(0x0DE1, 33);
    image(0x0DE1, 0, 0x1908, 512, 512, 0, 0x1908, 0x1401, px);
    CHECK(g_plain_images == 2 && g_tex_got.internal == 0x1908);
    mg_gltex_report(report, sizeof(report));
    CHECK(strstr(report, "kept as DXT5 or BC1: 2, ") != NULL);
    mg_gltex_reset();
    free(px);
}

/* Textures packed on their way in (mg_gltex.c): what comes out of the DXT5 blocks is what went in, as near as the format allows. */
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
    CHECK(mg_gltex_dxt5_size(8, 8) == 64 && mg_gltex_dxt5_size(6, 2) == 32 && mg_gltex_dxt5_size(1, 1) == 16);
    mg_gltex_dxt5(img, 8, 8, 32, packed);
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
    mg_gltex_dxt5(img, 4, 4, 16, packed);
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
    mg_gltex_dxt5(img, 4, 4, 16, packed);
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
        mg_gltex_dxt5(edge, 6, 2, 24, packed);
        dxt5_decode(packed + 16, back);
        CHECK(far_apart(back[0][0], 20) <= 20 && far_apart(back[4][0], 220) <= 20 && back[0][3] == 255);
        free(edge);
    }
    test_etc();
    test_textures_pressed();
}

/*
 * A film through libmediandk (ndk/mg_media.c), with a decoder that makes its
 * pictures up: the samples an extractor hands out, a codec fed with them, and
 * what comes out, in the order and with the notices an engine counts on.
 */
static int g_fake_video, g_fake_audio;
static u64 g_fake_size;

static void *fake_open(void *user, const char *path, megler_media_info *info)
{
    mg_stat st;
    (void)user;
    g_fake_size = mg_path_stat(path, &st, NULL) ? st.size : 0;
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
    void *(*ex_new)(void) = (void *(*)(void))mg_bionic_find("AMediaExtractor_new");
    int (*ex_fd)(void *, int, i64, i64) = (int (*)(void *, int, i64, i64))mg_bionic_find("AMediaExtractor_setDataSourceFd");
    u32 (*ex_count)(void *) = (u32 (*)(void *))mg_bionic_find("AMediaExtractor_getTrackCount");
    void *(*ex_format)(void *, u32) = (void *(*)(void *, u32))mg_bionic_find("AMediaExtractor_getTrackFormat");
    int (*ex_select)(void *, u32) = (int (*)(void *, u32))mg_bionic_find("AMediaExtractor_selectTrack");
    int (*ex_unselect)(void *, u32) = (int (*)(void *, u32))mg_bionic_find("AMediaExtractor_unselectTrack");
    i32 (*ex_read)(void *, u8 *, u32) = (i32 (*)(void *, u8 *, u32))mg_bionic_find("AMediaExtractor_readSampleData");
    i64 (*ex_time)(void *) = (i64 (*)(void *))mg_bionic_find("AMediaExtractor_getSampleTime");
    int (*ex_track)(void *) = (int (*)(void *))mg_bionic_find("AMediaExtractor_getSampleTrackIndex");
    u8 (*ex_advance)(void *) = (u8 (*)(void *))mg_bionic_find("AMediaExtractor_advance");
    int (*ex_seek)(void *, i64, int) = (int (*)(void *, i64, int))mg_bionic_find("AMediaExtractor_seekTo");
    int (*ex_delete)(void *) = (int (*)(void *))mg_bionic_find("AMediaExtractor_delete");
    u8 (*fmt_string)(void *, const char *, const char **) = (u8 (*)(void *, const char *, const char **))mg_bionic_find("AMediaFormat_getString");
    u8 (*fmt_i32)(void *, const char *, i32 *) = (u8 (*)(void *, const char *, i32 *))mg_bionic_find("AMediaFormat_getInt32");
    int (*fmt_delete)(void *) = (int (*)(void *))mg_bionic_find("AMediaFormat_delete");
    void *(*codec_new)(const char *) = (void *(*)(const char *))mg_bionic_find("AMediaCodec_createDecoderByType");
    int (*codec_configure)(void *, void *, void *, void *, u32) = (int (*)(void *, void *, void *, void *, u32))mg_bionic_find("AMediaCodec_configure");
    int (*codec_start)(void *) = (int (*)(void *))mg_bionic_find("AMediaCodec_start");
    i32 (*codec_in)(void *, i64) = (i32 (*)(void *, i64))mg_bionic_find("AMediaCodec_dequeueInputBuffer");
    u8 *(*codec_in_buffer)(void *, u32, u32 *) = (u8 *(*)(void *, u32, u32 *))mg_bionic_find("AMediaCodec_getInputBuffer");
    int (*codec_queue)(void *, u32, i32, u32, u64, u32) = (int (*)(void *, u32, i32, u32, u64, u32))mg_bionic_find("AMediaCodec_queueInputBuffer");
    i32 (*codec_out)(void *, buffer_info *, i64) = (i32 (*)(void *, buffer_info *, i64))mg_bionic_find("AMediaCodec_dequeueOutputBuffer");
    u8 *(*codec_out_buffer)(void *, u32, u32 *) = (u8 *(*)(void *, u32, u32 *))mg_bionic_find("AMediaCodec_getOutputBuffer");
    int (*codec_release)(void *, u32, u8) = (int (*)(void *, u32, u8))mg_bionic_find("AMediaCodec_releaseOutputBuffer");
    int (*codec_delete)(void *) = (int (*)(void *))mg_bionic_find("AMediaCodec_delete");
    const char **key_mime = (const char **)mg_bionic_find("AMEDIAFORMAT_KEY_MIME");
    char *path = mg_sprintf("%s/film-source.bin", scratch);
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
    mg_media_set_host(&fake);
    for (i = 0; i < 1000; i++) bytes[i] = (u8)i;
    CHECK(mg_file_write_all(path, bytes, sizeof(bytes)));
    fd = mg_fd_open_host(path, MG_O_READ);
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
    mg_fd_close(fd);
    mg_media_set_host(NULL);
    free(path);
}

/*
 * What libc exports that is not a function, and calls only old or odd code makes: Android 9's 32-bit libc has the page size
 * as data (__page_size, __page_shift), for code built by NDKs that read PAGE_SIZE from it; a crash reporter built so read
 * address 0 for it here. And setpgrp, and ptrace, which an app is refused.
 */
/*
 * Anonymous memory mapped with no access is address space alone (bionic/mg_libc_mem.c): 64 MB of it commits nothing;
 * mprotect commits what it gives access to, and only that; a range with committed and uncommitted pages can be made
 * no-access, and what was written survives that and the access given back; mapped again over itself with no access
 * (MAP_FIXED) it is given back, and zero when given access again; memory mapped with access is committed at once. Where
 * the host does not say what is committed (POSIX), the behaviour alone is checked.
 */
static void test_mmap_commit(void)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, i32);
    typedef int (*mprotect_fn)(void *, size_t, int);
    typedef int (*munmap_fn)(void *, size_t);
    enum { NONE = 0, RW = 3, PRIVATE = 0x02, ANONYMOUS = 0x20, FIXED = 0x10, MB = 1 << 20 };
    mmap_fn mmap_ = (mmap_fn)mg_bionic_find("mmap");
    mprotect_fn mprotect_ = (mprotect_fn)mg_bionic_find("mprotect");
    munmap_fn munmap_ = (munmap_fn)mg_bionic_find("munmap");
    u8 *p, *q;
    i64 c, c0;
    say("mmap: no access is address space alone, committed when given access");
    CHECK(mmap_ && mprotect_ && munmap_);
    if (!mmap_ || !mprotect_ || !munmap_) return;
    /* Inline hooks must see failure, not permission to overwrite a host
     * bionic implementation. Neither request may change the host page. */
    {
        void *host_page = (void *)MG_ALIGN_DOWN((mg_addr)test_mmap_commit, (mg_addr)MG_PAGE);
        typedef int *(*errno_fn)(void);
        errno_fn errno_ = (errno_fn)mg_bionic_find("__errno");
        CHECK(host_page && errno_);
        if (host_page && errno_) {
            CHECK(mprotect_(host_page, MG_PAGE, 5) == -1 && *errno_() == 13);
            CHECK(mprotect_(host_page, MG_PAGE, 7) == -1 && *errno_() == 13);
        }
    }
    p = (u8 *)mmap_(NULL, 64 * MB, NONE, PRIVATE | ANONYMOUS, -1, 0);
    CHECK(p && p != (u8 *)(mg_addr)-1);
    if (!p || p == (u8 *)(mg_addr)-1) return;
    c0 = mg_mem_committed(p, 64 * MB);
    CHECK(c0 == -1 || c0 == 0);
    CHECK(mprotect_(p + MB, MB, RW) == 0);
    c = mg_mem_committed(p, 64 * MB);
    CHECK(c == -1 || c == MB);
    say("  64 MB with no access: %lld bytes committed (-1: the host does not say); 1 MB of it given access: %lld", (long long)c0, (long long)c);
    p[MB] = 7;
    p[2 * MB - 1] = 9;
    CHECK(mprotect_(p, 64 * MB, NONE) == 0);
    CHECK(mprotect_(p + MB, MB, RW) == 0 && p[MB] == 7 && p[2 * MB - 1] == 9);
    CHECK(mmap_(p + MB, MB, NONE, PRIVATE | ANONYMOUS | FIXED, -1, 0) == p + MB);
    c = mg_mem_committed(p, 64 * MB);
    CHECK(c == -1 || c == 0);
    CHECK(mprotect_(p + MB, MB, RW) == 0 && p[MB] == 0 && p[2 * MB - 1] == 0);
    CHECK(munmap_(p, 64 * MB) == 0);
    q = (u8 *)mmap_(NULL, MB, RW, PRIVATE | ANONYMOUS, -1, 0);
    CHECK(q && q != (u8 *)(mg_addr)-1);
    if (q && q != (u8 *)(mg_addr)-1) {
        c = mg_mem_committed(q, MB);
        CHECK(c == -1 || c == MB);
        CHECK(q[0] == 0 && q[MB - 1] == 0);
        munmap_(q, MB);
    }
}

/*
 * A file mapped read-only and shared is a view of the file (bionic/mg_libc_mem.c, mg_fd_file_map): its bytes from a
 * page's offset, none of it memory of the process's own (on Windows, where that is said); a mapping that reaches past the
 * file's end is a copy, zero past it, as before; unmapped like any other.
 */
static void test_mmap_file(const char *scratch)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, i32);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*close_fn)(int);
    enum { READ = 1, SHARED = 0x01, CHUNK = 65536, LEN = 3 * 65536 + 100 };
    mmap_fn mmap_ = (mmap_fn)mg_bionic_find("mmap");
    munmap_fn munmap_ = (munmap_fn)mg_bionic_find("munmap");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    char *path = mg_sprintf("%s/mapped.bin", scratch);
    u8 *data = (u8 *)mg_alloc(LEN), *p, *q;
    mg_file *f;
    int err = 0, fd, i;
    bool same = true;
    i64 c;
    say("mmap: a file read-only and shared is a view of the file");
    for (i = 0; i < LEN; i++) data[i] = (u8)(i * 7 + (i >> 8));
    f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    CHECK(f && mg_file_write(f, data, LEN, &err) == LEN);
    if (f) mg_file_close(f);
    fd = mg_fd_open_host(path, MG_O_READ);
    CHECK(mmap_ && munmap_ && close_ && fd >= 0);
    if (mmap_ && munmap_ && close_ && fd >= 0) {
        p = (u8 *)mmap_(NULL, 2 * CHUNK, READ, SHARED, fd, 4096);
        CHECK(p && p != (u8 *)(mg_addr)-1);
        if (p && p != (u8 *)(mg_addr)-1) {
            for (i = 0; i < 2 * CHUNK; i++)
                if (p[i] != data[4096 + i]) same = false;
            CHECK(same);
            c = mg_mem_committed(p, 2 * CHUNK);
            CHECK(c == -1 || c == 0);
            say("  128 KB of it from 4 KB on: %s; %lld bytes of the process's own (-1: the host does not say)", same ? "its bytes" : "NOT its bytes",
                (long long)c);
            CHECK(munmap_(p, 2 * CHUNK) == 0);
        }
        q = (u8 *)mmap_(NULL, CHUNK, READ, SHARED, fd, 3 * CHUNK);
        CHECK(q && q != (u8 *)(mg_addr)-1);
        if (q && q != (u8 *)(mg_addr)-1) {
            CHECK(q[0] == data[3 * CHUNK] && q[99] == data[3 * CHUNK + 99] && q[100] == 0 && q[CHUNK - 1] == 0);
            CHECK(munmap_(q, CHUNK) == 0);
        }
        close_(fd);
    }
    free(data);
    free(path);
}

/*
 * Anonymous memory given back with madvise (MADV_DONTNEED, bionic/mg_libc_mem.c) is the system's again -- decommitted, where
 * the host says what is -- and still the app's: a page of it touched is zero, read or written; one the system itself writes
 * into (a file read into it) is read into; made no-access and given access again it is zero; what was not given back keeps
 * what it had; an address off a page is refused, as Linux refuses it.
 */
static void test_mmap_madvise(const char *scratch)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, i32);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*mprotect_fn)(void *, size_t, int);
    typedef int (*madvise_fn)(void *, size_t, int);
    typedef i32 (*read_fn)(int, void *, u32);
    typedef int (*close_fn)(int);
    enum { NONE = 0, RW = 3, PRIVATE = 0x02, ANONYMOUS = 0x20, DONTNEED = 4, MB = 1 << 20, FILE_BYTES = 8000 };
    mmap_fn mmap_ = (mmap_fn)mg_bionic_find("mmap");
    munmap_fn munmap_ = (munmap_fn)mg_bionic_find("munmap");
    mprotect_fn mprotect_ = (mprotect_fn)mg_bionic_find("mprotect");
    madvise_fn madvise_ = (madvise_fn)mg_bionic_find("madvise");
    read_fn read_ = (read_fn)mg_bionic_find("read");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    char *path = mg_sprintf("%s/madvised.bin", scratch);
    u8 *data = (u8 *)mg_alloc(FILE_BYTES), *p;
    mg_file *f;
    int err = 0, fd, i;
    bool same = true;
    i64 before, after;
    u64 anon0 = 0, anon1 = 0, anon2 = 0, anon3 = 0;
    say("mmap: anonymous memory given back with madvise is the system's again, and zero when touched");
    CHECK(mmap_ && munmap_ && mprotect_ && madvise_ && read_ && close_);
    if (!mmap_ || !munmap_ || !mprotect_ || !madvise_ || !read_ || !close_) goto done;
    mg_mmap_usage(&anon0, NULL);
    p = (u8 *)mmap_(NULL, 4 * MB, RW, PRIVATE | ANONYMOUS, -1, 0);
    CHECK(p && p != (u8 *)(mg_addr)-1);
    if (!p || p == (u8 *)(mg_addr)-1) goto done;
    memset(p, 0x5A, 4 * MB);
    mg_mmap_usage(&anon1, NULL);
    before = mg_mem_committed(p, 4 * MB);
    CHECK(madvise_(p + MB, 2 * MB, DONTNEED) == 0);
    after = mg_mem_committed(p, 4 * MB);
    CHECK(before == -1 || (before == 4 * MB && after == 2 * MB));
    /* What the frames line says of the app's mappings (mg_mmap_usage): the 4 MB mapped, less the 2 MB given back. */
    mg_mmap_usage(&anon2, NULL);
    CHECK(anon1 - anon0 == 4 * MB && anon1 - anon2 == 2 * MB);
    say("  4 MB written, 2 MB of it given back: %lld bytes committed before, %lld after (-1: the host does not say)", (long long)before,
        (long long)after);
    CHECK(p[0] == 0x5A && p[MB - 1] == 0x5A && p[3 * MB] == 0x5A && p[4 * MB - 1] == 0x5A);
    /* Read, then written: the page and those given back of its 64 KB are committed again, zero. */
    CHECK(p[MB] == 0 && p[MB + 4095] == 0);
    p[MB + 20 * 4096 + 7] = 1;
    CHECK(p[MB + 20 * 4096 + 7] == 1 && p[MB + 20 * 4096 + 8] == 0);
    after = mg_mem_committed(p, 4 * MB);
    CHECK(after == -1 || (after > 2 * MB && after < 3 * MB));
    /* The system writes into a page given back: a file read into it, past a page's start. */
    for (i = 0; i < FILE_BYTES; i++) data[i] = (u8)(i * 13 + 1);
    f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    CHECK(f && mg_file_write(f, data, FILE_BYTES, &err) == FILE_BYTES);
    if (f) mg_file_close(f);
    fd = mg_fd_open_host(path, MG_O_READ);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(read_(fd, p + 2 * MB + 100, FILE_BYTES) == FILE_BYTES);
        for (i = 0; i < FILE_BYTES; i++)
            if (p[2 * MB + 100 + i] != data[i]) same = false;
        CHECK(same && p[2 * MB + 99] == 0 && p[2 * MB + 100 + FILE_BYTES] == 0);
        close_(fd);
    }
    say("  a file read by the system into a page given back: %s", same ? "its bytes" : "NOT its bytes");
    /* Given back, made no-access, given access again: zero, committed by mprotect. */
    memset(p, 0x6B, MB);
    CHECK(madvise_(p, MB, DONTNEED) == 0 && mprotect_(p, MB, NONE) == 0 && mprotect_(p, MB, RW) == 0);
    CHECK(p[0] == 0 && p[MB - 1] == 0);
    CHECK(madvise_(p + 100, 4096, DONTNEED) == -1);
    /*
     * A page the app has and did not make no-access, decommitted with nothing marking it given back (the way one of the
     * layer's decommits once left a page of PartitionAlloc's on the phone): touched, it is committed again, zero, and the
     * log says so -- not a fault that ends the app.
     */
    p[3 * MB + 8192] = 0x77;
    CHECK(mg_mem_decommit(p + 3 * MB + 8192, MG_PAGE));
    CHECK(p[3 * MB + 8192] == 0);
    p[3 * MB + 8192] = 0x78;
    CHECK(p[3 * MB + 8192] == 0x78 && p[3 * MB] == 0x5A);
    CHECK(munmap_(p, 4 * MB) == 0);
    mg_mmap_usage(&anon3, NULL);
    CHECK(anon3 == anon0);
done:
    free(data);
    free(path);
}

static void count_entry(const char *name, bool is_dir, void *user)
{
    (void)is_dir;
    if (strcmp(name, "config.fb")) say("  unexpected entry %s", name);
    (*(int *)user)++;
}

static void write_whole(const char *path, const char *text)
{
    int err = 0;
    mg_file *f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &err);
    CHECK(f && mg_file_write(f, text, strlen(text), &err) == (i64)strlen(text));
    if (f) mg_file_close(f);
}

static bool reads_as(mg_file *f, const char *text)
{
    char got[16] = { 0 };
    int err = 0;
    return f && mg_file_pread(f, got, strlen(text), 0, &err) == (i64)strlen(text) && !memcmp(got, text, strlen(text));
}

/*
 * A rename over a file that is open and mapped replaces it, as on Android, and whoever had the old file keeps it: Google's
 * GmsCore writes its Chimera config so, opening the new one and moving it over the live one it has mapped. Windows refuses
 * that move; the host puts the old file aside (mg_host_win32.c, replace_in_use), and the folder lists, and is removed, as
 * if it were gone.
 */
static void test_rename_in_use(const char *scratch)
{
    char *dir = mg_sprintf("%s/replace", scratch), *live = mg_sprintf("%s/config.fb", dir), *pending = mg_sprintf("%s/config.fb.pending", dir);
    mg_file *old_file, *now, *moved;
    void *base = NULL;
    size_t mapped = 0;
    const u8 *view;
    int err = 0, entries = 0;
    say("rename: over a file that is open and mapped, which keeps its old content for those who have it");
    mg_path_mkdirs(dir);
    write_whole(live, "old config");
    old_file = mg_file_open(live, MG_O_READ, &err);
    view = old_file ? (const u8 *)mg_file_map(old_file, 0, 10, &base, &mapped) : NULL;
    CHECK(old_file && view);
    write_whole(pending, "new config");
    moved = mg_file_open(pending, MG_O_READ, &err);
    CHECK(moved != NULL);
    CHECK(mg_path_rename(pending, live, &err));
    now = mg_file_open(live, MG_O_READ, &err);
    CHECK(reads_as(now, "new config") && reads_as(moved, "new config"));
    CHECK(reads_as(old_file, "old config") && view && !memcmp(view, "old config", 10));
    CHECK(mg_path_list(dir, count_entry, &entries, &err) && entries == 1);
    /* And again over the one just moved there, which is open twice now. */
    write_whole(pending, "3rd config");
    CHECK(mg_path_rename(pending, live, &err));
    CHECK(reads_as(now, "new config"));
    if (now) mg_file_close(now);
    now = mg_file_open(live, MG_O_READ, &err);
    CHECK(reads_as(now, "3rd config"));
    entries = 0;
    CHECK(mg_path_list(dir, count_entry, &entries, &err) && entries == 1);
    if (now) mg_file_close(now);
    if (moved) mg_file_close(moved);
    if (view) mg_section_unmap(base, mapped);
    if (old_file) mg_file_close(old_file);
    /* With nothing left open, a listing removes what was put aside, and the folder can go. */
    entries = 0;
    CHECK(mg_path_list(dir, count_entry, &entries, &err) && entries == 1);
    CHECK(mg_path_remove(live, &err));
    CHECK(mg_path_rmdir(dir, &err));
    free(pending);
    free(live);
    free(dir);
}

/*
 * Shared memory holds no address space of its own (bionic/mg_libc_io.c): a region made, written and read through its
 * descriptor, and grown past its section, has no view until the app maps it; mapped, it is one view of its size; unmapped,
 * none again. The views are counted by the host (mg_views_kb).
 */
static void test_shm_views(void)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, i32);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*close_fn)(int);
    typedef i32 (*pread_fn)(int, void *, u32, i32);
    typedef i32 (*pwrite_fn)(int, const void *, u32, i32);
    typedef int (*ftruncate_fn)(int, i32);
    enum { MB = 1 << 20, SIZE = 24 * MB, RW = 3, SHARED = 1 };
    mmap_fn mmap_ = (mmap_fn)mg_bionic_find("mmap");
    munmap_fn munmap_ = (munmap_fn)mg_bionic_find("munmap");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    pread_fn pread_ = (pread_fn)mg_bionic_find("pread");
    pwrite_fn pwrite_ = (pwrite_fn)mg_bionic_find("pwrite");
    ftruncate_fn ftruncate_ = (ftruncate_fn)mg_bionic_find("ftruncate");
    u32 before = mg_views_kb(), made, written, grown, mapped = 0, unmapped = 0;
    char back[4] = { 0 };
    u8 *p;
    int fd;
    say("shared memory: no view of its own, one when mapped");
    CHECK(mmap_ && munmap_ && close_ && pread_ && pwrite_ && ftruncate_);
    if (!mmap_ || !munmap_ || !close_ || !pread_ || !pwrite_ || !ftruncate_) return;
    fd = mg_fd_shm_create("views", SIZE);
    CHECK(fd >= 0);
    if (fd < 0) return;
    made = mg_views_kb();
    CHECK(pwrite_(fd, "abc", 3, SIZE - 3) == 3 && pread_(fd, back, 3, SIZE - 3) == 3 && !memcmp(back, "abc", 3));
    written = mg_views_kb();
    /* Past what its section holds: a new section, what there was copied in. */
    CHECK(ftruncate_(fd, SIZE + MB) == 0);
    memset(back, 0, sizeof(back));
    CHECK(pread_(fd, back, 3, SIZE - 3) == 3 && !memcmp(back, "abc", 3));
    grown = mg_views_kb();
    p = (u8 *)mmap_(NULL, SIZE + MB, RW, SHARED, fd, 0);
    CHECK(p && p != (u8 *)(mg_addr)-1);
    if (p && p != (u8 *)(mg_addr)-1) {
        mapped = mg_views_kb();
        CHECK(p[SIZE - 3] == 'a' && p[SIZE - 1] == 'c' && p[SIZE] == 0);
        p[7] = 5;
        CHECK(pread_(fd, back, 1, 7) == 1 && back[0] == 5);
        CHECK(munmap_(p, SIZE + MB) == 0);
        unmapped = mg_views_kb();
    }
    close_(fd);
    say("  views: %u KB before; made %+d, written and read %+d, grown %+d, mapped %+d, unmapped %+d", (unsigned)before, (int)(made - before),
        (int)(written - before), (int)(grown - before), (int)(mapped - before), (int)(unmapped - before));
    CHECK(made == before && written == before && grown == before && unmapped == before);
    CHECK(mapped >= before + (SIZE + MB) / 1024 && mapped < before + (SIZE + MB) / 1024 + 64);
}

/* The largest free stretches of address space, which the crash report names (mg_host_free_stretches): largest first. */
static void test_free_stretches(void)
{
    u32 s[4] = { 0, 0, 0, 0 }, free_mb = 0, largest = 0, reserved = 0, committed = 0;
    if (!mg_host_free_stretches(s, 4)) {
        say("(the address space's free stretches: the host does not say)");
        return;
    }
    CHECK(mg_host_address_space(&free_mb, &largest, &reserved, &committed));
    say("address space: the largest free stretches %u, %u, %u and %u MB, of %u MB free", (unsigned)s[0], (unsigned)s[1], (unsigned)s[2],
        (unsigned)s[3], (unsigned)free_mb);
    CHECK(s[0] == largest && s[0] >= s[1] && s[1] >= s[2] && s[2] >= s[3] && s[0] + s[1] + s[2] + s[3] <= free_mb);
    {
        /* What holds the addresses, by kind: the program's image, and private memory, a 3 MB allocation made here among it. */
        mg_host_address_kinds k, after;
        void *big = mg_mem_reserve(3u << 20);
        u8 *small = (u8 *)malloc(8u << 20);
        int i;
        bool seen = false;
        CHECK(big && mg_mem_commit(big, 3u << 20, MG_PROT_RW) && small);
        if (small) memset(small, 1, 8u << 20);
        CHECK(mg_host_address_census(&k));
        for (i = 0; i < k.nsizes; i++) seen = seen || (k.sizes[i].kb == 3072 && k.sizes[i].count >= 1);
        say("addresses by kind: images %u MB, views %u MB, private %u MB committed and %u reserved; threads the core started %u",
            (unsigned)k.images_mb, (unsigned)k.views_mb, (unsigned)k.private_mb, (unsigned)k.private_reserved_mb, (unsigned)mg_thread_live_count());
        CHECK(k.images_mb > 0 && k.private_mb >= 3 && k.nsizes > 0);
        /* (A 3 MB allocation is among the four sizes holding the most only where little else is; it is said, not checked.) */
        if (!seen) say("  (the 3 MB allocation is not among the four sizes that hold the most here)");
        free(small);
        if (big) mg_mem_release(big, 3u << 20);
        CHECK(mg_host_address_census(&after) && after.private_mb + 3 <= k.private_mb + 1);
    }
}

/*
 * Chromium's end out of memory, in the crash report (mg_core.c, mg_chromium_oom_global): OnNoMemoryInternal as the
 * WebView's library has it -- its bytes, libwebviewchromium.so+0x1f4c348 to +0x1f4c364 of 128.0.6613.88 -- with its
 * literal made to point at a global here. Found from its bkpt and from its udf; not from elsewhere, nor once changed.
 */
static void test_chromium_oom_report(void)
{
    static const u8 code[28] = { 0x82, 0xb0, 0x04, 0x49, 0x01, 0x90, 0x79, 0x44, 0x08, 0x60, 0x01, 0xa8, 0x40, 0xf1,
                                 0xb9, 0xfa, 0x00, 0xbe, 0x00, 0xde, 0x06, 0x45, 0x73, 0x02, 0xff, 0xf7, 0xf2, 0xff };
    static union {
        u8 b[64];
        u32 align;
    } fn;
    static u32 g_oom_size = 126337600u;
    mg_addr f = (mg_addr)fn.b;
    i32 offset;
    say("Chromium's end out of memory: the size it asked for, from its global");
    memcpy(fn.b, code, sizeof(code));
    /* The literal (at +0x14): the global's distance from where `add r1, pc` reads pc, f + 10. */
    offset = (i32)((mg_addr)&g_oom_size - (f + 10));
    memcpy(fn.b + 0x14, &offset, 4);
    CHECK(mg_chromium_oom_global(f + 18) == (mg_addr)&g_oom_size);
    CHECK(mg_chromium_oom_global(f + 16) == (mg_addr)&g_oom_size);
    CHECK(*(const u32 *)mg_chromium_oom_global(f + 18) == 126337600u);
    CHECK(mg_chromium_oom_global(f + 14) == 0 && mg_chromium_oom_global(f + 12) == 0 && mg_chromium_oom_global(f + 17) == 0);
    fn.b[8] = 0x09;                    /* str r1, [r1] -- not the function */
    CHECK(mg_chromium_oom_global(f + 18) == 0);
}

/* The manifest of an Android app's own package, made from one shaped as Megler's (core/src/appx/mg_appx_manifest.c). */
static void test_appx_manifest(void)
{
    static const char megler[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<!-- Megler -->\r\n<Package xmlns=\"x\">\r\n"
        "  <Identity\r\n      Name=\"Megler\"\r\n      Publisher=\"CN=Computershik\"\r\n      Version=\"0.1.0.351\" />\r\n"
        "  <mp:PhoneIdentity\r\n      PhoneProductId=\"af179803-d7e7-4d18-ae09-61b18f61d561\"\r\n      PhonePublisherId=\"0\" />\r\n"
        "  <Properties>\r\n    <DisplayName>Megler</DisplayName>\r\n  </Properties>\r\n"
        "  <Applications><Application Id=\"App\" Executable=\"Megler.exe\" EntryPoint=\"Megler.App\">\r\n"
        "      <uap:VisualElements\r\n          DisplayName=\"Megler\"\r\n          Description=\"Android games\"\r\n"
        "          BackgroundColor=\"#000000\">\r\n"
        "        <uap:DefaultTile Wide310x150Logo=\"w.png\" ShortName=\"Megler\"/>\r\n"
        "        <uap:SplashScreen Image=\"s.png\" BackgroundColor=\"#000000\" />\r\n"
        "      </uap:VisualElements>\r\n      <Extensions>\r\n"
        "        <uap:Extension Category=\"windows.fileTypeAssociation\">\r\n"
        "          <uap:FileTypeAssociation Name=\"apk\"><uap:SupportedFileTypes><uap:FileType>.apk</uap:FileType></uap:SupportedFileTypes>\r\n"
        "          </uap:FileTypeAssociation>\r\n        </uap:Extension>\r\n"
        "        <uap:Extension Category=\"windows.protocol\">\r\n          <uap:Protocol Name=\"megler\" />\r\n        </uap:Extension>\r\n"
        "      </Extensions>\r\n    </Application></Applications></Package>\r\n";
    char err[200], name[64], name2[64];
    char *m, *m2;
    say("an app's own package: its manifest from Megler's");
    /* Every name 50 characters long: the package's, a dot and a hash to fill it. */
    CHECK(mg_appx_app_identity("com.instagram.android", name, sizeof(name)) && !strncmp(name, "MeglerApp.com.instagram.android.", 32) &&
          strlen(name) == 50);
    CHECK(mg_appx_app_identity("com.instagram.android", name2, sizeof(name2)) && !strcmp(name, name2));
    CHECK(mg_appx_app_identity("com.example.my_app", name, sizeof(name)) && !strncmp(name, "MeglerApp.com.example.my-app.", 29) &&
          strlen(name) == 50);
    /* One of 49 gets one hash digit, no dot (10 + 39 = 49). */
    CHECK(mg_appx_app_identity("com.example.abcdefghijklmnopqrstuvwx.ab", name, sizeof(name)) && strlen(name) == 50 &&
          !strncmp(name, "MeglerApp.com.example.abcdefghijklmnopqrstuvwx.ab", 49) && name[49] != '.');
    CHECK(mg_appx_app_identity("com.example.a.very.long.package.name.of.some.app.that.goes.on", name, sizeof(name)) && strlen(name) == 50);
    CHECK(mg_appx_app_identity("com.example.a.very.long.package.name.of.some.app.that.goes.on", name2, sizeof(name2)) && !strcmp(name, name2));
    CHECK(!mg_appx_app_identity("com.example/../x", name, sizeof(name)));
    m = mg_appx_app_manifest(megler, "com.instagram.android", "Insta & <gram> \"x\"", NULL, NULL, err, sizeof(err));
    CHECK(m != NULL);
    if (!m) return;
    CHECK(strstr(m, "Name=\"MeglerApp.com.instagram.android.") != NULL);
    CHECK(strstr(m, "Publisher=\"CN=Computershik\"") != NULL && strstr(m, "Version=\"0.1.0.351\"") != NULL);
    CHECK(strstr(m, "af179803") == NULL && strstr(m, "PhoneProductId=\"") != NULL);
    CHECK(strstr(m, "<DisplayName>Insta &amp; &lt;gram&gt; &quot;x&quot;</DisplayName>") != NULL);
    CHECK(strstr(m, "DisplayName=\"Insta &amp; &lt;gram&gt; &quot;x&quot;\"") != NULL);
    CHECK(strstr(m, "ShortName=\"Insta &amp; &lt;gram&gt; &quot;x&quot;\"") != NULL);
    CHECK(strstr(m, "EntryPoint=\"Megler.App\"") != NULL && strstr(m, "Description=\"Android games\"") != NULL);
    /* The same package again: the same product id; another: another. */
    m2 = mg_appx_app_manifest(megler, "com.instagram.android", "Instagram", "transparent", "#102030", err, sizeof(err));
    CHECK(m2 && !strncmp(strstr(m, "PhoneProductId=\""), strstr(m2, "PhoneProductId=\""), 54));
    /* The colours given, the tile's and the splash screen's; none given, Megler's. */
    CHECK(m2 && strstr(m2, "BackgroundColor=\"transparent\">") != NULL && strstr(m2, "Image=\"s.png\" BackgroundColor=\"#102030\"") != NULL);
    CHECK(strstr(m, "BackgroundColor=\"#000000\">") != NULL && strstr(m, "Image=\"s.png\" BackgroundColor=\"#000000\"") != NULL);
    free(m2);
    m2 = mg_appx_app_manifest(megler, "com.whatsapp", "WhatsApp", NULL, NULL, err, sizeof(err));
    CHECK(m2 && strncmp(strstr(m, "PhoneProductId=\""), strstr(m2, "PhoneProductId=\""), 54));
    free(m2);
    /* A short name of forty characters at most, a sequence of UTF-8 never cut. */
    m2 = mg_appx_app_manifest(megler, "ru.example", "\xD0\x9E\xD1\x87\xD0\xB5\xD0\xBD\xD1\x8C \xD0\xB4\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB5 "
                                                  "\xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F "
                                                  "\xD0\xB4\xD0\xBB\xD1\x8F \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBA\xD0\xB8 \xF0\x9F\x98\x80\xF0\x9F\x98\x80",
                              NULL, NULL, err, sizeof(err));
    if (m2) {
        const char *s = strstr(m2, "ShortName=\"") + 11, *e = strchr(s, '"');
        size_t chars = 0;
        for (; s < e; s++) chars += ((unsigned char)*s & 0xC0) != 0x80;
        CHECK_INT((int)chars, 40);
    }
    CHECK(m2 != NULL);
    free(m2);
    CHECK(mg_appx_app_manifest("<Package/>", "com.example", "x", NULL, NULL, err, sizeof(err)) == NULL && strstr(err, "Identity") != NULL);
    /* Megler's association of APKs is not the app's; its megler: links are. */
    CHECK(strstr(m, "windows.fileTypeAssociation") == NULL && strstr(m, "<uap:FileTypeAssociation") == NULL);
    CHECK(strstr(m, "<uap:Extension Category=\"windows.protocol\">\r\n          <uap:Protocol Name=\"megler\" />") != NULL);
    CHECK(strstr(m, "      </uap:VisualElements>\r\n      <Extensions>\r\n        <uap:Extension Category=\"windows.protocol\">") != NULL);
    free(m);
}

/* resources.pri made for another name (core/src/appx/mg_appx_pri.c): a file of the same shape -- the name twice in UTF-16,
   a schema whose checksum is the CRC-32 of the name twice, lower-cased, followed by the rest -- renamed, has the checksum a
   CRC of the new data has. */
static void put_utf16(u8 *out, const char *s, bool lower_case)
{
    for (; *s; s++, out += 2) {
        char c = *s;
        if (lower_case && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[0] = (u8)c;
        out[1] = 0;
    }
}

/* Material You's palettes of a seed (megler_palette_tone, core/src/run/mg_palette.c): what Android's framework gives as its system
   colours. Material 3's baseline scheme is made of the seed #6750A4, and its colours are these; a channel may be one off, as a
   bit of the floating point may round another way. */
static void check_tone(uint32_t seed, int palette, int tone, uint32_t want)
{
    const uint32_t got = megler_palette_tone(seed, palette, tone);
    bool near = true;
    int shift;
    for (shift = 0; shift < 32; shift += 8) near = near && abs((int)((got >> shift) & 0xFF) - (int)((want >> shift) & 0xFF)) <= 1;
    if (!near) say("  palette %d at tone %d of #%08X is #%08X, not #%08X", palette, tone, (unsigned)seed, (unsigned)got, (unsigned)want);
    CHECK(near);
}

static void test_palette(void)
{
    say("Material You's palettes made from a seed colour, in HCT");
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT1, 40, 0xFF6750A4u);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT1, 80, 0xFFD0BCFFu);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT1, 90, 0xFFEADDFFu);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT1, 10, 0xFF21005Du);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT2, 40, 0xFF625B71u);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT2, 90, 0xFFE8DEF8u);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ACCENT3, 40, 0xFF7D5260u);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_NEUTRAL1, 99, 0xFFFFFBFEu);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_NEUTRAL1, 10, 0xFF1C1B1Fu);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_NEUTRAL2, 30, 0xFF49454Fu);
    check_tone(0xFF6750A4u, MEGLER_PALETTE_ERROR, 40, 0xFFBA1B1Bu);
    /* A blue seed stays blue (CIE LCh, used before, made it violet); and Windows' cobalt. */
    check_tone(0xFF0000FFu, MEGLER_PALETTE_ACCENT1, 80, 0xFFBDC2FFu);
    check_tone(0xFF0063B1u, MEGLER_PALETTE_ACCENT1, 40, 0xFF005FADu);
    check_tone(0xFF0063B1u, MEGLER_PALETTE_ACCENT1, 0, 0xFF000000u);
    check_tone(0xFF0063B1u, MEGLER_PALETTE_ACCENT1, 100, 0xFFFFFFFFu);
    CHECK(megler_palette_tone(0xFF0063B1u, 6, 50) == 0);
}

static void test_appx_pri(void)
{
    static const char placeholder[] = "MeglerApp.00000000000000000000000000000000000000Xy";
    static const char name[] = "MeglerApp.com.ghisler.android.TotalCommander.1a2b3";
    static const char rest1[] = "between the two", rest2[] = "and all the schema's names after the second, a few hundred bytes of them";
    u8 file[600], out[600], data[400];
    size_t n = strlen(placeholder), dn;
    u32 sum, want;
    char err[160];
    say("an app's own package: its resources.pri from one made for a placeholder");
    CHECK_INT((int)strlen(name), (int)n);
    memset(file, 0x5a, sizeof(file));
    memcpy(file + 40, "[def_hnamesx]  ", 16);
    put_utf16(file + 100, placeholder, false);
    put_utf16(file + 300, placeholder, false);
    /* The checksum data: the name, rest1, the name, rest2, as the schema's (lower-cased, UTF-16). */
#define DATA(s)                                                                                                  \
    do {                                                                                                         \
        dn = 0;                                                                                                  \
        put_utf16(data, s, true);                                                                                \
        dn += 2 * n;                                                                                             \
        memcpy(data + dn, rest1, sizeof(rest1));                                                                 \
        dn += sizeof(rest1);                                                                                     \
        put_utf16(data + dn, s, true);                                                                           \
        dn += 2 * n;                                                                                             \
        memcpy(data + dn, rest2, sizeof(rest2));                                                                 \
        dn += sizeof(rest2);                                                                                     \
    } while (0)
    DATA(placeholder);
    sum = (u32)crc32(0, data, (uInt)dn);
    memcpy(file + 64, &sum, 4);
    CHECK(mg_appx_pri_rename(file, sizeof(file), placeholder, (u32)sizeof(rest2), (u32)(sizeof(rest1) + 2 * n + sizeof(rest2)), name, out,
                             err, sizeof(err)));
    DATA(name);
    want = (u32)crc32(0, data, (uInt)dn);
#undef DATA
    memcpy(&sum, out + 64, 4);
    CHECK(sum == want);
    {
        u8 n16[128];
        put_utf16(n16, name, false);
        CHECK(!memcmp(out + 100, n16, 2 * n) && !memcmp(out + 300, n16, 2 * n));
    }
    CHECK(!mg_appx_pri_rename(file, sizeof(file), placeholder, 1, 2, "MeglerApp.short", out, err, sizeof(err)));
}

static void test_libc_odds(void)
{
    const u32 *size = (const u32 *)mg_bionic_find("__page_size"), *shift = (const u32 *)mg_bionic_find("__page_shift");
    say("libc's page size as data, setpgrp, ptrace");
    CHECK(size && *size == 4096 && shift && *shift == 12);
    CHECK(mg_bionic_find("setpgrp") != NULL && mg_bionic_find("ptrace") != NULL);
}

/* ------------------------------------------------------------------ the decoders behind a Java app's MediaCodec */

/* A NAL unit of H.264 after a start code: its header byte, then a few bytes nobody reads. */
static void put_avc_nal(u8 *out, size_t *n, u8 header, bool length_prefixed)
{
    static const u8 body[5] = { 0x42, 0x00, 0x1e, 0x9a, 0x11 };
    if (length_prefixed) {
        out[(*n)++] = 0, out[(*n)++] = 0, out[(*n)++] = 0, out[(*n)++] = 6;
    } else {
        out[(*n)++] = 0, out[(*n)++] = 0, out[(*n)++] = 0, out[(*n)++] = 1;
    }
    out[(*n)++] = header;
    memcpy(out + *n, body, sizeof(body));
    *n += sizeof(body);
}

/* Frames of a decoder, as MediaCodec takes them from it: their times in thirtieths of a second, and whether each picture was its time's grey. */
static int drain_frames(mg_mcodec *c, int *times, int max, bool *grey)
{
    int n = 0;
    i64 info[3];
    mg_mframe *f;
    while ((f = mg_mcodec_next_frame(c, info)) != NULL) {
        const megler_codec_picture *p = mg_mcodec_frame_picture(f);
        u8 bgra[64 * 4];
        int want = (298 * (mg_codec_standin_luma(info[0]) - 16) + 128) >> 8;
        if (want > 255) want = 255;
        if (!mg_mcodec_render(p, bgra, 64, 1, 64 * 4) || p->data[0] != mg_codec_standin_luma(info[0]) || bgra[0] != want || bgra[1] != want || bgra[2] != want ||
            bgra[3] != 255 || info[1] != p->width || info[2] != p->height)
            *grey = false;
        if (n < max) times[n] = (int)((info[0] * 30 + 500000) / 1000000);
        n++;
        mg_mcodec_free_frame(f);
    }
    return n;
}

/* Decode order of a stream with two B-pictures between references, as the test streams have it: 0 3 1 2 6 4 5 9 7 8. */
static const int k_decode_order[10] = { 0, 3, 1, 2, 6, 4, 5, 9, 7, 8 };

/*
 * Pictures through the codec (mg_java_media.c) and the stand-in for the shell's decoders (mg_codec_standin.c), which takes
 * units only as a decoder must have them, and hands pictures out in decode order: the codec gives them in display order,
 * with the parameter sets in front of the first unit after a start or a flush, whichever form the app had them in.
 */
static void test_codec_pictures(void)
{
    static const u8 avcc[] = { 1, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0, 6, 0x67, 0x42, 0x00, 0x1e, 0x9a, 0x11, 1, 0, 6, 0x68, 0x42, 0x00, 0x1e, 0x9a, 0x11 };
    int pass, i, n, times[16];
    for (pass = 0; pass < 4; pass++) {
        /* 0: csd-0 and csd-1 in Annex-B; 1: the units length-prefixed, csd-0 the avcC record; 2: the parameter sets in the first unit; 3: in a codec-config buffer */
        mg_mcodec *c = mg_mcodec_create("video/avc");
        u8 sps[16], pps[16], unit[64];
        size_t ns = 0, np = 0, nu, size[3] = { 0, 0, 0 };
        const u8 *csd[3] = { NULL, NULL, NULL };
        mg_codec_standin_counts before, after;
        bool grey = true, ordered = true;
        mg_codec_standin_counts_get(&before);
        put_avc_nal(sps, &ns, 0x67, false);
        put_avc_nal(pps, &np, 0x68, false);
        if (pass == 0) csd[0] = sps, size[0] = ns, csd[1] = pps, size[1] = np;
        if (pass == 1) csd[0] = avcc, size[0] = sizeof(avcc);
        CHECK(c && mg_mcodec_configure(c, 168, 94, 0, 0, csd, size));
        if (!c) continue;
        CHECK(mg_mcodec_size(c) == (168 << 16 | 94));
        if (pass == 3) {
            u8 config[32];
            memcpy(config, sps, ns);
            memcpy(config + ns, pps, np);
            mg_mcodec_decode(c, config, ns + np, 0, 2);
        }
        n = 0;
        for (i = 0; i < 10; i++) {
            nu = 0;
            if (pass == 2 && i == 0) {
                memcpy(unit, sps, ns);
                memcpy(unit + ns, pps, np);
                nu = ns + np;
            }
            put_avc_nal(unit, &nu, k_decode_order[i] % 3 == 0 && !i ? 0x65 : 0x41, pass == 1);
            mg_mcodec_decode(c, unit, nu, (i64)k_decode_order[i] * 1000000 / 30, i == 9 ? 4 : 0);
            n += drain_frames(c, times + n, 16 - n, &grey);
        }
        for (i = 0; i < 10 && i < n; i++)
            if (times[i] != i) ordered = false;
        mg_codec_standin_counts_get(&after);
        CHECK_INT(n, 10);
        CHECK(ordered && grey);
        CHECK_INT(after.refused - before.refused, 0);
        CHECK(mg_mcodec_size(c) == (168 << 16 | 94));
        mg_mcodec_destroy(c);
        mg_codec_standin_counts_get(&after);
        CHECK_INT(after.pictures_released - before.pictures_released, 10);
        CHECK_INT(after.closed - before.closed, 1);
    }
    {
        /*
         * A flush: four units in, none out yet (the stand-in's four are held to be put in order), then a flush, which drops them
         * here and in the decoder, and a key frame and three more after it: those four alone come out, the parameter sets in front
         * of the key frame again.
         */
        mg_mcodec *c = mg_mcodec_create("video/avc");
        u8 sps[16], pps[16], unit[64];
        size_t ns = 0, np = 0, nu, size[3] = { 0, 0, 0 };
        const u8 *csd[3] = { NULL, NULL, NULL };
        static const int after_flush[4] = { 12, 15, 13, 14 };
        mg_codec_standin_counts before, after;
        bool grey = true;
        put_avc_nal(sps, &ns, 0x67, false);
        put_avc_nal(pps, &np, 0x68, false);
        csd[0] = sps, size[0] = ns, csd[1] = pps, size[1] = np;
        mg_codec_standin_counts_get(&before);
        CHECK(mg_mcodec_configure(c, 0, 0, 0, 0, csd, size));
        n = 0;
        for (i = 0; i < 4; i++) {
            nu = 0;
            put_avc_nal(unit, &nu, i ? 0x41 : 0x65, false);
            mg_mcodec_decode(c, unit, nu, (i64)k_decode_order[i] * 1000000 / 30, 0);
            n += drain_frames(c, times + n, 16 - n, &grey);
        }
        CHECK_INT(n, 0);
        mg_mcodec_flush(c);
        mg_codec_standin_counts_get(&after);
        CHECK_INT(after.pictures_released - before.pictures_released, 4);
        CHECK_INT(after.flushed - before.flushed, 1);
        for (i = 0; i < 4; i++) {
            nu = 0;
            put_avc_nal(unit, &nu, i ? 0x41 : 0x65, false);
            mg_mcodec_decode(c, unit, nu, (i64)after_flush[i] * 1000000 / 30, i == 3 ? 4 : 0);
            n += drain_frames(c, times + n, 16 - n, &grey);
        }
        mg_codec_standin_counts_get(&after);
        CHECK(n == 4 && times[0] == 12 && times[1] == 13 && times[2] == 14 && times[3] == 15 && grey);
        CHECK_INT(after.refused - before.refused, 0);
        /* (The stand-in was opened without a size: 64 x 48 is what its pictures are, and what the codec says now.) */
        CHECK(mg_mcodec_size(c) == (64 << 16 | 48));
        mg_mcodec_destroy(c);
    }
    {
        /*
         * A decoder that says it gives pictures in display order (reorder 0) and gives them in decode order: the first picture out
         * of place (1, after 3) goes on as it is, and from then on four are held back, so that the rest come in order; after a
         * flush the same units come out in order from the first.
         */
        static megler_codec_host liar;
        mg_mcodec *c;
        u8 sps[16], pps[16], unit[64];
        size_t ns = 0, np = 0, nu, size[3] = { 0, 0, 0 };
        const u8 *csd[3] = { NULL, NULL, NULL };
        static const int first_run[10] = { 0, 3, 1, 2, 4, 5, 6, 7, 8, 9 };
        int run;
        bool grey = true;
        liar = *mg_codec_standin();
        liar.reorder = 0;
        mg_java_media_set_host(&liar);
        put_avc_nal(sps, &ns, 0x67, false);
        put_avc_nal(pps, &np, 0x68, false);
        csd[0] = sps, size[0] = ns, csd[1] = pps, size[1] = np;
        c = mg_mcodec_create("video/avc");
        CHECK(mg_mcodec_configure(c, 0, 0, 0, 0, csd, size));
        for (run = 0; run < 2; run++) {
            bool as_said = true;
            n = 0;
            for (i = 0; i < 10; i++) {
                nu = 0;
                put_avc_nal(unit, &nu, i ? 0x41 : 0x65, false);
                mg_mcodec_decode(c, unit, nu, (i64)k_decode_order[i] * 1000000 / 30, i == 9 ? 4 : 0);
                n += drain_frames(c, times + n, 16 - n, &grey);
            }
            for (i = 0; i < 10 && i < n; i++)
                if (times[i] != (run ? i : first_run[i])) as_said = false;
            CHECK(n == 10 && as_said && grey);
            mg_mcodec_flush(c);
        }
        mg_mcodec_destroy(c);
        mg_java_media_set_host(mg_codec_standin());
    }
    {
        /* No parameter sets anywhere: nothing to put in front, and the decoder refuses the unit, as a real one finds nothing to decode. */
        mg_mcodec *c = mg_mcodec_create("video/avc");
        u8 unit[32];
        size_t nu = 0, size[3] = { 0, 0, 0 };
        const u8 *csd[3] = { NULL, NULL, NULL };
        mg_codec_standin_counts before, after;
        bool grey = true;
        mg_codec_standin_counts_get(&before);
        CHECK(mg_mcodec_configure(c, 32, 32, 0, 0, csd, size));
        put_avc_nal(unit, &nu, 0x65, false);
        mg_mcodec_decode(c, unit, nu, 0, 4);
        mg_codec_standin_counts_get(&after);
        CHECK(drain_frames(c, times, 16, &grey) == 0 && after.refused - before.refused == 1);
        mg_mcodec_destroy(c);
    }
}

/* Sound blocks of a decoder: how many, and the frames in all; the rate and channels of the last. */
static int drain_pcm(mg_mcodec *c, int *frames, i64 *rate, i64 *channels)
{
    int blocks = 0;
    i64 info[3];
    size_t bytes;
    u8 *pcm;
    while ((pcm = mg_mcodec_next_pcm(c, info, &bytes)) != NULL) {
        blocks++;
        *rate = info[1];
        *channels = info[2];
        if (info[2] > 0) *frames += (int)(bytes / 2 / (size_t)info[2]);
        free(pcm);
    }
    return blocks;
}

/* An ADTS header of AAC LC at 44100 Hz in two channels before a frame of `n` bytes. */
static size_t put_adts(u8 *out, size_t n)
{
    size_t len = n + 7;
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (u8)(1 << 6 | 4 << 2);
    out[3] = (u8)(2 << 6 | (len >> 11));
    out[4] = (u8)(len >> 3);
    out[5] = (u8)((len & 7) << 5 | 0x1F);
    out[6] = 0xFC;
    memset(out + 7, 0x21, n);
    return len;
}

/* Sound through the codec and the stand-in: AAC raw or with ADTS headers, with an AudioSpecificConfig or without, and MP3. */
static void test_codec_sound(void)
{
    static const u8 lc_44100_stereo[2] = { 0x12, 0x10 }, lc_48000_mono[2] = { 0x11, 0x88 };
    u8 packet[300];
    size_t size[3] = { 0, 0, 0 };
    const u8 *csd[3] = { NULL, NULL, NULL };
    mg_codec_standin_counts before, after;
    mg_mcodec *c;
    int frames, i;
    i64 rate, channels;
    mg_codec_standin_counts_get(&before);
    /* Raw packets and csd-0, one block of 1024 frames for each. */
    c = mg_mcodec_create("audio/mp4a-latm");
    csd[0] = lc_44100_stereo, size[0] = 2;
    CHECK(mg_mcodec_configure(c, 0, 0, 44100, 2, csd, size));
    memset(packet, 0x21, sizeof(packet));
    for (i = 0; i < 3; i++) mg_mcodec_decode(c, packet, 200, (i64)i * 23220, i == 2 ? 4 : 0);
    frames = 0, rate = channels = 0;
    CHECK(drain_pcm(c, &frames, &rate, &channels) == 3 && frames == 3 * 1024 && rate == 44100 && channels == 2);
    mg_mcodec_destroy(c);
    /* The configuration is what the decoder is opened with, over what the format says: 48 kHz in one channel. */
    c = mg_mcodec_create("audio/mp4a-latm");
    csd[0] = lc_48000_mono;
    CHECK(mg_mcodec_configure(c, 0, 0, 44100, 2, csd, size));
    mg_mcodec_decode(c, packet, 200, 0, 0);
    frames = 0, rate = channels = 0;
    CHECK(drain_pcm(c, &frames, &rate, &channels) == 1 && frames == 1024 && rate == 48000 && channels == 1);
    /* A codec-config buffer with another configuration: a decoder for that one. */
    mg_mcodec_decode(c, lc_44100_stereo, 2, 0, 2);
    mg_mcodec_decode(c, packet, 200, 0, 0);
    frames = 0, rate = channels = 0;
    CHECK(drain_pcm(c, &frames, &rate, &channels) == 1 && frames == 1024 && rate == 44100 && channels == 2);
    mg_mcodec_destroy(c);
    /* ADTS and no configuration: the header says it, and comes off; a buffer with two frames is two packets. */
    c = mg_mcodec_create("audio/mp4a-latm");
    csd[0] = NULL, size[0] = 0;
    CHECK(mg_mcodec_configure(c, 0, 0, 22050, 1, csd, size));
    mg_mcodec_decode(c, packet, put_adts(packet, 100), 0, 0);
    {
        size_t first = put_adts(packet, 90);
        mg_mcodec_decode(c, packet, first + put_adts(packet + first, 80), 23220, 4);
    }
    frames = 0, rate = channels = 0;
    CHECK(drain_pcm(c, &frames, &rate, &channels) == 3 && frames == 3 * 1024 && rate == 44100 && channels == 2);
    mg_mcodec_destroy(c);
    /* MP3 with no rate said: the first frame's header says 22050 Hz in one channel (MPEG-2 Layer III, mono). */
    c = mg_mcodec_create("audio/mpeg");
    CHECK(mg_mcodec_configure(c, 0, 0, 0, 0, csd, size));
    memset(packet, 0, sizeof(packet));
    packet[0] = 0xFF, packet[1] = 0xF3, packet[2] = 0x80, packet[3] = 0xC4;
    mg_mcodec_decode(c, packet, 144, 0, 0);
    mg_mcodec_decode(c, packet, 144, 52245, 4);
    frames = 0, rate = channels = 0;
    CHECK(drain_pcm(c, &frames, &rate, &channels) == 2 && frames == 2 * 1152 && rate == 22050 && channels == 1);
    mg_mcodec_destroy(c);
    mg_codec_standin_counts_get(&after);
    CHECK_INT(after.refused - before.refused, 0);
    CHECK_INT(after.opened - before.opened, after.closed - before.closed);
}

/* NV12 to the bitmaps' colours: red in BT.601 and BT.709 at video range, and at full range; and RGBA as it is. */
static void test_codec_colours(void)
{
    u8 y[2], uv[2], rgba[4] = { 10, 20, 30, 40 }, out[8];
    megler_codec_picture p;
    memset(&p, 0, sizeof(p));
    p.format = MEGLER_CODEC_NV12;
    p.width = 2;
    p.height = 1;
    p.data = y;
    p.chroma = uv;
    p.stride = p.chroma_stride = 2;
    y[0] = y[1] = 81, uv[0] = 90, uv[1] = 240;
    p.matrix = MEGLER_CODEC_BT601;
    CHECK(mg_mcodec_render(&p, out, 2, 1, 8) && out[2] >= 253 && out[1] <= 2 && out[0] <= 2 && out[3] == 255 && out[6] == out[2]);
    y[0] = y[1] = 63, uv[0] = 102, uv[1] = 240;
    p.matrix = MEGLER_CODEC_BT709;
    CHECK(mg_mcodec_render(&p, out, 2, 1, 8) && out[2] >= 253 && out[1] <= 2 && out[0] <= 2);
    /* Not said, a small picture: BT.601, under which BT.709's red is a darker one. */
    p.matrix = 0;
    CHECK(mg_mcodec_render(&p, out, 2, 1, 8) && out[2] >= 225 && out[2] <= 240);
    y[0] = y[1] = 76, uv[0] = 85, uv[1] = 255;
    p.matrix = MEGLER_CODEC_BT601;
    p.full_range = true;
    CHECK(mg_mcodec_render(&p, out, 2, 1, 8) && out[2] >= 253 && out[1] <= 2 && out[0] <= 2);
    p.format = MEGLER_CODEC_RGBA;
    p.width = 1;
    p.data = rgba;
    p.stride = 4;
    CHECK(mg_mcodec_render(&p, out, 1, 1, 4) && out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 255);
}

static void test_codecs(void)
{
    say("the decoders behind a Java app's MediaCodec, over a stand-in for the shell's");
    mg_java_media_set_host(NULL);
    CHECK(!mg_mcodec_supported("video/avc") && !mg_mcodec_create("video/avc"));
    mg_java_media_set_host(mg_codec_standin());
    CHECK(mg_mcodec_supported("video/avc") && mg_mcodec_supported("VIDEO/AVC") && mg_mcodec_supported("audio/mp4a-latm") && mg_mcodec_supported("audio/mpeg"));
    CHECK(!mg_mcodec_supported("video/hevc") && !mg_mcodec_supported("video/x-vnd.on2.vp8") && !mg_mcodec_supported(NULL));
    test_codec_pictures();
    test_codec_sound();
    test_codec_colours();
    mg_java_media_set_host(NULL);
}

/* ---------------------------------------------------------------- cameras */

enum { CAM_R, CAM_G, CAM_B, CAM_W };

/* The stand-in's picture as the core holds it (NV21, 320 x 240): every byte of the luma and of the V, U pairs what its quarter says. */
static bool camera_nv21_right(const u8 *p, int w, int h)
{
    static const u8 q[4][3] = { { MG_CAMERA_STANDIN_RED }, { MG_CAMERA_STANDIN_GREEN }, { MG_CAMERA_STANDIN_BLUE }, { MG_CAMERA_STANDIN_WHITE } };
    int x, y;
    for (y = 0; y < h; y++)
        for (x = y ? 0 : 4; x < w; x++)
            if (p[y * w + x] != q[(y < h / 2 ? 0 : 2) + (x < w / 2 ? 0 : 1)][0]) return false;
    for (y = 0; y < h / 2; y++)
        for (x = 0; x < w / 2; x++) {
            const u8 *want = q[(y < h / 4 ? 0 : 2) + (x < w / 4 ? 0 : 1)], *vu = p + w * h + (y * (w / 2) + x) * 2;
            if (vu[0] != want[2] || vu[1] != want[1]) return false;
        }
    return true;
}

/* Whether the four quarters' middles of a picture in the bitmaps' order (B, G, R, A) are the colours named, each channel within 2. */
static bool camera_quarters(const u8 *bgra, int w, int h, const int want[4])
{
    static const int rgb[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
    int i;
    for (i = 0; i < 4; i++) {
        const u8 *p = bgra + ((size_t)((i / 2) * h / 2 + h / 4) * (size_t)w + (size_t)((i % 2) * w / 2 + w / 4)) * 4;
        const int *c = rgb[want[i]];
        if (abs(p[2] - c[0]) > 2 || abs(p[1] - c[1]) > 2 || abs(p[0] - c[2]) > 2 || p[3] != 255) return false;
    }
    return true;
}

/* News from the camera, gathered until one of `want` comes or `ms` pass. */
static int camera_news(mg_jcamera *c, int want, int ms)
{
    int news = 0, i;
    for (i = 0; i < ms / 20 && !(news & want); i++) news |= mg_jcamera_wait(c, 20);
    return news;
}

static i32 camera_number(const u8 *p) { return (i32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); }

/*
 * The cameras behind a Java app's android.hardware.Camera (drivers/mg_java_camera.c), over the stand-in for the shell's
 * (mg_camera_standin.c): what the cameras are, opening (refused, busy, no such camera), the stand-in's pictures held as
 * NV21 byte for byte from rows longer than the picture, copied as NV21 and YV12, drawn for a view in the four turns with
 * and without mirroring, focusing and its cancelling, the torch, the pictures' end with stop, and no more pictures than the
 * rate the camera was started at.
 */
static void test_camera(void)
{
    /* The quarters' colours (top left, top right, bottom left, bottom right) after each turn, without and with the mirror. */
    static const int turned[4][4] = { { CAM_R, CAM_G, CAM_B, CAM_W }, { CAM_B, CAM_R, CAM_W, CAM_G }, { CAM_W, CAM_B, CAM_G, CAM_R }, { CAM_G, CAM_W, CAM_R, CAM_B } };
    static const int mirrored[4][4] = { { CAM_G, CAM_R, CAM_W, CAM_B }, { CAM_W, CAM_G, CAM_B, CAM_R }, { CAM_B, CAM_W, CAM_R, CAM_G }, { CAM_R, CAM_B, CAM_G, CAM_W } };
    megler_camera_info info;
    mg_camera_standin_counts k0, k1;
    mg_jcamera *c, *front;
    u8 *nv21, *yv12, *bgra;
    int err = -1, r, m, news;
    i32 first, second;
    bool all;
    say("the cameras behind a Java app's android.hardware.Camera, over a stand-in for the shell's");
    mg_java_camera_set_host(NULL);
    CHECK(mg_jcamera_count() == 0 && mg_jcamera_features() == 0 && !mg_jcamera_open(0, &err));
    mg_java_camera_set_host(mg_camera_standin());
    CHECK_INT(mg_jcamera_count(), 2);
    CHECK(mg_jcamera_info(0, &info) && info.facing == MEGLER_CAMERA_BACK && info.orientation == 90 && info.focus == 1 && info.light == 1);
    CHECK(mg_jcamera_info(1, &info) && info.facing == MEGLER_CAMERA_FRONT && info.orientation == 270 && info.focus == 0 && info.light == 0);
    CHECK(!mg_jcamera_info(2, &info) && !mg_jcamera_info(-1, &info));
    CHECK_INT(mg_jcamera_features(), MG_JCAMERA_ANY | MG_JCAMERA_HAS_BACK | MG_JCAMERA_HAS_FRONT | MG_JCAMERA_HAS_AUTOFOCUS | MG_JCAMERA_HAS_FLASH);
    mg_camera_standin_counts_get(&k0);
    c = mg_jcamera_open(0, &err);
    CHECK(c && err == MEGLER_CAMERA_OK);
    if (!c) return;
    CHECK(mg_jcamera_caps(c)->size_count == 3 && mg_jcamera_caps(c)->sizes[0].width == 1280 && mg_jcamera_caps(c)->sizes[2].height == 240 &&
          mg_jcamera_caps(c)->sizes[2].max_fps == 30 && mg_jcamera_caps(c)->focus && mg_jcamera_caps(c)->torch);
    /* The same camera again is busy; a camera there is not is no camera. */
    CHECK(!mg_jcamera_open(0, &err) && err == MEGLER_CAMERA_BUSY);
    CHECK(!mg_jcamera_open(7, &err) && err == MEGLER_CAMERA_FAILED);
    /* Nothing comes and nothing is held before it streams; focusing is refused. */
    CHECK(mg_jcamera_wait(c, 50) == 0 && mg_jcamera_size(c) == 0 && !mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_ONCE));
    CHECK(mg_jcamera_start(c, 320, 240, 30));
    news = camera_news(c, MG_JCAMERA_FRAME, 5000);
    CHECK((news & MG_JCAMERA_FRAME) && mg_jcamera_size(c) == (320 << 16 | 240));
    nv21 = (u8 *)mg_alloc(320 * 240 * 3 / 2);
    yv12 = (u8 *)mg_alloc(320 * 240 * 3 / 2);
    bgra = (u8 *)mg_alloc(320 * 240 * 4);
    CHECK_INT(mg_jcamera_frame_bytes(320, 240, 17), 115200);
    CHECK_INT(mg_jcamera_copy(c, nv21, 115199, 17), -1);
    CHECK_INT(mg_jcamera_copy(c, nv21, 115200, 17), 115200);
    first = camera_number(nv21);
    CHECK(first >= 1 && camera_nv21_right(nv21, 320, 240));
    /* YV12: rows of 320 and 160 (both 16-byte aligned here; 100 wide has 112 and 64), V's plane before U's. */
    CHECK_INT(mg_jcamera_frame_bytes(320, 240, 0x32315659), 115200);
    CHECK_INT(mg_jcamera_frame_bytes(100, 10, 0x32315659), 112 * 10 + 64 * 5 * 2);
    CHECK_INT(mg_jcamera_copy(c, yv12, 115200, 0x32315659), 115200);
    CHECK(!memcmp(yv12, nv21, 320 * 240) && yv12[76800] == 240 && yv12[76800 + 159] == 34 && yv12[76800 + 160 * 119] == 110 && yv12[76800 + 19200] == 90 &&
          yv12[76800 + 19200 + 160 * 119 + 159] == 128);
    /* For a view: each turn, mirrored and not, the quarters where the turn puts them; a bitmap of the wrong size refused. */
    all = true;
    for (m = 0; m < 2; m++)
        for (r = 0; r < 4; r++) {
            int w = r % 2 ? 240 : 320, h = r % 2 ? 320 : 240;
            memset(bgra, 0, 320 * 240 * 4);
            if (!mg_jcamera_render(c, bgra, w, h, w * 4, r * 90, m == 1) || !camera_quarters(bgra, w, h, m ? mirrored[r] : turned[r])) {
                all = false;
                say("  the picture turned %d degrees%s is not as it should be", r * 90, m ? ", mirrored," : "");
            }
        }
    CHECK(all);
    CHECK(!mg_jcamera_render(c, bgra, 320, 240, 320 * 4, 90, false));
    /* The next picture has a higher number. */
    news = camera_news(c, MG_JCAMERA_FRAME, 5000);
    CHECK((news & MG_JCAMERA_FRAME) && mg_jcamera_copy(c, nv21, 115200, 17) == 115200 && (second = camera_number(nv21)) > first && camera_nv21_right(nv21, 320, 240));
    /* Focusing once: in focus. Asked and cancelled: no answer after the cancel. */
    CHECK(mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_ONCE));
    CHECK(camera_news(c, MG_JCAMERA_FOCUSED, 5000) & MG_JCAMERA_FOCUSED);
    CHECK(mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_MACRO) && mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_NONE));
    CHECK(!(camera_news(c, MG_JCAMERA_FOCUSED, 300) & (MG_JCAMERA_FOCUSED | MG_JCAMERA_NOT_FOCUSED)));
    CHECK(mg_jcamera_focus(c, MEGLER_CAMERA_FOCUS_CONTINUOUS) && (camera_news(c, MG_JCAMERA_FOCUSED, 5000) & MG_JCAMERA_FOCUSED));
    /* The torch, on and off; and off when the camera stops. */
    mg_jcamera_torch(c, true);
    mg_camera_standin_counts_get(&k1);
    CHECK(k1.torch);
    mg_jcamera_stop(c);
    mg_camera_standin_counts_get(&k1);
    CHECK(!k1.torch && k1.stopped == k0.stopped + 1 && k1.width == 320 && k1.height == 240 && k1.fps == 30);
    /* Stopped: no more pictures. */
    mg_sleep_ns(100000000ull);
    mg_camera_standin_counts_get(&k0);
    CHECK(!(camera_news(c, MG_JCAMERA_FRAME, 200) & MG_JCAMERA_FRAME));
    mg_camera_standin_counts_get(&k1);
    CHECK_INT(k1.frames, k0.frames);
    /* The front camera beside it: fixed focus, no torch; its picture mirrored and turned as an app shows it (90 for 270). */
    front = mg_jcamera_open(1, &err);
    CHECK(front && !mg_jcamera_caps(front)->focus && !mg_jcamera_caps(front)->torch && mg_jcamera_caps(front)->size_count == 2);
    if (front) {
        CHECK(mg_jcamera_start(front, 640, 480, 15));
        CHECK((camera_news(front, MG_JCAMERA_FRAME, 5000) & MG_JCAMERA_FRAME) && mg_jcamera_size(front) == (640 << 16 | 480));
        free(bgra);
        bgra = (u8 *)mg_alloc(640 * 480 * 4);
        CHECK(mg_jcamera_render(front, bgra, 480, 640, 480 * 4, 90, true) && camera_quarters(bgra, 480, 640, mirrored[1]));
        CHECK(!mg_jcamera_focus(front, MEGLER_CAMERA_FOCUS_ONCE));
        mg_jcamera_close(front);
    }
    mg_jcamera_close(c);
    /*
     * Closed, a camera opens again. Started at 10 frames a second, which the stand-in's slowest (15) is faster than: the pictures
     * that come sooner than the rate asked for are dropped before they are copied, and each picture is kept or dropped.
     */
    c = mg_jcamera_open(0, &err);
    CHECK(c != NULL);
    if (c) {
        i64 kept, dropped;
        mg_camera_standin_counts_get(&k0);
        CHECK(mg_jcamera_start(c, 320, 240, 10));
        mg_sleep_ns(1500000000ull);
        mg_jcamera_stop(c);
        mg_camera_standin_counts_get(&k1);
        mg_jcamera_counts(c, &kept, &dropped);
        say("  started at 10 frames a second on a camera of 15: %lld pictures kept, %lld dropped", (long long)kept, (long long)dropped);
        CHECK(k1.fps == 15 && kept > 0 && dropped > 0 && kept + dropped == k1.frames - k0.frames);
        mg_jcamera_close(c);
    }
    /* Refused, it does not open, and says so. */
    mg_camera_standin_refuse(true);
    CHECK(!mg_jcamera_open(0, &err) && err == MEGLER_CAMERA_DENIED);
    mg_camera_standin_refuse(false);
    /* What the app leaves open is closed when it ends. */
    mg_camera_standin_counts_get(&k0);
    c = mg_jcamera_open(0, &err);
    CHECK(c && mg_jcamera_start(c, 640, 480, 30));
    mg_java_camera_close_all();
    mg_camera_standin_counts_get(&k1);
    CHECK(k1.closed == k0.closed + 1 && k1.stopped == k0.stopped + 1 && (mg_jcamera_wait(c, 10) & MG_JCAMERA_GONE));
    free(nv21);
    free(yv12);
    free(bgra);
    mg_java_camera_set_host(NULL);
}

/* The battery the core answers for (run/mg_run.h): a full one on mains until one is set, then the one set, made safe to divide by and to read as text. */
static void test_battery(void)
{
    mg_battery b, set;
    say("the battery");
    memset(&b, 0, sizeof(b));
    mg_battery_read(&b);
    CHECK(b.level == 100 && b.scale == 100 && b.status == 5 && b.plugged == 1 && b.health == 2 && b.voltage_mv == 4200 && b.temperature == 250 && b.present && !strcmp(b.technology, "Li-ion"));
    memset(&set, 0, sizeof(set));
    set.level = 12;
    set.status = 3;
    set.health = 2;
    set.voltage_mv = 3600;
    set.temperature = -52;
    set.present = true;
    memset(set.technology, 'x', sizeof(set.technology));
    mg_battery_set(&set);
    mg_battery_read(&b);
    CHECK(b.level == 12 && b.scale == 100 && b.status == 3 && b.plugged == 0 && b.voltage_mv == 3600 && b.temperature == -52 && b.present);
    CHECK(strlen(b.technology) == sizeof(b.technology) - 1);
    mg_battery_set(NULL);
    mg_battery_read(&b);
    CHECK(b.level == 100 && b.plugged == 1 && !strcmp(b.technology, "Li-ion"));
    {
        /*
         * The shell's battery (megler.h): what a phone with Windows says, and the rest filled in. And the count of changes an app is
         * told of: one for each time the level, the status or the plug became another, none for the same again or for the
         * voltage alone.
         */
        megler_battery phone;
        u32 changes = mg_battery_changes();
        memset(&phone, 0, sizeof(phone));
        phone.level = 50;
        phone.status = 3;
        phone.temperature = INT_MIN;
        phone.present = true;
        megler_set_battery(&phone);
        mg_battery_read(&b);
        CHECK(b.level == 50 && b.scale == 100 && b.status == 3 && b.plugged == 0 && b.health == 2 && b.voltage_mv == 3800 && b.temperature == 250 && b.present
              && !strcmp(b.technology, "Li-ion"));
        CHECK(mg_battery_changes() == changes + 1);
        megler_set_battery(&phone);
        CHECK(mg_battery_changes() == changes + 1);
        phone.voltage_mv = 3777;
        phone.temperature = -15;
        megler_set_battery(&phone);
        mg_battery_read(&b);
        CHECK(b.voltage_mv == 3777 && b.temperature == -15 && mg_battery_changes() == changes + 1);
        phone.plugged = 2;
        phone.status = 2;
        megler_set_battery(&phone);
        CHECK(mg_battery_changes() == changes + 2);
        /* What cannot be is brought to what can. */
        phone.level = 140;
        phone.status = 9;
        phone.plugged = -1;
        phone.present = false;
        megler_set_battery(&phone);
        mg_battery_read(&b);
        CHECK(b.level == 100 && b.status == 1 && b.plugged == 0 && b.health == 1 && !b.present);
        megler_set_battery(NULL);
        mg_battery_read(&b);
        CHECK(b.level == 100 && b.status == 5 && b.plugged == 1 && b.voltage_mv == 4200);
    }
}

static void test_input(void)
{
    enum { TAPS = 60000 };
    mg_thread *taker;
    int i, waited;
    say("input queue");
    if (!mg_bionic_find("AInputQueue_getEvent") || !mg_bionic_find("ALooper_pollAll")) {
        CHECK(!"the NDK's functions are registered");
        return;
    }
    memset(&G_INPUT, 0, sizeof(G_INPUT));
    G_INPUT.queue = mg_input_queue_create();
    taker = mg_thread_start(input_taker, NULL, 256 * 1024, "test-input");
    for (i = 0; i < TAPS; i++) {
        mg_input_queue_touch(G_INPUT.queue, 0, 1, (float)i, 10.0f);
        mg_input_queue_touch(G_INPUT.queue, 1, 1, (float)i, 11.0f);
        mg_input_queue_touch(G_INPUT.queue, 2, 1, (float)i, 12.0f);
        /* The queue is not endless; a touch is not put in faster than it is taken for long. */
        for (waited = 0; waited < 2000000 && i - G_INPUT.ups > 40; waited++) mg_thread_yield();
    }
    for (waited = 0; waited < 2000 && G_INPUT.ups < TAPS; waited++) mg_sleep_ns(1000000ull);
    mg_atomic_xchg(&G_INPUT.stop, 1);
    mg_thread_join(taker);
    /* No touch lost, however they were put in... */
    CHECK_INT(G_INPUT.downs, TAPS);
    CHECK_INT(G_INPUT.ups, TAPS);
    /* ...and nothing said to be there once there is not. */
    CHECK_INT(G_INPUT.left_readable, 0);
    say("  wakes with nothing to take: %d", (int)G_INPUT.idle_wakes);
}

#if MG_HOST_ARM32

/*
 * An interface proxy of Unity's (jni/mg_jni_loop.c): a call through it and its
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
typedef mg_jobj *(*jni_find_class_fn)(void *, const char *);
typedef void *(*jni_id_fn)(void *, mg_jobj *, const char *, const char *);
typedef mg_jobj *(*jni_object_a_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
typedef void (*jni_void_a_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
typedef u8 (*jni_boolean_a_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
typedef mg_jobj *(*jni_string_fn)(void *, const char *);
typedef mg_jobj *(*jni_element_fn)(void *, mg_jobj *, i32);
enum { JNI_FIND_CLASS = 6, JNI_NEW_OBJECT_A = 30, JNI_METHOD_ID = 33, JNI_OBJECT_A = 36, JNI_BOOLEAN_A = 39, JNI_VOID_A = 63, JNI_STATIC_ID = 113,
       JNI_STATIC_OBJECT_A = 116, JNI_STATIC_VOID_A = 143, JNI_NEW_STRING_UTF = 167, JNI_ARRAY_ELEMENT = 173, JNI_REGISTER = 215 };

static void *proxied_invoke(void *env, void *cls, i64 handle, void *iface, void *method, void *args)
{
    proxied *p = (proxied *)(mg_addr)handle;
    int i;
    (void)cls; (void)iface; (void)method;
    if (p->quits) {
        /*
         * A last message to a thread, whose handler ends the thread's loop from inside. 1: Looper.myLooper().quit().
         * 2: message.getTarget().getLooper().quit(), which is how Unity 2020 ends a decoder's thread. 3: it forgets to.
         */
        void **table = *(void ***)env;
        jni_id_fn id = (jni_id_fn)table[JNI_METHOD_ID];
        mg_jobj *looper_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Looper");
        mg_jobj *looper = NULL;
        if (p->quits == 1) {
            void *mine = ((jni_id_fn)table[JNI_STATIC_ID])(env, looper_cls, "myLooper", "()Landroid/os/Looper;");
            looper = ((jni_object_a_fn)table[JNI_STATIC_OBJECT_A])(env, looper_cls, mine, NULL);
        } else if (p->quits == 2) {
            mg_jobj *message_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Message");
            mg_jobj *handler_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Handler");
            mg_jobj *message = ((jni_element_fn)table[JNI_ARRAY_ELEMENT])(env, (mg_jobj *)args, 0);
            mg_jobj *target = message ? ((jni_object_a_fn)table[JNI_OBJECT_A])(env, message, id(env, message_cls, "getTarget", "()Landroid/os/Handler;"), NULL) : NULL;
            looper = target ? ((jni_object_a_fn)table[JNI_OBJECT_A])(env, target, id(env, handler_cls, "getLooper", "()Landroid/os/Looper;"), NULL) : NULL;
        }
        if (looper) ((jni_void_a_fn)table[JNI_VOID_A])(env, looper, id(env, looper_cls, "quit", "()V"), NULL);
        mg_sleep_ns(40000000ull);
        mg_atomic_xchg(&p->done, looper || p->quits == 3 ? 1 : -1);
        return NULL;
    }
    if (!p->alive) mg_atomic_add(&p->dead, 1);
    for (i = 0; i < 20; i++) mg_thread_yield();
    if (!p->alive) mg_atomic_add(&p->dead, 1);
    mg_atomic_add(&p->calls, 1);
    return NULL;
}

typedef struct proxy_caller {
    mg_jobj *proxy;
    proxied *p;
} proxy_caller;

static void proxy_caller_thread(void *arg)
{
    proxy_caller *c = (proxy_caller *)arg;
    while (!c->p->stop) mg_jni_unref(mg_jni_proxy_call(c->proxy, "java/lang/Runnable", "run", "()V", NULL, 0));
}

static void test_proxies(void)
{
    enum { ROUNDS = 200 };
    /* The JNIEnv's own functions, by their places in its table, as an app's library calls them. */
    typedef struct { u32 name, sig, fn; } native_method;
    typedef mg_jobj *(*find_class_fn)(void *, const char *);
    typedef i32 (*register_fn)(void *, mg_jobj *, const native_method *, i32);
    typedef void *(*static_id_fn)(void *, mg_jobj *, const char *, const char *);
    typedef mg_jobj *(*call_object_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
    typedef void (*call_void_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
    static const char bridge[] = "bitter/jnibridge/JNIBridge";
    static const char invoke_sig[] = "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;";
    void *env;
    void **table;
    native_method m;
    mg_jobj *cls;
    void *new_id, *off_id;
    i32 dead = 0, calls = 0;
    int round, waited;
    say("interface proxies: a call and a disable keep out of each other's way");
    mg_jni_init();
    mg_jni_loop_install();
    env = mg_jni_env();
    table = *(void ***)env;
    cls = ((find_class_fn)table[6])(env, bridge);
    m.name = (u32)(mg_addr)"invoke";
    m.sig = (u32)(mg_addr)invoke_sig;
    m.fn = (u32)(mg_addr)proxied_invoke;
    ((register_fn)table[215])(env, cls, &m, 1);
    CHECK(mg_jni_native(bridge, "invoke", invoke_sig) == (void *)proxied_invoke);
    new_id = ((static_id_fn)table[113])(env, cls, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;");
    off_id = ((static_id_fn)table[113])(env, cls, "disableInterfaceProxy", "(Ljava/lang/Object;)V");
    CHECK(new_id && off_id);
    if (!new_id || !off_id) return;
    for (round = 0; round < ROUNDS; round++) {
        proxied p;
        proxy_caller caller;
        mg_jvalue a[2];
        mg_thread *t;
        memset(&p, 0, sizeof(p));
        p.alive = 1;
        memset(a, 0, sizeof(a));
        a[0].j = (i64)(mg_addr)&p;
        caller.proxy = ((call_object_fn)table[116])(env, cls, new_id, a);
        caller.p = &p;
        if (!caller.proxy) break;
        t = mg_thread_start(proxy_caller_thread, &caller, 256 * 1024, "test-proxy");
        for (waited = 0; waited < 2000000 && p.calls < 3; waited++) mg_thread_yield();
        memset(a, 0, sizeof(a));
        a[0].l = caller.proxy;
        ((call_void_fn)table[143])(env, cls, off_id, a);
        /* What the engine does next: the object behind the proxy goes. */
        mg_atomic_xchg(&p.alive, 0);
        for (waited = 0; waited < 50; waited++) mg_thread_yield();
        mg_atomic_xchg(&p.stop, 1);
        mg_thread_join(t);
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
        mg_jvalue a[2];
        mg_jobj *ht_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/HandlerThread");
        mg_jobj *h_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Handler");
        mg_jobj *m_cls = ((jni_find_class_fn)table[JNI_FIND_CLASS])(env, "android/os/Message");
        jni_id_fn id = (jni_id_fn)table[JNI_METHOD_ID];
        jni_object_a_fn object_a = (jni_object_a_fn)table[JNI_OBJECT_A];
        jni_void_a_fn void_a = (jni_void_a_fn)table[JNI_VOID_A];
        mg_jobj *ht, *looper, *proxy, *handler, *msg;
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
        a[0].j = (i64)(mg_addr)&p;
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
static u64 host_hypot(u64 a, u64 b) { return mg_db(sqrt(mg_d(a) * mg_d(a) + mg_d(b) * mg_d(b))); }

#define SYM(lib, name) mg_ld_sym(lib, name)
#define FN(lib, type, name) MG_GFN(type, SYM(lib, name))

static void test_library(mg_lib *lib, const char *what, const char *scratch)
{
    char buf[128], word[16];
    int a = 0, *counter;
    float f = 0;
    double ip = 0;
    mg_ld_stats st;
    say("guest library: %s", what);
    mg_ld_get_stats(lib, &st);
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
    CHECK(near(mg_d(FN(lib, u64 (*)(u64, u32), "t_fmul")(mg_db(1.5), mg_fb(4.0f))), 6.0));
    /* (int, float, double, float, int, double): r0, r1, [r2:r3], then the stack -- float, int, pad, double. */
    CHECK(near(mg_f(FN(lib, u32 (*)(int, u32, u64, u32, int, u64), "t_mixed")(1, mg_fb(2.0f), mg_db(3.0), mg_fb(4.0f), 5, mg_db(6.5))), 21.5));
    CHECK(near(mg_f(FN(lib, u32 (*)(u32), "t_sqrt")(mg_fb(2.25f))), 1.5));
    CHECK(near(mg_d(FN(lib, u64 (*)(u64), "t_sin")(mg_db(0.5))), sin(0.5)));
    CHECK(near(mg_d(FN(lib, u64 (*)(u64, u64), "t_pow")(mg_db(2.0), mg_db(10.0))), 1024.0));
    CHECK_INT(FN(lib, long (*)(u64), "t_lround")(mg_db(-2.5)), -3);
    CHECK(near(mg_d(FN(lib, u64 (*)(u64, double *), "t_modf")(mg_db(3.75), &ip)), 0.75) && near(ip, 3.0));
    CHECK(near(mg_d(FN(lib, u64 (*)(const char *), "t_strtod")("-12.5e2")), -1250.0));
    CHECK(near(mg_d(FN(lib, u64 (*)(void *, u64, u64), "t_call_d")((void *)host_hypot, mg_db(3.0), mg_db(4.0))), 5.0));

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
        mg_thread_interrupt_report(report, sizeof(report));
        say("  threads interrupted in guest code: %s", report);
    }

    /* Files, the made-up /proc, properties, dlopen. */
    snprintf(buf, sizeof(buf), "/scratch");
    (void)scratch;
    CHECK_INT(FN(lib, int (*)(const char *), "t_files")(buf), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_proc")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_boot_id")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_smaps")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_props")(), 28);
    CHECK_INT(FN(lib, int (*)(void), "t_dl")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_loader_dl")(), 5);
    CHECK_INT(FN(lib, int (*)(void), "t_libc_inline_hook")(), 0);
    CHECK_INT(FN(lib, int (*)(void), "t_own_code_hook")(), 0);
    CHECK_INT(FN(lib, int (*)(void), "t_trap_hook")(), 0);
    CHECK_INT(FN(lib, int (*)(void), "t_libc_more")(), 6);
    CHECK_INT(FN(lib, int (*)(void), "t_shm")(), 9);
    CHECK_INT(FN(lib, int (*)(void), "t_ashmem")(), 10);
    CHECK_INT(FN(lib, int (*)(void), "t_shm_views")(), 6);
    CHECK_INT(FN(lib, int (*)(void), "t_mremap")(), 5);
    CHECK_INT(FN(lib, int (*)(void), "t_mmap_hint")(), 3);
    CHECK_INT(FN(lib, int (*)(void), "t_surface_control")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_hardware_buffer")(), 6);
    CHECK_INT(FN(lib, int (*)(void), "t_aaudio")(), 2);
    CHECK_INT(FN(lib, int (*)(void), "t_cpuclock")(), 6);
    CHECK_INT(FN(lib, int (*)(const char *), "t_ffcalls")(buf), 7);
    CHECK_INT(FN(lib, int (*)(void), "t_ifunc")(), 205);
    CHECK_INT(FN(lib, int (*)(void), "t_chrome_cmdline")(), 3);
    CHECK_INT(FN(lib, int (*)(const char *), "t_flock")(buf), 3);
    /* (No Android root here, so no /system/fonts: an empty iterator, opened and closed; the fonts themselves under the apps' runs.) */
    CHECK(FN(lib, int (*)(void), "t_fonts")() >= 0);
    CHECK_INT(FN(lib, int (*)(void), "t_timerfd")(), 8);
    CHECK_INT(FN(lib, int (*)(void), "t_timerfd_syscall")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_fnmatch")(), 16);
    CHECK_INT(FN(lib, int (*)(const char *), "t_stack")("a string that fits"), 18);
}

/*
 * The app's libraries and the runtime's, kept apart (ld/mg_ld.h): there is a
 * libt_same.so of each kind, and a library of each kind that needs
 * "libt_same.so". The runtime's are loaded first, so that anything that
 * looked for a library or a symbol by name alone would find the wrong one.
 *
 * The runtime's two are in the folder runtime/ beside the others: on a phone
 * <package>/selftest/runtime, which tools/build-appx.sh packs. A package
 * without it once failed here as if the linker mixed the two kinds; it is now
 * said as what it is.
 */
static void test_guest_kinds(const char *libdir)
{
    char *runtime_dir = mg_sprintf("%s/runtime", libdir), *probe = mg_sprintf("%s/libt_ruser.so", runtime_dir);
    mg_lib *ruser, *user, *rsame, *same, *in = NULL;
    mg_stat st;
    bool there;
    say("guest libraries: the app's and the runtime's of one name");
    there = mg_path_stat(probe, &st, NULL) && !st.is_dir;
    if (!there) say("  %s is not there: the runtime's test libraries were not packed with the others", probe);
    CHECK(there);
    free(probe);
    mg_ld_add_runtime_dir(runtime_dir);
    free(runtime_dir);
    if (!there) return;
    /* Each kind is looked for only where it is kept. */
    CHECK(mg_ld_load_runtime("libt_basic.so") == NULL && mg_ld_load("libt_ruser.so") == NULL);
    ruser = mg_ld_load_runtime("libt_ruser.so");
    CHECK(ruser && mg_ld_init(ruser) && mg_ld_lib_is_runtime(ruser));
    /* What it needs was loaded with it, as the runtime's; the app has no library of that name yet. */
    rsame = mg_ld_find_runtime_lib("libt_same.so");
    CHECK(rsame && mg_ld_lib_is_runtime(rsame) && mg_ld_find_lib("libt_same.so") == NULL);
    user = mg_ld_load("libt_user.so");
    CHECK(user && mg_ld_init(user) && !mg_ld_lib_is_runtime(user));
    same = mg_ld_find_lib("libt_same.so");
    CHECK(same && same != rsame && !mg_ld_lib_is_runtime(same) && mg_ld_find_runtime_lib("libt_same.so") == rsame);
    if (!ruser || !user || !same || !rsame) return;
    /* Each got the libt_same.so of its own kind. */
    CHECK_INT(FN(ruser, int (*)(void), "t_ruser_which")(), 202);
    CHECK_INT(FN(user, int (*)(void), "t_user_which")(), 101);
    /* Its thread-local pointer, relocated, in this thread's block, which existed before the library did. */
    CHECK_INT(FN(user, int (*)(void), "t_user_tls_len")(), 19);
    /* A name only the other kind exports is not bound across: the weak import stays zero on both sides. */
    CHECK_INT(FN(ruser, int (*)(void), "t_ruser_stray")(), -1);
    CHECK_INT(FN(user, int (*)(void), "t_user_stray")(), -1);
    /* dlsym(RTLD_DEFAULT), which is the app's: its own libraries, never the runtime's. */
    CHECK(mg_ld_sym(NULL, "t_same_app_only") != NULL && mg_ld_sym(NULL, "t_same_runtime_only") == NULL && mg_ld_sym(NULL, "t_ruser_which") == NULL);
    CHECK(mg_ld_lib_of(mg_ld_sym(NULL, "t_same_which")) == same);
    /* And by kind: where the runtime looks for its classes' native methods, and the app for its own. */
    CHECK(mg_ld_sym_among(true, "t_same_which", &in) != NULL && in == rsame);
    CHECK(mg_ld_sym_among(false, "t_same_which", &in) != NULL && in == same);
    CHECK(mg_ld_sym_among(true, "t_user_which", &in) == NULL && in == NULL && mg_ld_sym_among(false, "t_ruser_which", NULL) == NULL);
    /* Loading either again by name gives the one already there. */
    CHECK(mg_ld_load("libt_same.so") == same && mg_ld_load_runtime("libt_same.so") == rsame);
    /* A service has its own copy of a client's library, and resolves dependencies there. */
    {
        char *same_path = mg_sprintf("%s/libt_same.so", libdir), *user_path = mg_sprintf("%s/libt_user.so", libdir);
        mg_lib *service_same, *service_user;
        int previous = mg_ld_set_scope(15);
        CHECK(mg_ld_find_lib("libt_same.so") == NULL && mg_ld_sym(NULL, "t_same_app_only") == NULL);
        service_same = mg_ld_load_file(same_path);
        service_user = mg_ld_load_file(user_path);
        CHECK(service_same && service_same != same && service_same != rsame);
        CHECK(service_user && service_user != user && mg_ld_init(service_user));
        if (service_user) {
            CHECK_INT(FN(service_user, int (*)(void), "t_user_which")(), 101);
            CHECK(mg_ld_sym_deep(service_user, "t_same_which", &in) != NULL && in == service_same);
        }
        CHECK(mg_ld_sym_among(false, "t_same_which", &in) != NULL && in == service_same);
        CHECK(mg_ld_find_runtime_lib("libt_same.so") == rsame);
        mg_ld_set_scope(previous);
        CHECK(mg_ld_find_lib("libt_same.so") == same && mg_ld_sym_among(false, "t_same_which", &in) != NULL && in == same);
        free(same_path); free(user_path);
    }
}

static void test_guest(const char *libdir, const char *scratch)
{
    mg_lib *basic, *dep;
    mg_ld_stats st;
    u64 images = mg_ld_image_bytes();
    mg_ld_add_search_dir(libdir);
    mg_fs_mount("/scratch", scratch);

    basic = mg_ld_load("libt_basic.so");
    CHECK(basic != NULL);
    if (!basic) return;
    /* Its segments, committed, in what the frames line says of the native libraries: at least a page, at most its extent. */
    CHECK(mg_ld_image_bytes() - images >= MG_PAGE && mg_ld_image_bytes() - images <= mg_ld_lib_extent(basic));
    CHECK(mg_ld_init(basic));
    mg_ld_get_stats(basic, &st);
    CHECK(st.tp_reads >= 4);                        /* the initial-exec TLS accesses */
    CHECK(st.thumb_functions > 20 && st.arm_functions == 0);
    test_library(basic, "Thumb-2", scratch);

    /* Loaded first as an app asks with android_dlopen_ext: into a range it reserved; the PLT's tests below run on it there. */
    say("guest library: loaded into a range the app reserved (android_dlopen_ext, ANDROID_DLEXT_RESERVED_ADDRESS)");
    CHECK_INT(FN(basic, int (*)(void), "t_dlext")(), 7);
    dep = mg_ld_load("libt_dep.so");
    CHECK(dep != NULL);
    if (dep) {
        u32 before = mg_core_plt_rescues();
        say("guest library: calls through a rewritten PLT");
        CHECK(mg_ld_init(dep));
        mg_ld_get_stats(dep, &st);
        CHECK_INT(st.plt_entries, 5);
        CHECK_INT(FN(dep, int (*)(int, int), "d_add_twice")(1, 2), 5);
        CHECK_INT(FN(dep, int (*)(void), "d_bump")(), 6);
        CHECK_INT(*FN(basic, int *(*)(void), "t_counter_ptr")(), 6);
        CHECK_INT(FN(dep, int (*)(void), "d_hello_len")(), 16);
        CHECK(near(mg_d(FN(dep, u64 (*)(u64, u32), "d_fmul")(mg_db(2.0), mg_fb(3.0f))), 7.0));
        CHECK_INT(FN(dep, int (*)(void), "d_value")(), 0x1234);
        CHECK(FN(dep, void *(*)(void), "d_add_ptr")() == SYM(basic, "t_add"));
        CHECK_INT(FN(dep, int (*)(int), "d_spin")(2000000), 2000000);
        say("  PLT entries rescued after an interrupt: %u", mg_core_plt_rescues() - before);
    }

    CHECK(mg_ld_find_lib("libt_basic.so") == basic && mg_ld_lib_of(SYM(basic, "t_add")) == basic);
    CHECK(mg_ld_load("libc.so") == NULL && mg_ld_load("libnothere.so") == NULL);
    test_guest_kinds(libdir);

    /* Both ISAs must return the shim pid (4242), never the host kernel's pid.
     * Literal r7 loads and a 4-byte-aligned guest sp reproduce Chromium's stub. */
    {
        const char *names[] = { "libt_svc.so", "libt_svc_arm.so" };
        int j;
        mg_patch_set_trap_svc(false);
        for (j = 0; j < 2; ++j) {
            mg_lib *svc = mg_ld_load(names[j]);
            CHECK(svc != NULL);
            if (svc) {
                int saved = *mg_errno_ptr();
                mg_ld_get_stats(svc, &st);
                CHECK_INT(st.svc_sites, 5);
                *mg_errno_ptr() = 123;
                CHECK_INT(FN(svc, int (*)(void), "t_svc_getpid")(), 4242);
                CHECK_INT(FN(svc, int (*)(void), "t_svc_literal")(), 4242);
                CHECK_INT(FN(svc, int (*)(void), "t_svc_unknown")(), -38);
                CHECK_INT(FN(svc, int (*)(void *, unsigned), "t_svc_cacheflush")(SYM(svc, "t_svc_getpid"), 2), 0);
                /* compiler-rt's __clear_cache, whose number is finished by movt: patched too, and it succeeds. */
                CHECK_INT(FN(svc, int (*)(void *, unsigned), "t_svc_clear_cache")(SYM(svc, "t_svc_getpid"), 2), 0);
                CHECK_INT(*mg_errno_ptr(), 123);
                *mg_errno_ptr() = saved;
            }
        }
    }
    /* Diagnostic mode still stops, and is not accepted as a dispatch trap. */
    {
        mg_lib *svc;
        const u8 *fn;
        bool trapped = false, left = false;
        int k;
        mg_patch_set_trap_svc(true);
        svc = mg_ld_load("libt_svc_diag.so");
        mg_patch_set_trap_svc(false);
        CHECK(svc != NULL);
        if (svc) {
            mg_addr target;
            mg_ld_get_stats(svc, &st);
            CHECK_INT(st.svc_sites, 5);
            fn = (const u8 *)((mg_addr)SYM(svc, "t_svc_getpid") & ~(mg_addr)1);
            for (k = 0; k < 16; k += 2) {
                trapped |= mg_rd16(fn + k) == 0xDEA5;
                left |= mg_rd16(fn + k) == 0xDF00;
                CHECK(!mg_patch_svc_target((mg_addr)(fn + k), &target));
            }
            CHECK(trapped && !left);
        }
    }

    /*
     * AAudio as Spotify's core uses it (core/tests/guest/t_aaudio.c says what each result is): libaaudio.so opened by name and
     * its functions looked up; a stream fed by its callback on the mixer's thread, one written to, which takes a second of
     * sound in about a second, and the streams that are refused. The mixer has no output here and keeps time by itself.
     */
    {
        mg_lib *aa = mg_ld_load("libt_aaudio.so");
        int r[20];
        CHECK(aa != NULL);
        if (aa && mg_ld_init(aa)) {
            say("guest library: AAudio streams over the mixer");
            CHECK_INT(FN(aa, int (*)(int *), "t_aaudio_run")(r), 1);
            say("  %d callbacks of %d frames in 300 ms, %d after the stop; a second written in %d ms", r[5], r[2], r[10], r[12]);
            CHECK_INT(r[0], 1);
            CHECK_INT(r[1], 0);
            CHECK_INT(r[2], 480);
            CHECK_INT(r[3], 480002);
            CHECK_INT(r[4], 4);
            CHECK(r[5] >= 10);
            CHECK_INT(r[6], 0);
            CHECK_INT(r[7], 1);
            CHECK_INT(r[8], 0);
            CHECK_INT(r[9], 10);
            CHECK(r[10] <= 1);
            CHECK_INT(r[11], 22050);
            CHECK(r[12] >= 700 && r[12] <= 3000);
            CHECK_INT(r[13], 0);
            CHECK_INT(r[14], -889);
            CHECK_INT(r[15], -883);
            CHECK_INT(r[16], 10);
            CHECK_INT(r[17], 3);
            CHECK_INT(r[18], 0);
            CHECK_INT(r[19], 1);
        }
    }
}

/*
 * The same library built in ARM mode, run as it is. Last of all: on a host
 * that does not keep a thread in ARM state this is where the process ends,
 * and everything else has been said by then. When the probe has already found
 * that out (mg_core_arm_state), it is not attempted.
 */
static void test_guest_arm(const char *scratch)
{
    mg_lib *arm;
    mg_ld_stats st;
    int state = mg_core_arm_state();
    if (state < 0) {
        say("guest library: ARM mode -- not run: the probe found that this device does not keep a thread in ARM state");
        return;
    }
    if (state == 0) say("(the probe has not been run here: if the log ends in the next section, ARM-mode code does not survive on this device)");
    arm = mg_ld_load("libt_basic_arm.so");
    CHECK(arm != NULL);
    if (arm) {
        CHECK(mg_ld_init(arm));
        mg_ld_get_stats(arm, &st);
        CHECK(st.arm_functions > 20);
        test_library(arm, "ARM mode, packed relocations", scratch);
    }
}

/* The mechanisms the port rests on, tried directly. */
static void test_mechanisms(void)
{
    u32 before = mg_core_plt_rescues();
    void *entry = mg_stub(MG_STUB_ARM_ONCE_ENTRY);
    say("mechanisms");
    /* One ARM instruction and back, as a rewritten PLT entry is. */
    CHECK_INT(((int (*)(int))mg_stub(MG_STUB_ARM_ONCE))(41), 42);
    /* The same four bytes entered in Thumb state -- what Windows does to a thread interrupted there -- must fault and be rescued. */
    CHECK_INT(((int (*)(int))((mg_addr)entry | 1u))(7), 7);
    CHECK_INT(mg_core_plt_rescues() - before, 1);
}

/*
 * A thread stopped and resumed while it runs ARM-mode code, as the collector stops every thread of the runtime's
 * (mg_thread_suspend, its registers read, mg_thread_resume; the context is never written back). docs/ARCHITECTURE.md ("ARM
 * mode") says why it is asked: Windows on ARM is Thumb-2 only, and a thread whose context the kernel rebuilt could come
 * back in Thumb state, where ARM code is nonsense -- and real apps' libraries have hundreds of ARM-mode functions. The
 * thread counts in ARM state (mg_stub_arm_count), each turn depending on the one before. Stopped and resumed three thousand
 * times, it must go on counting and return when told; the registers read while it was stopped in the loop must say ARM
 * state. On a device that lost the state the thread would run off and the process end here, at this test's line.
 */
typedef struct arm_count_job {
    volatile u32 words[2];
    volatile u32 result;
    volatile i32 done;
} arm_count_job;

static void arm_count_main(void *p)
{
    arm_count_job *j = (arm_count_job *)p;
    j->result = ((u32 (*)(volatile u32 *))mg_stub(MG_STUB_ARM_COUNT))(j->words);
    mg_atomic_xchg(&j->done, 1);
}

static void test_suspend_arm(void)
{
    static arm_count_job job;
    mg_addr code = (mg_addr)mg_stub(MG_STUB_ARM_COUNT) & ~(mg_addr)1;
    mg_thread_regs regs;
    mg_thread *t;
    int i, stopped = 0, in_loop = 0, arm_state = 0, thumb_state = 0, still = 0;
    u32 seen;
    say("suspending a thread in ARM state");
    memset((void *)&job, 0, sizeof(job));
    t = mg_thread_start(arm_count_main, (void *)&job, 0, "arm-count");
    CHECK(t != NULL);
    if (!t) return;
    while (job.words[0] == 0 && !job.done) mg_thread_yield();
    seen = job.words[0];
    for (i = 0; i < 3000 && !job.done; i++) {
        if (!mg_thread_suspend(t, &regs)) continue;
        stopped++;
        /* Stopped inside the loop's instructions (from its first ARM one, past the bx pc that enters it): what state it says. */
        if ((regs.pc & ~(mg_addr)1) >= code + 4 && (regs.pc & ~(mg_addr)1) < code + 0x24) {
            in_loop++;
            if ((regs.flags & 0x20u) || (regs.pc & 1u)) thumb_state++;
            else arm_state++;
        }
        mg_thread_resume(t);
        if (i % 100 == 99) {
            /* It goes on: the count moves between one stop and the next hundredth. */
            u64 until = mg_time_monotonic_ns() + 200000000ull;
            while (job.words[0] == seen && !job.done && mg_time_monotonic_ns() < until) mg_thread_yield();
            if (job.words[0] == seen) still++;
            seen = job.words[0];
        }
    }
    job.words[1] = 1;
    mg_thread_join(t);
    say("  stopped %d times, %d of them in the ARM loop (%d said ARM state, %d Thumb state); the count reached %u", stopped, in_loop, arm_state,
        thumb_state, (unsigned)job.result);
    CHECK_INT(stopped, 3000);
    CHECK(in_loop > stopped / 2);
    CHECK_INT(thumb_state, 0);
    CHECK_INT(still, 0);
    CHECK(job.done && job.result >= seen);
}

#include "mg_tests_app.inc"

#endif /* MG_HOST_ARM32 */

/*
 * The Dalvik runtime's tests (mg_dvm_tests.c). They start the runtime, which
 * from then on is part of the Java world native code sees: so they come after
 * everything that tests that world as it is without one.
 */
static const char *g_dvm_fixtures, *g_dvm_apk, *g_dvm_android;
static int g_dvm_trace;

void mg_tests_set_dvm_android(const char *root) { g_dvm_android = root; }

#if MG_HOST_ARM32 && !defined(_WIN32)
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/elf/mg_elf.h"

typedef struct oom_text {
    mg_addr lo, hi;
} oom_text;

static int oom_text_of(mg_addr bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    oom_text *t = (oom_text *)user;
    const Elf32_Phdr *ph = (const Elf32_Phdr *)phdr;
    unsigned i;
    if (!name || !strstr(name, "libwebviewchromium.so")) return 0;
    for (i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X)) {
            t->lo = bias + ph[i].p_vaddr;
            t->hi = t->lo + ph[i].p_memsz;
            return 1;
        }
    }
    return 0;
}

static void oom_call(void *fn)
{
    ((void (*)(size_t))fn)(123456789u);
}

/* The child of test_chromium_oom_end: the WebView's library loaded, its TerminateBecauseOutOfMemory called. Never returns. */
static void oom_child(const char *apk)
{
    oom_text text = { 0, 0 };
    mg_addr pc, f = 0, call;
    const u16 *h;
    mg_thread *t;
    if (!mg_ld_add_apk(apk) || !mg_ld_load("libwebviewchromium.so")) {
        fprintf(stderr, "oom: the WebView's library did not load\n");
        _exit(3);
    }
    mg_ld_iterate(oom_text_of, &text);
    for (pc = (text.lo + 1) & ~(mg_addr)1; pc + 4 <= text.hi; pc += 2) {
        h = (const u16 *)pc;
        if (h[0] == 0xbe00 && h[1] == 0xde00 && mg_chromium_oom_global(pc + 2)) {
            f = pc - 16;
            break;
        }
    }
    if (!f) {
        fprintf(stderr, "oom: no OnNoMemoryInternal in the library\n");
        _exit(3);
    }
    /* TerminateBecauseOutOfMemory follows it and its literal: a bl to it. Else the function itself is called. */
    call = f;
    h = (const u16 *)(f + 0x18);
    if ((h[0] & 0xf800) == 0xf000 && (h[1] & 0xd000) == 0xd000) {
        u32 s = (h[0] >> 10) & 1, j1 = (h[1] >> 13) & 1, j2 = (h[1] >> 11) & 1;
        i32 off = (i32)((s << 24) | ((1 - (j1 ^ s)) << 23) | ((1 - (j2 ^ s)) << 22) | ((u32)(h[0] & 0x3ff) << 12) | ((u32)(h[1] & 0x7ff) << 1));
        if (s) off -= 1 << 25;
        if (f + 0x18 + 4 + (mg_addr)(intptr_t)off == f) call = f + 0x18;
    }
    fprintf(stderr, "oom: calling %s at +0x%x\n", call == f ? "OnNoMemoryInternal" : "TerminateBecauseOutOfMemory",
            (unsigned)(call - (mg_addr)mg_ld_lib_base(mg_ld_find_lib("libwebviewchromium.so"))));
    /* On a thread that has run no Android code: its fault is reported, and then the process ends, as the signal ends it. */
    t = mg_thread_start(oom_call, (void *)(call | 1), 0, "oom");
    if (t) mg_thread_join(t);
    fprintf(stderr, "oom: it returned\n");
    _exit(4);
}

/*
 * Under qemu, with the Android root that carries the WebView: its library's TerminateBecauseOutOfMemory, called for 123456789
 * bytes in a child process, ends that process with a fault whose report says so (mg_core.c). The real code, the real trap,
 * the report as the phone prints it.
 */
static void test_chromium_oom_end(void)
{
    char *apk;
    static char out[65536], drop[4096];
    size_t got = 0;
    int fds[2], status = 0;
    pid_t pid;
    u64 until;
    const char *line;
    mg_stat st;
    int err = 0;
    if (!g_dvm_android || !g_dvm_android[0]) {
        say("(Chromium's end out of memory is not tried: no Android root, --dvm-android)");
        return;
    }
    apk = mg_sprintf("%s/app/webview/webview.apk", g_dvm_android);
    if (!mg_path_stat(apk, &st, &err)) {
        say("(Chromium's end out of memory is not tried: no %s)", apk);
        free(apk);
        return;
    }
    say("Chromium's end out of memory: the WebView's TerminateBecauseOutOfMemory(123456789), in a child process");
    CHECK(pipe(fds) == 0);
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        oom_child(apk);
    }
    close(fds[1]);
    CHECK(pid > 0);
    until = mg_time_monotonic_ns() + 300000000000ull;
    while (pid > 0) {
        struct pollfd pfd;
        ssize_t n;
        pfd.fd = fds[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (mg_time_monotonic_ns() > until) {
            say("  the child did not end in 300 s");
            kill(pid, SIGKILL);
            break;
        }
        if (poll(&pfd, 1, 1000) <= 0) continue;
        /* (What does not fit is read and dropped: the report comes first, the trace after it.) */
        n = read(fds[0], got < sizeof(out) - 1 ? out + got : drop, got < sizeof(out) - 1 ? sizeof(out) - 1 - got : sizeof(drop));
        if (n <= 0) break;
        if (got < sizeof(out) - 1) got += (size_t)n;
    }
    out[got] = 0;
    close(fds[0]);
    if (pid > 0) waitpid(pid, &status, 0);
    CHECK(pid > 0 && WIFSIGNALED(status));
    line = strstr(out, "oom: ");
    if (line) say("  %.*s", (int)strcspn(line, "\n"), line);
    line = strstr(out, "fault: ");
    if (line) say("  %.*s", (int)strcspn(line, "\n"), line);
    line = strstr(out, "Chromium ran out of memory");
    CHECK(line && !strncmp(line, "Chromium ran out of memory asking for 123456789 bytes (117 MB)", 61));
    say("  the report: %.*s", line ? (int)strcspn(line, "\n") : 4, line ? line : "none");
    free(apk);
}
#endif

void mg_tests_set_dvm(const char *fixtures, const char *apk, int trace)
{
    g_dvm_fixtures = fixtures;
    g_dvm_apk = apk;
    g_dvm_trace = trace;
}

/* A path beside a folder: its last part replaced (not ".." on the end, which not every way of opening a file here resolves). */
static char *beside_folder(const char *dir, const char *name)
{
    char *parent = mg_strdup(dir), *out;
    size_t n = strlen(parent);
    while (n && (parent[n - 1] == '/' || parent[n - 1] == '\\')) parent[--n] = 0;
    while (n && parent[n - 1] != '/' && parent[n - 1] != '\\') n--;
    parent[n] = 0;
    out = mg_sprintf("%s%s", n ? parent : "./", name);
    free(parent);
    return out;
}

static void test_dvm(const char *libdir, const char *scratch, bool guest_code)
{
    mg_dvm_test_config config;
    char *beside = !g_dvm_fixtures && libdir ? mg_sprintf("%s/dvm", libdir) : NULL;
    /* The Android root: in the app's package it is the folder `android` beside the one the guest libraries are in. */
    char *android = !g_dvm_android && libdir ? beside_folder(libdir, "android/framework/core-oj.dex") : NULL;
    char *java_scratch = mg_sprintf("%s/java", scratch);
    mg_stat st;
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
    else if (android && mg_path_stat(android, &st, NULL)) {
        android[strlen(android) - strlen("/framework/core-oj.dex")] = 0;
        config.android_root = android;
    }
    g_failed += mg_dvm_tests_run(&config, g_sink, g_sink_user, &checks);
    g_checks += checks;
    /* megler.Gfx's native methods make their arrays and strings with the runtime, which is running from here on. */
    checks = 0;
    g_failed += mg_gfx_natives_tests_run(g_sink, g_sink_user, &checks);
    g_checks += checks;
    /* And OpenGL ES's natives, likewise, with the stand-in that draws in ANGLE's place. */
    checks = 0;
    g_failed += mg_gl_natives_tests_run(g_sink, g_sink_user, &checks);
    g_checks += checks;
    free(android);
    free(java_scratch);
    free(beside);
}

/*
 * SQLite as the core has it (sqlite/mg_sqlite.c): its files through the host
 * layer, its locks kept in the process, its mutexes the host's. On every host,
 * since the file layer under it is each host's own. Where the core was built
 * without SQLite this says so and checks only that it knows.
 */
#if defined(MG_HAVE_SQLITE) && MG_HAVE_SQLITE
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
    char *path = mg_sprintf("%s/sqlite-test.db", scratch), *journal = mg_sprintf("%s/sqlite-test.db-journal", scratch);
    sqlite3 *a = NULL, *b = NULL, *m = NULL;
    sqlite3_stmt *st = NULL;
    mg_stat fs;
    say("SQLite %s, compiled into the core, on the host layer's files", sqlite3_libversion());
    CHECK(mg_sqlite_available() && mg_sqlite_start() && mg_sqlite_start());
    CHECK(sqlite3_vfs_find(NULL) && !strcmp(sqlite3_vfs_find(NULL)->zName, "megler") && sqlite3_threadsafe() == 1);
    mg_path_remove(path, NULL);
    mg_path_remove(journal, NULL);
    /* A database that is not there is not made unless asked. */
    CHECK(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE, NULL) == SQLITE_CANTOPEN);
    sqlite3_close(a);
    a = NULL;
    CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, v REAL); INSERT INTO t(name, v) VALUES('one', 1.5), ('two', 2.5), ('three', 3.5);", NULL, NULL, NULL), SQLITE_OK);
    CHECK_INT(sqlite3_changes(a), 3);
    CHECK(sqlite3_last_insert_rowid(a) == 3);
    /* It is a file of the host's, with SQLite's header, and no journal is left beside it. */
    CHECK(mg_path_stat(path, &fs, NULL) && fs.size >= 1024 && !mg_path_stat(journal, &fs, NULL));
    {
        size_t n = 0;
        u8 *head = mg_file_read_all(path, &n);
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
    CHECK(sq_int(a, "SELECT count(*) FROM t") == 1 && mg_path_stat(journal, &fs, NULL));
    CHECK_INT(sqlite3_exec(a, "ROLLBACK", NULL, NULL, NULL), SQLITE_OK);
    CHECK(sq_int(a, "SELECT count(*) FROM t") == 3 && !mg_path_stat(journal, &fs, NULL));
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
    {
        /* A file named in the app's SQL by the app's path (Gmail's EmailProvider attaches its bodies' database so): the
           host's file under that path is the one opened and made. */
        char *attached = mg_sprintf("%s/sqlite-attached.db", scratch);
        mg_path_remove(attached, NULL);
        mg_fs_mount("/data/data/megler.sqlite.test", scratch);
        a = NULL;
        /* (An attached file is opened as the database it is attached to was: one that may be made, as Android opens its.) */
        CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL), SQLITE_OK);
        CHECK_INT(sqlite3_exec(a, "ATTACH '/data/data/megler.sqlite.test/sqlite-attached.db' AS att; CREATE TABLE att.u(x); INSERT INTO att.u VALUES(7);",
                               NULL, NULL, NULL), SQLITE_OK);
        CHECK_INT(sq_int(a, "SELECT x FROM att.u"), 7);
        sqlite3_close(a);
        CHECK(mg_path_stat(attached, &fs, NULL) && fs.size >= 1024);
        mg_path_remove(attached, NULL);
        free(attached);
    }
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
    CHECK(!mg_sqlite_available() && !mg_sqlite_start());
}
#endif

/* The strings of a table's resources, as a count and a hash of them in their order. */
typedef struct arsc_strings {
    u32 n;
    u32 hash;
} arsc_strings;

static void arsc_string_seen(const u8 *s, size_t len, void *user)
{
    arsc_strings *a = (arsc_strings *)user;
    size_t i;
    a->n++;
    for (i = 0; i < len; i++) a->hash = (a->hash ^ s[i]) * 16777619u;
    a->hash = (a->hash ^ 0xFF) * 16777619u;
}

/*
 * The kernel's user helpers done in their place (arm/mg_kuser.h): __kuser_get_tls gives the thread pointer, the barrier
 * returns, each to the caller's lr; __kuser_cmpxchg and __kuser_cmpxchg64 swap when the value is the old one (r0 0, C set)
 * and leave it otherwise (r0 not 0, C clear) -- on a host whose addresses fit in a register; and a load of the version at
 * 0xffff0ffc, in each form old code reads it, gives 5 and goes on past the load, its base written back where it says so.
 * An address that is no helper, and a load of another address, are not taken.
 */
static void test_kuser(void)
{
    u32 r[16], cpsr = 0;
    mg_addr resume = 0, next = 0;
    u32 aligned;                       /* the instruction's bytes on a word, as code is */
    u8 *code = (u8 *)&aligned;
    say("the kernel's user helpers");
    memset(r, 0, sizeof(r));
    r[14] = 0x12345679u;
    CHECK(mg_kuser_call(0xffff0fe0u, r, &cpsr, 0xabcd0000u, &resume) && r[0] == 0xabcd0000u && resume == 0x12345679u);
    CHECK(mg_kuser_call(0xffff0fa0u, r, &cpsr, 0, &resume) && resume == 0x12345679u);
    CHECK(!mg_kuser_call(0xffff0f00u, r, &cpsr, 0, &resume));
    if (sizeof(void *) == 4) {
        static volatile i32 word = 7;
        static volatile i64 wide = 70;
        static i64 old64, new64;
        r[0] = 7; r[1] = 9; r[2] = (u32)(mg_addr)&word;
        CHECK(mg_kuser_call(0xffff0fc0u, r, &cpsr, 0, &resume) && r[0] == 0 && word == 9 && (cpsr & 0x20000000u));
        r[0] = 7; r[1] = 11; r[2] = (u32)(mg_addr)&word;
        CHECK(mg_kuser_call(0xffff0fc0u, r, &cpsr, 0, &resume) && r[0] != 0 && word == 9 && !(cpsr & 0x20000000u));
        old64 = 70; new64 = 90;
        r[0] = (u32)(mg_addr)&old64; r[1] = (u32)(mg_addr)&new64; r[2] = (u32)(mg_addr)&wide;
        CHECK(mg_kuser_call(0xffff0f60u, r, &cpsr, 0, &resume) && r[0] == 0 && wide == 90 && (cpsr & 0x20000000u));
    }
    /* ldr r0, [r1, #28] (Thumb, 16 bits), r1 = 0xffff0fe0 */
    memset(r, 0, sizeof(r));
    r[1] = 0xffff0fe0u;
    code[0] = 0xc8; code[1] = 0x69;
    CHECK(mg_kuser_read(0xffff0ffcu, code, true, r, &next) && r[0] == MG_KUSER_VERSION && next == ((mg_addr)code + 2) + 1);
    /* ldr.w r2, [r3, #0xffc] (Thumb, 32 bits), r3 = 0xffff0000 */
    memset(r, 0, sizeof(r));
    r[3] = 0xffff0000u;
    code[0] = 0xd3; code[1] = 0xf8; code[2] = 0xfc; code[3] = 0x2f;
    CHECK(mg_kuser_read(0xffff0ffcu, code, true, r, &next) && r[2] == MG_KUSER_VERSION && next == ((mg_addr)code + 4) + 1);
    /* ldr r4, [r5, #-4]! (Thumb, 32 bits, written back), r5 = 0xffff1000 */
    memset(r, 0, sizeof(r));
    r[5] = 0xffff1000u;
    code[0] = 0x55; code[1] = 0xf8; code[2] = 0x04; code[3] = 0x4d;
    CHECK(mg_kuser_read(0xffff0ffcu, code, true, r, &next) && r[4] == MG_KUSER_VERSION && r[5] == 0xffff0ffcu);
    /* ldr r0, [r0, #-3] where r0 = 0xffff0fff: ARM, LDR (immediate), offset, not written back */
    memset(r, 0, sizeof(r));
    r[0] = 0xffff0fffu;
    code[0] = 0x03; code[1] = 0x00; code[2] = 0x10; code[3] = 0xe5;
    CHECK(mg_kuser_read(0xffff0ffcu, code, false, r, &next) && r[0] == MG_KUSER_VERSION && next == (mg_addr)code + 4);
    /* the same load of another address is not taken */
    memset(r, 0, sizeof(r));
    r[1] = 0xffff0f00u;
    code[0] = 0xc8; code[1] = 0x69;
    CHECK(!mg_kuser_read(0xffff0ffcu, code, true, r, &next));
}

/*
 * A resource table cut down to the phone's language and English (mg_arsc_keep_languages), on microG's GmsCore's (80
 * languages): much smaller; the resources it keeps have the same strings in the same order as in the whole table; nothing
 * more goes the second time; a table that is not one is left alone.
 */
static void test_arsc(const char *libdir)
{
    static const char *const langs[] = { "ru", "en" };
    /* Beside the guest libraries' folder (build/guest/...), from the repository's folder, or where the ARM run's files are. */
    char *places[3], *apk = NULL, err[160];
    int k;
    mg_zip z;
    const mg_zip_entry *e;
    u8 *table = NULL, *cut = NULL, *again;
    size_t size = 0, cut_size = 0, again_size = 0;
    arsc_strings whole = { 0, 2166136261u }, kept = { 0, 2166136261u }, all_left = { 0, 2166136261u };
    say("a resource table cut down to two languages");
    places[0] = libdir ? mg_sprintf("%s/../../microg/GmsCore.apk", libdir) : mg_sprintf("build/microg/GmsCore.apk");
    places[1] = mg_sprintf("build/microg/GmsCore.apk");
    places[2] = mg_sprintf("GmsCore.apk");
    for (k = 0; k < 3; k++) {
        if (!apk && mg_zip_open(&z, places[k], err, sizeof(err))) apk = places[k];
        else free(places[k]);
    }
    if (!apk) {
        say("  (no build/microg/GmsCore.apk: tools/microg-fetch.sh gets it; these checks are left out)");
        return;
    }
    e = mg_zip_find(&z, "resources.arsc");
    table = e ? mg_zip_read(&z, e, 64u << 20, &size) : NULL;
    mg_zip_close(&z);
    CHECK(table != NULL);
    if (table) cut = mg_arsc_keep_languages(table, size, langs, 2, &cut_size);
    CHECK(cut && cut_size < size / 2);
    if (cut) {
        say("  %u KB, %u KB with ru and en", (unsigned)(size >> 10), (unsigned)(cut_size >> 10));
        CHECK(mg_arsc_each_string(table, size, langs, 2, arsc_string_seen, &whole));
        CHECK(mg_arsc_each_string(cut, cut_size, langs, 2, arsc_string_seen, &kept));
        CHECK(mg_arsc_each_string(cut, cut_size, NULL, -1, arsc_string_seen, &all_left));
        CHECK(whole.n > 100 && kept.n == whole.n && kept.hash == whole.hash && all_left.n == whole.n && all_left.hash == whole.hash);
        again = mg_arsc_keep_languages(cut, cut_size, langs, 2, &again_size);
        CHECK(again == NULL);
        free(again);
        /* Cut short, it is not a table, and is left alone. */
        CHECK(mg_arsc_keep_languages(table, size / 3, langs, 2, &again_size) == NULL);
    }
    free(cut);
    free(table);
    free(apk);
}

int mg_tests_run(const char *libdir, const char *scratch, mg_test_sink sink, void *user)
{
    bool can_run;
    g_checks = g_failed = 0;
    g_sink = sink;
    g_sink_user = user;
    can_run = mg_core_init();
    mg_path_mkdirs(scratch);
    test_host(scratch);
    g_failed += mg_storage_tests_run(scratch, g_sink, g_sink_user, &g_checks);
    g_failed += mg_net_tests_run(g_sink, g_sink_user, &g_checks);
    test_zip(scratch);
    test_sqlite(scratch);
    test_textures();
    test_media(scratch);
    test_codecs();
    test_camera();
    test_battery();
    test_libc_odds();
    test_appx_manifest();
    test_appx_pri();
    test_arsc(libdir);
    test_kuser();
    test_palette();
    test_mmap_commit();
    test_mmap_file(scratch);
    test_mmap_madvise(scratch);
    test_pread_threads(scratch);
    test_rename_in_use(scratch);
    test_chromium_oom_report();
    test_shm_views();
    test_free_stretches();
    test_input();
    {
        /* The stand-in for the shell's drawing under a Java app: it is what the Java driver's tests see pictures through. */
        int checks = 0;
        mg_gfx_tests_set_scratch(scratch);
        g_failed += mg_gfx_tests_run(g_sink, g_sink_user, &checks);
        g_checks += checks;
    }
#if MG_HOST_ARM32
    test_proxies();
    if (can_run) {
#if !defined(_WIN32)
        /* (First, while this process has few threads of its own: the test forks.) */
        test_chromium_oom_end();
#endif
        test_mechanisms();
        test_suspend_arm();
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
