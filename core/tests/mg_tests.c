/*
 * The core's tests.
 *
 *   mg_tests <guest library dir> <scratch dir>
 *
 * Everything that needs no guest code runs anywhere (MSVC's x64 build on this
 * PC). The rest loads the guest test libraries of the build's own ABI
 * (tools/build-guest.sh <abi>) and calls into them, which needs a build that
 * runs guest code (MG_GUEST_CODE): the 32-bit ARM one under qemu-arm
 * (tools/build-tests.sh arm) and, linked into the app, on the phone itself;
 * Linux on x86_64 (tools/build-tests.sh linux64); the core's ELF on desktop
 * Windows (tools/build-desktop.sh). What is 32-bit ARM's alone -- Thumb and ARM
 * mode, rewritten PLT entries, svc, the kernel's helpers -- is tested on ARM.
 *
 * A guest function with floating-point parameters is called here through a
 * pointer typed with gfloat/gdouble (mg_base.h): their bits on 32-bit ARM, the
 * softfp convention spelled out, and the values themselves elsewhere.
 */
#include "mg_camera_standin.h"
#include "mg_codec_standin.h"
#include "mg_gfx_tests.h"
#include "mg_net_tests.h"
#include "mg_sensor_tests.h"
#include "mg_bluetooth_tests.h"
#include "mg_wifi_tests.h"
#include "mg_storage_tests.h"
#include "mg_tests.h"

#include <math.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#endif

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
static MG_HOSTCB void futex_waker(void *arg)
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

static MG_HOSTCB void list_one(const char *name, bool is_dir, void *user)
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

static MG_HOSTCB void pread_racer(void *arg)
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

/*
 * Who holds a lock, as the hang report reads it from its own thread (mg_base.c, mg_holder). It read its clock first and
 * the holder's record after: a hold begun between the two was 2^64 ns old, "18446744073.7 s", and said as an app that
 * stands still (Viber's write-back thread on 0.1.0.669, Tuya Smart's fault report on 671). A look reads the clock after
 * the record, and gets the record whole -- one hold's owner, time and words, however fast the lock changes hands.
 */
typedef struct holder_race {
    mg_holder *h;
    volatile i32 stop;
    volatile u32 host_id;
    u32 rounds;
} holder_race;

static MG_HOSTCB void holder_racer(void *arg)
{
    holder_race *r = (holder_race *)arg;
    r->host_id = mg_thread_self_id();
    while (!r->stop) {
        mg_holder_take(r->h, "the racer's hold");
        mg_holder_let_go(r->h);
        r->rounds++;
    }
}

static void test_lock_holders(void)
{
    mg_holder h = MG_HOLDER_INIT;
    mg_hold_seen seen;
    char text[64];
    u64 before;
    say("a lock's holder, as the hang report looks at it");
    CHECK(!mg_holder_look(&h, &seen, text, sizeof(text)) && seen.owner == 0 && seen.held_ns == 0 && text[0] == 0);
    /* The clock read first, the hold begun after it: counted from that reading it wraps round; looked at, it is a moment old. */
    before = mg_time_monotonic_ns();
    mg_sleep_ns(2000000ull);
    mg_holder_take(&h, "a test's hold");
    CHECK(mg_holder_mine(&h));
    CHECK(mg_holder_look(&h, &seen, text, sizeof(text)) && seen.owner == mg_thread_self_id() && !strcmp(text, "a test's hold"));
    say("  a hold begun after the clock was read: %.6f s held as looked at; %.1f s counted from the reading (the old way)",
        (double)seen.held_ns / 1e9, (double)(before - seen.since) / 1e9);
    CHECK(seen.held_ns < 1000000000ull && before - seen.since > 1000000000ull * 1000000000ull && mg_held_ns(seen.since, before) == 0);
    /* Held a while: as long as it was. */
    mg_sleep_ns(60000000ull);
    CHECK(mg_holder_look(&h, &seen, NULL, 0) && seen.held_ns >= 50000000ull && seen.held_ns < 30000000000ull);
    mg_holder_let_go(&h);
    CHECK(!mg_holder_mine(&h) && !mg_holder_look(&h, &seen, text, sizeof(text)) && seen.owner == 0 && text[0] == 0);
    /* Taken and let go by another thread as fast as it can, looked at all the while: every look one hold's, whole. */
    {
        holder_race race;
        mg_thread *t;
        u32 looks = 0, held = 0, torn = 0;
        u64 until;
        memset(&race, 0, sizeof(race));
        race.h = &h;
        t = mg_thread_start(holder_racer, &race, 64u * 1024u, "holder-racer");
        CHECK(t != NULL);
        until = mg_time_monotonic_ns() + 300000000ull;
        while (t && mg_time_monotonic_ns() < until) {
            looks++;
            if (!mg_holder_look(&h, &seen, text, sizeof(text))) continue;
            held++;
            if (seen.owner != race.host_id || strcmp(text, "the racer's hold") || seen.held_ns > 10000000000ull || !seen.since) torn++;
        }
        race.stop = 1;
        if (t) mg_thread_join(t);
        say("  %u looks while another thread took it %u times: %u found it held, %u of them not one hold's", looks, race.rounds, held, torn);
        CHECK(race.rounds > 0 && torn == 0);
    }
    /*
     * A report's first line says when it was said and by which build (bionic/mg_bionic.c, mg_bionic_hang_heading): a Lumia
     * 650's hang.txt of 0.1.0.674 was read as 0.1.0.687's, and named with 687's map. Today's date, as the C library has it,
     * and on Windows the program's linker time stamp, which its map says at its top.
     */
    {
        char heading[400], today[16];
        time_t now = time(NULL);
        struct tm *local = localtime(&now);
        size_t n = mg_bionic_hang_heading(heading, sizeof(heading), 0);
        if (local) snprintf(today, sizeof(today), "%04d-%02d-%02d", local->tm_year + 1900, local->tm_mon + 1, local->tm_mday);
        else today[0] = 0;
        say("  a hang report's first line: %.*s", (int)(n && heading[n - 1] == '\n' ? n - 1 : n), heading);
        CHECK(n > 20 && heading[n - 1] == '\n' && !strncmp(heading, "hang: said ", 11) && strstr(heading, today) == heading + 11);
#if MG_HOST_WINDOWS && !defined(MG_CORE_ELF)
        CHECK(strstr(heading, ".exe of time stamp ") != NULL && strstr(heading, "its build's map says \"Timestamp is ") != NULL);
#endif
    }
}

/*
 * The log's sink is called past the log's lock (mg_base.c): a sink that stands -- the shell's, writing a fault's report to
 * its file line by line on a phone's busy flash -- holds up no other thread's line. Under the lock Tuya Smart's main
 * thread waited in mg_logv while a fault's report wrote its trace lines (0.1.0.671).
 */
static volatile i32 g_stand_entered, g_stand_release, g_stand_other;
static mg_log_sink g_stand_before;
static void *g_stand_before_user;

static void standing_sink(int level, const char *line, void *user)
{
    int i;
    (void)user;
    if (strstr(line, "a line whose sink stands")) {
        mg_atomic_xchg(&g_stand_entered, 1);
        /* (Two seconds at most: the test goes on either way.) */
        for (i = 0; i < 1000 && !g_stand_release; i++) mg_sleep_ns(2000000ull);
        mg_atomic_xchg(&g_stand_entered, 2);
    } else if (strstr(line, "a line said meanwhile")) {
        mg_atomic_xchg(&g_stand_other, 1);
    }
    if (g_stand_before) g_stand_before(level, line, g_stand_before_user);
}

static MG_HOSTCB void standing_logger(void *arg)
{
    (void)arg;
    mg_log(MG_LOG_ERROR, "log test: a line whose sink stands (the tests' own, not an error)");
}

static void test_log_sink_past_lock(void)
{
    mg_thread *t;
    mg_hold_seen seen;
    char line[128];
    u64 began, took;
    int i;
    bool held;
    say("the log's sink is called past the log's lock");
    mg_log_get_sink(&g_stand_before, &g_stand_before_user);
    g_stand_entered = g_stand_release = g_stand_other = 0;
    mg_log_set_sink(standing_sink, NULL);
    t = mg_thread_start(standing_logger, NULL, 64u * 1024u, "log-stander");
    CHECK(t != NULL);
    for (i = 0; i < 1000 && t && g_stand_entered != 1; i++) mg_sleep_ns(2000000ull);
    CHECK(g_stand_entered == 1);
    /* While that sink stands: nobody holds the log's lock, and a line of this thread's goes through. */
    held = mg_log_holder(&seen, line, sizeof(line));
    began = mg_time_monotonic_ns();
    mg_log(MG_LOG_ERROR, "log test: a line said meanwhile (the tests' own, not an error)");
    took = mg_time_monotonic_ns() - began;
    say("  a line said while another thread's sink stood: through in %.1f ms, the log's lock %s, the other sink still standing: %s",
        (double)took / 1e6, held ? "held" : "free", g_stand_entered == 1 ? "yes" : "no");
    CHECK(!held && g_stand_other == 1 && g_stand_entered == 1);
    mg_atomic_xchg(&g_stand_release, 1);
    if (t) mg_thread_join(t);
    mg_log_set_sink(g_stand_before, g_stand_before_user);
}

#if defined(MG_DL_TESTS)
/*
 * A DLL that does not load says why (mg_dl_open, mg_host_win32.c): the Windows error, and which of what it imports this
 * process cannot have. tools/build-tests.sh win makes the DLLs beside mg_tests.exe: mg-dl-t1.dll imports a function of
 * mg-dl-nowhere.dll, which is nowhere; mg-dl-t2.dll one that kernel32.dll has not got. On an Xbox ANGLE's DLLs did not
 * load, and the log said they were missing (0.1.0.679).
 */
static char g_dl_line[1024];
static mg_log_sink g_dl_before;
static void *g_dl_before_user;

static void dl_sink(int level, const char *line, void *user)
{
    (void)user;
    if (strstr(line, "did not load")) mg_strlcpy(g_dl_line, line, sizeof(g_dl_line));
    if (g_dl_before) g_dl_before(level, line, g_dl_before_user);
}

static void test_dl_open_why(void)
{
    static const struct { const char *dll, *error, *why; } cases[] = {
        { "mg-dl-t1.dll", "Windows error 126 ", "; this process cannot have mg-dl-nowhere.dll (error 126)" },
        { "mg-dl-t2.dll", "Windows error 127 ", "; this process cannot have KERNEL32.dll!MeglerNoSuchFunction" },
        { "mg-dl-absent.dll", "Windows error 126 ", "; its file could not be opened (error 2)" },
    };
    size_t i;
    say("a DLL that does not load says why");
    mg_log_get_sink(&g_dl_before, &g_dl_before_user);
    mg_log_set_sink(dl_sink, NULL);
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        void *h;
        g_dl_line[0] = 0;
        h = mg_dl_open(cases[i].dll);
        say("  %s: %s", cases[i].dll, g_dl_line[0] ? g_dl_line : "(no line)");
        CHECK(h == NULL);
        CHECK(strstr(g_dl_line, cases[i].error) != NULL);
        CHECK(strstr(g_dl_line, cases[i].why) != NULL);
    }
    mg_log_set_sink(g_dl_before, g_dl_before_user);
}
#endif

/*
 * A file removed while it is open goes from its folder at once, as POSIX's unlink takes it: the name can be made again, the
 * folder renamed, and the handle still reads what the file had. Windows 10 Mobile's DeleteFile left it there until the last
 * handle went, and TikTok Lite's updates, whose archive it deletes as it reads it before renaming their folder into place,
 * never took on a phone (0.1.0.680, mg_path_remove's remove_open).
 */
static void test_unlink_open(const char *scratch)
{
    char *dir = mg_sprintf("%s/unlink-open/139141132--updating", scratch);
    char *done = mg_sprintf("%s/unlink-open/139141132", scratch);
    char *path = mg_sprintf("%s/res.zst", dir);
    char *trash = mg_sprintf("%s/unlink-open", scratch);
    char buf[8] = { 0 };
    mg_stat st;
    mg_file *f, *again;
    int err = 0;
    say("a file removed while open goes from its folder at once");
    mg_path_rmdir(done, NULL);
    CHECK(mg_path_mkdirs(dir));
    mg_path_add_trash(trash);
    CHECK(mg_file_write_all(path, "zstd", 4));
    f = mg_file_open(path, MG_O_READ, &err);
    CHECK(f != NULL);
    CHECK(mg_path_remove(path, &err));
    CHECK(!mg_path_stat(path, &st, NULL));                          /* the name is gone */
    again = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_EXCL, &err);
    CHECK(again != NULL);                                           /* and can be made again */
    if (again) mg_file_close(again);
    CHECK(mg_path_remove(path, &err));
    CHECK(mg_path_rename(dir, done, &err));                         /* the folder moves with the first file still open */
    if (f) {
        CHECK_INT(mg_file_pread(f, buf, 4, 0, &err), 4);            /* and that handle reads what the file had */
        CHECK(!memcmp(buf, "zstd", 4));
        mg_file_close(f);
    }
    CHECK(mg_path_stat(done, &st, NULL) && st.is_dir);
    CHECK(mg_path_rmdir(done, &err));                               /* empty: nothing of the file left in it */
    free(dir);
    free(done);
    free(path);
    free(trash);
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
#if defined(_WIN32)
    {
        /* A DLL of the system's as it lies in memory (megler_dump_module): its image, MZ first, SizeOfImage long. */
        char *img = mg_sprintf("%s/kernel32.img", scratch);
        mg_addr base = 0;
        size_t size = 0, got = 0;
        u8 *bytes;
        CHECK(mg_host_module_dump("kernel32.dll", img, &base, &size) && base && size > 4096);
        bytes = mg_file_read_all(img, &got);
        CHECK(bytes && got == size && bytes[0] == 'M' && bytes[1] == 'Z' && !memcmp(bytes, (const void *)base, 512));
        free(bytes);
        CHECK(mg_path_remove(img, &err));
        free(img);
    }
#endif
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

/* --------------------------------------------------- an app's libraries placed */

/* An APK of stored entries, each with its CRC-32, as an app's carries its libraries. False when it cannot be written. */
static bool stored_apk(const char *path, const char *const *names, const u8 *const *data, const u32 *sizes, int n)
{
    size_t cap = 256, at = 0, cd;
    u32 offsets[16];
    u8 *z;
    int i;
    bool ok;
    for (i = 0; i < n; i++) cap += 2 * (46 + strlen(names[i])) + sizes[i];
    z = (u8 *)mg_alloc(cap);
    for (i = 0; i < n && i < 16; i++) {
        u16 nl = (u16)strlen(names[i]);
        u32 crc = (u32)crc32(crc32(0L, Z_NULL, 0), data[i], (uInt)sizes[i]);
        offsets[i] = (u32)at;
        mg_wr32(z + at, 0x04034b50u); mg_wr16(z + at + 4, 20); mg_wr16(z + at + 6, 0); mg_wr16(z + at + 8, 0);
        mg_wr32(z + at + 10, 0); mg_wr32(z + at + 14, crc); mg_wr32(z + at + 18, sizes[i]); mg_wr32(z + at + 22, sizes[i]);
        mg_wr16(z + at + 26, nl); mg_wr16(z + at + 28, 0);
        memcpy(z + at + 30, names[i], nl);
        memcpy(z + at + 30 + nl, data[i], sizes[i]);
        at += 30 + nl + sizes[i];
    }
    cd = at;
    for (i = 0; i < n && i < 16; i++) {
        u16 nl = (u16)strlen(names[i]);
        memset(z + at, 0, 46);
        mg_wr32(z + at, 0x02014b50u); mg_wr16(z + at + 4, 20); mg_wr16(z + at + 6, 20);
        mg_wr32(z + at + 16, (u32)crc32(crc32(0L, Z_NULL, 0), data[i], (uInt)sizes[i]));
        mg_wr32(z + at + 20, sizes[i]); mg_wr32(z + at + 24, sizes[i]); mg_wr16(z + at + 28, nl); mg_wr32(z + at + 42, offsets[i]);
        memcpy(z + at + 46, names[i], nl);
        at += 46 + nl;
    }
    memset(z + at, 0, 22);
    mg_wr32(z + at, 0x06054b50u); mg_wr16(z + at + 8, (u16)n); mg_wr16(z + at + 10, (u16)n); mg_wr32(z + at + 12, (u32)(at - cd)); mg_wr32(z + at + 16, (u32)cd);
    at += 22;
    ok = mg_file_write_all(path, z, at);
    free(z);
    return ok;
}

/* Whether a file holds exactly these bytes. */
static bool file_is(const char *dir, const char *name, const u8 *data, size_t size)
{
    char *path = mg_sprintf("%s/%s", dir, name);
    size_t n = 0;
    u8 *got = mg_file_read_all(path, &n);
    bool same = got && n == size && !memcmp(got, data, size);
    free(got);
    free(path);
    return same;
}

static bool file_there(const char *dir, const char *name)
{
    char *path = mg_sprintf("%s/%s", dir, name);
    mg_stat st;
    bool there = mg_path_stat(path, &st, NULL);
    free(path);
    return there;
}

/* The names of a folder's files, sorted and joined by spaces. */
typedef struct names_listed {
    char names[16][64];
    int n;
} names_listed;

static MG_HOSTCB void name_listed(const char *name, bool is_dir, void *user)
{
    names_listed *l = (names_listed *)user;
    if (!is_dir && l->n < 16) mg_strlcpy(l->names[l->n++], name, sizeof(l->names[0]));
}

/* A folder's files removed (its folders left): what a run before this one left. */
static void folder_emptied(const char *dir)
{
    names_listed l;
    int i;
    memset(&l, 0, sizeof(l));
    mg_path_list(dir, name_listed, &l, NULL);
    for (i = 0; i < l.n; i++) {
        char *path = mg_sprintf("%s/%s", dir, l.names[i]);
        mg_path_remove(path, NULL);
        free(path);
    }
}

static void folder_files(const char *dir, char *out, size_t cap)
{
    names_listed l;
    int i, k;
    memset(&l, 0, sizeof(l));
    mg_path_list(dir, name_listed, &l, NULL);
    for (i = 1; i < l.n; i++)
        for (k = i; k > 0 && strcmp(l.names[k - 1], l.names[k]) > 0; k--) {
            char t[64];
            memcpy(t, l.names[k], sizeof(t));
            memcpy(l.names[k], l.names[k - 1], sizeof(t));
            memcpy(l.names[k - 1], t, sizeof(t));
        }
    out[0] = 0;
    for (i = 0; i < l.n; i++) {
        size_t used = strlen(out);
        snprintf(out + used, cap - used, "%s%s", i ? " " : "", l.names[i]);
    }
}

/*
 * An app's libraries as files of their own in its library folder (ld/mg_ld_place.c, ApplicationInfo.nativeLibraryDir), across
 * an update, as Android's installer leaves the folder. Up to 0.1.0.685 a file was written only when it was not there or not of
 * the entry's size: an update whose library kept its size kept the old file, and a library the update dropped stayed. TikTok
 * Lite 47.7.4 moved libnpth.so out of its APK into an archive it unpacks itself and loads by its path; the older version's
 * still in the folder was found by its name and loaded instead, its natives not the new Java's, and the app ended at its start.
 */
static void test_place_libs(const char *scratch)
{
    enum { A = 96, B = 120, GONE = 50, NEW = 40, SPLIT_A = 70, CASE = 30 };
    static u8 a1[A], a2[A], b[B], gone[GONE], fresh[NEW], split_a[SPLIT_A], casey[CASE], junk[B];
    char *dir = mg_sprintf("%s/place/data/lib", scratch), *index = mg_sprintf("%s/place/megler/lib-index/megler.test.place", scratch);
    char *base1 = mg_sprintf("%s/place/base1.apk", scratch), *base2 = mg_sprintf("%s/place/base2.apk", scratch), *split1 = mg_sprintf("%s/place/split1.apk", scratch);
    char *notes = mg_sprintf("%s/notes.txt", dir), *odd_case = mg_sprintf("%s/libCase.so", dir), listed[400], err[160];
    const char *names1[] = { "AndroidManifest.xml", "lib/armeabi-v7a/liba.so", "lib/armeabi-v7a/libb.so", "lib/armeabi-v7a/libgone.so",
                             "lib/armeabi-v7a/sub/libnested.so", "lib/armeabi-v7a/readme.txt", "lib/x86/libx.so", "lib/armeabi-v7a/libcase.so" };
    const u8 *data1[] = { a1, a1, b, gone, b, b, b, casey };
    const u32 sizes1[] = { 8, A, B, GONE, B, 10, B, CASE };
    const char *names2[] = { "lib/armeabi-v7a/liba.so", "lib/armeabi-v7a/libb.so", "lib/armeabi-v7a/libnew.so", "lib/armeabi-v7a/libcase.so" };
    const u8 *data2[] = { a2, b, fresh, casey };
    const u32 sizes2[] = { A, B, NEW, CASE };
    const char *names_split[] = { "lib/armeabi-v7a/liba.so" };
    const u8 *data_split[] = { split_a };
    const u32 sizes_split[] = { SPLIT_A };
    mg_zip z1, z2, zs;
    const mg_zip *apks[2];
    mg_ld_placed p;
    int i;
    say("an app's libraries placed as files, across an update");
    for (i = 0; i < A; i++) a1[i] = a2[i] = (u8)(i * 7 + 1);
    a2[A / 2] ^= 0x5A;                     /* the update's liba.so: of the same size, one byte other */
    for (i = 0; i < B; i++) b[i] = (u8)(i * 3 + 2), junk[i] = (u8)(0xA5 ^ i);
    for (i = 0; i < GONE; i++) gone[i] = (u8)(0x40 + i);
    for (i = 0; i < NEW; i++) fresh[i] = (u8)(0x90 + i);
    for (i = 0; i < SPLIT_A; i++) split_a[i] = (u8)(0xC0 + i);
    for (i = 0; i < CASE; i++) casey[i] = (u8)(0x20 + i);
    memset(&z1, 0, sizeof(z1));
    memset(&z2, 0, sizeof(z2));
    memset(&zs, 0, sizeof(zs));
    mg_path_mkdirs(dir);
    folder_emptied(dir);
    mg_path_remove(index, NULL);
    CHECK(stored_apk(base1, names1, data1, sizes1, 8) && stored_apk(base2, names2, data2, sizes2, 4) && stored_apk(split1, names_split, data_split, sizes_split, 1));
    if (!mg_zip_open(&z1, base1, err, sizeof(err)) || !mg_zip_open(&z2, base2, err, sizeof(err)) || !mg_zip_open(&zs, split1, err, sizeof(err))) {
        say("  the test's APKs do not open: %s", err);
        CHECK(false);
        goto done;
    }
    /* What is no library of the app's stays: a file of another kind, and a folder. A library under another case of a name is an older one. */
    CHECK(mg_file_write_all(notes, "the app's own", 13) && mg_file_write_all(odd_case, "an older libcase", 16));

    /* The first install: the base's libraries (its liba.so, not the split's after it), none of another ABI or folder. */
    apks[0] = &z1;
    apks[1] = &zs;
    CHECK(mg_ld_place_libs(apks, 2, "armeabi-v7a", dir, index, &p));
    folder_files(dir, listed, sizeof(listed));
    say("  installed: %d libraries, %d written, %d summed, %d removed (%s); the folder: %s", p.libs, p.written, p.summed, p.removed, p.removed_names, listed);
    CHECK(p.libs == 4 && p.written == 4 && p.summed == 0 && p.removed == 1 && !strcmp(p.removed_names, "libCase.so"));
    CHECK(!strcmp(listed, "liba.so libb.so libcase.so libgone.so notes.txt"));
    CHECK(file_is(dir, "liba.so", a1, A) && file_is(dir, "libb.so", b, B) && file_is(dir, "libgone.so", gone, GONE) && file_is(dir, "libcase.so", casey, CASE));

    /* Started again: nothing written, nothing read -- the index and the APK's central directory say so. */
    CHECK(mg_ld_place_libs(apks, 2, "armeabi-v7a", dir, index, &p));
    CHECK(p.libs == 4 && p.written == 0 && p.summed == 0 && p.removed == 0 && p.failed == 0);

    /* The update: liba.so of the same size and other bytes, written; libgone.so, which it has not, removed; libnew.so written. */
    apks[0] = &z2;
    CHECK(mg_ld_place_libs(apks, 1, "armeabi-v7a", dir, index, &p));
    folder_files(dir, listed, sizeof(listed));
    say("  updated: %d libraries, %d written, %d summed, %d removed (%s); the folder: %s", p.libs, p.written, p.summed, p.removed, p.removed_names, listed);
    CHECK(p.libs == 4 && p.written == 2 && p.summed == 0 && p.removed == 1 && !strcmp(p.removed_names, "libgone.so"));
    CHECK(!strcmp(listed, "liba.so libb.so libcase.so libnew.so notes.txt"));
    CHECK(file_is(dir, "liba.so", a2, A) && file_is(dir, "libnew.so", fresh, NEW) && !file_there(dir, "libgone.so"));

    /* With no index (Megler's of before it, or lost): files of the entries' sizes are summed once, and only the one that differs
       -- of the old rule's kind, of the right size and other bytes -- is written. */
    CHECK(mg_path_remove(index, NULL));
    {
        char *bpath = mg_sprintf("%s/libb.so", dir);
        CHECK(mg_file_write_all(bpath, junk, B));
        free(bpath);
    }
    CHECK(mg_ld_place_libs(apks, 1, "armeabi-v7a", dir, index, &p));
    say("  no index: %d written, %d summed", p.written, p.summed);
    CHECK(p.written == 1 && p.summed == 4 && p.removed == 0 && file_is(dir, "libb.so", b, B));
    CHECK(mg_ld_place_libs(apks, 1, "armeabi-v7a", dir, index, &p));
    CHECK(p.written == 0 && p.summed == 0);

    /* An index that is no index of these: read as none. */
    CHECK(mg_file_write_all(index, "something else\n00000000 96 liba.so\n", 35));
    CHECK(mg_ld_place_libs(apks, 1, "armeabi-v7a", dir, index, &p));
    CHECK(p.written == 0 && p.summed == 4);

    /* An update with no native code at all: every library goes, the app's own file stays. */
    CHECK(mg_ld_place_libs(apks, 1, NULL, dir, index, &p));
    folder_files(dir, listed, sizeof(listed));
    say("  no libraries: %d removed (%s); the folder: %s", p.removed, p.removed_names, listed);
    CHECK(p.libs == 0 && p.removed == 4 && !strcmp(listed, "notes.txt"));
done:
    if (z1.count) mg_zip_close(&z1);
    if (z2.count) mg_zip_close(&z2);
    if (zs.count) mg_zip_close(&zs);
    free(dir);
    free(index);
    free(base1);
    free(base2);
    free(split1);
    free(notes);
    free(odd_case);
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

static MG_HOSTCB void input_taker(void *arg)
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

static MEGLER_CB void *fake_open(void *user, const char *path, megler_media_info *info)
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
static MEGLER_CB int fake_video(void *user, void *media, uint8_t *rgba, int64_t *time_us)
{
    (void)user; (void)media;
    if (g_fake_video >= 3) return 0;
    memset(rgba, 10 + g_fake_video, 4 * 2 * 4);
    *time_us = g_fake_video * 33333;
    g_fake_video++;
    return 1;
}
static MEGLER_CB int fake_audio(void *user, void *media, int16_t *samples, int max_frames, int64_t *time_us)
{
    int n = 4800 - g_fake_audio < max_frames ? 4800 - g_fake_audio : max_frames;
    (void)user; (void)media;
    memset(samples, 1, (size_t)n * 4);
    *time_us = (int64_t)g_fake_audio * 1000000 / 48000;
    g_fake_audio += n;
    return n;
}
static MEGLER_CB bool fake_seek(void *user, void *media, int64_t time_us)
{
    (void)user; (void)media;
    g_fake_video = (int)(time_us * 30 / 1000000);
    g_fake_audio = (int)(time_us * 48000 / 1000000);
    return true;
}
static MEGLER_CB void fake_close(void *user, void *media) { (void)user; (void)media; }

static void test_media(const char *scratch)
{
    typedef struct { i32 offset, size; i64 time_us; u32 flags; } buffer_info;
    static const megler_media_host fake = { NULL, fake_open, fake_video, fake_audio, fake_seek, fake_close };
    void *(*ex_new)(void) = (void *(*)(void))mg_bionic_find("AMediaExtractor_new");
    int (*ex_fd)(void *, int, i64, i64) = (int (*)(void *, int, i64, i64))mg_bionic_find("AMediaExtractor_setDataSourceFd");
    u32 (*ex_count)(void *) = (u32 (*)(void *))mg_bionic_find("AMediaExtractor_getTrackCount");
    void *(*ex_format)(void *, u32) = (void *(*)(void *, u32))mg_bionic_find("AMediaExtractor_getTrackFormat");
    /* (The NDK's types: indexes and sizes are size_t, counts ssize_t, an offset off_t -- 64 bits on the 64-bit ABIs.) */
    int (*ex_select)(void *, size_t) = (int (*)(void *, size_t))mg_bionic_find("AMediaExtractor_selectTrack");
    int (*ex_unselect)(void *, size_t) = (int (*)(void *, size_t))mg_bionic_find("AMediaExtractor_unselectTrack");
    gssize (*ex_read)(void *, u8 *, size_t) = (gssize (*)(void *, u8 *, size_t))mg_bionic_find("AMediaExtractor_readSampleData");
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
    gssize (*codec_in)(void *, i64) = (gssize (*)(void *, i64))mg_bionic_find("AMediaCodec_dequeueInputBuffer");
    u8 *(*codec_in_buffer)(void *, size_t, size_t *) = (u8 *(*)(void *, size_t, size_t *))mg_bionic_find("AMediaCodec_getInputBuffer");
    int (*codec_queue)(void *, size_t, goff, size_t, u64, u32) = (int (*)(void *, size_t, goff, size_t, u64, u32))mg_bionic_find("AMediaCodec_queueInputBuffer");
    gssize (*codec_out)(void *, buffer_info *, i64) = (gssize (*)(void *, buffer_info *, i64))mg_bionic_find("AMediaCodec_dequeueOutputBuffer");
    u8 *(*codec_out_buffer)(void *, size_t, size_t *) = (u8 *(*)(void *, size_t, size_t *))mg_bionic_find("AMediaCodec_getOutputBuffer");
    int (*codec_release)(void *, size_t, u8) = (int (*)(void *, size_t, u8))mg_bionic_find("AMediaCodec_releaseOutputBuffer");
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
        size_t size = 0;
        i32 slot = (i32)codec_in(codec, 0), n;
        u8 *buffer = codec_in_buffer(codec, (size_t)slot, &size);
        n = (i32)ex_read(ex, buffer, size);
        if (n < 0) {
            codec_queue(codec, (size_t)slot, 0, 0, 0, 4);
            break;
        }
        codec_queue(codec, (size_t)slot, 0, (size_t)n, (u64)ex_time(ex), 0);
        ex_advance(ex);
    }
    CHECK_INT(codec_out(codec, &info, 0), -2);
    for (i = 0; i < 3; i++) {
        size_t size = 0;
        i32 slot = (i32)codec_out(codec, &info, 0);
        const u8 *frame = slot >= 0 ? codec_out_buffer(codec, (size_t)slot, &size) : NULL;
        CHECK(slot >= 0 && frame && size == 32 && frame[0] == 10 + i && info.size == 32 && info.time_us == (i64)i * 33333 && !info.flags);
        if (slot >= 0) codec_release(codec, (size_t)slot, 1);
    }
    {
        i32 slot = (i32)codec_out(codec, &info, 0);
        CHECK(slot >= 0 && (info.flags & 4) && !info.size);
        if (slot >= 0) codec_release(codec, (size_t)slot, 0);
    }
    CHECK_INT(codec_out(codec, &info, 0), -1);
    codec_delete(codec);
    /* A decoder is full after a handful of samples nobody has asked the output for, and has room again once one is. */
    ex_seek(ex, 0, 0);
    codec = codec_new("video/avc");
    codec_configure(codec, NULL, NULL, NULL, 0);
    codec_start(codec);
    for (i = 0; i < 64 && codec_in(codec, 0) == 0; i++) {
        size_t size = 0;
        u8 *buffer = codec_in_buffer(codec, 0, &size);
        codec_queue(codec, 0, 0, (size_t)ex_read(ex, buffer, size), 0, 0);
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
#if MG_GUEST_CODE && !MG_ARCH_ARM32
/* g_abi_nr: the guest ABI's number of each call of syscall()'s and 32-bit ARM's (bionic/mg_syscall_abi.c). */
#include "../src/bionic/mg_syscall_abi.inc"
#endif

/* The guest ABI's number of the call 32-bit ARM numbers `arm`, which the tests below are written in; -1 where it has none. */
static long abi_nr(long arm)
{
#if MG_GUEST_CODE && !MG_ARCH_ARM32
    size_t i;
    for (i = 0; i < MG_COUNT(g_abi_nr); i++)
        if (g_abi_nr[i][1] == arm) return g_abi_nr[i][0];
    return -1;
#else
    return arm;
#endif
}

typedef long (*syscall_fn)(long, long, long, long, long, long, long);
static syscall_fn g_sys;

/* syscall() with 32-bit ARM's number of a call, as the guest's ABI numbers it (ENOSYS for one it has not). */
static long sys(long arm, long a, long b, long c, long d, long e, long f)
{
    long nr = abi_nr(arm);
    return g_sys(nr < 0 ? 100000 : nr, a, b, c, d, e, f);
}

/*
 * syscall() by number for pipes, epoll and eventfd, as Discord's media engine made its event loop (it said "syscall(359) is
 * not supported", and 357, 356, 351): each gives a descriptor that works -- a byte through the pipe, the eventfd's count,
 * the pipe's read end ready in the epoll set. Each call by the ABI's own number (ARM64 has no eventfd, epoll_wait, open or
 * unlink, only their newer forms, which take their place there).
 */
static void test_syscall_fds(const char *scratch)
{
    typedef int (*close_fn)(int);
    typedef long (*io_fn)(int, void *, size_t);
    close_fn close_ = (close_fn)mg_bionic_find("close");
    io_fn write_ = (io_fn)mg_bionic_find("write"), read_ = (io_fn)mg_bionic_find("read");
    int fds[2] = { -1, -1 }, ev, ep, ready;
    u8 b = 0;
    u64 count = 0, one = 1;
    u64 event[2] = { 1 /* EPOLLIN */, 7 }, got[2] = { 0, 0 };   /* epoll_event: events, then data at 8 (at 4 on x86_64, packed) */
    /* syscall()'s arguments are longs, addresses among them, as on the guest's ARM: a long holds an address only where
       both are 32 bits (Windows' x64 build has longs of 32 bits and addresses of 64). */
    if (sizeof(long) < sizeof(void *)) {
        say("syscall() by number: left out, a long does not hold an address on this build");
        return;
    }
    g_sys = (syscall_fn)mg_bionic_find("syscall");
    say("syscall() by number: pipe2, eventfd2, eventfd, epoll_create1, epoll_ctl, epoll_wait");
    CHECK(g_sys && close_ && write_ && read_);
    if (!g_sys || !close_ || !write_ || !read_) return;
#if MG_ARCH_X86_64
    /* (x86_64's epoll_event is packed: its data at 4.) */
    event[0] = 1 | (7ull << 32);
    event[1] = 0;
#endif
    CHECK(sys(359, (long)(mg_addr)fds, 0, 0, 0, 0, 0) == 0 && fds[0] >= 0 && fds[1] >= 0);
    b = 42;
    CHECK(write_(fds[1], &b, 1) == 1);
    ev = (int)sys(356, 0, 0, 0, 0, 0, 0);
    CHECK(ev >= 0 && write_(ev, &one, 8) == 8 && read_(ev, &count, 8) == 8 && count == 1);
    if (ev >= 0) close_(ev);
    if (abi_nr(351) >= 0) {
        ev = (int)sys(351, 3, 0, 0, 0, 0, 0);
        CHECK(ev >= 0 && read_(ev, &count, 8) == 8 && count == 3);
        if (ev >= 0) close_(ev);
    }
    ep = (int)sys(357, 0, 0, 0, 0, 0, 0);
    CHECK(ep >= 0 && sys(251, ep, 1 /* EPOLL_CTL_ADD */, fds[0], (long)(mg_addr)event, 0, 0) == 0);
    /* (epoll_wait where the ABI has it, else epoll_pwait with no mask.) */
    ready = abi_nr(252) >= 0 ? (int)sys(252, ep, (long)(mg_addr)got, 1, 0, 0, 0) : (int)sys(346, ep, (long)(mg_addr)got, 1, 0, 0, 0);
#if MG_ARCH_X86_64
    CHECK(ready == 1 && (got[0] & 1) && (got[0] >> 32) == 7);
#else
    CHECK(ready == 1 && (got[0] & 1) && got[1] == 7);
#endif
    b = 0;
    CHECK(read_(fds[0], &b, 1) == 1 && b == 42);
    if (ep >= 0) close_(ep);
    close_(fds[0]);
    close_(fds[1]);

    /* Files by number, as TikTok Lite's loader writes its libraries out (syscall(__NR_open)): open, write, close, open
       again, lseek, read, fstat64 (st_size at 48 in ARM's struct stat64, at 44 in x86's, whose long long is 4-aligned, and
       at 48 in the 64-bit ABIs' struct stat, 144 bytes on x86_64), syncfs (and EBADF for a descriptor not open, as Google
       Maps asks), close, unlink; and sched_getaffinity, whose raw call says the mask's size. (Each by the ABI's own number:
       openat and unlinkat where it has no open or unlink, ARM64.) */
    {
        enum { ST_SIZE_AT = MG_ARCH_X86 ? 44 : 48 };
        const char *file = "/syscall-files/written.bin";
        char back[4] = { 0 };
        u8 st[160], mask[16];
        int fd;
        mg_fs_mount("/syscall-files", scratch);
        say("syscall() by number: open, write, read, lseek, fstat64, syncfs, close, unlink, sched_getaffinity");
#define OPEN_(path, flags, mode) (abi_nr(5) >= 0 ? sys(5, (long)(mg_addr)(path), flags, mode, 0, 0, 0) \
                                                : sys(322, -100 /* AT_FDCWD */, (long)(mg_addr)(path), flags, mode, 0, 0))
        fd = (int)OPEN_(file, 01 | 0100 | 01000, 0600);   /* O_WRONLY | O_CREAT | O_TRUNC */
        CHECK(fd >= 0 && sys(4, fd, (long)(mg_addr)"megler", 6, 0, 0, 0) == 6 && sys(6, fd, 0, 0, 0, 0, 0) == 0);
        fd = (int)OPEN_(file, 0, 0);
        CHECK(fd >= 0 && sys(19, fd, 2, 0, 0, 0, 0) == 2 && sys(3, fd, (long)(mg_addr)back, 4, 0, 0, 0) == 4 && !memcmp(back, "gler", 4));
        memset(st, 0, sizeof(st));
        CHECK(fd >= 0 && sys(197, fd, (long)(mg_addr)st, 0, 0, 0, 0) == 0 && mg_rd32(st + ST_SIZE_AT) == 6);
        CHECK(fd >= 0 && sys(373, fd, 0, 0, 0, 0, 0) == 0 && sys(373, 999, 0, 0, 0, 0, 0) == -1);
        CHECK(fd >= 0 && sys(6, fd, 0, 0, 0, 0, 0) == 0
              && (abi_nr(10) >= 0 ? sys(10, (long)(mg_addr)file, 0, 0, 0, 0, 0) : sys(328, -100, (long)(mg_addr)file, 0, 0, 0, 0)) == 0);
        CHECK(OPEN_(file, 0, 0) == -1);
#undef OPEN_
        memset(mask, 0, sizeof(mask));
        CHECK(sys(242, 0, sizeof(mask), (long)(mg_addr)mask, 0, 0, 0) == 4 && mask[0] != 0 && sys(241, 0, sizeof(mask), (long)(mg_addr)mask, 0, 0, 0) == 0);
    }
}

static void test_mmap_commit(void)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, goff);
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
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, goff);
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
 * A file mapped shared shows what is written to it through a descriptor afterwards (bionic/mg_libc_mem.c,
 * mg_mem_file_written), as a kernel's mapping of a file does. Bolt (Psiphon's database, in WhatsApp's libgojni.so) maps
 * its file read-only and shared, from a descriptor open for writing, past the file's end; writes it with pwrite, and
 * reads its new meta page through the mapping; grows the file and writes past the old end. LMDB writes its meta page
 * through a second descriptor. The pages stay read-only (where the host says), one the app unmapped is left alone, and a
 * writable mapping's bytes written back by msync reach a read-only mapping of the file too.
 */
static void test_mmap_file_written(const char *scratch)
{
    typedef int (*open_fn)(const char *, int, int);
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, goff);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*msync_fn)(void *, size_t, int);
    typedef int (*close_fn)(int);
    typedef int (*unlink_fn)(const char *);
    typedef gssize (*pwrite_fn)(int, const void *, size_t, goff);
    typedef gssize (*write_fn)(int, const void *, size_t);
    typedef goff (*lseek_fn)(int, goff, int);
    typedef int (*ftruncate_fn)(int, goff);
    enum { READ = 1, RW = 3, SHARED = 0x01, PAGE = 4096 };
    open_fn open_ = (open_fn)mg_bionic_find("open");
    mmap_fn mmap_ = (mmap_fn)mg_bionic_find("mmap");
    munmap_fn munmap_ = (munmap_fn)mg_bionic_find("munmap");
    msync_fn msync_ = (msync_fn)mg_bionic_find("msync");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    unlink_fn unlink_ = (unlink_fn)mg_bionic_find("unlink");
    pwrite_fn pwrite_ = (pwrite_fn)mg_bionic_find("pwrite");
    write_fn write_ = (write_fn)mg_bionic_find("write");
    lseek_fn lseek_ = (lseek_fn)mg_bionic_find("lseek");
    ftruncate_fn ftruncate_ = (ftruncate_fn)mg_bionic_find("ftruncate");
    const char *file = "/syscall-files/bolt.db";
    u8 *p, *w;
    int fd, fd2, prot = -1;
    bool committed = false;
    say("mmap: a file mapped shared shows what is written to it afterwards, through any descriptor of it");
    mg_fs_mount("/syscall-files", scratch);
    CHECK(open_ && mmap_ && munmap_ && msync_ && close_ && unlink_ && pwrite_ && write_ && lseek_ && ftruncate_);
    if (!open_ || !mmap_ || !munmap_ || !msync_ || !close_ || !unlink_ || !pwrite_ || !write_ || !lseek_ || !ftruncate_) return;
    fd = open_(file, 02 | 0100 | 01000, 0600);                   /* O_RDWR | O_CREAT | O_TRUNC */
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(ftruncate_(fd, 3 * PAGE) == 0 && pwrite_(fd, "meta0", 5, PAGE) == 5);
    p = (u8 *)mmap_(NULL, 4 * PAGE, READ, SHARED, fd, 0);         /* past the end, as Bolt maps */
    CHECK(p && p != (u8 *)(mg_addr)-1);
    if (p && p != (u8 *)(mg_addr)-1) {
        CHECK(!memcmp(p + PAGE, "meta0", 5));
        CHECK(pwrite_(fd, "meta1", 5, PAGE) == 5 && !memcmp(p + PAGE, "meta1", 5));
        /* Across a page's edge, and by write() at the descriptor's offset. */
        CHECK(pwrite_(fd, "edge", 4, 2 * PAGE - 2) == 4 && !memcmp(p + 2 * PAGE - 2, "edge", 4));
        CHECK(lseek_(fd, 100, 0) == 100 && write_(fd, "seq", 3) == 3 && !memcmp(p + 100, "seq", 3));
        /* Grown, and written past the old end, through another descriptor of the file. */
        fd2 = open_(file, 02, 0);
        CHECK(fd2 >= 0 && ftruncate_(fd2, 4 * PAGE) == 0 && pwrite_(fd2, "grow", 4, 3 * PAGE) == 4);
        if (fd2 >= 0) close_(fd2);
        CHECK(!memcmp(p + 3 * PAGE, "grow", 4));
        if (mg_mem_page_state(p + PAGE, &committed, &prot)) CHECK(committed && prot == MG_PROT_R);
        /* A page unmapped is left alone; the others still follow. */
        CHECK(munmap_(p + 2 * PAGE, PAGE) == 0);
        CHECK(pwrite_(fd, "gone", 4, 2 * PAGE + 8) == 4 && pwrite_(fd, "meta2", 5, PAGE) == 5 && !memcmp(p + PAGE, "meta2", 5));
        CHECK(munmap_(p, 2 * PAGE) == 0 && munmap_(p + 3 * PAGE, PAGE) == 0);
    }
    /* A writable shared mapping's bytes, written back by msync, in a read-only mapping of the file. */
    w = (u8 *)mmap_(NULL, PAGE, RW, SHARED, fd, 0);
    p = (u8 *)mmap_(NULL, PAGE, READ, SHARED, fd, 0);
    CHECK(w && w != (u8 *)(mg_addr)-1 && p && p != (u8 *)(mg_addr)-1);
    if (w && w != (u8 *)(mg_addr)-1 && p && p != (u8 *)(mg_addr)-1) {
        CHECK(!memcmp(w + 100, "seq", 3));
        memcpy(w + 200, "mapped", 6);
        CHECK(msync_(w, PAGE, 4 /* MS_SYNC */) == 0 && !memcmp(p + 200, "mapped", 6));
        /* And what is written to the file, in both. */
        CHECK(pwrite_(fd, "both", 4, 300) == 4 && !memcmp(w + 300, "both", 4) && !memcmp(p + 300, "both", 4));
        munmap_(w, PAGE);
        munmap_(p, PAGE);
    }
    CHECK(!mg_mem_file_copies());
    close_(fd);
    unlink_(file);
}

/*
 * A writable shared file mapping is written back with the mappings' lock let go (bionic/mg_libc_mem.c, write_back_out):
 * while a write-back reads and writes its file, another thread maps and unmaps memory -- which takes that lock -- and the
 * hang report finds nobody holding it. The write-back read every such file and wrote it under the lock, which every mmap,
 * munmap, mprotect and madvise of the app's and the fault handler wait for (Viber's hang report on 0.1.0.669 named the
 * write-back thread as its holder). msync, the 5-second pass, the suspension's and munmap each write back so, and the
 * file has the bytes.
 */
typedef void *(*wb_mmap_fn)(void *, size_t, int, int, int, goff);
typedef int (*wb_munmap_fn)(void *, size_t);
static wb_mmap_fn g_wb_mmap;
static wb_munmap_fn g_wb_munmap;
static volatile i32 g_wb_calls, g_wb_mapped, g_wb_lock_free;

static MG_HOSTCB void wb_mapper(void *arg)
{
    u8 *p = (u8 *)g_wb_mmap(NULL, 65536, 3, 0x22, -1, 0);        /* PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS */
    (void)arg;
    if (p && p != (u8 *)(mg_addr)-1) {
        p[0] = 1;
        if (g_wb_munmap(p, 65536) == 0) mg_atomic_xchg(&g_wb_mapped, 1);
    }
}

/* At the first write-back of each step, as it is about to read and write its file: the lock free, and another thread's mmap through. */
static void wb_io_hook(void)
{
    mg_hold_seen seen;
    mg_thread *t;
    int i;
    if (mg_atomic_add(&g_wb_calls, 1) != 1) return;
    g_wb_lock_free = !mg_mem_lock_look(&seen);
    g_wb_mapped = 0;
    t = mg_thread_start(wb_mapper, NULL, 256u * 1024u, "wb-mapper");
    for (i = 0; i < 1000 && t && !g_wb_mapped; i++) mg_sleep_ns(2000000ull);
    /* (Not joined: had the write-back held the lock, the other thread would wait for it, and this one for that.) */
    if (t) mg_thread_detach(t);
}

/* Whether the file at `path` holds `text` at `at`. */
static bool file_has(const char *path, u64 at, const char *text)
{
    char buf[64];
    size_t n = strlen(text);
    int err = 0;
    mg_file *f = mg_file_open(path, MG_O_READ, &err);
    bool same;
    if (!f) return false;
    same = mg_file_pread(f, buf, (u32)n, at, &err) == (i64)n && !memcmp(buf, text, n);
    mg_file_close(f);
    return same;
}

static void test_mmap_write_back_unlocked(const char *scratch)
{
    typedef int (*open_fn)(const char *, int, int);
    typedef int (*msync_fn)(void *, size_t, int);
    typedef int (*close_fn)(int);
    typedef int (*unlink_fn)(const char *);
    typedef int (*ftruncate_fn)(int, goff);
    enum { RW = 3, SHARED = 0x01, PAGE = 4096 };
    open_fn open_ = (open_fn)mg_bionic_find("open");
    msync_fn msync_ = (msync_fn)mg_bionic_find("msync");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    unlink_fn unlink_ = (unlink_fn)mg_bionic_find("unlink");
    ftruncate_fn ftruncate_ = (ftruncate_fn)mg_bionic_find("ftruncate");
    char *host = mg_sprintf("%s/store.bin", scratch);
    const char *file = "/write-back-files/store.bin";
    u8 *w;
    int fd;
    say("mmap: a shared file mapping is written back with the mappings' lock let go");
    g_wb_mmap = (wb_mmap_fn)mg_bionic_find("mmap");
    g_wb_munmap = (wb_munmap_fn)mg_bionic_find("munmap");
    mg_fs_mount("/write-back-files", scratch);
    CHECK(open_ && msync_ && close_ && unlink_ && ftruncate_ && g_wb_mmap && g_wb_munmap);
    if (!open_ || !msync_ || !close_ || !unlink_ || !ftruncate_ || !g_wb_mmap || !g_wb_munmap) {
        free(host);
        return;
    }
    fd = open_(file, 02 | 0100 | 01000, 0600);                    /* O_RDWR | O_CREAT | O_TRUNC */
    CHECK(fd >= 0 && ftruncate_(fd, 3 * PAGE) == 0);
    w = fd >= 0 ? (u8 *)g_wb_mmap(NULL, 3 * PAGE, RW, SHARED, fd, 0) : NULL;
    if (fd >= 0) close_(fd);                                      /* the mapping holds the file apart */
    CHECK(w && w != (u8 *)(mg_addr)-1);
    if (w && w != (u8 *)(mg_addr)-1) {
        mg_mem_write_back_io_hook = wb_io_hook;
        /* msync. */
        memcpy(w + 10, "msync'd", 7);
        g_wb_calls = 0;
        CHECK(msync_(w, 3 * PAGE, 4 /* MS_SYNC */) == 0);
        say("  msync: the write-back's file read and written %s the mappings' lock, another thread's mmap and munmap %s", g_wb_lock_free ? "without" : "UNDER",
            g_wb_mapped ? "through meanwhile" : "NOT THROUGH");
        CHECK(g_wb_calls >= 1 && g_wb_lock_free && g_wb_mapped && file_has(host, 10, "msync'd"));
        /* The 5-second pass. */
        memcpy(w + PAGE + 5, "the pass", 8);
        g_wb_calls = 0;
        mg_mem_write_back_all(4u << 20);
        CHECK(g_wb_calls >= 1 && g_wb_lock_free && g_wb_mapped && file_has(host, PAGE + 5, "the pass"));
        /* The suspension's, which waits no longer than it is told. */
        memcpy(w + 2 * PAGE + 7, "suspended", 9);
        g_wb_calls = 0;
        CHECK(mg_mem_write_back_all_until(mg_time_monotonic_ns() + 5000000000ull));
        CHECK(g_wb_calls >= 1 && g_wb_lock_free && g_wb_mapped && file_has(host, 2 * PAGE + 7, "suspended"));
        /* munmap: the pages to the file first, then gone. */
        memcpy(w + 100, "unmapped", 8);
        g_wb_calls = 0;
        CHECK(g_wb_munmap(w, 3 * PAGE) == 0);
        CHECK(g_wb_calls >= 1 && g_wb_lock_free && g_wb_mapped && file_has(host, 100, "unmapped") && file_has(host, 10, "msync'd"));
        mg_mem_write_back_io_hook = NULL;
    }
    CHECK(!mg_mem_file_copies());
    unlink_(file);
    free(host);
}

/*
 * The mappings' lock is never held waiting for the descriptors' lock (bionic/mg_libc_io.c, io_lock_wanted). A thread that
 * holds the descriptors' lock and meets a page the app gave back with madvise -- a pipe's bytes copied into a buffer there --
 * is mended by the fault handler, which takes the mappings' lock. The write-back took its hold of its file under the
 * mappings' lock, and mmap of a shared file its hold of the descriptor's, and each hold took the descriptors' lock: such a
 * thread and a fault's waited for each other for good, and every mmap, munmap, mprotect, madvise and descriptor of the app's
 * after them. Here a pipe is read into a page given back; then this thread holds the descriptors' lock, as a pipe's read
 * holds it while it copies, while msync writes back on another thread: the write-back lets the mappings' lock go before it
 * waits, another thread's mmap goes through meanwhile, and a page given back, touched by this thread with the descriptors'
 * lock held, is mended. (Should the write-back hold the mappings' lock the while, the page is not touched -- that would stand
 * still for good -- and the test fails.) Neither mmap of a shared file nor msync, the 5-second pass, the suspension's or
 * munmap takes the descriptors' lock under the mappings' lock.
 */
typedef int (*lo_msync_fn)(void *, size_t, int);
static lo_msync_fn g_lo_msync;
static u8 *g_lo_map;
static volatile i32 g_lo_past, g_lo_done;

static void lo_io_hook(void) { mg_atomic_xchg(&g_lo_past, 1); }

static MG_HOSTCB void lo_msync_thread(void *arg)
{
    (void)arg;
    g_lo_msync(g_lo_map, 3 * 4096, 4 /* MS_SYNC */);
    mg_atomic_xchg(&g_lo_done, 1);
}

static void test_mmap_lock_order(const char *scratch)
{
    typedef int (*open_fn)(const char *, int, int);
    typedef int (*close_fn)(int);
    typedef int (*unlink_fn)(const char *);
    typedef int (*ftruncate_fn)(int, goff);
    typedef int (*madvise_fn)(void *, size_t, int);
    typedef int (*pipe_fn)(int *);
    typedef gssize (*read_fn)(int, void *, size_t);
    typedef gssize (*write_fn)(int, const void *, size_t);
    enum { READ = 1, RW = 3, SHARED = 0x01, PRIVATE = 0x02, ANONYMOUS = 0x20, DONTNEED = 4, PAGE = 4096, BLOCK = 65536 };
    open_fn open_ = (open_fn)mg_bionic_find("open");
    close_fn close_ = (close_fn)mg_bionic_find("close");
    unlink_fn unlink_ = (unlink_fn)mg_bionic_find("unlink");
    ftruncate_fn ftruncate_ = (ftruncate_fn)mg_bionic_find("ftruncate");
    madvise_fn madvise_ = (madvise_fn)mg_bionic_find("madvise");
    pipe_fn pipe_ = (pipe_fn)mg_bionic_find("pipe");
    read_fn read_ = (read_fn)mg_bionic_find("read");
    write_fn write_ = (write_fn)mg_bionic_find("write");
    char *host = mg_sprintf("%s/lock-order.bin", scratch);
    const char *file = "/lock-order-files/lock-order.bin";
    u8 *w = NULL, *ro = NULL;
    volatile u8 *g = NULL;
    int fd = -1, fds[2] = { -1, -1 }, i;
    u32 before, taken;
    mg_thread *t;
    mg_hold_seen seen;
    bool stuck = false, mapped = false, touched = false, piped = false;
    u32 stuck_owner = 0;
    say("mmap: the mappings' lock is never held waiting for the descriptors' lock");
    g_wb_mmap = (wb_mmap_fn)mg_bionic_find("mmap");
    g_wb_munmap = (wb_munmap_fn)mg_bionic_find("munmap");
    g_lo_msync = (lo_msync_fn)mg_bionic_find("msync");
    CHECK(open_ && close_ && unlink_ && ftruncate_ && madvise_ && pipe_ && read_ && write_ && g_wb_mmap && g_wb_munmap && g_lo_msync);
    if (!open_ || !close_ || !unlink_ || !ftruncate_ || !madvise_ || !pipe_ || !read_ || !write_ || !g_wb_mmap || !g_wb_munmap || !g_lo_msync) {
        free(host);
        return;
    }
    mg_fs_mount("/lock-order-files", scratch);
    before = mg_fd_lock_taken_under_mappings();
    fd = open_(file, 02 | 0100 | 01000, 0600);                    /* O_RDWR | O_CREAT | O_TRUNC */
    CHECK(fd >= 0 && ftruncate_(fd, 3 * PAGE) == 0);
    if (fd >= 0) {
        /* A writable one, written back, and a read-only copy kept in step: mmap holds the file for each. */
        w = (u8 *)g_wb_mmap(NULL, 3 * PAGE, RW, SHARED, fd, 0);
        ro = (u8 *)g_wb_mmap(NULL, PAGE, READ, SHARED, fd, 0);
        close_(fd);
    }
    if (w == (u8 *)(mg_addr)-1) w = NULL;
    if (ro == (u8 *)(mg_addr)-1) ro = NULL;
    CHECK(w && ro);
    /* Two blocks of 64 KB of anonymous memory, written, then given back. */
    g = (volatile u8 *)g_wb_mmap(NULL, 2 * BLOCK, RW, PRIVATE | ANONYMOUS, -1, 0);
    if (g == (volatile u8 *)(mg_addr)-1) g = NULL;
    CHECK(g != NULL);
    if (!w || !ro || !g) goto done;
    memset((void *)g, 0x5A, 2 * BLOCK);
    CHECK(madvise_((void *)g, 2 * BLOCK, DONTNEED) == 0);
    /* A pipe read into a page of the first block: the bytes copied with the descriptors' lock held, the fault mended meanwhile. */
    CHECK(pipe_(fds) == 0);
    if (fds[0] >= 0 && fds[1] >= 0) {
        piped = write_(fds[1], "piped", 5) == 5 && read_(fds[0], (void *)(g + 3 * PAGE + 10), 5) == 5;
        piped = piped && !memcmp((const void *)(g + 3 * PAGE + 10), "piped", 5) && g[3 * PAGE + 9] == 0 && g[3 * PAGE + 15] == 0;
        close_(fds[0]);
        close_(fds[1]);
    }
    say("  a pipe read into a page given back: %s", piped ? "its bytes" : "NOT ITS BYTES");
    CHECK(piped);
    /* The descriptors' lock held while msync writes back on another thread. */
    memcpy(w + 50, "held", 4);
    g_lo_map = w;
    g_lo_past = g_lo_done = 0;
    mg_mem_write_back_io_hook = lo_io_hook;
    mg_fd_lock();
    t = mg_thread_start(lo_msync_thread, NULL, 256u * 1024u, "lock-order-msync");
    /* Until the write-back is past the mappings' lock (its hook), or 0.4 s at least: the 5-second pass's write-back may hold
       the write-backs' lock meanwhile and wait for this thread itself, letting go nothing but the mappings' lock either. */
    for (i = 0; t && i < 200 && !g_lo_past && !stuck; i++) {
        mg_sleep_ns(2000000ull);
        if (mg_mem_lock_look(&seen) && seen.held_ns >= 200000000ull) {
            stuck = true;
            stuck_owner = seen.owner;
        }
    }
    if (t && !stuck) {
        mg_thread *m;
        int k;
        /* Its pages copied and the mappings' lock let go, it is on to its file and to letting its hold go, which waits for this
           thread (or it waits behind the pass's write-back, which does): another thread maps and unmaps meanwhile, and this
           one touches a page given back, in the second block. */
        g_wb_mapped = 0;
        m = mg_thread_start(wb_mapper, NULL, 256u * 1024u, "lock-order-mapper");
        for (k = 0; m && k < 1000 && !g_wb_mapped; k++) mg_sleep_ns(2000000ull);
        if (m) mg_thread_detach(m);
        mapped = g_wb_mapped != 0;
        touched = g[BLOCK + 100] == 0;
        g[BLOCK + 200] = 0x42;
        touched = touched && g[BLOCK + 200] == 0x42 && g[BLOCK + 201] == 0;
    }
    mg_fd_unlock();
    for (i = 0; t && i < 2500 && !g_lo_done; i++) mg_sleep_ns(2000000ull);
    if (t) {
        /* (Not joined while it has not ended: the test goes on either way.) */
        if (g_lo_done) mg_thread_join(t);
        else mg_thread_detach(t);
    }
    mg_mem_write_back_io_hook = NULL;
    if (stuck)
        say("  the descriptors' lock held: the mappings' lock HELD 0.2 s by host thread %u, which waits for it -- a fault on a page given "
            "back meanwhile would stand still for good", (unsigned)stuck_owner);
    else
        say("  the descriptors' lock held while msync wrote back on another thread: the mappings' lock %s, another thread's mmap %s, a "
            "page given back touched %s", g_lo_past ? "let go before it waited" : "free (it waited behind the 5-second pass's write-back)",
            mapped ? "through" : "NOT THROUGH", touched ? "and mended" : "NOT MENDED");
    CHECK(t != NULL && !stuck && mapped && touched && g_lo_done && file_has(host, 50, "held"));
    /* The other write-backs, and munmap's. */
    memcpy(w + PAGE + 20, "the pass", 8);
    mg_mem_write_back_all(4u << 20);
    CHECK(file_has(host, PAGE + 20, "the pass") && !memcmp(ro + 50, "held", 4));
    memcpy(w + 2 * PAGE + 30, "suspended", 9);
    CHECK(mg_mem_write_back_all_until(mg_time_monotonic_ns() + 5000000000ull) && file_has(host, 2 * PAGE + 30, "suspended"));
    memcpy(w + 70, "unmapped", 8);
    CHECK(g_wb_munmap(ro, PAGE) == 0 && g_wb_munmap(w, 3 * PAGE) == 0 && file_has(host, 70, "unmapped"));
    ro = w = NULL;
done:
    taken = mg_fd_lock_taken_under_mappings() - before;
    say("  the descriptors' lock taken under the mappings' lock: %u times", (unsigned)taken);
    CHECK_INT(taken, 0);
    if (ro) g_wb_munmap(ro, PAGE);
    if (w) g_wb_munmap(w, 3 * PAGE);
    if (g) g_wb_munmap((void *)g, 2 * BLOCK);
    CHECK(!mg_mem_file_copies());
    unlink_(file);
    free(host);
}

/*
 * What Zalo's libraries import of libc, libm and libz that was not there (each a stub that ended the app when called): POSIX
 * regular expressions (bionic/mg_regex.c), zlib's gzip files on the guest's files (bionic/mg_libz.c), j0 and j1,
 * eventfd_read and eventfd_write, fchmodat.
 */
typedef struct t_regex {
    int re_magic;
    size_t re_nsub;
    const char *re_endp;
    void *re_g;
} t_regex;
/* (regoff_t is an off_t: 32 bits in 32-bit Android, 64 in 64-bit.) */
typedef struct t_regmatch {
    goff so, eo;
} t_regmatch;

/* regexec of `pattern` (cflags) on `subject`: its result, and the first two matches' places as "so-eo so-eo" in `out`. */
static int regex_run(const char *pattern, int cflags, const char *subject, int eflags, char *out, size_t out_size)
{
    typedef int (*regcomp_fn)(t_regex *, const char *, int);
    typedef int (*regexec_fn)(const t_regex *, const char *, size_t, t_regmatch *, int);
    typedef void (*regfree_fn)(t_regex *);
    regcomp_fn comp = (regcomp_fn)mg_bionic_find("regcomp");
    regexec_fn exec = (regexec_fn)mg_bionic_find("regexec");
    regfree_fn rfree = (regfree_fn)mg_bionic_find("regfree");
    t_regex re;
    t_regmatch m[4];
    int r;
    if (out_size) out[0] = 0;
    if (!comp || !exec || !rfree) return -100;
    memset(&re, 0, sizeof(re));
    r = comp(&re, pattern, cflags);
    if (r) return -r;
    memset(m, 0xff, sizeof(m));
    if (eflags & 4) {          /* REG_STARTEND: the range in m[0] */
        m[0].so = 2;
        m[0].eo = (i32)strlen(subject) - 2;
    }
    r = exec(&re, subject, 4, m, eflags);
    if (r == 0) snprintf(out, out_size, "%d-%d %d-%d %d-%d", (int)m[0].so, (int)m[0].eo, (int)m[1].so, (int)m[1].eo, (int)m[2].so, (int)m[2].eo);
    rfree(&re);
    return r;
}

static void test_libc_gaps(const char *scratch)
{
    enum { EXT = 1, ICASE = 2, NOSUB = 4, NEWLINE = 8 };
    char out[96];
    say("regcomp and regexec: extended and basic, the leftmost longest, subexpressions, classes, flags, errors");
    CHECK(regex_run("a|ab", EXT, "xxab", 0, out, sizeof(out)) == 0 && !strncmp(out, "2-4", 3));
    CHECK(regex_run("^([a-z]+)@([a-z]+)\\.com$", EXT, "bob@mail.com", 0, out, sizeof(out)) == 0 && !strcmp(out, "0-12 0-3 4-8"));
    CHECK(regex_run("\\(ab\\)*c\\1", 0, "ababcab", 0, out, sizeof(out)) == 0 && !strncmp(out, "0-7 2-4", 7));
    CHECK(regex_run("hello", ICASE, "Say HELLO", 0, out, sizeof(out)) == 0 && !strncmp(out, "4-9", 3));
    CHECK(regex_run("^b", NEWLINE, "a\nb", 0, out, sizeof(out)) == 0 && !strncmp(out, "2-3", 3));
    CHECK(regex_run("^b", 0, "a\nb", 0, out, sizeof(out)) == 1);
    CHECK(regex_run("x{2,3}", EXT, "xxxx", 0, out, sizeof(out)) == 0 && !strncmp(out, "0-3", 3));
    CHECK(regex_run("x\\{2\\}", 0, "axxx", 0, out, sizeof(out)) == 0 && !strncmp(out, "1-3", 3));
    CHECK(regex_run("[[:digit:]]+", EXT, "ab123c", 0, out, sizeof(out)) == 0 && !strncmp(out, "2-5", 3));
    CHECK(regex_run("[^[:space:]]+", EXT, "  hi ", 0, out, sizeof(out)) == 0 && !strncmp(out, "2-4", 3));
    CHECK(regex_run("[]a]+", EXT, "x]a]", 0, out, sizeof(out)) == 0 && !strncmp(out, "1-4", 3));
    CHECK(regex_run("[a-c-]+", EXT, "zb-cq", 0, out, sizeof(out)) == 0 && !strncmp(out, "1-4", 3));
    CHECK(regex_run("\\bcat\\b", EXT, "concat cat", 0, out, sizeof(out)) == 0 && !strncmp(out, "7-10", 4));
    CHECK(regex_run("(a*)+b", EXT, "aab", 0, out, sizeof(out)) == 0 && !strncmp(out, "0-3", 3));
    CHECK(regex_run("b+", EXT | NOSUB, "abbbc", 0, out, sizeof(out)) == 0);
    CHECK(regex_run("^abc$", EXT, "xxabcxx", 4 /* REG_STARTEND */, out, sizeof(out)) == 0 && !strncmp(out, "2-5", 3));
    CHECK(regex_run("abc", EXT, "abd", 0, out, sizeof(out)) == 1);
    CHECK(regex_run("", EXT, "anything", 0, out, sizeof(out)) == 0 && !strncmp(out, "0-0", 3));
    /* The errors, as bionic's numbers. */
    CHECK(regex_run("a(", EXT, "a", 0, out, sizeof(out)) == -8);
    CHECK(regex_run("[a", EXT, "a", 0, out, sizeof(out)) == -7);
    CHECK(regex_run("a{2,1}", EXT, "a", 0, out, sizeof(out)) == -10);
    CHECK(regex_run("\\(a\\)\\2", 0, "aa", 0, out, sizeof(out)) == -6);
    CHECK(regex_run("[[:foo:]]", EXT, "a", 0, out, sizeof(out)) == -4);
    CHECK(regex_run("a\\", EXT, "a", 0, out, sizeof(out)) == -5);
    {
        typedef size_t (*regerror_fn)(int, const t_regex *, char *, size_t);
        regerror_fn err = (regerror_fn)mg_bionic_find("regerror");
        char text[16];
        CHECK(err && err(1, NULL, text, sizeof(text)) == strlen("regexec() failed to match") + 1 && !strcmp(text, "regexec() faile"));
    }
    /* A long subject: one byte's repetition takes no stack a byte; a nested one gives up a path too deep, and returns. */
    {
        enum { N = 200000 };
        char *big = (char *)mg_alloc(N + 2);
        memset(big, 'a', N);
        big[N] = 'b';
        CHECK(regex_run(".*b", EXT, big, 0, out, sizeof(out)) == 0 && !strncmp(out, "0-200001", 8));
        big[N] = 'a';
        CHECK(regex_run("(a|b)*c", EXT, big, 0, out, sizeof(out)) == 1);
        free(big);
    }

    say("gzip files on the guest's files: written, read back by lines, rewound; a plain file read as it is");
    {
        typedef void *(*gzopen_fn)(const char *, const char *);
        typedef int (*gzputs_fn)(void *, const char *);
        typedef int (*gzwrite_fn)(void *, const void *, unsigned);
        typedef char *(*gzgets_fn)(void *, char *, int);
        typedef int (*gzfile_fn)(void *);
        typedef int (*open_fn)(const char *, int, int);
        typedef long (*io_fn)(int, void *, size_t);
        typedef int (*close_fn)(int);
        gzopen_fn gzopen_ = (gzopen_fn)mg_bionic_find("gzopen");
        gzputs_fn gzputs_ = (gzputs_fn)mg_bionic_find("gzputs");
        gzwrite_fn gzwrite_ = (gzwrite_fn)mg_bionic_find("gzwrite");
        gzgets_fn gzgets_ = (gzgets_fn)mg_bionic_find("gzgets");
        gzfile_fn gzclose_ = (gzfile_fn)mg_bionic_find("gzclose"), gzeof_ = (gzfile_fn)mg_bionic_find("gzeof");
        gzfile_fn gzrewind_ = (gzfile_fn)mg_bionic_find("gzrewind"), gzdirect_ = (gzfile_fn)mg_bionic_find("gzdirect");
        open_fn open_ = (open_fn)mg_bionic_find("open");
        io_fn read_ = (io_fn)mg_bionic_find("read"), write_ = (io_fn)mg_bionic_find("write");
        close_fn close_ = (close_fn)mg_bionic_find("close");
        const char *gz = "/syscall-files/log.gz", *plain = "/syscall-files/plain.txt";
        char line[64];
        u8 head[2] = { 0, 0 };
        void *f;
        int fd;
        mg_fs_mount("/syscall-files", scratch);
        CHECK(gzopen_ && gzputs_ && gzwrite_ && gzgets_ && gzclose_ && gzeof_ && gzrewind_ && gzdirect_ && open_ && read_ && write_ && close_);
        if (gzopen_ && gzputs_ && gzwrite_ && gzgets_ && gzclose_ && gzeof_ && gzrewind_ && gzdirect_ && open_ && read_ && write_ && close_) {
            f = gzopen_(gz, "wb6");
            CHECK(f && gzputs_(f, "line one\n") == 9 && gzwrite_(f, "line two\n", 9) == 9 && gzclose_(f) == 0);
            fd = open_(gz, 0, 0);
            CHECK(fd >= 0 && read_(fd, head, 2) == 2 && head[0] == 0x1f && head[1] == 0x8b);
            if (fd >= 0) close_(fd);
            f = gzopen_(gz, "rb");
            CHECK(f != NULL);
            if (f) {
                CHECK(gzgets_(f, line, sizeof(line)) && !strcmp(line, "line one\n"));
                CHECK(gzgets_(f, line, sizeof(line)) && !strcmp(line, "line two\n"));
                CHECK(!gzgets_(f, line, sizeof(line)) && gzeof_(f));
                CHECK(gzrewind_(f) == 0 && gzgets_(f, line, sizeof(line)) && !strcmp(line, "line one\n") && !gzdirect_(f));
                CHECK(gzclose_(f) == 0);
            }
            fd = open_(plain, 01 | 0100 | 01000, 0600);
            CHECK(fd >= 0 && write_(fd, "as it is\n", 9) == 9);
            if (fd >= 0) close_(fd);
            f = gzopen_(plain, "rb");
            CHECK(f && gzgets_(f, line, sizeof(line)) && !strcmp(line, "as it is\n") && gzdirect_(f));
            if (f) gzclose_(f);
        }
    }

    say("j0 and j1, eventfd_read and eventfd_write, fchmodat");
    {
        typedef gdouble (*d_fn)(gdouble);                /* (as guest code calls them: softfp's bits on 32-bit ARM) */
        typedef int (*eventfd_fn)(unsigned, int);
        typedef int (*efd_read_fn)(int, u64 *);
        typedef int (*efd_write_fn)(int, u64);
        typedef int (*fchmodat_fn)(int, const char *, unsigned, int);
        typedef int (*close_fn)(int);
        d_fn j0_ = (d_fn)mg_bionic_find("j0"), j1_ = (d_fn)mg_bionic_find("j1");
        eventfd_fn eventfd_ = (eventfd_fn)mg_bionic_find("eventfd");
        efd_read_fn efd_read = (efd_read_fn)mg_bionic_find("eventfd_read");
        efd_write_fn efd_write = (efd_write_fn)mg_bionic_find("eventfd_write");
        fchmodat_fn fchmodat_ = (fchmodat_fn)mg_bionic_find("fchmodat");
        close_fn close_ = (close_fn)mg_bionic_find("close");
        u64 v = 0;
        int fd;
        CHECK(j0_ && j1_ && eventfd_ && efd_read && efd_write && fchmodat_ && close_);
        if (j0_ && j1_) {
#define J0(x) mg_gd(j0_(mg_gdb(x)))
#define J1(x) mg_gd(j1_(mg_gdb(x)))
            CHECK(fabs(J0(0) - 1) < 1e-15 && fabs(J0(1) - 0.7651976865579666) < 1e-12 && fabs(J0(-3) - J0(3)) < 1e-15);
            CHECK(fabs(J0(10) - -0.2459357644513483) < 1e-10 && fabs(J0(20) - 0.1670246643405831) < 1e-10);
            CHECK(fabs(J1(1) - 0.4400505857449335) < 1e-12 && fabs(J1(-1) + 0.4400505857449335) < 1e-12 && fabs(J1(15) - 0.2051040386135228) < 1e-10);
#undef J0
#undef J1
        }
        if (eventfd_ && efd_read && efd_write && close_) {
            fd = eventfd_(0, 0);
            CHECK(fd >= 0 && efd_write(fd, 5) == 0 && efd_write(fd, 2) == 0 && efd_read(fd, &v) == 0 && v == 7);
            if (fd >= 0) close_(fd);
        }
        if (fchmodat_) CHECK(fchmodat_(-100, "/syscall-files/log.gz", 0644, 0) == 0 && fchmodat_(-100, "/syscall-files/none", 0644, 0) == -1);
    }
}

/*
 * libm's rounding to a whole number, which the core works out on the bits (bionic/mg_libm.c), against the host's libm,
 * whose results IEEE 754 decides (to nearest, the default mode): every exponent of a float and of a double, with the
 * fractions a rounding decides at -- just under, at and just over a half, the largest, an odd and an even whole part --
 * both signs, zeros, denormals, infinities and NaNs, and random bits. The integer ones where the result has room.
 */
static u32 g_round_seed = 12345;
static u32 round_rand(void)
{
    g_round_seed = g_round_seed * 1664525u + 1013904223u;
    return g_round_seed;
}

static int rounding_floats(u32 *out)
{
    int n = 0, e, s, j;
    for (e = 0; e < 256; e++) {
        u32 picks[16];
        int np = 0, k = 150 - e;
        picks[np++] = 0;
        picks[np++] = 1;
        picks[np++] = 0x7FFFFF;
        picks[np++] = 0x400000;
        picks[np++] = 0x3FFFFF;
        picks[np++] = 0x400001;
        if (k >= 1 && k <= 23) {
            /* (k bits of fraction below the units: the half is bit k - 1; the unit's bit k is the last of the whole part.) */
            u32 half = 1u << (k - 1);
            picks[np++] = half;
            picks[np++] = half - 1;
            picks[np++] = half + 1;
            picks[np++] = (half << 1) | half;
            picks[np++] = (half << 1) | (half - 1);
            picks[np++] = (half << 1) - 1;
        }
        picks[np++] = round_rand();
        picks[np++] = round_rand();
        for (s = 0; s < 2; s++)
            for (j = 0; j < np; j++) out[n++] = ((u32)s << 31) | ((u32)e << 23) | (picks[j] & 0x7FFFFFu);
    }
    for (j = 0; j < 1000; j++) out[n++] = round_rand();
    return n;
}

static int rounding_doubles(u64 *out)
{
    int n = 0, e, s, j;
    for (e = 0; e < 2048; e++) {
        u64 picks[16];
        int np = 0, k = 1075 - e;
        picks[np++] = 0;
        picks[np++] = 1;
        picks[np++] = 0xFFFFFFFFFFFFFull;
        picks[np++] = 0x8000000000000ull;
        picks[np++] = 0x7FFFFFFFFFFFFull;
        picks[np++] = 0x8000000000001ull;
        if (k >= 1 && k <= 52) {
            u64 half = 1ull << (k - 1);
            picks[np++] = half;
            picks[np++] = half - 1;
            picks[np++] = half + 1;
            picks[np++] = (half << 1) | half;
            picks[np++] = (half << 1) | (half - 1);
            picks[np++] = (half << 1) - 1;
        }
        picks[np++] = ((u64)round_rand() << 32) | round_rand();
        for (s = 0; s < 2; s++)
            for (j = 0; j < np; j++) out[n++] = ((u64)s << 63) | ((u64)e << 52) | (picks[j] & 0xFFFFFFFFFFFFFull);
    }
    for (j = 0; j < 1000; j++) out[n++] = ((u64)round_rand() << 32) | round_rand();
    return n;
}

static void test_libm_rounding(void)
{
    typedef gfloat (*ff_fn)(gfloat);
    typedef gdouble (*dd_fn)(gdouble);
    typedef glong (*lf_fn)(gfloat);
    typedef glong (*ld_fn)(gdouble);
    typedef i64 (*llf_fn)(gfloat);
    typedef i64 (*lld_fn)(gdouble);
    static const char *const fnames[] = { "floorf", "ceilf", "truncf", "roundf", "rintf", "nearbyintf" };
    static const char *const dnames[] = { "floor", "ceil", "trunc", "round", "rint", "nearbyint" };
    float (*const fref[])(float) = { floorf, ceilf, truncf, roundf, rintf, nearbyintf };
    double (*const dref[])(double) = { floor, ceil, trunc, round, rint, nearbyint };
    u32 *fv = (u32 *)mg_alloc(sizeof(u32) * 20000);
    u64 *dv = (u64 *)mg_alloc(sizeof(u64) * 70000);
    int nf = rounding_floats(fv), nd = rounding_doubles(dv), i, f;
    say("libm's rounding on the bits: %d floats and %d doubles, against the host's", nf, nd);
    for (f = 0; f < 6; f++) {
        ff_fn fn = (ff_fn)mg_bionic_find(fnames[f]);
        dd_fn dn = (dd_fn)mg_bionic_find(dnames[f]);
        int bad = 0;
        CHECK(fn && dn);
        if (!fn || !dn) continue;
        for (i = 0; i < nf; i++) {
            float want = fref[f](mg_f(fv[i])), got = mg_gf(fn(mg_gfb(mg_f(fv[i]))));
            if (isnan(want) ? !isnan(got) : mg_fb(got) != mg_fb(want)) {
                if (!bad++) say("  %s(%08x) = %08x, the host's %08x", fnames[f], (unsigned)fv[i], (unsigned)mg_fb(got), (unsigned)mg_fb(want));
            }
        }
        CHECK_INT(bad, 0);
        bad = 0;
        for (i = 0; i < nd; i++) {
            double want = dref[f](mg_d(dv[i])), got = mg_gd(dn(mg_gdb(mg_d(dv[i]))));
            if (isnan(want) ? !isnan(got) : mg_db(got) != mg_db(want)) {
                if (!bad++)
                    say("  %s(%08x%08x) = %08x%08x, the host's %08x%08x", dnames[f], (unsigned)(dv[i] >> 32), (unsigned)dv[i],
                        (unsigned)(mg_db(got) >> 32), (unsigned)mg_db(got), (unsigned)(mg_db(want) >> 32), (unsigned)mg_db(want));
            }
        }
        CHECK_INT(bad, 0);
    }
    {
        lf_fn lroundf_ = (lf_fn)mg_bionic_find("lroundf"), lrintf_ = (lf_fn)mg_bionic_find("lrintf");
        ld_fn lround_ = (ld_fn)mg_bionic_find("lround"), lrint_ = (ld_fn)mg_bionic_find("lrint");
        llf_fn llroundf_ = (llf_fn)mg_bionic_find("llroundf"), llrintf_ = (llf_fn)mg_bionic_find("llrintf");
        lld_fn llround_ = (lld_fn)mg_bionic_find("llround"), llrint_ = (lld_fn)mg_bionic_find("llrint");
        int bad = 0;
        CHECK(lroundf_ && lrintf_ && lround_ && lrint_ && llroundf_ && llrintf_ && llround_ && llrint_);
        if (lroundf_ && lrintf_ && lround_ && lrint_ && llroundf_ && llrintf_ && llround_ && llrint_) {
            /* (In the range of a 32-bit long, which every guest's long holds, and of a 64-bit one; out of it, C's conversion.) */
            for (i = 0; i < nf; i++) {
                float x = mg_f(fv[i]);
                gfloat g = mg_gfb(x);
                if (!(fabsf(x) < 2147483520.0f)) continue;
                if (lroundf_(g) != (glong)lroundf(x) || lrintf_(g) != (glong)lrintf(x) || llroundf_(g) != (i64)llroundf(x) || llrintf_(g) != (i64)llrintf(x)) {
                    if (!bad++)
                        say("  lroundf, lrintf, llroundf, llrintf(%08x) = %ld %ld %lld %lld, the host's %ld %ld %lld %lld", (unsigned)fv[i], (long)lroundf_(g),
                            (long)lrintf_(g), (long long)llroundf_(g), (long long)llrintf_(g), (long)lroundf(x), (long)lrintf(x), (long long)llroundf(x),
                            (long long)llrintf(x));
                }
            }
            for (i = 0; i < nd; i++) {
                double x = mg_d(dv[i]);
                gdouble g = mg_gdb(x);
                if (!(fabs(x) < 2147483647.0)) continue;
                if (lround_(g) != (glong)lround(x) || lrint_(g) != (glong)lrint(x) || llround_(g) != (i64)llround(x) || llrint_(g) != (i64)llrint(x)) {
                    if (!bad++)
                        say("  lround, lrint, llround, llrint(%08x%08x) = %ld %ld %lld %lld, the host's %ld %ld %lld %lld", (unsigned)(dv[i] >> 32), (unsigned)dv[i],
                            (long)lround_(g), (long)lrint_(g), (long long)llround_(g), (long long)llrint_(g), (long)lround(x), (long)lrint(x),
                            (long long)llround(x), (long long)llrint(x));
                }
            }
            for (i = 0; i < nf; i++) {
                float x = mg_f(fv[i]);
                if (!(fabsf(x) < 9.2e18f)) continue;
                if (llroundf_(mg_gfb(x)) != (i64)llroundf(x) || llrintf_(mg_gfb(x)) != (i64)llrintf(x)) {
                    if (!bad++) say("  llroundf, llrintf(%08x) wrong", (unsigned)fv[i]);
                }
            }
            for (i = 0; i < nd; i++) {
                double x = mg_d(dv[i]);
                if (!(fabs(x) < 9.2e18)) continue;
                if (llround_(mg_gdb(x)) != (i64)llround(x) || llrint_(mg_gdb(x)) != (i64)llrint(x)) {
                    if (!bad++) say("  llround, llrint(%08x%08x) wrong", (unsigned)(dv[i] >> 32), (unsigned)dv[i]);
                }
            }
            CHECK_INT(bad, 0);
            /* A few by name: halves, the sign of a zero, a whole number of 2^23 and more. */
            CHECK_INT(lroundf_(mg_gfb(-2.5f)), -3);
            CHECK_INT(lrintf_(mg_gfb(-2.5f)), -2);
            CHECK_INT(lrintf_(mg_gfb(3.5f)), 4);
            CHECK_INT(lroundf_(mg_gfb(0.49999997f)), 0);
            CHECK_INT(lroundf_(mg_gfb(8388609.0f)), 8388609);
            CHECK_INT(lroundf_(mg_gfb(-1073741824.0f)), -1073741824);
            CHECK(llroundf_(mg_gfb(1099511627776.0f)) == 1099511627776ll);
            CHECK(llround_(mg_gdb(-4503599627370497.0)) == -4503599627370497ll);
            CHECK_INT(lround_(mg_gdb(-2147483648.0)), (glong)-2147483647 - 1);
        }
        {
            ff_fn roundf_ = (ff_fn)mg_bionic_find("roundf"), ceilf_ = (ff_fn)mg_bionic_find("ceilf"), floorf_ = (ff_fn)mg_bionic_find("floorf");
            if (roundf_ && ceilf_ && floorf_) {
                CHECK(mg_fb(mg_gf(roundf_(mg_gfb(-0.25f)))) == 0x80000000u);
                CHECK(mg_fb(mg_gf(ceilf_(mg_gfb(-0.5f)))) == 0x80000000u);
                CHECK(mg_gf(floorf_(mg_gfb(-0.5f))) == -1.0f);
                CHECK(mg_gf(roundf_(mg_gfb(-0.5f))) == -1.0f);
                CHECK(isnan(mg_gf(roundf_(mg_gfb(mg_f(0x7FA00000u))))) && (mg_fb(mg_gf(roundf_(mg_gfb(mg_f(0x7FA00000u))))) & 0x00400000u));
            }
        }
    }
    free(fv);
    free(dv);
}

/*
 * Anonymous memory given back with madvise (MADV_DONTNEED, bionic/mg_libc_mem.c) is the system's again -- decommitted, where
 * the host says what is -- and still the app's: a page of it touched is zero, read or written; one the system itself writes
 * into (a file read into it) is read into; made no-access and given access again it is zero; what was not given back keeps
 * what it had; an address off a page is refused, as Linux refuses it.
 */
static void test_mmap_madvise(const char *scratch)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, goff);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*mprotect_fn)(void *, size_t, int);
    typedef int (*madvise_fn)(void *, size_t, int);
    typedef gssize (*read_fn)(int, void *, size_t);
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
#if MG_GUEST_LP64
    /*
     * A mapping of 4 GB or more, with no access, as Chromium's PartitionAlloc reserves its 16 GB pools on a 64-bit
     * processor: a page given access far into it is written, given back, and written again -- committed again, zero.
     * Its size was once taken as its lower 32 bits (MG_ALIGN_UP), 0, and the page given back was none of the app's.
     */
    {
        size_t pool = (size_t)16 << 30, at = ((size_t)8 << 30) + 0x358000;
        u8 *q = (u8 *)mmap_(NULL, pool, NONE, PRIVATE | ANONYMOUS, -1, 0);
        CHECK(q && q != (u8 *)(mg_addr)-1);
        if (q && q != (u8 *)(mg_addr)-1) {
            CHECK(mprotect_(q + at, 16384, RW) == 0);
            q[at + 0x800] = 7;
            CHECK(madvise_(q + at, 16384, DONTNEED) == 0);
            q[at + 0x800] = 9;
            CHECK(q[at + 0x800] == 9 && q[at + 0x801] == 0);
            CHECK(munmap_(q, pool) == 0);
        }
        say("  16 GB with no access, a page 8 GB in given access, given back and written again: %s",
            q && q != (u8 *)(mg_addr)-1 ? "done" : "NOT MAPPED");
    }
#endif
done:
    free(data);
    free(path);
}

static MG_HOSTCB void count_entry(const char *name, bool is_dir, void *user)
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
 * A folder renamed with a file in it open and mapped, as Google Play services stages a module: the module's APK is written
 * into a folder of its own, opened (the app's AssetManager keeps it), and the folder renamed to the module's name; the same
 * again when a folder is put aside to be removed. Windows refuses to rename a folder with an open file in it, whatever the
 * open shared; the host moves it an entry at a time then (mg_host_win32.c, move_dir_in_use). The file is found under the new
 * name, the open one still reads, a folder inside goes along, and the old name is free to be made again.
 */
static void test_rename_dir_in_use(const char *scratch)
{
    char *top = mg_sprintf("%s/app_chimera", scratch), *stage = mg_sprintf("%s/s", top), *apk = mg_sprintf("%s/AdsDynamite.apk", stage);
    char *inner = mg_sprintf("%s/oat", stage), *odex = mg_sprintf("%s/oat/arm.odex", stage);
    char *final_dir = mg_sprintf("%s/9", top), *final_apk = mg_sprintf("%s/9/AdsDynamite.apk", top), *final_odex = mg_sprintf("%s/9/oat/arm.odex", top);
    char *condemned = mg_sprintf("%s/c-9-1793639062482", top), *condemned_apk = mg_sprintf("%s/c-9-1793639062482/AdsDynamite.apk", top);
    mg_file *open_apk, *open_odex;
    void *base = NULL;
    size_t mapped = 0;
    const u8 *view;
    mg_stat st;
    int err = 0;
    say("rename: a folder with a file in it open and mapped, and a folder in it with another open");
    mg_path_mkdirs(inner);
    write_whole(apk, "module apk");
    write_whole(odex, "odex");
    open_apk = mg_file_open(apk, MG_O_READ, &err);
    view = open_apk ? (const u8 *)mg_file_map(open_apk, 0, 10, &base, &mapped) : NULL;
    open_odex = mg_file_open(odex, MG_O_READ, &err);
    CHECK(open_apk && view && open_odex);
    CHECK(mg_path_rename(stage, final_dir, &err));
    CHECK(mg_path_stat(final_apk, &st, &err) && st.size == 10 && mg_path_stat(final_odex, &st, &err) && st.size == 4);
    CHECK(!mg_path_stat(stage, &st, &err) && err == MG_E_NOENT);
    CHECK(reads_as(open_apk, "module apk") && view && !memcmp(view, "module apk", 10) && reads_as(open_odex, "odex"));
    CHECK(mg_path_mkdir(stage, &err) && mg_path_rmdir(stage, &err));
    /* Put aside to be removed, still open; and over a folder that is there and empty, as POSIX's rename replaces one. */
    CHECK(mg_path_rename(final_dir, condemned, &err) && mg_path_stat(condemned_apk, &st, &err) && st.size == 10);
    CHECK(mg_path_mkdir(final_dir, &err) && mg_path_rename(condemned, final_dir, &err) && mg_path_stat(final_apk, &st, &err));
    /* Not over a folder with something in it. */
    CHECK(mg_path_mkdirs(condemned) && mg_path_mkdir(condemned_apk, &err));
    CHECK(!mg_path_rename(final_dir, condemned, &err) && mg_path_stat(final_apk, &st, &err) && mg_path_stat(final_odex, &st, &err));
    CHECK(mg_path_rmdir(condemned_apk, &err) && mg_path_rmdir(condemned, &err));
    if (view) mg_section_unmap(base, mapped);
    if (open_apk) mg_file_close(open_apk);
    if (open_odex) mg_file_close(open_odex);
    CHECK(mg_path_remove(final_odex, &err) && mg_path_remove(final_apk, &err));
    {
        char *final_inner = mg_sprintf("%s/9/oat", top);
        CHECK(mg_path_rmdir(final_inner, &err) && mg_path_rmdir(final_dir, &err) && mg_path_rmdir(top, &err));
        free(final_inner);
    }
    free(condemned_apk);
    free(condemned);
    free(final_odex);
    free(final_apk);
    free(final_dir);
    free(odex);
    free(inner);
    free(apk);
    free(stage);
    free(top);
}

/*
 * Shared memory holds no address space of its own (bionic/mg_libc_io.c): a region made, written and read through its
 * descriptor, and grown past its section, has no view until the app maps it; mapped, it is one view of its size; unmapped,
 * none again. The views are counted by the host (mg_views_kb).
 */
static void test_shm_views(void)
{
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, goff);
    typedef int (*munmap_fn)(void *, size_t);
    typedef int (*close_fn)(int);
    typedef gssize (*pread_fn)(int, void *, size_t, goff);
    typedef gssize (*pwrite_fn)(int, const void *, size_t, goff);
    typedef int (*ftruncate_fn)(int, goff);
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

/* The census's owner of the test's 3 MB allocation (test_free_stretches): told apart by its name's pointer. */
static const void *g_census_big;
static const char k_census_big[] = "the test's 3 MB";
static MG_HOSTCB const char *census_whose(const void *base, size_t size)
{
    (void)size;
    return base && base == g_census_big ? k_census_big : NULL;
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
        /*
         * What holds the addresses, by kind: the program's image, and private memory, a 3 MB allocation made here among it,
         * named by the owner the census is told (the core's, mg_java.c's address_whose, names the Java heap's arenas, a
         * library's mappings, Go's runtime's).
         */
        mg_host_address_kinds k, after;
        void *big = mg_mem_reserve(3u << 20);
        u8 *small = (u8 *)malloc(8u << 20);
        int i;
        bool seen = false, named = false;
        g_census_big = big;
        CHECK(big && mg_mem_commit(big, 3u << 20, MG_PROT_RW) && small);
        if (small) memset(small, 1, 8u << 20);
        CHECK(mg_host_address_census(&k, census_whose));
        for (i = 0; i < k.nsizes; i++)
            if (k.sizes[i].kb == 3072 && k.sizes[i].count >= 1) {
                int o;
                seen = true;
                for (o = 0; o < MG_ADDRESS_OWNERS; o++) named = named || (k.sizes[i].by[o].whose == k_census_big && k.sizes[i].by[o].count == 1);
            }
        say("addresses by kind: images %u MB, views %u MB, private %u MB committed and %u reserved; threads the core started %u",
            (unsigned)k.images_mb, (unsigned)k.views_mb, (unsigned)k.private_mb, (unsigned)k.private_reserved_mb, (unsigned)mg_thread_live_count());
        CHECK(k.images_mb > 0 && k.private_mb >= 3 && k.nsizes > 0);
        /* (A 3 MB allocation is among the four sizes holding the most only where little else is; it is said, not checked.) */
        if (!seen) say("  (the 3 MB allocation is not among the four sizes that hold the most here)");
        else CHECK(named);
        free(small);
        if (big) mg_mem_release(big, 3u << 20);
        g_census_big = NULL;
        CHECK(mg_host_address_census(&after, NULL) && after.private_mb + 3 <= k.private_mb + 1);
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
        "      </Extensions>\r\n    </Application></Applications>\r\n"
        "  <Capabilities>\r\n    <!-- The network: <Capability Name=\"webcam\" /> in a comment is no capability. -->\r\n"
        "    <Capability Name=\"internetClient\" />\r\n    <Capability Name=\"internetClientServer\" />\r\n"
        "    <Capability Name=\"privateNetworkClientServer\" />\r\n    <!-- The libraries. -->\r\n"
        "    <uap:Capability Name=\"picturesLibrary\" />\r\n    <uap:Capability Name=\"videosLibrary\" />\r\n"
        "    <uap:Capability Name=\"musicLibrary\" />\r\n    <uap:Capability Name=\"documentsLibrary\" />\r\n"
        "    <uap:Capability Name=\"removableStorage\" />\r\n    <uap:Capability Name=\"chat\" />\r\n"
        "    <uap2:Capability Name=\"phoneCallHistoryPublic\" />\r\n    <uap3:Capability Name=\"backgroundMediaPlayback\" />\r\n"
        "    <rescap:Capability Name=\"codeGeneration\" />\r\n    <rescap:Capability Name=\"packageManagement\" />\r\n"
        "    <rescap:Capability Name=\"packageQuery\" />\r\n    <!-- The devices. -->\r\n    <DeviceCapability Name=\"webcam\" />\r\n"
        "    <DeviceCapability Name=\"location\" />\r\n    <DeviceCapability Name=\"microphone\" />\r\n"
        "    <DeviceCapability Name=\"bluetooth\" />\r\n    <DeviceCapability Name=\"wiFiControl\" />\r\n"
        "    <DeviceCapability Name=\"bluetooth.rfcomm\">\r\n      <Device Id=\"any\"><Function Type=\"name:serialPort\" /></Device>\r\n"
        "    </DeviceCapability>\r\n  </Capabilities>\r\n"
        "  <Extensions><Extension Category=\"windows.publisherCacheFolders\" /></Extensions></Package>\r\n";
    static const char *const permissions[] = { "android.permission.INTERNET", "android.permission.CAMERA", "com.example.permission.C2D_MESSAGE",
                                               "android.permission.READ_MEDIA_AUDIO", "android.permission.VIBRATE" };
    static const char *const storage[] = { "android.permission.WRITE_EXTERNAL_STORAGE" };
    static const char *const phone[] = { "android.permission.READ_PHONE_STATE", "android.permission.RECEIVE_SMS" };
    static const char *const devices[] = { "android.permission.ACCESS_COARSE_LOCATION", "android.permission.RECORD_AUDIO", "android.permission.READ_CALL_LOG",
                                           "android.permission.READ_MEDIA_VISUAL_USER_SELECTED" };
    static const char *const bluetooth[] = { "android.permission.BLUETOOTH_SCAN", "android.permission.BLUETOOTH_CONNECT", "android.permission.INTERNET" };
    static const char *const bluetooth_old[] = { "android.permission.BLUETOOTH", "android.permission.BLUETOOTH_ADMIN" };
    /* Transit's: the location, no Wi-Fi permission of its own (Google Play services in the app scans for it); and the Wi-Fi's
       alone, without the location, and the background location alone, which give no scans. */
    static const char *const transit[] = { "android.permission.ACCESS_FINE_LOCATION", "android.permission.ACCESS_COARSE_LOCATION",
                                           "android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE" };
    static const char *const wifi_alone[] = { "android.permission.ACCESS_WIFI_STATE", "android.permission.CHANGE_WIFI_STATE",
                                              "android.permission.ACCESS_BACKGROUND_LOCATION" };
    static const char *const nearby_wifi[] = { "android.permission.NEARBY_WIFI_DEVICES" };
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
    m = mg_appx_app_manifest(megler, "com.instagram.android", "Insta & <gram> \"x\"", NULL, NULL, -1, err, sizeof(err));
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
    m2 = mg_appx_app_manifest(megler, "com.instagram.android", "Instagram", "transparent", "#102030", -1, err, sizeof(err));
    CHECK(m2 && !strncmp(strstr(m, "PhoneProductId=\""), strstr(m2, "PhoneProductId=\""), 54));
    /* The colours given, the tile's and the splash screen's; none given, Megler's. */
    CHECK(m2 && strstr(m2, "BackgroundColor=\"transparent\">") != NULL && strstr(m2, "Image=\"s.png\" BackgroundColor=\"#102030\"") != NULL);
    CHECK(strstr(m, "BackgroundColor=\"#000000\">") != NULL && strstr(m, "Image=\"s.png\" BackgroundColor=\"#000000\"") != NULL);
    free(m2);
    m2 = mg_appx_app_manifest(megler, "com.whatsapp", "WhatsApp", NULL, NULL, -1, err, sizeof(err));
    CHECK(m2 && strncmp(strstr(m, "PhoneProductId=\""), strstr(m2, "PhoneProductId=\""), 54));
    free(m2);
    /* A short name of forty characters at most, a sequence of UTF-8 never cut. */
    m2 = mg_appx_app_manifest(megler, "ru.example", "\xD0\x9E\xD1\x87\xD0\xB5\xD0\xBD\xD1\x8C \xD0\xB4\xD0\xBB\xD0\xB8\xD0\xBD\xD0\xBD\xD0\xBE\xD0\xB5 "
                                                  "\xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB2\xD0\xB0\xD0\xBD\xD0\xB8\xD0\xB5 \xD0\xBF\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBE\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F "
                                                  "\xD0\xB4\xD0\xBB\xD1\x8F \xD0\xBF\xD1\x80\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x80\xD0\xBA\xD0\xB8 \xF0\x9F\x98\x80\xF0\x9F\x98\x80",
                              NULL, NULL, -1, err, sizeof(err));
    if (m2) {
        const char *s = strstr(m2, "ShortName=\"") + 11, *e = strchr(s, '"');
        size_t chars = 0;
        for (; s < e; s++) chars += ((unsigned char)*s & 0xC0) != 0x80;
        CHECK_INT((int)chars, 40);
    }
    CHECK(m2 != NULL);
    free(m2);
    CHECK(mg_appx_app_manifest("<Package/>", "com.example", "x", NULL, NULL, -1, err, sizeof(err)) == NULL && strstr(err, "Identity") != NULL);
    /* Megler's association of APKs is not the app's; its megler: links are. */
    CHECK(strstr(m, "windows.fileTypeAssociation") == NULL && strstr(m, "<uap:FileTypeAssociation") == NULL);
    CHECK(strstr(m, "<uap:Extension Category=\"windows.protocol\">\r\n          <uap:Protocol Name=\"megler\" />") != NULL);
    CHECK(strstr(m, "      </uap:VisualElements>\r\n      <Extensions>\r\n        <uap:Extension Category=\"windows.protocol\">") != NULL);
    /* Asked for none of its own (-1): every capability of Megler's, its comments too. */
    CHECK(strstr(m, "<!-- The devices. -->\r\n    <DeviceCapability Name=\"webcam\" />") != NULL && strstr(m, "Name=\"packageManagement\"") != NULL &&
          strstr(m, "Name=\"chat\"") != NULL && strstr(m, "Name=\"documentsLibrary\"") != NULL);
    free(m);

    /*
     * What an app's package declares, from the permissions its manifest asks for: each permission's capabilities, the pictures
     * library in every one (Megler's diagnosis folder), a permission Windows has nothing for adding nothing.
     */
    CHECK_INT((int)mg_appx_capabilities(NULL, 0, false), MG_APPX_CAP_PICTURES);
    CHECK_INT((int)mg_appx_capabilities(permissions, (int)MG_COUNT(permissions), false),
              MG_APPX_CAP_NETWORK | MG_APPX_CAP_PICTURES | MG_APPX_CAP_WEBCAM | MG_APPX_CAP_MUSIC);
    CHECK_INT((int)mg_appx_capabilities(storage, 1, false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_MUSIC);
    CHECK_INT((int)mg_appx_capabilities(phone, 2, false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_CALL_HISTORY | MG_APPX_CAP_CHAT);
    CHECK_INT((int)mg_appx_capabilities(phone, 1, false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_CALL_HISTORY);
    /* An app that hears its code through Google Play services' SMS Retriever has the phone's messages heard for it. */
    CHECK_INT((int)mg_appx_capabilities(NULL, 0, true), MG_APPX_CAP_PICTURES | MG_APPX_CAP_CHAT);
    CHECK_INT((int)mg_appx_capabilities(devices, (int)MG_COUNT(devices), false),
              MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_LOCATION | MG_APPX_CAP_WIFI | MG_APPX_CAP_MICROPHONE | MG_APPX_CAP_CALL_HISTORY);
    /* The Wi-Fi's networks with the location, which Android gives them to, or with Android 13's NEARBY_WIFI_DEVICES; not with the
       Wi-Fi's own permissions alone, nor with the background location alone. */
    CHECK_INT((int)mg_appx_capabilities(transit, (int)MG_COUNT(transit), false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_NETWORK | MG_APPX_CAP_LOCATION | MG_APPX_CAP_WIFI);
    CHECK_INT((int)mg_appx_capabilities(wifi_alone, (int)MG_COUNT(wifi_alone), false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_LOCATION);
    CHECK_INT((int)mg_appx_capabilities(nearby_wifi, (int)MG_COUNT(nearby_wifi), false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_WIFI);
    /* Bluetooth's: Android 12's permissions of nearby devices, and the Androids' before it. */
    CHECK_INT((int)mg_appx_capabilities(bluetooth, (int)MG_COUNT(bluetooth), false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_NETWORK | MG_APPX_CAP_BLUETOOTH);
    CHECK_INT((int)mg_appx_capabilities(bluetooth_old, (int)MG_COUNT(bluetooth_old), false), MG_APPX_CAP_PICTURES | MG_APPX_CAP_BLUETOOTH);
    m = mg_appx_app_manifest(megler, "com.example.camera", "Camera", NULL, NULL, (int)mg_appx_capabilities(permissions, (int)MG_COUNT(permissions), false),
                             err, sizeof(err));
    CHECK(m != NULL);
    if (!m) return;
    /* Those of its permissions and Megler's own every package needs, in Megler's order, on lines of their own; no comments. */
    CHECK(strstr(m, "  <Capabilities>\r\n    <Capability Name=\"internetClient\" />\r\n    <Capability Name=\"internetClientServer\" />\r\n"
                    "    <Capability Name=\"privateNetworkClientServer\" />\r\n    <uap:Capability Name=\"picturesLibrary\" />\r\n"
                    "    <uap:Capability Name=\"musicLibrary\" />\r\n    <uap3:Capability Name=\"backgroundMediaPlayback\" />\r\n"
                    "    <rescap:Capability Name=\"codeGeneration\" />\r\n    <rescap:Capability Name=\"packageQuery\" />\r\n"
                    "    <DeviceCapability Name=\"webcam\" />\r\n"
                    "    <DeviceCapability Name=\"bluetooth.rfcomm\">\r\n      <Device Id=\"any\"><Function Type=\"name:serialPort\" /></Device>\r\n"
                    "    </DeviceCapability>\r\n  </Capabilities>\r\n  <Extensions><Extension Category=\"windows.publisherCacheFolders\" />") != NULL);
    /* Nothing it did not ask for, and none of Megler's that no app's package has. */
    CHECK(!strstr(m, "videosLibrary") && !strstr(m, "documentsLibrary") && !strstr(m, "removableStorage") && !strstr(m, "\"chat\"") &&
          !strstr(m, "phoneCallHistoryPublic") && !strstr(m, "packageManagement") && !strstr(m, "\"location\"") && !strstr(m, "\"microphone\"") &&
          !strstr(m, "Name=\"bluetooth\"") && !strstr(m, "wiFiControl") &&
          !strstr(m, "The network:") && !strstr(m, "The libraries.") && !strstr(m, "The devices."));
    CHECK(strstr(m, "Name=\"MeglerApp.com.example.camera.") != NULL && strstr(m, "<DisplayName>Camera</DisplayName>") != NULL);
    free(m);
    /* An app of Bluetooth LE (MeshCore's): the bluetooth device capability, in Megler's place for it. */
    m = mg_appx_app_manifest(megler, "com.meshcore.mcoadvanced", "MCO A", NULL, NULL, (int)mg_appx_capabilities(bluetooth, (int)MG_COUNT(bluetooth), false), err,
                             sizeof(err));
    CHECK(m && strstr(m, "    <rescap:Capability Name=\"packageQuery\" />\r\n    <DeviceCapability Name=\"bluetooth\" />\r\n"
                         "    <DeviceCapability Name=\"bluetooth.rfcomm\">") != NULL && !strstr(m, "DeviceCapability Name=\"webcam\""));
    free(m);
    /* An app that asks Google Play services for the location (Transit's): the location, and wiFiControl in Megler's place for it. */
    m = mg_appx_app_manifest(megler, "com.thetransitapp.droid", "Transit", NULL, NULL, (int)mg_appx_capabilities(transit, (int)MG_COUNT(transit), false), err,
                             sizeof(err));
    CHECK(m && strstr(m, "    <rescap:Capability Name=\"packageQuery\" />\r\n    <DeviceCapability Name=\"location\" />\r\n"
                         "    <DeviceCapability Name=\"wiFiControl\" />\r\n    <DeviceCapability Name=\"bluetooth.rfcomm\">") != NULL &&
          !strstr(m, "Name=\"bluetooth\""));
    free(m);
    /* An app that asks for nothing Windows guards: the pictures library and Megler's own alone, no network. */
    m = mg_appx_app_manifest(megler, "com.example.calculator", "Calculator", NULL, NULL, (int)mg_appx_capabilities(NULL, 0, false), err, sizeof(err));
    CHECK(m && strstr(m, "  <Capabilities>\r\n    <uap:Capability Name=\"picturesLibrary\" />\r\n    <uap3:Capability Name=\"backgroundMediaPlayback\" />\r\n") &&
          !strstr(m, "internetClient") && !strstr(m, "DeviceCapability Name=\"webcam\""));
    free(m);
    /* A block that does not end is no manifest to make one from. */
    CHECK(mg_appx_app_manifest("<Identity Name=\"M\" /><mp:PhoneIdentity PhoneProductId=\"x\" /><DisplayName>M</DisplayName>"
                               "<uap:VisualElements DisplayName=\"M\"><uap:DefaultTile ShortName=\"M\"/><Capabilities><Capability Name=\"x\" />",
                               "com.example", "x", NULL, NULL, 0, err, sizeof(err)) == NULL && strstr(err, "capabilities") != NULL);
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
    say("libc's page size as data, setpgrp, ptrace, the ids' setters, res_search, and malloc's alignment");
    CHECK(size && *size == 4096 && shift && *shift == 12);
    CHECK(mg_bionic_find("setpgrp") != NULL && mg_bionic_find("ptrace") != NULL);
    /* What Go's syscall and net packages link (WhatsApp's libgojni.so): there, and a DNS query by res_search refused. */
    CHECK(mg_bionic_find("setresuid") && mg_bionic_find("setresgid") && mg_bionic_find("setgroups") && mg_bionic_find("res_search"));
    if (mg_bionic_find("res_search")) {
        u8 answer[64];
        CHECK(MG_GFN(int (*)(const char *, int, int, u8 *, int), (mg_addr)mg_bionic_find("res_search"))("example.com", 1, 16, answer, 64) == -1);
    }
    /* getrlimit64's limits are 64-bit: the limit on descriptors, and an unlimited one (RLIMIT_AS) as RLIM64_INFINITY. */
    {
        typedef int (*rlimit64_fn)(int, u64 *);
        rlimit64_fn get64 = (rlimit64_fn)mg_bionic_find("getrlimit64");
        u64 lim[2] = { 7, 7 };
        CHECK(get64 && get64(7, lim) == 0 && lim[0] == 1024 && lim[1] == 1024);
        CHECK(get64 && get64(9, lim) == 0 && lim[0] == ~(u64)0 && lim[1] == ~(u64)0);
    }
    /*
     * getauxval: what every Linux process's vector has -- AT_EXECFN, the path it was started from, an app's its zygote's (it was
     * zero: a string read through null), the clock's ticks, the app's ids -- and zero with ENOENT for what it has not. And the
     * process's executable through realpath is what readlink says it links to.
     */
    {
        typedef unsigned long (*auxval_fn)(unsigned long);
        typedef char *(*realpath_fn)(const char *, char *);
        auxval_fn aux = (auxval_fn)mg_bionic_find("getauxval");
        realpath_fn real = (realpath_fn)mg_bionic_find("realpath");
        const char *exe = MG_GUEST_LP64 ? "/system/bin/app_process64" : "/system/bin/app_process32";
        static char path[4096];
        CHECK(aux && aux(31) != 0 && aux(17) == 100 && aux(11) == 10100 && aux(14) == 10100);
        /* (A string's address fits an unsigned long where the guest's ABI is the host's: not in MSVC's x64 build.) */
        if (aux && sizeof(unsigned long) == sizeof(void *)) CHECK(!strcmp((const char *)(mg_addr)aux(31), exe));
        *mg_errno_ptr() = 0;
        CHECK(aux && aux(9999) == 0 && *mg_errno_ptr() == GE_NOENT);
        CHECK(real && real("/proc/self/exe", path) == path && !strcmp(path, exe));
    }
    /* The environment Android's init gives every process, as an app has it: its storage's root and the rest beside the data and the root. */
    {
        typedef char *(*getenv_fn)(const char *);
        getenv_fn env = (getenv_fn)mg_bionic_find("getenv");
        const char *storage = env ? env("ANDROID_STORAGE") : NULL, *assets = env ? env("ANDROID_ASSETS") : NULL, *data = env ? env("ANDROID_DATA") : NULL;
        CHECK(storage && !strcmp(storage, "/storage") && assets && !strcmp(assets, "/system/app") && data && !strcmp(data, "/data"));
        CHECK(env && env("ASEC_MOUNTPOINT") && env("ANDROID_BOOTLOGO") && env("ANDROID_ROOT") && env("EXTERNAL_STORAGE") && !env("BOOTCLASSPATH"));
    }
    /*
     * malloc's blocks at bionic's alignment, 16 bytes but on 32-bit ARM 8 (x86-64's SSE stores fault on less: exteraGram's
     * libtmessages), through calloc and realloc too, the contents kept; memalign's at theirs. And of jemalloc's sizes, a
     * multiple of that and at least one, all of it the block's to write: malloc(0) too (MaxVibe's libqrcode.so writes a byte
     * into one, and Windows' heap was corrupted on x64 when that byte was past the host's block).
     */
    {
        typedef void *(*malloc_fn)(size_t);
        typedef void *(*calloc_fn)(size_t, size_t);
        typedef void *(*realloc_fn)(void *, size_t);
        typedef void *(*memalign_fn)(size_t, size_t);
        typedef void (*free_fn)(void *);
        typedef size_t (*usable_fn)(const void *);
        malloc_fn gmalloc = (malloc_fn)mg_bionic_find("malloc");
        calloc_fn gcalloc = (calloc_fn)mg_bionic_find("calloc");
        realloc_fn grealloc = (realloc_fn)mg_bionic_find("realloc");
        memalign_fn gmemalign = (memalign_fn)mg_bionic_find("memalign");
        free_fn gfree = (free_fn)mg_bionic_find("free");
        usable_fn gusable = (usable_fn)mg_bionic_find("malloc_usable_size");
        mg_addr align = MG_ARCH_ARM32 ? 8 : 16, odd = 0;
        size_t sizes[6] = { 1, 8, 24, 40, 4000, 100000 };
        int i;
        CHECK(gmalloc && gcalloc && grealloc && gmemalign && gfree && gusable);
        if (gmalloc && gcalloc && grealloc && gmemalign && gfree && gusable) {
            for (i = 0; i < 6; i++) {
                u8 *m = (u8 *)gmalloc(sizes[i]), *c = (u8 *)gcalloc(sizes[i], 1), *a = (u8 *)gmemalign(64, sizes[i]);
                odd |= ((mg_addr)m | (mg_addr)c) & (align - 1);
                odd |= (mg_addr)a & 63;
                CHECK(m && c && a && gusable(m) == MG_ALIGN_UP(sizes[i], (size_t)align) && c[sizes[i] - 1] == 0);
                if (m) {
                    memset(m, 0x5A, gusable(m));
                    m = (u8 *)grealloc(m, sizes[i] * 3);
                    CHECK(m && m[0] == 0x5A && m[sizes[i] - 1] == 0x5A);
                    odd |= (mg_addr)m & (align - 1);
                }
                if (a) {
                    a[0] = 7;
                    a = (u8 *)grealloc(a, sizes[i] + 100);
                    CHECK(a && a[0] == 7);
                    odd |= (mg_addr)a & (align - 1);
                }
                gfree(m);
                gfree(c);
                gfree(a);
            }
            CHECK(odd == 0);
            {
                /* memalign's big block at 64 whatever smaller alignment it asks for, as malloc's of its size is. */
                u8 *a16 = (u8 *)gmemalign(16, 300), *a32 = (u8 *)gmemalign(32, 4000), *a8 = (u8 *)gmemalign(8, 256);
                CHECK(a16 && a32 && a8 && ((mg_addr)a16 & 63) == 0 && ((mg_addr)a32 & 63) == 0 && ((mg_addr)a8 & 63) == 0);
                CHECK(a16 && gusable(a16) == MG_ALIGN_UP(300, (size_t)align) && a32 && gusable(a32) == 4000);
                gfree(a16);
                gfree(a32);
                gfree(a8);
            }
            {
                /* A big block (256 bytes or more) at 64, as jemalloc's runs and pages leave it -- Geekbench 5's zlib loads
                   its 64 KB hash table with an aligned NEON load -- through calloc, and through realloc's every turn: small
                   to big, bigger, smaller, small again, its contents kept each time. */
                size_t steps[5] = { 40, 300, 65536, 1000, 24 };
                u8 *b = (u8 *)gmalloc(65536), *z = (u8 *)gcalloc(65536, 1), *r = NULL;
                size_t was = 0, k;
                bool kept = true, zeroed = true;
                CHECK(b && z && ((mg_addr)b & 63) == 0 && ((mg_addr)z & 63) == 0);
                for (k = 0; z && k < 65536; k++) zeroed &= z[k] == 0;
                CHECK(zeroed);
                for (i = 0; i < 5; i++) {
                    size_t keep = was < steps[i] ? was : steps[i];
                    r = (u8 *)grealloc(r, steps[i]);
                    CHECK(r != NULL);
                    if (!r) break;
                    for (k = 0; k < keep; k++) kept &= r[k] == (u8)(k * 7 + 1);
                    for (k = 0; k < steps[i]; k++) r[k] = (u8)(k * 7 + 1);
                    if (steps[i] >= 256) CHECK(((mg_addr)r & 63) == 0);
                    else CHECK(((mg_addr)r & (align - 1)) == 0);
                    was = steps[i];
                }
                CHECK(kept);
                gfree(b);
                gfree(z);
                gfree(r);
            }
            for (i = 0; i < 64; i++) {
                u8 *z = (u8 *)gmalloc(0);
                CHECK(z && gusable(z) == (size_t)align);
                if (z) memset(z, 0, gusable(z));
                gfree(z);
            }
        }
    }
}

/* ------------------------------------------------------------------ malloc across threads
 *
 * How many malloc-and-free pairs threads make together, the guest's malloc's and the host's own under it: Geekbench 5's
 * multi-core workloads, threads of C++ that allocate and free all the time, scaled worse on the HP Elite x3 than Android
 * did on the same phone (690).
 */
typedef void *(*bench_malloc_fn)(size_t);
typedef void (*bench_free_fn)(void *);

typedef struct malloc_racer {
    bench_malloc_fn m;
    bench_free_fn f;
    int pattern;                       /* MALLOC_MIXED, MALLOC_SMALL or MALLOC_PAIRS */
    volatile i32 *state;               /* 0 wait, 1 run, 2 stop */
    u32 seed;
    u64 pairs;
} malloc_racer;

enum { MALLOC_MIXED, MALLOC_SMALL, MALLOC_PAIRS };
#define RACER_SLOTS 1024

static u32 racer_next(u32 *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* Sizes as a C++ program's: most of a few dozen bytes, some of a few kilobytes, a few of tens of kilobytes (none `small`). */
static size_t racer_size(u32 *s, bool small)
{
    u32 r = racer_next(s) % 100;
    if (r < 80) return 8 + racer_next(s) % 248;
    if (r < 95 || small) return 256 + racer_next(s) % 3840;
    return 4096 + racer_next(s) % 61440;
}

/*
 * MALLOC_MIXED: a working set of RACER_SLOTS blocks of those sizes, one replaced at random each turn; MALLOC_SMALL: the same
 * of 4 KB and less; MALLOC_PAIRS: two small blocks made and freed at once, last first, as a function's temporaries are.
 */
static MG_HOSTCB void malloc_racer_main(void *arg)
{
    malloc_racer *r = (malloc_racer *)arg;
    void **slot = (void **)calloc(RACER_SLOTS, sizeof(void *));
    u32 s = r->seed, i;
    u64 n = 0;
    while (*r->state == 0) mg_thread_yield();
    while (slot && *r->state == 1) {
        for (i = 0; i < 256; i++) {
            if (r->pattern != MALLOC_PAIRS) {
                u32 k = racer_next(&s) & (RACER_SLOTS - 1);
                if (slot[k]) r->f(slot[k]);
                slot[k] = r->m(racer_size(&s, r->pattern == MALLOC_SMALL));
                if (slot[k]) *(volatile u8 *)slot[k] = 1;
            } else {
                size_t size = 16 + (i & 7) * 24;
                u8 *a = (u8 *)r->m(size), *b = (u8 *)r->m(size * 2);
                if (a) *(volatile u8 *)a = 1;
                if (b) *(volatile u8 *)b = 1;
                r->f(b);
                r->f(a);
            }
        }
        n += r->pattern == MALLOC_PAIRS ? 512 : 256;
    }
    for (i = 0; slot && i < RACER_SLOTS; i++)
        if (slot[i]) r->f(slot[i]);
    free(slot);
    r->pairs = n;
}

/* Millions of malloc-and-free pairs a second that `threads` threads (8 at most) make together in `ms` milliseconds. */
static double malloc_race(bench_malloc_fn m, bench_free_fn f, int pattern, int threads, u32 ms)
{
    malloc_racer r[8];
    mg_thread *t[8];
    volatile i32 state = 0;
    u64 t0, t1, pairs = 0;
    int i;
    for (i = 0; i < threads; i++) {
        r[i].m = m;
        r[i].f = f;
        r[i].pattern = pattern;
        r[i].state = &state;
        r[i].seed = 0x9E3779B9u * (u32)(i + 1);
        r[i].pairs = 0;
        t[i] = mg_thread_start(malloc_racer_main, &r[i], 256u * 1024u, "malloc-racer");
    }
    mg_sleep_ns(20000000ull);
    t0 = mg_time_monotonic_ns();
    mg_atomic_xchg(&state, 1);
    mg_sleep_ns((u64)ms * 1000000ull);
    mg_atomic_xchg(&state, 2);
    t1 = mg_time_monotonic_ns();
    for (i = 0; i < threads; i++)
        if (t[i]) {
            mg_thread_join(t[i]);
            pairs += r[i].pairs;
        }
    return t1 > t0 ? (double)pairs * 1000.0 / (double)(t1 - t0) : 0.0;
}

/* One line of the race at 1, 2, 4 (and 8) threads: millions a second, and how many times one thread's that is. */
static void malloc_race_line(const char *what, bench_malloc_fn m, bench_free_fn f, int pattern, int max_threads, u32 ms)
{
    char line[300];
    size_t at;
    double one = 0;
    int n;
    at = (size_t)snprintf(line, sizeof(line), "  %s:", what);
    for (n = 1; n <= max_threads && n <= 8 && at < sizeof(line); n *= 2) {
        double v = malloc_race(m, f, pattern, n, ms);
        if (n == 1) {
            one = v;
            at += (size_t)snprintf(line + at, sizeof(line) - at, " 1 thread %.1f M/s", v);
        } else {
            at += (size_t)snprintf(line + at, sizeof(line) - at, ", %d threads %.1f M/s (%.2fx)", n, v, one > 0 ? v / one : 0.0);
        }
    }
    say("%s", line);
}

static void *host_malloc(size_t n) { return malloc(n); }
static void host_free(void *p) { free(p); }

/* (The threads' caches of freed blocks off for the first of each pair of lines -- malloc as it was before them -- and on for the second.) */
void mg_tests_malloc_bench(int ms, int max_threads)
{
    bench_malloc_fn gmalloc = (bench_malloc_fn)mg_bionic_find("malloc");
    bench_free_fn gfree = (bench_free_fn)mg_bionic_find("free");
    if (!gmalloc || !gfree) return;
    say("malloc-and-free pairs a second, threads together (the host's malloc takes its blocks from %s; %d processors):", mg_heap_kind(),
        mg_cpu_count());
    mg_g_malloc_caches_allow(0);
    malloc_race_line("the guest's malloc without caches, mixed sizes", gmalloc, gfree, MALLOC_MIXED, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(1);
    malloc_race_line("the guest's malloc, mixed sizes", gmalloc, gfree, MALLOC_MIXED, max_threads, (u32)ms);
    malloc_race_line("the host's malloc, mixed sizes", host_malloc, host_free, MALLOC_MIXED, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(0);
    malloc_race_line("the guest's malloc without caches, mixed sizes of 4 KB and less", gmalloc, gfree, MALLOC_SMALL, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(1);
    malloc_race_line("the guest's malloc, mixed sizes of 4 KB and less", gmalloc, gfree, MALLOC_SMALL, max_threads, (u32)ms);
    malloc_race_line("the host's malloc, mixed sizes of 4 KB and less", host_malloc, host_free, MALLOC_SMALL, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(0);
    malloc_race_line("the guest's malloc without caches, pairs", gmalloc, gfree, MALLOC_PAIRS, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(1);
    malloc_race_line("the guest's malloc, pairs", gmalloc, gfree, MALLOC_PAIRS, max_threads, (u32)ms);
    malloc_race_line("the host's malloc, pairs", host_malloc, host_free, MALLOC_PAIRS, max_threads, (u32)ms);
    mg_g_malloc_caches_allow(-1);
}

/*
 * The guest's malloc family as threads use it at once, each fast enough to keep a cache of the blocks it frees
 * (mg_libc_mem.c, "each thread's cache"): blocks made by one thread and freed or reallocated by another, at bionic's
 * alignment and of jemalloc's sizes whichever cache they pass through, calloc's zeroed, realloc's contents kept; what is
 * counted as allocated back where it was once the threads are gone, and their caches gone with them.
 */
typedef struct malloc_fns {
    void *(*m)(size_t);
    void *(*c)(size_t, size_t);
    void *(*r)(void *, size_t);
    void *(*a)(size_t, size_t);
    void (*f)(void *);
    size_t (*usable)(const void *);
} malloc_fns;

typedef struct malloc_share {
    const malloc_fns *fn;
    void *volatile *slots;
    u32 nslots, rounds, seed;
    volatile i32 bad, ops;
    u32 caches_seen;                   /* the threads with caches a thread saw before it ended */
    u64 cached_seen;
} malloc_share;

static size_t share_quantum(void) { return MG_ARCH_ARM32 ? 8 : 16; }
static void *xchg_ptr(void *volatile *at, void *value)
{
    void *was;
    do was = *at;
    while (mg_atomic_cas_ptr(at, was, value) != was);
    return was;
}
static size_t share_usable(size_t n) { return MG_ALIGN_UP(n ? n : 1, share_quantum()); }

/* A block of `size` bytes filled so that its size and every byte of it can be told again: the size in its first four, then a byte of the size's. */
static void share_fill(u8 *p, size_t size)
{
    u32 s = (u32)size;
    size_t i;
    if (size < 4) return;
    memcpy(p, &s, 4);
    for (i = 4; i < size; i++) p[i] = (u8)(s * 7u + (u32)i);
}

/* Whether a filled block is as it was filled, of malloc_usable_size's and at an alignment a block of its size has (`align` at least). */
static bool share_check(const malloc_fns *fn, const u8 *p, size_t size, size_t align)
{
    u32 s = (u32)size;
    size_t i, want = share_usable(size) >= 256 ? 64 : share_quantum();
    if (align < want) align = want;
    if (fn->usable(p) != share_usable(size) || ((mg_addr)p & (align - 1))) return false;
    if (size < 4) return true;
    if (memcmp(p, &s, 4)) return false;
    for (i = 4; i < size; i++)
        if (p[i] != (u8)(s * 7u + (u32)i)) return false;
    return true;
}

/* A block's size as share_fill wrote it (0 for one of fewer than four bytes, which none of these is). */
static size_t share_size(const u8 *p)
{
    u32 s;
    memcpy(&s, p, 4);
    return s;
}

static size_t share_pick(u32 *s)
{
    u32 r = racer_next(s) % 100;
    if (r < 70) return 4 + racer_next(s) % 252;
    if (r < 95) return 256 + racer_next(s) % 3900;
    return 4097 + racer_next(s) % 12000;
}

/*
 * Each round a slot is taken at random: a block found there is checked and then freed, or reallocated to another size
 * (its contents checked again) and put back; an empty one gets a new block from malloc, calloc (zero checked), memalign
 * or realloc of NULL. Every block passes between the threads in the slots.
 */
static MG_HOSTCB void malloc_sharer(void *arg)
{
    malloc_share *sh = (malloc_share *)arg;
    const malloc_fns *fn = sh->fn;
    u32 s = sh->seed, i;
    for (i = 0; i < sh->rounds; i++) {
        u32 k = racer_next(&s) % sh->nslots, how = racer_next(&s) % 8;
        u8 *p = (u8 *)xchg_ptr(&sh->slots[k], NULL);
        size_t size;
        if (p) {
            size = share_size(p);
            if (!share_check(fn, p, size, 0)) mg_atomic_add(&sh->bad, 1);
            if (how < 5) {
                fn->f(p);
                continue;
            }
            {
                size_t to = share_pick(&s), keep = to < size ? to : size, j;
                u8 *q = (u8 *)fn->r(p, to);
                bool same = q != NULL;
                for (j = 4; same && j < keep; j++) same = q[j] == (u8)((u32)size * 7u + (u32)j);
                if (!same || !q) {
                    mg_atomic_add(&sh->bad, 1);
                    if (q) fn->f(q);
                    continue;
                }
                p = q;
                size = to;
            }
        } else {
            size = share_pick(&s);
            if (how < 3) {
                p = (u8 *)fn->m(size);
            } else if (how < 5) {
                size_t z;
                bool zero = true;
                p = (u8 *)fn->c(1, size);
                for (z = 0; p && z < share_usable(size); z++) zero &= p[z] == 0;
                if (!zero) mg_atomic_add(&sh->bad, 1);
            } else if (how < 7) {
                size_t align = how == 5 ? 64 : 128;
                p = (u8 *)fn->a(align, size);
                if (p && (fn->usable(p) != share_usable(size) || ((mg_addr)p & (align - 1)))) mg_atomic_add(&sh->bad, 1);
            } else {
                p = (u8 *)fn->r(NULL, size);
            }
            if (!p) {
                mg_atomic_add(&sh->bad, 1);
                continue;
            }
        }
        share_fill(p, size);
        p = (u8 *)xchg_ptr(&sh->slots[k], p);
        /* (Another thread filled the slot meanwhile: its block is checked and freed here.) */
        if (p) {
            if (!share_check(fn, p, share_size(p), 0)) mg_atomic_add(&sh->bad, 1);
            fn->f(p);
        }
        mg_atomic_add(&sh->ops, 1);
    }
    mg_g_malloc_caches(&sh->caches_seen, &sh->cached_seen);
}

/* A producer's blocks for a consumer: made here, checked, freed or reallocated and freed there. */
typedef struct malloc_pipe {
    const malloc_fns *fn;
    void *volatile ring[64];
    u32 count;
    volatile i32 bad;
} malloc_pipe;

static size_t pipe_size(u32 i) { return 4 + (i * 37u) % 700u + (i % 50u == 0 ? 3000u : 0u); }

static MG_HOSTCB void malloc_producer(void *arg)
{
    malloc_pipe *pp = (malloc_pipe *)arg;
    u32 i;
    for (i = 0; i < pp->count; i++) {
        size_t size = pipe_size(i);
        u8 *p = (u8 *)(i % 3 == 0 ? pp->fn->c(size, 1) : pp->fn->m(size));
        if (!p) {
            mg_atomic_add(&pp->bad, 1);
            p = (u8 *)pp->fn->m(4);
            size = 4;
        }
        share_fill(p, size);
        while (mg_atomic_cas_ptr(&pp->ring[i % 64], NULL, p) != NULL) mg_thread_yield();
    }
}

static MG_HOSTCB void malloc_consumer(void *arg)
{
    malloc_pipe *pp = (malloc_pipe *)arg;
    u32 i;
    for (i = 0; i < pp->count; i++) {
        u8 *p;
        while ((p = (u8 *)xchg_ptr(&pp->ring[i % 64], NULL)) == NULL) mg_thread_yield();
        if (!share_check(pp->fn, p, share_size(p), 0)) mg_atomic_add(&pp->bad, 1);
        if (i % 4 == 1) {
            size_t size = share_size(p);
            u8 *q = (u8 *)pp->fn->r(p, size * 2 + 100);
            if (!q || share_size(q) != size || ((mg_addr)q & (share_usable(size * 2 + 100) >= 256 ? 63 : share_quantum() - 1)))
                mg_atomic_add(&pp->bad, 1);
            p = q;
        }
        pp->fn->f(p);
    }
}

/* A thread that allocates fast enough to keep a cache, leaves blocks in it, and says what it saw before it ends. */
typedef struct malloc_leaver {
    const malloc_fns *fn;
    u32 caches;
    u64 cached;
    bool reused;
} malloc_leaver;

static MG_HOSTCB void malloc_leaver_main(void *arg)
{
    malloc_leaver *lv = (malloc_leaver *)arg;
    void *keep[40];
    u32 i;
    for (i = 0; i < 4000; i++) lv->fn->f(lv->fn->m(16 + (i % 40) * 8));
    for (i = 0; i < 40; i++) keep[i] = lv->fn->m(16 + i * 8);
    for (i = 0; i < 40; i++) lv->fn->f(keep[i]);
    /* (The last block freed is the next of its size: a cache's, the last in first out.) */
    lv->reused = lv->fn->m(16 + 39 * 8) == keep[39];
    lv->fn->f(keep[39]);
    mg_g_malloc_caches(&lv->caches, &lv->cached);
}

static void test_malloc_threads(void)
{
    malloc_fns fn;
    u32 caches0 = 0, caches1 = 0;
    u64 cached0 = 0, cached1 = 0, bytes0, bytes1;
    fn.m = (void *(*)(size_t))mg_bionic_find("malloc");
    fn.c = (void *(*)(size_t, size_t))mg_bionic_find("calloc");
    fn.r = (void *(*)(void *, size_t))mg_bionic_find("realloc");
    fn.a = (void *(*)(size_t, size_t))mg_bionic_find("memalign");
    fn.f = (void (*)(void *))mg_bionic_find("free");
    fn.usable = (size_t (*)(const void *))mg_bionic_find("malloc_usable_size");
    CHECK(fn.m && fn.c && fn.r && fn.a && fn.f && fn.usable);
    if (!fn.m || !fn.c || !fn.r || !fn.a || !fn.f || !fn.usable) return;
    mg_g_malloc_caches_allow(1);
    bytes0 = mg_g_malloc_bytes();
    mg_g_malloc_caches(&caches0, &cached0);
    {
        /* This thread's own cache: the block freed last is the next of its bin, calloc's zeroed though it was written, and
           a realloc within a bin stays where it is. */
        u32 i;
        u8 *a, *b;
        for (i = 0; i < 2000; i++) fn.f(fn.m(24 + (i % 7) * 40));
        a = (u8 *)fn.m(100);
        if (a) memset(a, 0xAB, fn.usable(a));
        fn.f(a);
        b = (u8 *)fn.c(100, 1);
        CHECK(b == a && b && fn.usable(b) == share_usable(100));
        for (i = 0; b && i < share_usable(100); i++)
            if (b[i]) break;
        CHECK(b && i == share_usable(100));
        fn.f(b);
        a = (u8 *)fn.m(300);
        if (a) share_fill(a, 300);
        b = (u8 *)fn.r(a, 310);
        CHECK(a && b == a && share_size(b) == 300 && b[299] == (u8)(300u * 7u + 299u) && fn.usable(b) == share_usable(310) && ((mg_addr)b & 63) == 0);
        fn.f(b);
        a = (u8 *)fn.m(316);
        CHECK(a == b && fn.usable(a) == share_usable(316));
        fn.f(a);
    }
    {
        malloc_leaver lv;
        mg_thread *t;
        memset(&lv, 0, sizeof(lv));
        lv.fn = &fn;
        mg_g_malloc_caches(&caches0, &cached0);
        t = mg_thread_start(malloc_leaver_main, &lv, 256u * 1024u, "malloc-leaver");
        CHECK(t != NULL);
        if (t) mg_thread_join(t);
        mg_g_malloc_caches(&caches1, &cached1);
        CHECK(lv.reused && lv.caches == caches0 + 1 && lv.cached > cached0);
        CHECK(caches1 == caches0 && cached1 == cached0);
    }
    {
        malloc_pipe pp;
        mg_thread *tp, *tc;
        memset(&pp, 0, sizeof(pp));
        pp.fn = &fn;
        pp.count = 20000;
        tp = mg_thread_start(malloc_producer, &pp, 256u * 1024u, "malloc-producer");
        tc = mg_thread_start(malloc_consumer, &pp, 256u * 1024u, "malloc-consumer");
        CHECK(tp && tc);
        if (tp) mg_thread_join(tp);
        if (tc) mg_thread_join(tc);
        CHECK(pp.bad == 0);
    }
    {
        static void *volatile slots[512];
        malloc_share sh[4];
        mg_thread *t[4];
        int i;
        u32 k;
        for (i = 0; i < 4; i++) {
            memset(&sh[i], 0, sizeof(sh[i]));
            sh[i].fn = &fn;
            sh[i].slots = slots;
            sh[i].nslots = 512;
            sh[i].rounds = 30000;
            sh[i].seed = 0x2545F491u * (u32)(i + 3);
        }
        for (i = 0; i < 4; i++) t[i] = mg_thread_start(malloc_sharer, &sh[i], 256u * 1024u, "malloc-sharer");
        for (i = 0; i < 4; i++)
            if (t[i]) mg_thread_join(t[i]);
        for (k = 0; k < 512; k++) {
            u8 *p = (u8 *)slots[k];
            slots[k] = NULL;
            if (p && !share_check(&fn, p, share_size(p), 0)) sh[0].bad++;
            fn.f(p);
        }
        CHECK(t[0] && t[1] && t[2] && t[3]);
        CHECK(sh[0].bad + sh[1].bad + sh[2].bad + sh[3].bad == 0);
        CHECK(sh[0].caches_seen > caches0);
    }
    /* What is counted as allocated is what it was (the threads' parts in their caches, back in the whole as they ended), give or
       take what another of the process's threads allocated meanwhile; and their caches are gone. */
    bytes1 = mg_g_malloc_bytes();
    mg_g_malloc_caches(&caches1, &cached1);
    CHECK(bytes1 - bytes0 < 65536 || bytes0 - bytes1 < 65536);
    CHECK(caches1 == caches0);
    {
        /* And how fast: one thread and four, blocks of mixed sizes and pairs, without the caches (malloc as it was before them)
           and with them. */
        static const int patterns[2] = { MALLOC_MIXED, MALLOC_PAIRS };
        static const char *const names[2] = { "mixed sizes", "pairs" };
        int k;
        say("  malloc across threads (the host's malloc takes its blocks from %s), malloc-and-free pairs a second:", mg_heap_kind());
        for (k = 0; k < 2; k++) {
            double off1, off4, on1, on4;
            mg_g_malloc_caches_allow(0);
            off1 = malloc_race(fn.m, fn.f, patterns[k], 1, 150);
            off4 = malloc_race(fn.m, fn.f, patterns[k], 4, 150);
            mg_g_malloc_caches_allow(1);
            on1 = malloc_race(fn.m, fn.f, patterns[k], 1, 150);
            on4 = malloc_race(fn.m, fn.f, patterns[k], 4, 150);
            say("    %s: without the threads' caches 1 thread %.1f M/s, 4 threads %.1f M/s (%.2fx); with them %.1f M/s, %.1f M/s (%.2fx)",
                names[k], off1, off4, off1 > 0 ? off4 / off1 : 0.0, on1, on4, on1 > 0 ? on4 / on1 : 0.0);
        }
    }
    mg_g_malloc_caches_allow(-1);
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

#if MG_GUEST_CODE

/*
 * An interface proxy of Unity's (jni/mg_jni_loop.c): a call through it and its
 * being disabled exclude each other, as they do under the proxy's lock in the
 * engine's own Java. One thread calls through a proxy as fast as it can while
 * this one disables the proxy and then "deletes" what was behind it; no call
 * may find it deleted. On a phone a film skipped with a tap did exactly that
 * to its decoder's thread before there was a lock. (Here, where a native
 * method can be a function of this program: it is called as guest code.)
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

/* What the main looper's thread found (test_proxies, at its end): its native looper, a descriptor watched on it, its callback's calls. */
typedef struct ui_looper_probe {
    int fd;
    void *looper;
    volatile i32 watched, added, called;
    u32 thread, called_thread;
} ui_looper_probe;

/* ALooper's callback: the descriptor read, once; 0 takes it off the looper. */
static int ui_looper_ready(int fd, int events, void *data)
{
    ui_looper_probe *p = (ui_looper_probe *)data;
    u64 v;
    (void)events;
    mg_fd_read(fd, &v, 8);
    p->called_thread = mg_thread_self_id();
    mg_atomic_add(&p->called, 1);
    return 0;
}

/* On the main looper's thread: its looper, and the descriptor watched on it, as native code that loads there does. */
static void ui_looper_watch(void *arg)
{
    typedef void *(*for_thread_fn)(void);
    typedef int (*add_fd_fn)(void *, int, int, int, void *, void *);
    ui_looper_probe *p = (ui_looper_probe *)arg;
    for_thread_fn for_thread = (for_thread_fn)mg_bionic_find("ALooper_forThread");
    add_fd_fn add_fd = (add_fd_fn)mg_bionic_find("ALooper_addFd");
    p->thread = mg_thread_self_id();
    p->looper = for_thread ? for_thread() : NULL;
    if (p->looper && add_fd) p->added = add_fd(p->looper, p->fd, 0, 1 /* ALOOPER_EVENT_INPUT */, (void *)ui_looper_ready, p);
    mg_atomic_xchg(&p->watched, 1);
}

static MG_HOSTCB void proxy_caller_thread(void *arg)
{
    proxy_caller *c = (proxy_caller *)arg;
    while (!c->p->stop) mg_jni_unref(mg_jni_proxy_call(c->proxy, "java/lang/Runnable", "run", "()V", NULL, 0));
}

static void test_proxies(void)
{
    enum { ROUNDS = 200 };
    /* The JNIEnv's own functions, by their places in its table, as an app's library calls them; JNINativeMethod, three pointers. */
    typedef struct { const char *name, *sig; void *fn; } native_method;
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
    m.name = "invoke";
    m.sig = invoke_sig;
    m.fn = (void *)proxied_invoke;
    ((register_fn)table[215])(env, cls, &m, 1);
    CHECK(mg_jni_native(bridge, "invoke", invoke_sig) == (void *)proxied_invoke);
    /* (A native found by its name alone, with the signature it was registered with, and the class's natives said: the Unity driver's initJni.) */
    {
        char said[400];
        const char *sig = mg_jni_native_sig(bridge, "invoke");
        CHECK(sig && !strcmp(sig, invoke_sig) && !mg_jni_native_sig(bridge, "noSuchNative"));
        CHECK(mg_jni_natives_said(bridge, said, sizeof(said)) == 1 && !strncmp(said, "invoke(JLjava/lang/Class;", 25));
    }
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

    /*
     * The main thread's native looper (mg_jni_loop.c, looper_thread): ALooper_forThread there gives one, as on Android's UI
     * thread, where Unity's engine takes it as it loads, and a descriptor watched on it with a callback has the callback run
     * on that thread once it is ready -- and only then.
     */
    say("the main looper's thread: a native looper, whose descriptors' callbacks it runs");
    {
        ui_looper_probe probe;
        u64 one = 1;
        memset(&probe, 0, sizeof(probe));
        probe.fd = mg_fd_eventfd(0, 04000);
        CHECK(probe.fd >= 0);
        mg_jni_loop_call(NULL, ui_looper_watch, &probe);
        for (waited = 0; waited < 1000 && !probe.watched; waited++) mg_sleep_ns(2000000ull);
        CHECK(probe.watched && probe.looper != NULL && probe.added == 1);
        mg_sleep_ns(30000000ull);
        CHECK_INT(probe.called, 0);
        mg_fd_write(probe.fd, &one, 8);
        for (waited = 0; waited < 1000 && !probe.called; waited++) mg_sleep_ns(2000000ull);
        CHECK_INT(probe.called, 1);
        CHECK(probe.called_thread == probe.thread && probe.thread != mg_thread_self_id());
        say("  looper %p on the main thread %u; the callback ran %d times, on thread %u", probe.looper, (unsigned)probe.thread, (int)probe.called,
            (unsigned)probe.called_thread);
        mg_sleep_ns(30000000ull);
        if (probe.fd >= 0) mg_fd_close(probe.fd);
    }
}

static int host_times_three(int v) { return v * 3; }
/* Called by guest code with the guest's floats: on 32-bit ARM softfp's, bits in, bits out. */
static gdouble host_hypot(gdouble a, gdouble b) { return mg_gdb(sqrt(mg_gd(a) * mg_gd(a) + mg_gd(b) * mg_gd(b))); }

/*
 * MG_TESTS_SKIP=name,name...: guest functions of the test libraries that are not called (a function that returns 0 is,
 * and the check of it fails) -- to see what comes after one that ends the process, as one can on a processor whose
 * guest code has just begun to run.
 */
static int skipped_call(void) { return 0; }

static void *guest_fn(mg_lib *lib, const char *name)
{
    const char *list = getenv("MG_TESTS_SKIP"), *at = list ? strstr(list, name) : NULL;
    size_t n = strlen(name);
    while (at && ((at != list && at[-1] != ',') || (at[n] && at[n] != ','))) at = strstr(at + 1, name);
    if (!at) return mg_ld_sym(lib, name);
    say("  (%s is not called: MG_TESTS_SKIP)", name);
    return (void *)skipped_call;
}

#define SYM(lib, name) mg_ld_sym(lib, name)
#define FN(lib, type, name) MG_GFN(type, guest_fn(lib, name))

/*
 * A thread stopped as the collector stops one (mg_thread_suspend: dvm/mg_dvm_gc.c takes the registers it reads for the
 * thread's own, and keeps what they point at) while it keeps coming back from signals: t_gcr_spin (guest/t_basic.c) holds
 * a mark in each register in a loop that changes none of them, and another thread of the library's sends it a signal as
 * soon as the one before was handled -- on Windows each goes back to the loop through NtContinue. Stopped twenty thousand
 * times, a little apart, wherever it is found in the loop its registers must be the marks. (Not x18 on ARM64, the
 * platform's, which the spinner leaves as it is: GCR_SKIP.)
 */
#if MG_ARCH_X86_64 || MG_ARCH_ARM64
#define GCR_MARK 0x6d67000047430000ull
#else
#define GCR_MARK 0x47430000ull
#endif
#if MG_ARCH_ARM64
#define GCR_SKIP (1u << 18)
#else
#define GCR_SKIP 0u
#endif

static void test_stop_signalled(mg_lib *lib)
{
    mg_addr loop = MG_CODE_ADDR(SYM(lib, "t_gcr_loop")), end = MG_CODE_ADDR(SYM(lib, "t_gcr_loop_end"));
    mg_thread_regs regs;
    mg_thread *h;
    int tid, i, k, stopped = 0, in_loop = 0, wrong = 0;
    u32 wrong_regs = 0;
    long handled;
    char first[240] = "none";
    u64 began = mg_time_monotonic_ns();
    tid = FN(lib, int (*)(void), "t_gcr_start")();
    CHECK(tid > 0 && loop && end > loop);
    if (tid <= 0) return;
    h = mg_gthread_host_of_tid(tid);
    CHECK(h != NULL);
    for (i = 0; h && i < 20000; i++) {
        u64 until;
        if (!mg_thread_suspend(h, &regs)) continue;
        stopped++;
        if (MG_CODE_ADDR(regs.pc) >= loop && MG_CODE_ADDR(regs.pc) < end) {
            u32 bad = 0;
            in_loop++;
            for (k = 0; k < regs.n; k++)
                if (!(GCR_SKIP & (1u << k)) && (u64)regs.r[k] != GCR_MARK + (u64)k) bad |= 1u << k;
            if (bad && !wrong) {
                size_t n = (size_t)snprintf(first, sizeof(first), "the first at stop %d:", i);
                for (k = 0; k < regs.n && n + 24 < sizeof(first); k++)
                    if (bad & (1u << k)) n += (size_t)snprintf(first + n, sizeof(first) - n, " r[%d] %llx", k, (unsigned long long)regs.r[k]);
            }
            if (bad) wrong++;
            wrong_regs |= bad;
        }
        mg_thread_resume(h);
        /* (Let go for 0 to 60 us: stops at every point of a signal's way back.) */
        until = mg_time_monotonic_ns() + (u64)(i % 7) * 10000ull;
        while (mg_time_monotonic_ns() < until) {}
    }
    handled = FN(lib, long (*)(void), "t_gcr_finish")();
    say("  stopped %d times while signalled, %d in the loop, %d of them with registers not its own (mask %x; %s); %ld signals handled, %u ms",
        stopped, in_loop, wrong, (unsigned)wrong_regs, first, handled, (unsigned)((mg_time_monotonic_ns() - began) / 1000000u));
    {
        char report[320];
        mg_thread_suspend_report(report, sizeof(report));
        say("  stops of threads so far: %s", report);
    }
    CHECK_INT(stopped, 20000);
    CHECK(in_loop > 100);
    CHECK_INT(wrong, 0);
    CHECK(handled > 100);
}

#if MG_ARCH_X86_64 || (MG_ARCH_X86 && MG_HOST_WINDOWS)
/*
 * A thread stopped as the collector stops one while the handler of a signal sent to it waits -- spinning in guest code
 * (how 0), or in sigsuspend (how 1), as Boehm's and Mono's collectors park a thread for a whole collection of theirs:
 * t_gcw_start (guest/t_basic.c) sends the signal to a spinner holding a mark in each register. Every one of them must be
 * among what the collector looks at -- the registers mg_thread_suspend gives, those it gives of the code the thread was
 * interrupted in, and the thread's stack from sp up -- at every stop, or an object the interrupted code held in that
 * register alone could be freed under it meanwhile.
 */
#if MG_ARCH_X86_64
#define GCW_MARK 0x6d67000057410000ull
#define GCW_REGS 15
#else
#define GCW_MARK 0x57410000ull
#define GCW_REGS 7
#endif

/* Which mark a word is, as a bit (k for register k's), or 0. */
static u32 gcw_mark_bit(mg_addr w)
{
    u64 k = (u64)w - GCW_MARK;
    return k < GCW_REGS ? 1u << k : 0;
}

static void test_stop_in_handler(mg_lib *lib, int how)
{
    void *stack[2] = { NULL, NULL };
    mg_thread_regs regs;
    mg_thread *h = NULL;
    const u32 all = (1u << GCW_REGS) - 1;
    u32 missing = 0, first_regs = 0, first_was = 0, first_stack = 0;
    int tid, i, k, stopped = 0, short_of = 0;
    long done;
    tid = FN(lib, int (*)(int, void **), "t_gcw_start")(how, stack);
    CHECK(tid > 0);
    if (tid > 0) h = mg_gthread_host_of_tid(tid);
    CHECK(h != NULL);
    for (i = 0; h && i < 200; i++) {
        u32 in_regs = 0, in_was = 0, on_stack = 0;
        u64 until;
        if (!mg_thread_suspend(h, &regs)) continue;
        stopped++;
        /* (The words the collector takes, as it takes them: dvm/mg_dvm_gc.c, conservative_roots and scan_words.) */
        for (k = 0; k < regs.n; k++) in_regs |= gcw_mark_bit(regs.r[k]);
        for (k = 0; k < regs.n_was; k++) in_was |= gcw_mark_bit(regs.was[k]);
        if (regs.sp >= (mg_addr)stack[0] && regs.sp < (mg_addr)stack[1]) {
            const mg_addr *w = (const mg_addr *)MG_ALIGN_UP(regs.sp, sizeof(mg_addr));
            for (; w + 1 <= (const mg_addr *)stack[1]; w++) on_stack |= gcw_mark_bit(*w);
        }
        mg_thread_resume(h);
        if (stopped == 1) {
            first_regs = in_regs;
            first_was = in_was;
            first_stack = on_stack;
        }
        if ((in_regs | in_was | on_stack) != all) {
            short_of++;
            missing |= all & ~(in_regs | in_was | on_stack);
        }
        until = mg_time_monotonic_ns() + (u64)(i % 5) * 10000ull;
        while (mg_time_monotonic_ns() < until) {}
    }
    done = FN(lib, long (*)(void), "t_gcw_finish")();
    say("  stopped %d times while its handler waited %s: %d with registers it was interrupted with nowhere the collector looks (mask %x); "
        "at the first, marks in its registers %x, in those given of the code it interrupted %x, on its stack %x; the handler's end %ld",
        stopped, how ? "in sigsuspend" : "in guest code", short_of, (unsigned)missing, (unsigned)first_regs, (unsigned)first_was,
        (unsigned)first_stack, done);
    CHECK_INT(stopped, 200);
    CHECK_INT(short_of, 0);
    CHECK_INT(done, 0);
}
#endif

/* What t_round_sum (guest/t_basic.c) adds up, with the host's libm. */
static i64 round_sum_expected(int n)
{
    i64 sum = 0;
    int i;
    for (i = 0; i < n; i++) {
        float x = (float)(i - n / 2) * 0.37109375f + 0.25f;
        double d = (double)x * 1.5;
        sum += (i64)lroundf(x) * 3 + (i64)lrintf(x) * 5 + (i64)roundf(x) * 7 + (i64)floorf(x) * 11 + (i64)ceilf(x) * 13 + (i64)truncf(x) * 17
               + (i64)rintf(x) * 19 + (i64)lround(d) * 23 + (i64)lrint(d) * 29 + (i64)floor(d) * 31 + (i64)ceil(d) * 37 + (i64)round(d) * 41
               + llroundf(x) * 43 + llround(d) * 47;
    }
    return sum;
}

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
    /* (x86_64: t_fs_left's access of the segment through a register, which nothing can take the place of, is left.) */
    CHECK_INT(st.tp_reads_failed, MG_ARCH_X86_64 ? 1 : 0);

    /* Relocations, data, constructors. */
    CHECK_INT(FN(lib, int (*)(void), "t_ctor_ran")(), 1234);
    CHECK_INT(FN(lib, int (*)(int, int), "t_add")(40, 2), 42);
    counter = FN(lib, int *(*)(void), "t_counter_ptr")();
    CHECK(counter && *counter == 5);
    CHECK(!strcmp(FN(lib, const char *(*)(int), "t_name")(2), "two"));
    CHECK(!strcmp(FN(lib, const char *(*)(void), "t_hello")(), "hello from guest"));
    CHECK(SYM(lib, "t_exported_value") && *(int *)SYM(lib, "t_exported_value") == 0x1234);

    /* Floating point across the boundary, both ways. */
    CHECK(near(mg_gd(FN(lib, gdouble (*)(gdouble, gfloat), "t_fmul")(mg_gdb(1.5), mg_gfb(4.0f))), 6.0));
    /* (int, float, double, float, int, double): on ARM r0, r1, [r2:r3], then the stack -- float, int, pad, double. */
    CHECK(near(mg_gf(FN(lib, gfloat (*)(int, gfloat, gdouble, gfloat, int, gdouble), "t_mixed")(1, mg_gfb(2.0f), mg_gdb(3.0), mg_gfb(4.0f), 5, mg_gdb(6.5))), 21.5));
    CHECK(near(mg_gf(FN(lib, gfloat (*)(gfloat), "t_sqrt")(mg_gfb(2.25f))), 1.5));
    CHECK(near(mg_gd(FN(lib, gdouble (*)(gdouble), "t_sin")(mg_gdb(0.5))), sin(0.5)));
    CHECK(near(mg_gd(FN(lib, gdouble (*)(gdouble, gdouble), "t_pow")(mg_gdb(2.0), mg_gdb(10.0))), 1024.0));
    CHECK_INT(FN(lib, long (*)(gdouble), "t_lround")(mg_gdb(-2.5)), -3);
    {
        /* libm's rounding through the library's PLT (ARM entries in the ARM one, as Geekbench's are), against the host's. */
        u64 began = mg_time_monotonic_ns();
        i64 got = FN(lib, i64 (*)(int), "t_round_sum")(8192);
        CHECK(got == round_sum_expected(8192));
        say("  libm's rounding through the PLT: 8192 x 14 calls in %.1f ms", (double)(mg_time_monotonic_ns() - began) / 1e6);
    }
    CHECK(near(mg_gd(FN(lib, gdouble (*)(gdouble, double *), "t_modf")(mg_gdb(3.75), &ip)), 0.75) && near(ip, 3.0));
    CHECK(near(mg_gd(FN(lib, gdouble (*)(const char *), "t_strtod")("-12.5e2")), -1250.0));
    CHECK(near(mg_gd(FN(lib, gdouble (*)(void *, gdouble, gdouble), "t_call_d")((void *)host_hypot, mg_gdb(3.0), mg_gdb(4.0))), 5.0));

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
    counter = FN(lib, int *(*)(void), "t_tls_addr")();
    CHECK(counter && *counter == 555);
    FN(lib, void (*)(int), "t_tls_set")(100);
    CHECK_INT(FN(lib, int (*)(int, int), "t_threads")(8, 2000), 16000);
    CHECK_INT(FN(lib, int (*)(void), "t_tls_app_slot")(), 0);
    /* A wait is woken by what it waits on: a byte passed back and forth over two pipes, with 32 other threads waiting
       in epoll_wait each on an eventfd of its own (an app's Loopers), and with none. */
    {
        long long alone = FN(lib, long long (*)(int, int), "t_wait_herd")(0, 2000);
        long long herd = FN(lib, long long (*)(int, int), "t_wait_herd")(32, 2000);
        say("  2000 round trips over two pipes: %lld us with no other waiter, %lld us with 32 waiting elsewhere", alone, herd);
        /* (Each change woke every waiter: 87 ms and 1.76 s. On a PC busy with other work the two take a millisecond or
           three each, where its scheduling alone is a factor of three: 1056 and 3190 us once, so 20 ms passes too.) */
        CHECK(alone > 0 && herd > 0 && (herd < alone * 3 || herd < 20000));
    }
    /* Four thousand megabytes mapped and unmapped in two pieces each: the addresses come back. */
    CHECK_INT(FN(lib, int (*)(int), "t_map_pieces")(4096), 0);
    /* A 300 KB frame written bottom first, on this thread and on a new one. */
    CHECK_INT(FN(lib, int (*)(int), "t_big_frame")(10), 56);
    /* Signals between threads: a thread stopped and let go twenty times, wherever it was found. */
    CHECK_INT(FN(lib, int (*)(int), "t_signals")(20), 0);
    /* A thread stopped as the collector stops one, 20000 times while signals keep sending it back: its registers its own. */
    test_stop_signalled(lib);
#if MG_ARCH_X86_64 || (MG_ARCH_X86 && MG_HOST_WINDOWS)
    /* And one stopped while a signal's handler waits on it, spinning and in sigsuspend: the interrupted code's registers too. */
    test_stop_in_handler(lib, 0);
    test_stop_in_handler(lib, 1);
#endif
    /* The registers a function keeps for its caller, as they were after each of 300 signals to a thread spinning with marks
       in them, each sent some microseconds after its handler ran for the one before: on Windows, on its way back from
       that interruption (host/mg_host_win32.c, mg_thread_interrupt). */
    {
        long changed = FN(lib, long (*)(int), "t_signal_regs")(300);
        if (changed > 0) say("  registers that came back changed, the first time: %s", FN(lib, const char *(*)(long), "t_signal_regs_seen")(0));
        CHECK_INT(changed, 0);
    }
    {
        char report[448];
        mg_thread_interrupt_report(report, sizeof(report));
        say("  threads interrupted in guest code: %s", report);
    }
    /* Sixteen threads in rounds for a second, short turns on a normal and a recursive mutex: no round stalls. (When a
       woken waiter that lost the race took the lock as uncontended, a thread was left asleep on a free mutex: on this
       PC's x64 build one round in about a thousand, and t_threads hung now and then.) After the checks that time
       waits: just before t_wait_herd it pushed that one's ratio past its bound in a third of the runs. */
    {
        u64 t0 = mg_time_monotonic_ns();
        int rounds = FN(lib, int (*)(int, int, int), "t_mutex_stress")(16, 20, 1000);
        say("  %d rounds of 16 threads taking two mutexes 20 times each, in %d ms (-2: a round stalled)", rounds,
            (int)((mg_time_monotonic_ns() - t0) / 1000000));
        CHECK(rounds >= 100);
    }
    /* A reader kept out by a waiting writer goes in when the writer gives up (pthread_rwlock_timedwrlock): it slept on
       until the next unlock. */
    CHECK_INT(FN(lib, int (*)(void), "t_rwlock_writer_gives_up")(), 0);
    /* A timed rwlock wait ends at its deadline however often it is woken: a writer waits 250 ms for the lock while ten
       readers let go of it one by one every 100 ms. (Each wait was given the whole timeout again: woken by every
       unlock, the writer waited on until the last reader had gone, a second on, and took the lock.) */
    {
        int ms = -1, e = FN(lib, int (*)(int *), "t_rwlock_deadline")(&ms);
        say("  a writer's 250 ms pthread_rwlock_timedwrlock, readers leaving every 100 ms: %d after %d ms", e, ms);
        CHECK(e == GE_TIMEDOUT && ms >= 240 && ms < 600);
    }

    /* Files, the made-up /proc, properties, dlopen. */
    snprintf(buf, sizeof(buf), "/scratch");
    (void)scratch;
    CHECK_INT(FN(lib, int (*)(const char *), "t_files")(buf), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_proc")(), 1);
#if MG_ARCH_ARM32
    /* AT_HWCAP says SDIV (idiva, idivt) and VFPv4 where the processor has SDIV, and neither where it has not -- a Surface
       RT's Cortex-A9, as qemu-arm -cpu cortex-a9 plays it: an app that chose its code by them would fault there on VFPv4's
       instructions, or be trapped at every division. NEON either way. */
    {
        gulong words[64];
        size_t n = mg_bionic_auxv_words(words, MG_COUNT(words)), i;
        unsigned long hwcap = 0;
        for (i = 0; i + 1 < n; i += 2)
            if (words[i] == 16) hwcap = (unsigned long)words[i + 1];
        say("  AT_HWCAP 0x%lx: the processor %s SDIV", hwcap, mg_cpu_arm_divide() ? "has" : "has no");
        CHECK_INT((hwcap >> 18) & 1, mg_cpu_arm_divide() ? 1 : 0);
        CHECK_INT((hwcap >> 16) & 1, mg_cpu_arm_divide() ? 1 : 0);
        CHECK_INT((hwcap >> 12) & 1, 1);
    }
#endif
    CHECK_INT(FN(lib, int (*)(void), "t_boot_id")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_proc_status")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_proc_pid")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_smaps")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_props")(), 28);
    CHECK_INT(FN(lib, int (*)(void), "t_dl")(), 4);
    CHECK_INT(FN(lib, int (*)(void), "t_dl_absent")(), 0);
    CHECK_INT(FN(lib, int (*)(void), "t_loader_dl")(), 5);
#if MG_ARCH_ARM32
    /* (Thumb's hooks on libc's hookable entries, and a Thumb or ARM udf as an app's trap: 32-bit ARM's alone.) */
    CHECK_INT(FN(lib, int (*)(void), "t_libc_inline_hook")(), 0);
#endif
    CHECK_INT(FN(lib, int (*)(void), "t_own_code_hook")(), 0);
#if MG_ARCH_ARM32
    CHECK_INT(FN(lib, int (*)(void), "t_trap_hook")(), 0);
#endif
#if MG_ARCH_X86_64 || MG_ARCH_X86
    CHECK_INT(FN(lib, int (*)(void), "t_self_writing_page")(), 0);
#endif
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
    /* Go's runtime, as a library: its init function finds the auxiliary vector past envp, and its calls by number go. */
    CHECK_INT(FN(lib, int (*)(void), "t_init_auxv")(), 1);
    CHECK_INT(FN(lib, int (*)(void), "t_vdso")(), 0);
    CHECK_INT(FN(lib, int (*)(const char *), "t_go_calls")(buf), 0);
    CHECK_INT(FN(lib, int (*)(const char *), "t_go_net_files")(buf), 0);
#if MG_ARCH_X86_64
    /* System calls of the library's own (x86/mg_patch_x86.c): six of its seven led out to the core, the seventh left. */
    CHECK_INT(st.svc_sites, 6);
    CHECK_INT(FN(lib, int (*)(void), "t_raw_syscall")(), 0);
    /* Accesses of the thread pointer's segment through a register, Go's and C's initial-exec ones (x86/mg_patch_x86.c):
       nineteen led out to pieces of island, t_fs_left's left; on this thread and a new one, registers and flags kept, a
       thread interrupted two hundred times in the middle of them. */
    CHECK_INT(st.tp_led_out, 19);
    CHECK_INT(FN(lib, int (*)(void), "t_fs_access")(), 0);
#endif
#if MG_ARCH_X86
    /* x86's `int 0x80` of the library's own, made a system call where Windows faults at it (mg_core.c). */
    CHECK_INT(FN(lib, int (*)(void), "t_x86_int80")(), 0);
#endif
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

#if !MG_ARCH_X86
/*
 * A goroutine's stack in Go's heap, given back with madvise and run on again (core/tests/guest/t_go.c: a library of Go's
 * by its build ID note). Given back as Go's runtime gives its heap's pages back, by number from its own code: committed
 * again at once (bionic/mg_libc_mem.c), zero, the app's memory as the frames line counts it as before, and nothing takes
 * a page back -- not the goroutine's first frame on its stack, not a read of the rest of the heap -- there is none to
 * take. (Decommitted instead, that frame's first touch ends the process on Windows: the fault's records are due on the
 * very page the stack pointer is in.) Given back by any other code, on a POSIX host: decommitted, and the goroutine's
 * first touch of its own stack's page is a fault the handler takes on its stack for faults (host/mg_host_posix.c) and
 * repairs.
 */
static void test_go_stack(void)
{
    enum { STACK = 256 * 1024 };
    mg_lib *go = mg_ld_load("libt_go.so");
    mg_ld_stats st;
    u64 given0 = 0, again0 = 0, anon0 = 0, given = 0, again = 0, anon = 0;
    say("guest library: a goroutine's stack in Go's heap, given back with madvise and run on again");
    CHECK(go && mg_ld_init(go));
    if (!go) return;
    CHECK(mg_ld_lib_is_go(go));
    mg_ld_get_stats(go, &st);
#if MG_ARCH_ARM32
    /* Its Thumb svc a trap; its four ARM ones, Go's runtime's way, led out to the core (arm/mg_patch.c). */
    CHECK_INT(st.svc_sites, 5);
    CHECK_INT(st.svc_led_out, 4);
    {
        /*
         * Made so, on the thread's own stack and on a goroutine's of two kilobytes with no memory below it, here and on four
         * threads of their own (t_go_arm_svc): registers, flags and VFP registers kept, nothing written below the
         * goroutine's sp but the way out's 16 bytes; the 500 calls on goroutines' stacks made on the threads' stacks for
         * system calls. A trap's handler ran below such a stack's sp, where Windows laid the trap's records: MCO
         * Advanced's libgojni.so ended so as its runtime started (package 669).
         */
        u32 made0 = 0, on0 = 0, made = 0, on = 0;
        mg_core_svc_calls(&made0, &on0);
        CHECK_INT(FN(go, long (*)(void), "t_go_arm_svc")(), 0);
        mg_core_svc_calls(&made, &on);
        say("  ARM svc led out: %u calls, %u of them on a stack for system calls", made - made0, on - on0);
        CHECK(made - made0 == 1305 && on - on0 >= 500 && on - on0 < made - made0);
    }
#else
    /* (Its calls of three arguments and of six: t_go_syscall3, t_go_syscall6.) */
    CHECK_INT(st.svc_sites, 2);
#if MG_ARCH_ARM64
    /* (Led out to the core, as every library's svc on ARM64: arm64/mg_patch_arm64.c.) */
    CHECK_INT(st.svc_led_out, 2);
#endif
#endif
    /* prctl by number: the names Go's runtime gives its mappings refused as a kernel without them refuses them (EINVAL),
       a thread's name set and read as libc's prctl does it (MCO Advanced's libgojni.so, package 669). */
    CHECK_INT(FN(go, long (*)(void), "t_go_prctl")(), 0);
    CHECK_INT(FN(go, long (*)(long), "t_go_heap_new")(3), 14);
    mg_mmap_given_back(&given0, &again0);
    mg_mmap_usage(&anon0, NULL);
    CHECK_INT(FN(go, long (*)(long), "t_go_give_back")(0), 0);
    CHECK_INT(FN(go, long (*)(long), "t_go_give_back")(1), 0);
    mg_mmap_given_back(&given, &again);
    mg_mmap_usage(&anon, NULL);
    CHECK(given - given0 == 2 * STACK && anon == anon0);
    CHECK_INT(FN(go, long (*)(long), "t_go_run")(4), 16);
    CHECK_INT(FN(go, long (*)(long), "t_go_nonzero")(1), 0);
    mg_mmap_given_back(&given, &again);
    CHECK(again == again0);
#if !defined(_WIN32) && !defined(MG_CORE_ELF)
    CHECK_INT(FN(go, long (*)(long), "t_go_give_back")(2), 0);
    mg_mmap_usage(&anon, NULL);
    CHECK(anon0 - anon == STACK);
    CHECK_INT(FN(go, long (*)(long), "t_go_run")(5), 18);
    mg_mmap_given_back(&given, &again0);
    say("  given back by other code: %u KB taken back by the goroutine's faults on its own stack", (unsigned)((again0 - again) >> 10));
    CHECK(again0 > again);
#endif
    FN(go, void (*)(void), "t_go_heap_free")();
}
#endif

/*
 * The reservations Go's runtime makes for its heap as it starts (t_go_reserve: mallocinit's on a 32-bit processor, by
 * number from the library's code). On a 32-bit host its arena's 516, 260 and 132 MB are refused with ENOMEM
 * (bionic/mg_libc_mem.c, go_up_front_refused), as WhatsApp's libgojni.so took 516 MB of the phone's 2 GB so; its
 * metadata's 2.3 MB, the 4 MB its heap then grows by, 60 MB by number and 64 MB through libc's mmap are not refused, and
 * the 4 MB arena takes writes; 8 MB asked anywhere, as Go asks for its next arena where its hint is not followed, comes lined
 * up on 4 MB, which Go then uses whole (go_lined_up). On a 64-bit host nothing is refused.
 */
static void test_go_reserve(void)
{
    mg_lib *go = mg_ld_load("libt_go.so");
    long out[9];
    u32 refused0 = 0, refused = 0;
    u64 bytes0 = 0, bytes = 0;
    int i;
    say("guest library: the reservations Go's runtime makes for its heap as it starts");
    CHECK(go && mg_ld_init(go));
    if (!go) return;
    mg_mmap_go_refused(&refused0, &bytes0);
    CHECK_INT(FN(go, long (*)(long *), "t_go_reserve")(out), 0);
    mg_mmap_go_refused(&refused, &bytes);
    say("  the arena of 516, 260 and 132 MB: %ld, %ld, %ld; the heap's first 4 MB at %p; %u reservations refused (%u MB)", out[1], out[2], out[3],
        (void *)out[4], refused - refused0, (unsigned)((bytes - bytes0) >> 20));
    for (i = 1; i <= 3; i++) {
        if (sizeof(void *) == 4) CHECK_INT(out[i], -12);
        else CHECK((unsigned long)out[i] < (unsigned long)-4095L);
    }
    CHECK_INT(refused - refused0, sizeof(void *) == 4 ? 3 : 0);
    CHECK(bytes - bytes0 == (sizeof(void *) == 4 ? (u64)(908u << 20) : 0));
    CHECK((unsigned long)out[0] < (unsigned long)-4095L);
    CHECK(out[4] && !((unsigned long)out[4] & ((4ul << 20) - 1)));
    CHECK_INT(out[5], 0);
    CHECK(out[6] != -1);
    CHECK((unsigned long)out[7] < (unsigned long)-4095L);
    if (sizeof(void *) == 4) CHECK_INT(out[8], 1);
    else CHECK(out[8] == 0 || out[8] == 1);
}

/*
 * Imports nothing provides (guest/t_unres.c): the library's line in the log as it loads names each, once however many
 * relocations ask for it, as many as the line has room for and then how many more. DANA's libjabe.so had one and the line
 * said "1 imports are not provided", which one only at the loader's verbosity 2 (0.1.0.678). A function's import is a stub
 * that says so when called and gives 0; a variable's is NULL.
 */
static char g_unres_line[600], g_unres_called[200];
static mg_log_sink g_unres_before;
static void *g_unres_before_user;

static void unres_sink(int level, const char *line, void *user)
{
    (void)user;
    if (strstr(line, "imports are not provided")) mg_strlcpy(g_unres_line, line, sizeof(g_unres_line));
    if (strstr(line, "which nothing provides")) mg_strlcpy(g_unres_called, line, sizeof(g_unres_called));
    if (g_unres_before) g_unres_before(level, line, g_unres_before_user);
}

static int count_in(const char *s, const char *what)
{
    int n = 0;
    for (s = strstr(s, what); s; s = strstr(s + 1, what)) n++;
    return n;
}

static void test_unresolved_imports(void)
{
    static const char *const names[] = { "mg_absent_one", "mg_absent_two", "mg_absent_data" };
    const char *long_name = "mg_absent_function_whose_name_is_rather_long_number_";
    size_t before = mg_ld_unresolved_count();
    mg_ld_stats st;
    mg_lib *lib;
    char more[64];
    int i, said;
    say("guest library: imports nothing provides, named in the log as it loads");
    mg_log_get_sink(&g_unres_before, &g_unres_before_user);
    mg_log_set_sink(unres_sink, NULL);
    g_unres_line[0] = 0;
    lib = mg_ld_load("libt_unres.so");
    say("  %s", g_unres_line[0] ? g_unres_line : "(no line)");
    CHECK(lib != NULL && mg_ld_init(lib));
    if (lib) {
        mg_ld_get_stats(lib, &st);
        CHECK_INT(st.unresolved, 3);
        CHECK_INT((int)(mg_ld_unresolved_count() - before), 3);
        CHECK(strstr(g_unres_line, "ld: libt_unres.so: 3 imports are not provided: ") != NULL);
        for (i = 0; i < 3; i++) CHECK_INT(count_in(g_unres_line, names[i]), 1);
        CHECK(!strstr(g_unres_line, " more"));
        g_unres_called[0] = 0;
        CHECK_INT(FN(lib, int (*)(int), "t_unres_call_one")(5), 7);
        CHECK(strstr(g_unres_called, "mg_absent_one") != NULL);
        CHECK_INT(FN(lib, int (*)(void), "t_unres_call_two")(), 9);
        CHECK(FN(lib, int *(*)(void), "t_unres_data")() == NULL);
        CHECK(FN(lib, void *const *(*)(void), "t_unres_pointers")()[1] == NULL);
    }
    g_unres_line[0] = 0;
    lib = mg_ld_load("libt_unres_many.so");
    say("  %s", g_unres_line[0] ? g_unres_line : "(no line)");
    CHECK(lib != NULL);
    if (lib) {
        mg_ld_get_stats(lib, &st);
        CHECK_INT(st.unresolved, 40);
        CHECK(strstr(g_unres_line, "ld: libt_unres_many.so: 40 imports are not provided: mg_absent_function_whose_name_is_rather_long_number_") != NULL);
        said = count_in(g_unres_line, long_name);
        snprintf(more, sizeof(more), ", and %d more", 40 - said);
        CHECK(said >= 4 && said < 40);
        CHECK(strstr(g_unres_line, more) != NULL);
        CHECK(strlen(g_unres_line) < 400);
    }
    mg_log_set_sink(g_unres_before, g_unres_before_user);
}

#if MG_ARCH_ARM32
/*
 * The crash report of a thread at a small pc (mg_core.c), each under a guard as the runtime calls a service's natives:
 * a call through a pointer a little past null (t_call_small) is said to be one, lr a return address after it; a return
 * through a frame whose saved lr was written over (t_return_smashed, lr in the library's data, as DANA's libjabe.so's was
 * when it ended itself: DexGuard's answer to a sum of its code that differs; t_return_smashed_sp, lr on the stack) is not:
 * the report says lr is no return address, and how much of the library's code the loader changed. It said a call through a
 * null pointer for libjabe.so (0.1.0.678).
 */
static char g_small_pc_report[1100];
static mg_log_sink g_small_pc_before;
static void *g_small_pc_before_user;

static void small_pc_sink(int level, const char *line, void *user)
{
    (void)user;
    if (strstr(line, "fault: ") && strstr(line, "at pc 00000014")) mg_strlcpy(g_small_pc_report, line, sizeof(g_small_pc_report));
    if (g_small_pc_before) g_small_pc_before(level, line, g_small_pc_before_user);
}

static void test_small_pc_report(mg_lib *lib, const char *what)
{
    static const char *const names[] = { "t_call_small", "t_return_smashed", "t_return_smashed_sp" };
    char changed[96];
    int i;
    say("the crash report of a thread at a small pc, %s: a call through a pointer near null; a return through a saved lr written over", what);
    if (!mg_guest_tp_get()) {
        say("  (this thread has never run Android code: no guard can be had for the faults)");
        CHECK(mg_guest_tp_get() != NULL);
        return;
    }
    snprintf(changed, sizeof(changed), "the loader changed %s's code in ", mg_ld_lib_name(lib));
    mg_log_get_sink(&g_small_pc_before, &g_small_pc_before_user);
    mg_log_set_sink(small_pc_sink, NULL);
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        void *fn = SYM(lib, names[i]);
        bool faulted = false;
        g_small_pc_report[0] = 0;
        CHECK(fn != NULL);
        if (!fn) continue;
        (void)mg_jni_call_native_guarded(fn, NULL, "()I", NULL, &faulted);
        CHECK(faulted);
        CHECK(strstr(g_small_pc_report, "executing at pc 00000014") != NULL);
        if (i == 0) {
            CHECK(strstr(g_small_pc_report, "a call through a null pointer") != NULL);
            CHECK(strstr(g_small_pc_report, "\n  called from ") != NULL && strstr(g_small_pc_report, "not a call") == NULL);
        } else {
            CHECK(strstr(g_small_pc_report, "a call through a null pointer") == NULL);
            CHECK(strstr(g_small_pc_report, "not a call: lr is no return address") != NULL);
        }
        /* lr in the library's data: named, and what the loader changed of the library's code said. */
        if (i == 1) CHECK(strstr(g_small_pc_report, "\n  lr: ") != NULL && strstr(g_small_pc_report, changed) != NULL);
    }
    mg_log_set_sink(g_small_pc_before, g_small_pc_before_user);
}
#endif

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
#if MG_ARCH_ARM32
    CHECK(st.thumb_functions > 20 && st.arm_functions == 0);
    test_library(basic, "Thumb-2", scratch);
#else
    test_library(basic, MG_GUEST_ABI, scratch);
#endif

    /* Loaded first as an app asks with android_dlopen_ext: into a range it reserved; the PLT's tests below run on it there. */
    say("guest library: loaded into a range the app reserved (android_dlopen_ext, ANDROID_DLEXT_RESERVED_ADDRESS)");
    CHECK_INT(FN(basic, int (*)(void), "t_dlext")(), 7);
    dep = mg_ld_load("libt_dep.so");
    CHECK(dep != NULL);
    if (dep) {
        u32 before = mg_core_plt_rescues();
        say(MG_ARCH_ARM32 ? "guest library: calls through a rewritten PLT" : "guest library: calls through a PLT");
        CHECK(mg_ld_init(dep));
        mg_ld_get_stats(dep, &st);
#if MG_ARCH_ARM32
        CHECK_INT(st.plt_entries, 5);
#endif
        CHECK_INT(FN(dep, int (*)(int, int), "d_add_twice")(1, 2), 5);
        CHECK_INT(FN(dep, int (*)(void), "d_bump")(), 6);
        {
            int *counter = FN(basic, int *(*)(void), "t_counter_ptr")();
            CHECK(counter && *counter == 6);
        }
        CHECK_INT(FN(dep, int (*)(void), "d_hello_len")(), 16);
        CHECK(near(mg_gd(FN(dep, gdouble (*)(gdouble, gfloat), "d_fmul")(mg_gdb(2.0), mg_gfb(3.0f))), 7.0));
        CHECK_INT(FN(dep, int (*)(void), "d_value")(), 0x1234);
        CHECK(FN(dep, void *(*)(void), "d_add_ptr")() == SYM(basic, "t_add"));
        CHECK_INT(FN(dep, int (*)(int), "d_spin")(2000000), 2000000);
#if MG_ARCH_ARM32
        say("  PLT entries rescued after an interrupt: %u", mg_core_plt_rescues() - before);
#else
        (void)before;
#endif
    }

    CHECK(mg_ld_find_lib("libt_basic.so") == basic && mg_ld_lib_of(SYM(basic, "t_add")) == basic);
    CHECK(mg_ld_load("libc.so") == NULL && mg_ld_load("libnothere.so") == NULL);
    test_guest_kinds(libdir);
    test_unresolved_imports();
#if MG_ARCH_ARM32
    test_small_pc_report(basic, "Thumb-2");
#endif

#if MG_ARCH_ARM32
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
                /* The dead stub's svc: left in Thumb code, behind its branch; taken in ARM code, where every svc 0 is. */
                CHECK_INT(st.svc_sites, j == 0 ? 7 : 8);
                *mg_errno_ptr() = 123;
                CHECK_INT(FN(svc, int (*)(void), "t_svc_getpid")(), 4242);
                CHECK_INT(FN(svc, int (*)(void), "t_svc_literal")(), 4242);
                CHECK_INT(FN(svc, int (*)(void), "t_svc_unknown")(), -38);
                {
                    /* sysinfo by number, as Geekbench 5's own stub makes it: 0, and Linux's 32-bit struct sysinfo filled in
                       (totalram the 5th word, procs the low half of the 11th, mem_unit the 14th). */
                    u32 info[16];
                    memset(info, 0xEE, sizeof(info));
                    CHECK_INT(FN(svc, int (*)(void *), "t_svc_sysinfo")(info), 0);
                    CHECK(info[4] != 0 && info[4] != 0xEEEEEEEEu);
                    CHECK_INT((int)(info[10] & 0xFFFF), 1);
                    CHECK_INT((int)info[13], 1);
                }
                /* An svc whose number was set two instructions back (Chromium's syscall()). */
                CHECK_INT(FN(svc, int (*)(void), "t_svc_far_number")(), 4242);
                CHECK_INT(FN(svc, int (*)(void), "t_svc_dead_stub")(), 7);
                {
                    /* Read-only data in the executable segment: words that look like svc stay as the file has them. */
                    const u32 *data = (const u32 *)SYM(svc, "t_svc_rodata");
                    CHECK(data && data[0] == 0xEF000000u && data[1] == 0xDF002714u);
                }
                if (j == 0) {
                    /* As the file has it: libmetasec_ov.so sums such code, and ends the process when it has changed. */
                    const u8 *stub = (const u8 *)((mg_addr)SYM(svc, "t_svc_dead_stub") & ~(mg_addr)1);
                    bool kept = false;
                    int k;
                    for (k = 0; k < 16; k += 2) kept |= mg_rd16(stub + k) == 0xDF00;
                    CHECK(kept);
                }
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
            CHECK_INT(st.svc_sites, 7);
            fn = (const u8 *)((mg_addr)SYM(svc, "t_svc_getpid") & ~(mg_addr)1);
            for (k = 0; k < 16; k += 2) {
                trapped |= mg_rd16(fn + k) == 0xDEA5;
                left |= mg_rd16(fn + k) == 0xDF00;
                CHECK(!mg_patch_svc_target((mg_addr)(fn + k), &target));
            }
            CHECK(trapped && !left);
        }
    }
#elif MG_ARCH_ARM64
    /*
     * ARM64's svc 0 of a library's own, led out to the core through pieces of its island (arm64/mg_patch_arm64.c), not a
     * trap: on Windows a trap is an exception on the guest's stack, dispatched by ntdll's code (GitHub issue 4). What each
     * call must come to is in core/tests/guest/t_svc.c: results, errno, every register but x0 as the kernel keeps them,
     * a signal's handler with a call of its own inside one, goroutines' stacks, and the piece's trap where a thread has
     * no stack for system calls. Counted: 1310 led out (5 + 1 + 2 + 100 + 4 * 300 + 2), the 500 on goroutines' stacks and
     * the signal let through on one made on the threads' stacks for system calls; the trap's call is not counted.
     */
    {
        mg_lib *svc;
        mg_patch_set_trap_svc(false);
        svc = mg_ld_load("libt_svc.so");
        CHECK(svc != NULL);
        if (svc) {
            int saved = *mg_errno_ptr();
            u32 made0 = 0, on0 = 0, made = 0, on = 0;
            say("guest library: ARM64's svc led out to the core");
            mg_ld_get_stats(svc, &st);
            CHECK_INT(st.svc_sites, 3);
            CHECK_INT(st.svc_led_out, 3);
            /* The writes of x18 the loader counts and says (Windows' TEB): t_svc64_x18_writer's ten, never run, and not
               the two words of data after them that read as two more (its mapping symbols say they are data: twelve
               without them); none in the code clang makes for Android, which keeps x18 (libt_basic.so). */
            CHECK_INT(st.x18_writes, 10);
            {
                mg_ld_stats b;
                mg_ld_get_stats(basic, &b);
                CHECK_INT(b.x18_writes, 0);
            }
            /*
             * The reads of CTR_EL0, the cache's type, which Windows refuses in user mode (Telegram's WebView ended on V8's,
             * GitHub issue 4): t_svc64_ctr's and t_svc64_flush's two (one into xzr) made MG_ARM64_CTR_EL0's, and the flush
             * steps by the 64-byte lines it says. And what the fault handler does for one it finds (mg_patch_sys_emulate):
             * the read, the data cache's cleans and the instruction cache's invalidation by address, nothing else.
             */
            CHECK_INT(st.ctr_reads, 3);
            CHECK(FN(svc, u64 (*)(void), "t_svc64_ctr")() == MG_ARM64_CTR_EL0);
            {
                static u8 code[300];
                CHECK(FN(svc, u64 (*)(void *, long), "t_svc64_flush")(code + 7, 250) == ((u64)64 << 32 | 64));
            }
            {
                mg_addr regs[33];
                static u8 line[128];
                memset(regs, 0, sizeof(regs));
                regs[3] = (mg_addr)(line + 70);
                CHECK(mg_patch_sys_emulate(0xD53B0020u | 5u, regs) && regs[5] == MG_ARM64_CTR_EL0);          /* mrs x5, ctr_el0 */
                CHECK(mg_patch_sys_emulate(0xD53B0020u | 31u, regs));                                        /* mrs xzr, ctr_el0 */
                CHECK(mg_patch_sys_emulate(0xD50B7E20u | 3u, regs) && mg_patch_sys_emulate(0xD50B7B20u | 3u, regs) &&
                      mg_patch_sys_emulate(0xD50B7A20u | 3u, regs));                                         /* dc civac, cvau, cvac */
                CHECK(mg_patch_sys_emulate(0xD50B7520u | 3u, regs) && regs[3] == (mg_addr)(line + 70));      /* ic ivau, x3 */
                CHECK(!mg_patch_sys_emulate(0xD50B7420u | 3u, regs));                                        /* dc zva: memory zeroed, not this */
                CHECK(!mg_patch_sys_emulate(0xD5380000u | 1u, regs) && regs[1] == 0);                        /* mrs x1, midr_el1 */
                CHECK(!mg_patch_sys_emulate(0xD53BD040u | 1u, regs));                                        /* mrs x1, tpidr_el0 */
                CHECK(!mg_patch_sys_emulate(0xD503201Fu, regs));                                             /* nop */
            }
            *mg_errno_ptr() = 123;
            mg_core_svc_calls(&made0, &on0);
            CHECK_INT(FN(svc, long (*)(void), "t_svc64_results")(), 0);
            CHECK_INT(FN(svc, long (*)(void), "t_svc64_regs")(), 0);
            CHECK_INT(FN(svc, long (*)(void), "t_svc64_signal")(), 0);
            CHECK_INT(FN(svc, long (*)(void), "t_svc64_stacks")(), 0);
            CHECK_INT(FN(svc, long (*)(void), "t_svc64_fallback")(), 0);
            mg_core_svc_calls(&made, &on);
            say("  %u calls led out, %u of them on a stack for system calls", made - made0, on - on0);
            CHECK(made - made0 == 1310 && on - on0 >= 501 && on - on0 < made - made0);
            CHECK_INT(*mg_errno_ptr(), 123);
            *mg_errno_ptr() = saved;
        }
    }
    /* The flag trap-svc: every site a trap that is not answered (UDF 0xa5), none led out, none a dispatch trap. */
    {
        mg_lib *svc;
        mg_patch_set_trap_svc(true);
        svc = mg_ld_load("libt_svc_diag.so");
        mg_patch_set_trap_svc(false);
        CHECK(svc != NULL);
        if (svc) {
            const u8 *fn = (const u8 *)SYM(svc, "t_svc64_call");
            bool trapped = false, left = false;
            mg_addr target;
            int k;
            mg_ld_get_stats(svc, &st);
            CHECK_INT(st.svc_sites, 3);
            CHECK_INT(st.svc_led_out, 0);
            CHECK_INT(st.x18_writes, 10);
            CHECK(fn != NULL);
            for (k = 0; fn && k < 36; k += 4) {
                trapped |= mg_rd32(fn + k) == 0x000000A5u;
                left |= mg_rd32(fn + k) == 0xD4000001u;
                CHECK(!mg_patch_svc_target((mg_addr)(fn + k), &target));
            }
            CHECK(trapped && !left);
        }
    }
#endif

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
#if !MG_ARCH_X86
    test_go_stack();
#endif
    test_go_reserve();
}

#if MG_ARCH_ARM32
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
        test_small_pc_report(arm, "ARM mode");
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
#else
/*
 * The same library with packed relocations (Android's APS2 and RELR) and GNU hash only, its thread-local variables
 * found through __tls_get_addr (global-dynamic): the loader's paths libt_basic_arm.so takes on 32-bit ARM, on this ABI's
 * relocations (tools/build-guest.sh). Last, as that one is.
 */
static void test_guest_packed(const char *scratch)
{
    mg_lib *packed = mg_ld_load("libt_basic_packed.so");
    CHECK(packed != NULL);
    if (packed) {
        CHECK(mg_ld_init(packed));
        test_library(packed, "packed relocations, global-dynamic TLS", scratch);
    }
}
#endif

#include "mg_tests_app.inc"

#endif /* MG_GUEST_CODE */

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
 * layer, its locks kept in the process and on the file for other processes,
 * its mutexes the host's. On every host,
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

int mg_tests_sqlite_hold(const char *db, const char *flag)
{
    sqlite3 *h = NULL;
    mg_stat st;
    int tries, rc;
    if (!mg_sqlite_start() || sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) return 1;
    rc = sqlite3_exec(h, "BEGIN IMMEDIATE; INSERT INTO t(name) VALUES('held');", NULL, NULL, NULL);
    if (rc == SQLITE_OK && !mg_file_write_all(flag, "held", 4)) rc = SQLITE_IOERR;
    for (tries = 0; rc == SQLITE_OK && tries < 6000 && mg_path_stat(flag, &st, NULL); tries++) mg_sleep_ns(10000000ull);
    if (rc == SQLITE_OK) rc = sqlite3_exec(h, "ROLLBACK", NULL, NULL, NULL);
    if (rc == SQLITE_OK && sq_int(h, "SELECT count(*) FROM t WHERE name = 'held'") != 0) rc = SQLITE_CORRUPT;
    sqlite3_close(h);
    return rc == SQLITE_OK ? 0 : 1;
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
    /*
     * Another process's connection, as a handle of its own holding SQLite's locks where SQLite takes them (a handle's
     * locks bind every other, as Windows' handles' and Linux's open files' do): its writer (RESERVED's byte) keeps this
     * connection from beginning to write and lets it read; its writer waiting (PENDING's byte) keeps a new reader out;
     * its reader (the readers' range, shared) keeps this one from committing. And this connection's own locks, as the
     * other handle sees them: a writer's RESERVED byte, then nothing once it is done. Google Play services' databases are
     * every app's, open in several processes at once.
     */
    {
        enum { PENDING = 0x40000000, RESERVED = PENDING + 1, SHARED_FIRST = PENDING + 2, SHARED_SIZE = 510 };
        int err = 0;
        mg_file *other = mg_file_open(path, MG_O_READ | MG_O_WRITE, &err);
        say("SQLite: the locks of another process's connection, and this one's as another sees them");
        CHECK(other != NULL);
        a = NULL;
        CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
        sqlite3_busy_timeout(a, 0);
        if (other) {
            CHECK(mg_file_lock(other, RESERVED, 1, true, &err));
            CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE", NULL, NULL, NULL), SQLITE_BUSY);
            CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), 5);
            mg_file_unlock(other, RESERVED, 1);
            CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE; INSERT INTO t(name) VALUES('six'); COMMIT", NULL, NULL, NULL), SQLITE_OK);
            CHECK(mg_file_lock(other, PENDING, 1, true, &err));
            CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), -999);
            mg_file_unlock(other, PENDING, 1);
            CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), 6);
            CHECK(mg_file_lock(other, SHARED_FIRST, SHARED_SIZE, false, &err));
            CHECK_INT(sqlite3_exec(a, "INSERT INTO t(name) VALUES('seven')", NULL, NULL, NULL), SQLITE_BUSY);
            mg_file_unlock(other, SHARED_FIRST, SHARED_SIZE);
            CHECK_INT(sqlite3_exec(a, "INSERT INTO t(name) VALUES('seven')", NULL, NULL, NULL), SQLITE_OK);
            CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE", NULL, NULL, NULL), SQLITE_OK);
            CHECK(mg_file_locked_elsewhere(other, RESERVED, 1) && !mg_file_lock(other, RESERVED, 1, true, &err) && err == MG_E_BUSY);
            /* (As the app is suspended the shell waits for such a write to end, and gives up after the time it gives.) */
            CHECK(mg_sqlite_locks_held() == 1 && !megler_settle_databases(20));
            CHECK_INT(sqlite3_exec(a, "COMMIT", NULL, NULL, NULL), SQLITE_OK);
            CHECK(mg_sqlite_locks_held() == 0 && megler_settle_databases(20));
            CHECK(!mg_file_locked_elsewhere(other, RESERVED, 1) && !mg_file_locked_elsewhere(other, PENDING, 1) &&
                  !mg_file_locked_elsewhere(other, SHARED_FIRST, SHARED_SIZE));
            mg_file_close(other);
        }
        sqlite3_close(a);
    }
#ifdef _WIN32
    /*
     * And another process, for real: this program started again (--sqlite-hold), a write of its under way in the
     * database, its journal there. This process reads beside it, taking the journal for that writer's and not for one
     * left behind (which it would roll back under the writer), cannot begin to write, and can once the other has rolled
     * its write back, which went as it should.
     */
    {
        char *flag = mg_sprintf("%s/sqlite-held.flag", scratch), *exe = NULL, *qexe, *qdb, *qflag;
        intptr_t child;
        int tries, status = -1;
        say("SQLite: a write of another process's");
        mg_path_remove(flag, NULL);
        /* (The other process is told what it is, and starts none of its own whatever it runs.) */
        if (!getenv("MG_TESTS_SQLITE_CHILD")) _get_pgmptr(&exe);
        _putenv("MG_TESTS_SQLITE_CHILD=1");
        qexe = mg_sprintf("\"%s\"", exe ? exe : "");
        qdb = mg_sprintf("\"%s\"", path);
        qflag = mg_sprintf("\"%s\"", flag);
        child = exe ? _spawnl(_P_NOWAIT, exe, qexe, "--sqlite-hold", qdb, qflag, NULL) : -1;
        CHECK(child != -1);
        for (tries = 0; tries < 3000 && child != -1 && !mg_path_stat(flag, &fs, NULL); tries++) mg_sleep_ns(10000000ull);
        CHECK(mg_path_stat(flag, &fs, NULL) && mg_path_stat(journal, &fs, NULL));
        a = NULL;
        CHECK_INT(sqlite3_open_v2(path, &a, SQLITE_OPEN_READWRITE, NULL), SQLITE_OK);
        sqlite3_busy_timeout(a, 0);
        CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), 7);
        CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE", NULL, NULL, NULL), SQLITE_BUSY);
        CHECK(mg_path_stat(journal, &fs, NULL));
        mg_path_remove(flag, NULL);
        if (child != -1) _cwait(&status, child, 0);
        _putenv("MG_TESTS_SQLITE_CHILD=");
        say("  the other process ended with %d", status);
        CHECK(status == 0);
        CHECK_INT(sqlite3_exec(a, "BEGIN IMMEDIATE; INSERT INTO t(name) VALUES('eight'); COMMIT", NULL, NULL, NULL), SQLITE_OK);
        CHECK_INT(sq_int(a, "SELECT count(*) FROM t"), 8);
        sqlite3_close(a);
        free(qflag);
        free(qdb);
        free(qexe);
        free(flag);
    }
#endif
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
int mg_tests_sqlite_hold(const char *db, const char *flag)
{
    (void)db;
    (void)flag;
    return 1;
}

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
    test_lock_holders();
    test_log_sink_past_lock();
    test_unlink_open(scratch);
#if defined(MG_DL_TESTS)
    test_dl_open_why();
#endif
    g_failed += mg_storage_tests_run(scratch, g_sink, g_sink_user, &g_checks);
    g_failed += mg_net_tests_run(g_sink, g_sink_user, &g_checks);
    g_failed += mg_sensor_tests_run(g_sink, g_sink_user, &g_checks);
    g_failed += mg_bluetooth_tests_run(g_sink, g_sink_user, &g_checks);
    g_failed += mg_wifi_tests_run(g_sink, g_sink_user, &g_checks);
    test_zip(scratch);
    test_place_libs(scratch);
    test_sqlite(scratch);
    test_textures();
    test_media(scratch);
    test_codecs();
    test_camera();
    test_battery();
    test_libc_odds();
    test_malloc_threads();
    test_appx_manifest();
    test_appx_pri();
    test_arsc(libdir);
    test_kuser();
    test_palette();
    test_syscall_fds(scratch);
    test_mmap_commit();
    test_mmap_file(scratch);
    test_mmap_file_written(scratch);
    test_mmap_write_back_unlocked(scratch);
    test_mmap_lock_order(scratch);
    test_libc_gaps(scratch);
    test_libm_rounding();
    test_mmap_madvise(scratch);
    test_pread_threads(scratch);
    test_rename_in_use(scratch);
    test_rename_dir_in_use(scratch);
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
#if MG_GUEST_CODE
    test_proxies();
    if (can_run) {
#if MG_ARCH_ARM32
#if !MG_HOST_WINDOWS
        /* (First, while this process has few threads of its own: the test forks.) */
        test_chromium_oom_end();
#endif
        test_mechanisms();
        test_suspend_arm();
#endif
        if (libdir) {
            test_guest(libdir, scratch);
            test_app(libdir);
            test_dvm(libdir, scratch, true);
#if MG_ARCH_ARM32
            test_guest_arm(scratch);
#else
            test_guest_packed(scratch);
#endif
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
    say("(this build runs no guest code -- MSVC's x64 build, Windows' convention: the guest-code tests are skipped)");
    test_dvm(libdir, scratch, false);
#endif
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
