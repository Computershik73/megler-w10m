/*
 * The phone's own folders in an app's storage, tried without a phone.
 *
 * On a phone the user's music, pictures and videos are files Windows will
 * not open by path for an app; its broker hands over handles for them, and
 * the shell passes those on (megler_storage_host). Here a stand-in does the
 * same with folders under the tests' scratch folder: it opens them with the
 * operating system's plain calls and hands the handles over. Everything
 * between that and an app -- the host layer's foreign paths, the shared
 * storage's layout, the C library an app calls -- is the code a phone runs.
 * The C library's functions are called here as an app's code calls them, by
 * the names it binds against (mg_bionic_find), with bionic's structures.
 *
 * What a stand-in cannot say is whether the broker's handles do all that a
 * plain one does. core/tests is not where that is found out.
 */
#include "mg_storage_tests.h"

#include <stdio.h>

#include "../include/megler.h"
#include "../src/bionic/mg_bionic.h"
#include "../src/mg_base.h"
#include "../src/host/mg_host.h"
#include "../src/run/mg_run.h"


static int t_checks, t_failed;
static mg_test_sink t_sink;
static void *t_sink_user;
/* The check for a phone (below), run among the tests' own lines: only its steps that fail are said, each with where it was. */
static bool g_check_quiet;
static char g_check_where[240];

static void say(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (t_sink) t_sink(line, t_sink_user);
    else {
        fputs(line, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}

#define CHECK(cond)                                                    \
    do {                                                               \
        t_checks++;                                                    \
        if (!(cond)) {                                                 \
            t_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

#define CHECK_INT(expr, want)                                                                      \
    do {                                                                                           \
        long long got_ = (long long)(expr), want_ = (long long)(want);                             \
        t_checks++;                                                                                \
        if (got_ != want_) {                                                                       \
            t_failed++;                                                                            \
            say("  FAIL %s:%d: %s = %lld, expected %lld", __FILE__, __LINE__, #expr, got_, want_); \
        }                                                                                          \
    } while (0)

/* ------------------------------------------------- the stand-in for the shell
 *
 * Root n is the folder g_roots[n]. A handle is asked of the operating system
 * by path, which the stand-in may do and a phone's shell may not; the three
 * things done by name go through the host layer's own functions for ordinary
 * paths, whose answers are the ones a caller expects.
 */
#define FAKE_ROOTS 3
static char *g_roots[FAKE_ROOTS];

static char *fake_path(int root, const char *rel)
{
    if (root < 0 || root >= FAKE_ROOTS || !g_roots[root]) return NULL;
    return rel[0] ? mg_sprintf("%s/%s", g_roots[root], rel) : mg_strdup(g_roots[root]);
}

/* The operating system's own handle for a file or folder of the scratch folder, as the shell's storage hands one out. */
static void *fake_handle(const char *path, int flags, int kind, int *err) { return mg_host_open_handle(path, flags, kind, err); }

static MEGLER_CB void *fake_open(void *user, int root, const char *rel, int flags, int kind, int *err)
{
    char *path = fake_path(root, rel);
    void *h;
    (void)user;
    if (!path) {
        *err = MG_E_NOENT;
        return NULL;
    }
    h = fake_handle(path, flags, kind, err);
    free(path);
    return h;
}

static MEGLER_CB bool fake_mkdir(void *user, int root, const char *rel, int *err)
{
    char *path = fake_path(root, rel);
    bool ok;
    (void)user;
    if (!path) {
        *err = MG_E_NOENT;
        return false;
    }
    ok = mg_path_mkdir(path, err);
    free(path);
    return ok;
}

static MEGLER_CB bool fake_remove(void *user, int root, const char *rel, bool dir, int *err)
{
    char *path = fake_path(root, rel);
    bool ok;
    (void)user;
    if (!path) {
        *err = MG_E_NOENT;
        return false;
    }
    ok = dir ? mg_path_rmdir(path, err) : mg_path_remove(path, err);
    free(path);
    return ok;
}

/* Set, the stand-in moves no folder into another folder: what a phone's shell is expected to say of that, tried here. */
static bool g_fake_moves_no_folder;

static MEGLER_CB bool fake_rename(void *user, int from_root, const char *from, int to_root, const char *to, int *err)
{
    char *a = fake_path(from_root, from), *b = fake_path(to_root, to);
    const char *from_name = strrchr(from, '/'), *to_name = strrchr(to, '/');
    size_t from_dir = from_name ? (size_t)(from_name - from) : 0, to_dir = to_name ? (size_t)(to_name - to) : 0;
    mg_stat st;
    bool ok = false;
    (void)user;
    if (!a || !b) *err = MG_E_NOENT;
    else if (g_fake_moves_no_folder && (from_root != to_root || from_dir != to_dir || strncmp(from, to, from_dir)) && mg_path_stat(a, &st, NULL) && st.is_dir) *err = MG_E_XDEV;
    else ok = mg_path_rename(a, b, err);
    free(a);
    free(b);
    return ok;
}

/*
 * Who is asked: the stand-in, or the shell's own storage host where the
 * tests' program has it (mg_storage_tests_run_shell). Either way through
 * these, which count the calls: some checks are that nobody was asked.
 */
static megler_storage_host g_inner;
static volatile i32 g_calls;

static MEGLER_CB MG_HOSTCB void *counted_open(void *user, int link, const char *path, int flags, int kind, int *err)
{
    (void)user;
    mg_atomic_add(&g_calls, 1);
    return g_inner.open(g_inner.user, link, path, flags, kind, err);
}

static MEGLER_CB MG_HOSTCB bool counted_mkdir(void *user, int link, const char *path, int *err)
{
    (void)user;
    mg_atomic_add(&g_calls, 1);
    return g_inner.make_folder(g_inner.user, link, path, err);
}

static MEGLER_CB MG_HOSTCB bool counted_remove(void *user, int link, const char *path, bool dir, int *err)
{
    (void)user;
    mg_atomic_add(&g_calls, 1);
    return g_inner.remove(g_inner.user, link, path, dir, err);
}

static MEGLER_CB MG_HOSTCB bool counted_rename(void *user, int from_link, const char *from, int to_link, const char *to, int *err)
{
    (void)user;
    mg_atomic_add(&g_calls, 1);
    return g_inner.rename(g_inner.user, from_link, from, to_link, to, err);
}

static const mg_foreign k_counted = { NULL, counted_open, counted_mkdir, counted_remove, counted_rename };

/* ------------------------------------------------------- the host layer */

typedef struct listing {
    int count, dirs;
    char names[16][64];
} listing;

static MG_HOSTCB void list_one(const char *name, bool is_dir, void *user)
{
    listing *l = (listing *)user;
    if (l->count < 16) mg_strlcpy(l->names[l->count], name, sizeof(l->names[0]));
    l->count++;
    if (is_dir) l->dirs++;
}

static bool listed(const listing *l, const char *name)
{
    int i;
    for (i = 0; i < l->count && i < 16; i++)
        if (!strcmp(l->names[i], name)) return true;
    return false;
}

/* Empties a folder of the tests' own, by ordinary paths: what an earlier run that did not finish left there. */
typedef struct wipe_list {
    char *names[64];
    bool dirs[64];
    int count;
} wipe_list;

static MG_HOSTCB void wipe_one(const char *name, bool is_dir, void *user)
{
    wipe_list *w = (wipe_list *)user;
    if (w->count >= 64) return;
    w->names[w->count] = mg_strdup(name);
    w->dirs[w->count++] = is_dir;
}

static void wipe(const char *dir, int depth)
{
    int rounds;
    /* (A folder with more than 64 entries takes several rounds.) */
    for (rounds = 0; rounds < 16; rounds++) {
        wipe_list w;
        int i;
        memset(&w, 0, sizeof(w));
        if (!mg_path_list(dir, wipe_one, &w, NULL) || !w.count) return;
        for (i = 0; i < w.count; i++) {
            char *path = mg_sprintf("%s/%s", dir, w.names[i]);
            if (w.dirs[i]) {
                if (depth < 8) wipe(path, depth + 1);
                mg_path_rmdir(path, NULL);
            } else {
                mg_path_remove(path, NULL);
            }
            free(path);
            free(w.names[i]);
        }
    }
}

/* What a path holds, as a string; "" when it cannot be read. The caller frees it. */
static char *text_of(const char *path)
{
    size_t n = 0;
    u8 *all = mg_file_read_all(path, &n);
    return all ? (char *)all : mg_strdup("");
}

#define L0 MG_FOREIGN_PREFIX "0"
#define L1 MG_FOREIGN_PREFIX "1"

static void test_foreign_paths(const char *scratch)
{
    mg_stat st;
    mg_file *f;
    listing seen;
    char *text, *plain;
    int err = 0, i;
    i32 calls;
    say("foreign paths: files the host layer is handed, not allowed to open");

    /* Nobody answers yet: such a path is nowhere. */
    mg_foreign_set(NULL);
    CHECK(mg_path_is_foreign(L0) && mg_path_is_foreign(L0 "/a") && !mg_path_is_foreign(scratch) && !mg_path_is_foreign("megler-link"));
    CHECK(!mg_path_stat(L0, &st, &err) && err == MG_E_NOENT);
    CHECK(mg_file_open(L0 "/a.txt", MG_O_READ, &err) == NULL && err == MG_E_NOENT);
    CHECK(!mg_path_mkdir(L0 "/d", &err) && err == MG_E_NOENT);

    mg_foreign_set(&k_counted);
    CHECK(mg_path_stat(L0, &st, &err) && st.is_dir);
    CHECK(mg_path_stat(L1, &st, &err) && st.is_dir);
    /* A root the stand-in does not have, and a file that is not there. */
    CHECK(!mg_path_stat(MG_FOREIGN_PREFIX "7", &st, &err) && err == MG_E_NOENT);
    CHECK(!mg_path_stat(L0 "/nothing", &st, &err) && err == MG_E_NOENT);
    CHECK(mg_file_open(L0 "/nothing", MG_O_READ, &err) == NULL && err == MG_E_NOENT);

    /* Paths that could mean another place to the operating system never reach whoever answers. (A backslash or a colon is part of a name, mapped: test_odd_names.) */
    calls = g_calls;
    {
        static const char *const bad[] = {
            L0 "/../x", L0 "/a/../x", L0 "/./x", L0 "/a/.", L0 "//x", L0 "/a//b", L0 "/a/", L0 "x",
            MG_FOREIGN_PREFIX "x/a", MG_FOREIGN_PREFIX "", MG_FOREIGN_PREFIX "/a",
        };
        for (i = 0; i < (int)MG_COUNT(bad); i++) {
            err = 0;
            CHECK(!mg_path_stat(bad[i], &st, &err) && err == MG_E_INVAL);
            err = 0;
            CHECK(mg_file_open(bad[i], MG_O_WRITE | MG_O_CREATE, &err) == NULL && err == MG_E_INVAL);
        }
        err = 0;
        CHECK(!mg_path_mkdir(L0 "/../made", &err) && err == MG_E_INVAL);
        CHECK(!mg_path_remove(L0 "/a/../b", &err) && err == MG_E_INVAL);
        CHECK(!mg_path_rename(L0 "/a", L0 "/../b", &err) && err == MG_E_INVAL);
    }
    CHECK_INT(g_calls - calls, 0);

    /* A file written through the layer is in the root's folder, and reads back both ways. */
    CHECK(mg_file_write_all(L0 "/a.txt", "0123456789", 10));
    plain = mg_sprintf("%s/a.txt", g_roots[0]);
    text = text_of(plain);
    CHECK(!strcmp(text, "0123456789"));
    free(text);
    text = text_of(L0 "/a.txt");
    CHECK(!strcmp(text, "0123456789"));
    free(text);
    CHECK(mg_path_stat(L0 "/a.txt", &st, &err) && !st.is_dir && st.size == 10);
    CHECK(st.mtime_ns > 1600000000ll * 1000000000ll);

    /* The handle is a file like any other: positions, positioned reads, its length cut, what it says of itself. */
    f = mg_file_open(L0 "/a.txt", MG_O_READ | MG_O_WRITE, &err);
    CHECK(f != NULL && err == MG_E_OK);
    if (f) {
        char buf[16];
        CHECK_INT(mg_file_pread(f, buf, 4, 3, &err), 4);
        CHECK(!memcmp(buf, "3456", 4));
        CHECK_INT(mg_file_read(f, buf, 2, &err), 2);
        CHECK(!memcmp(buf, "01", 2));
        CHECK_INT(mg_file_seek(f, -2, 2, &err), 8);
        CHECK_INT(mg_file_write(f, "XYZ", 3, &err), 3);
        CHECK_INT(mg_file_pwrite(f, "ab", 2, 0, &err), 2);
        CHECK(mg_file_stat(f, &st) && st.size == 11 && !st.is_dir);
        CHECK(mg_file_truncate(f, 9, &err));
        CHECK(mg_file_sync(f));
        mg_file_close(f);
    }
    text = text_of(plain);
    CHECK(!strcmp(text, "ab234567X"));
    free(text);
    /* Appending, and the flags that decide what an open does to what is there. */
    f = mg_file_open(L0 "/a.txt", MG_O_WRITE | MG_O_APPEND, &err);
    CHECK(f != NULL);
    if (f) {
        CHECK_INT(mg_file_write(f, "+", 1, &err), 1);
        mg_file_close(f);
    }
    CHECK(mg_file_open(L0 "/a.txt", MG_O_WRITE | MG_O_CREATE | MG_O_EXCL, &err) == NULL && err == MG_E_EXIST);
    text = text_of(L0 "/a.txt");
    CHECK(!strcmp(text, "ab234567X+"));
    free(text);
    f = mg_file_open(L0 "/a.txt", MG_O_WRITE | MG_O_TRUNC, &err);
    CHECK(f != NULL);
    mg_file_close(f);
    CHECK(mg_path_stat(L0 "/a.txt", &st, &err) && st.size == 0);
    CHECK(mg_file_write_all(L0 "/a.txt", "mapped", 6));
    {
        mg_map map;
        CHECK(mg_map_open(&map, L0 "/a.txt") && map.size == 6 && !memcmp(map.data, "mapped", 6));
        mg_map_close(&map);
        CHECK(!mg_map_open(&map, L0 "/nothing"));
    }

    /* Folders: made one at a time and all at once, listed through a handle, in the way of a file. */
    CHECK(mg_path_mkdir(L0 "/dir", &err));
    CHECK(!mg_path_mkdir(L0 "/dir", &err) && err == MG_E_EXIST);
    CHECK(!mg_path_mkdir(L0, &err) && err == MG_E_EXIST);
    CHECK(!mg_path_mkdir(L0 "/no/such/parent", &err) && err == MG_E_NOENT);
    CHECK(mg_path_mkdirs(L0 "/deep/er/still"));
    free(plain);
    plain = mg_sprintf("%s/deep/er/still", g_roots[0]);
    CHECK(mg_path_stat(plain, &st, &err) && st.is_dir);
    CHECK(mg_path_stat(L0 "/deep/er", &st, &err) && st.is_dir);
    CHECK(mg_file_write_all(L0 "/deep/er/inner.bin", "\0\1\2", 3));
    CHECK(mg_file_open(L0 "/dir", MG_O_READ, &err) == NULL && err == MG_E_ISDIR);
    CHECK(mg_file_open(L0, MG_O_READ, &err) == NULL && err == MG_E_ISDIR);
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L0, list_one, &seen, &err));
    CHECK(seen.count == 3 && seen.dirs == 2 && listed(&seen, "a.txt") && listed(&seen, "dir") && listed(&seen, "deep"));
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L0 "/deep/er", list_one, &seen, &err) && seen.count == 2 && seen.dirs == 1 && listed(&seen, "still") && listed(&seen, "inner.bin"));
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L0 "/dir", list_one, &seen, &err) && seen.count == 0);
    CHECK(!mg_path_list(L0 "/a.txt", list_one, &seen, &err) && err == MG_E_NOTDIR);
    CHECK(!mg_path_list(L0 "/nothing", list_one, &seen, &err) && err == MG_E_NOENT);
    /* More entries than one bufferful of them (the Win32 host reads a directory 16 KB at a time). */
    CHECK(mg_path_mkdir(L1 "/many", &err));
    for (i = 0; i < 300; i++) {
        char *name = mg_sprintf(L1 "/many/a-file-with-a-name-long-enough-to-fill-the-buffer-%03d.dat", i);
        if (!mg_file_write_all(name, "x", 1)) break;
        free(name);
    }
    CHECK_INT(i, 300);
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L1 "/many", list_one, &seen, &err) && seen.count == 300 && seen.dirs == 0);
    for (i = 0; i < 300; i++) {
        char *name = mg_sprintf(L1 "/many/a-file-with-a-name-long-enough-to-fill-the-buffer-%03d.dat", i);
        if (!mg_path_remove(name, &err)) break;
        free(name);
    }
    CHECK_INT(i, 300);
    CHECK(mg_path_rmdir(L1 "/many", &err));

    /* Renaming: in a folder, to another, to another root, onto a file that is there; never a root, never out to an ordinary path. */
    CHECK(mg_path_rename(L0 "/a.txt", L0 "/b.txt", &err));
    CHECK(!mg_path_stat(L0 "/a.txt", &st, &err) && err == MG_E_NOENT && mg_path_stat(L0 "/b.txt", &st, &err) && st.size == 6);
    CHECK(mg_path_rename(L0 "/b.txt", L0 "/dir/c.txt", &err));
    CHECK(mg_path_rename(L0 "/dir/c.txt", L1 "/d.txt", &err));
    free(plain);
    plain = mg_sprintf("%s/d.txt", g_roots[1]);
    text = text_of(plain);
    CHECK(!strcmp(text, "mapped"));
    free(text);
    CHECK(mg_file_write_all(L1 "/e.txt", "newer", 5));
    CHECK(mg_path_rename(L1 "/e.txt", L1 "/d.txt", &err));
    text = text_of(L1 "/d.txt");
    CHECK(!strcmp(text, "newer") && !mg_path_stat(L1 "/e.txt", &st, NULL));
    free(text);
    CHECK(mg_path_rename(L0 "/deep", L0 "/dir/moved", &err) && mg_path_stat(L0 "/dir/moved/er/inner.bin", &st, &err) && st.size == 3);
    CHECK(!mg_path_rename(L0 "/nothing", L0 "/something", &err) && err == MG_E_NOENT);
    CHECK(!mg_path_rename(L0, L1 "/zero", &err) && err == MG_E_BUSY);
    CHECK(!mg_path_rename(L0 "/dir", L1, &err) && err == MG_E_BUSY);
    free(plain);
    plain = mg_sprintf("%s/outside.txt", scratch);
    CHECK(!mg_path_rename(L1 "/d.txt", plain, &err) && err == MG_E_XDEV);
    CHECK(mg_file_write_all(plain, "out", 3));
    CHECK(!mg_path_rename(plain, L1 "/inside.txt", &err) && err == MG_E_XDEV);
    CHECK(mg_path_stat(plain, &st, NULL) && mg_path_stat(L1 "/d.txt", &st, NULL) && !mg_path_stat(L1 "/inside.txt", &st, NULL));
    CHECK(mg_path_remove(plain, &err));

    /* Removing: a file as a file, a folder as a folder and only when it is empty; never a root. */
    CHECK(!mg_path_remove(L0 "/dir", &err) && err == MG_E_ISDIR);
    CHECK(!mg_path_rmdir(L0 "/dir", &err) && err == MG_E_NOTEMPTY);
    CHECK(!mg_path_rmdir(L1 "/d.txt", &err) && err == MG_E_NOTDIR);
    CHECK(!mg_path_remove(L1 "/nothing", &err) && err == MG_E_NOENT);
    CHECK(!mg_path_rmdir(L0, &err) && err == MG_E_BUSY);
    CHECK(!mg_path_remove(L0, &err) && err == MG_E_ISDIR);
    CHECK(mg_path_remove(L1 "/d.txt", &err) && !mg_path_stat(L1 "/d.txt", &st, NULL));
    CHECK(mg_path_remove(L0 "/dir/moved/er/inner.bin", &err));
    CHECK(mg_path_rmdir(L0 "/dir/moved/er/still", &err) && mg_path_rmdir(L0 "/dir/moved/er", &err) && mg_path_rmdir(L0 "/dir/moved", &err));
    CHECK(mg_path_rmdir(L0 "/dir", &err));
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L0, list_one, &seen, &err) && seen.count == 0);
    memset(&seen, 0, sizeof(seen));
    CHECK(mg_path_list(L1, list_one, &seen, &err) && seen.count == 0);
    free(plain);

    /* And when whoever answered is gone, so are the paths. */
    mg_foreign_set(NULL);
    CHECK(!mg_path_stat(L0, &st, &err) && err == MG_E_NOENT);
}

/* ------------------------------------------------- the storage an app sees
 *
 * The C library as an app's code has it. The structures are bionic's, as
 * mg_libc_io.c fills them in: of the build's processor (struct stat is laid
 * out three ways, x86_64's, ARM64's and the 32-bit ABIs'), in the guest's
 * widths (glong: 64 bits in 64-bit Android).
 */
#if MG_ARCH_X86_64
typedef struct t_stat {
    u64 st_dev, st_ino;
    gulong st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    i64 st_size;
    glong st_blksize, st_blocks;
    gtime at_sec;
    glong at_nsec;
    gtime mt_sec;
    glong mt_nsec;
    gtime ct_sec;
    glong ct_nsec;
    glong unused[3];
} t_stat;
#elif MG_ARCH_ARM64
typedef struct t_stat {
    u64 st_dev, st_ino;
    u32 st_mode, st_nlink, st_uid, st_gid;
    u64 st_rdev;
    gulong pad1;
    i64 st_size;
    i32 st_blksize, pad2;
    glong st_blocks;
    gtime at_sec;
    glong at_nsec;
    gtime mt_sec;
    glong mt_nsec;
    gtime ct_sec;
    glong ct_nsec;
    u32 unused4, unused5;
} t_stat;
#else
typedef struct t_stat {
    u64 st_dev;
    u8 pad0[4];
    u32 st_ino32;
    u32 st_mode, st_nlink, st_uid, st_gid;
    u64 st_rdev;
    u8 pad3[4];
    i64 st_size;
    u32 st_blksize;
    u64 st_blocks;
    gtime at_sec;
    glong at_nsec;
    gtime mt_sec;
    glong mt_nsec;
    gtime ct_sec;
    glong ct_nsec;
    u64 st_ino;
} t_stat;
#endif

typedef struct t_dirent {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[256];
} t_dirent;

/* (statfs's words but the counts and the id are 64 bits in 64-bit Android; statvfs's are all longs there, and 24 bytes more.) */
typedef struct t_statfs {
    gulong f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    gulong f_namelen, f_frsize, f_flags, f_spare[4];
} t_statfs;

typedef struct t_statvfs {
    gulong f_bsize, f_frsize;
    gulong f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
#if MG_GUEST_LP64
    u32 reserved[6];
#endif
} t_statvfs;

/* (O_DIRECTORY is the x86s' own number there, ARM's 040000 being their O_DIRECT.) */
enum { T_RDONLY = 0, T_WRONLY = 1, T_RDWR = 2, T_CREAT = 0100, T_EXCL = 0200, T_TRUNC = 01000, T_APPEND = 02000,
       T_DIRECTORY = MG_ARCH_X86_64 || MG_ARCH_X86 ? 0200000 : 040000 };
#define T_IFDIR 0040000u
#define T_IFREG 0100000u
#define T_DT_DIR 4
#define T_DT_REG 8

static struct {
    int (*open)(const char *, int, int);
    int (*close)(int);
    gssize (*read)(int, void *, size_t);
    gssize (*write)(int, const void *, size_t);
    int (*stat)(const char *, t_stat *);
    int (*fstat)(int, t_stat *);
    int (*access)(const char *, int);
    int (*mkdir)(const char *, int);
    int (*rmdir)(const char *);
    int (*unlink)(const char *);
    int (*rename)(const char *, const char *);
    void *(*opendir)(const char *);
    t_dirent *(*readdir)(void *);
    int (*closedir)(void *);
    int (*statfs)(const char *, t_statfs *);
    int (*statvfs)(const char *, t_statvfs *);
    char *(*realpath)(const char *, char *);
    int (*chdir)(const char *);
} C;

static bool libc_found(void)
{
    static const char *const names[] = { "open", "close", "read", "write", "stat", "fstat", "access", "mkdir", "rmdir", "unlink", "rename",
                                         "opendir", "readdir", "closedir", "statfs", "statvfs", "realpath", "chdir" };
    void **slot = (void **)&C;
    size_t i;
    for (i = 0; i < MG_COUNT(names); i++) {
        slot[i] = mg_bionic_find(names[i]);
        if (!slot[i]) return false;
    }
    return true;
}

static int last_errno(void) { return *mg_errno_ptr(); }

/* A file made (or made anew) with this in it. */
static bool put(const char *path, const char *text)
{
    int fd = C.open(path, T_WRONLY | T_CREAT | T_TRUNC, 0644);
    bool ok;
    if (fd < 0) return false;
    ok = C.write(fd, text, (u32)strlen(text)) == (i32)strlen(text);
    return C.close(fd) == 0 && ok;
}

/* What a file holds, in a buffer of the calling thread's; "" (and errno) when it cannot be read. */
static const char *get_into(char *buf, size_t cap, const char *path)
{
    int fd = C.open(path, T_RDONLY, 0);
    i32 n;
    buf[0] = 0;
    if (fd < 0) return buf;
    n = C.read(fd, buf, (u32)cap - 1);
    buf[n > 0 ? n : 0] = 0;
    C.close(fd);
    return buf;
}

static const char *get(const char *path)
{
    static char buf[256];
    return get_into(buf, sizeof(buf), path);
}

/* The errno an open ends with; 0 when it opens (and it is closed again). */
static int open_errno(const char *path, int flags)
{
    int fd = C.open(path, flags, 0644);
    if (fd < 0) return last_errno();
    C.close(fd);
    return 0;
}

static bool is_dir(const char *path)
{
    t_stat st;
    return C.stat(path, &st) == 0 && (st.st_mode & T_IFDIR) != 0;
}

static i64 size_of(const char *path)
{
    t_stat st;
    return C.stat(path, &st) == 0 && (st.st_mode & T_IFREG) ? st.st_size : -1;
}

static int stat_errno(const char *path)
{
    t_stat st;
    return C.stat(path, &st) == 0 ? 0 : last_errno();
}

/* A directory as readdir gives it: how many entries besides "." and "..", and the first of them by name. */
typedef struct dir_seen {
    int count;                         /* -1: it could not be opened */
    bool dots;                         /* "." and ".." were both there, once each */
    char names[48][128];
    u8 types[48];
} dir_seen;

static void read_dir(const char *path, dir_seen *s)
{
    void *d = C.opendir(path);
    t_dirent *e;
    int dot = 0, dotdot = 0;
    memset(s, 0, sizeof(*s));
    if (!d) {
        s->count = -1;
        return;
    }
    while ((e = C.readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".")) dot++;
        else if (!strcmp(e->d_name, "..")) dotdot++;
        else {
            if (s->count < 48) {
                mg_strlcpy(s->names[s->count], e->d_name, sizeof(s->names[0]));
                s->types[s->count] = e->d_type;
            }
            s->count++;
        }
    }
    s->dots = dot == 1 && dotdot == 1;
    C.closedir(d);
}

/* How often a name is in a listing, and as what. */
static int times(const dir_seen *s, const char *name)
{
    int i, n = 0;
    for (i = 0; i < s->count && i < 48; i++)
        if (!strcmp(s->names[i], name)) n++;
    return n;
}

static int type_of(const dir_seen *s, const char *name)
{
    int i;
    for (i = 0; i < s->count && i < 48; i++)
        if (!strcmp(s->names[i], name)) return s->types[i];
    return 0;
}

#define PKG "org.megler.test.storage"
#define SD "/storage/emulated/0"

/*
 * Clearing an app's data, as asked (megler_ask_clear_data writes <storage>/clear-data/<package>): before its folders are set
 * up, its private folder goes with everything in it, and its own Android/data folder; its Android/obb stays, as on Android,
 * and so does another app's data. Asked for no more, nothing goes.
 */
static void test_clear_data(const char *base)
{
    static const char *const made[] = { "apps/org.megler.test.cleared/data/shared_prefs/settings.xml", "apps/org.megler.test.cleared/data/databases/main.db",
                                        "apps/org.megler.test.cleared/sdcard/Android/data/org.megler.test.cleared/files/save.dat",
                                        "apps/org.megler.test.cleared/sdcard/Android/obb/org.megler.test.cleared/main.1.obb",
                                        "apps/org.megler.test.kept/data/shared_prefs/settings.xml", "clear-data/org.megler.test.cleared" };
    mg_stat st;
    char *data = mg_sprintf("%s/cleared", base), *p, *dir;   /* a folder of its own: the layout's test counts the apps it finds */
    size_t i;
    say("clearing an app's data, as asked: its private folder and its Android/data go, its obb and other apps' data stay");
    for (i = 0; i < MG_COUNT(made); i++) {
        p = mg_sprintf("%s/%s", data, made[i]);
        dir = mg_sprintf("%s", p);
        *strrchr(dir, '/') = 0;
        CHECK(mg_path_mkdirs(dir) && mg_file_write_all(p, "x", 1));
        free(dir);
        free(p);
    }
    CHECK(mg_run_clear_data_if_asked(data, data, "org.megler.test.cleared"));
    for (i = 0; i < MG_COUNT(made); i++) {
        bool kept = strstr(made[i], "/obb/") || strstr(made[i], "test.kept");
        p = mg_sprintf("%s/%s", data, made[i]);
        CHECK(mg_path_stat(p, &st, NULL) == kept);
        free(p);
    }
    p = mg_sprintf("%s/apps/org.megler.test.cleared/data", data);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);
    p = mg_sprintf("%s/apps/org.megler.test.cleared/sdcard/Android/data/org.megler.test.cleared", data);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);
    CHECK(!mg_run_clear_data_if_asked(data, data, "org.megler.test.cleared"));
    CHECK(!mg_run_clear_data_if_asked(data, data, "org.megler.test.kept"));
    p = mg_sprintf("%s/apps/org.megler.test.kept/data/shared_prefs/settings.xml", data);
    CHECK(mg_path_stat(p, &st, NULL));
    free(p);
    free(data);
}

static const megler_storage_link k_links[] = {
    { "Music", true }, { "Pictures", true },
    { "Books", false },                /* a name of the user's own, and only to be read */
    /* Three that cannot be: the apps' own folder, a path, and a name there is already. */
    { "Android", true }, { "a/b", true }, { "music", true },
};

static void test_layout(const char *data)
{
    static const char *const names[] = { SD, "/sdcard", "/mnt/sdcard", "/storage/sdcard0", "/storage/self/primary" };
    static const char *const standard[] = { "Alarms", "DCIM", "Documents", "Download", "Movies", "Music", "Notifications", "Pictures", "Podcasts", "Ringtones" };
    megler_storage_host host;
    dir_seen seen;
    mg_stat st;
    char *own = mg_sprintf("%s/apps/" PKG "/sdcard", data), *shared = mg_sprintf("%s/sdcard", data), *p, *text;
    char real[4096];
    size_t i;
    i32 calls;
    say("the shared storage: one for every app, the app's own part in it, and links to folders of the phone's");

    /* What an app left before the storage was shared: its own part, and something outside it. */
    p = mg_sprintf("%s/Android/data/" PKG "/files", own);
    CHECK(mg_path_mkdirs(p));
    free(p);
    p = mg_sprintf("%s/Android/data/" PKG "/files/old-save.dat", own);
    CHECK(mg_file_write_all(p, "saved", 5));
    free(p);
    p = mg_sprintf("%s/OldGame", own);
    CHECK(mg_path_mkdirs(p));
    free(p);
    /* Another app's own part, from when it ran: a file manager reaches it as on Android 9. */
    p = mg_sprintf("%s/apps/com.other.game/sdcard/Android/data/com.other.game/files", data);
    CHECK(mg_path_mkdirs(p));
    free(p);
    p = mg_sprintf("%s/apps/com.other.game/sdcard/Android/data/com.other.game/files/config.txt", data);
    CHECK(mg_file_write_all(p, "SKIP=0", 6));
    free(p);
    p = mg_sprintf("%s/apps/com.other.game/sdcard/Android/obb/com.other.game", data);
    CHECK(mg_path_mkdirs(p));
    free(p);
    /* And a folder of the shared storage's own by a name that is about to be a link. */
    p = mg_sprintf("%s/Music", shared);
    CHECK(mg_path_mkdirs(p));
    free(p);
    p = mg_sprintf("%s/Music/hidden.txt", shared);
    CHECK(mg_file_write_all(p, "h", 1));
    free(p);
    p = mg_sprintf("%s/book.txt", g_roots[2]);
    CHECK(mg_file_write_all(p, "a book", 6));
    free(p);

    memset(&host, 0, sizeof(host));
    host.links = k_links;
    host.link_count = (int)MG_COUNT(k_links);
    host.open = counted_open;
    host.make_folder = counted_mkdir;
    host.remove = counted_remove;
    host.rename = counted_rename;
    mg_run_files(data, data, PKG, &host);

    /* Five names, one place. */
    CHECK(put("/sdcard/one.txt", "one place"));
    for (i = 0; i < MG_COUNT(names); i++) {
        p = mg_sprintf("%s/one.txt", names[i]);
        CHECK(is_dir(names[i]) && !strcmp(get(p), "one place"));
        free(p);
    }
    p = mg_sprintf("%s/one.txt", shared);
    text = text_of(p);
    CHECK(!strcmp(text, "one place"));
    free(text);
    free(p);
    CHECK(C.realpath("/mnt/sdcard/one.txt", real) == real && !strcmp(real, SD "/one.txt"));
    CHECK(C.realpath("/storage/self/primary", real) == real && !strcmp(real, SD));
    p = mg_fs_host_path("/storage/sdcard0/Download/../one.txt");
    text = mg_sprintf("%s/one.txt", shared);
    CHECK(p && !strcmp(p, text));
    free(p);
    free(text);
    CHECK(!strcmp(mg_fs_external_dir(), SD));
    CHECK(stat_errno("/sdcard2") == GE_NOENT && stat_errno("/storage/emulated/1") == GE_NOENT);

    /* The standard folders are there from the start, each once, a link among them like any other. */
    read_dir("/sdcard", &seen);
    CHECK(seen.dots);
    for (i = 0; i < MG_COUNT(standard); i++) {
        CHECK_INT(times(&seen, standard[i]), 1);
        CHECK_INT(type_of(&seen, standard[i]), T_DT_DIR);
        p = mg_sprintf("/sdcard/%s", standard[i]);
        CHECK(is_dir(p) && C.access(p, 0) == 0);
        free(p);
    }
    CHECK(times(&seen, "Books") == 1 && times(&seen, "Android") == 1 && times(&seen, "one.txt") == 1 && type_of(&seen, "one.txt") == T_DT_REG);
    CHECK(!times(&seen, "music") && !times(&seen, "a") && !times(&seen, "OldGame"));
    CHECK_INT(seen.count, 13);
    /* Real folders in the shared storage's own folder, but for the links, which have none of their own to be. */
    p = mg_sprintf("%s/Download", shared);
    CHECK(mg_path_stat(p, &st, NULL) && st.is_dir);
    free(p);
    p = mg_sprintf("%s/Pictures", shared);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);
    p = mg_sprintf("%s/Books", shared);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);

    /* The directories on the way exist and list what they lead to. */
    read_dir("/storage", &seen);
    CHECK(seen.count == 2 && times(&seen, "emulated") == 1 && times(&seen, "self") == 1 && type_of(&seen, "self") == T_DT_DIR && seen.dots);
    read_dir("/storage/emulated", &seen);
    CHECK(seen.count == 1 && times(&seen, "0") == 1);
    read_dir("/storage/self", &seen);
    CHECK(seen.count == 1 && times(&seen, "primary") == 1);
    read_dir("/", &seen);
    CHECK(times(&seen, "storage") == 1 && times(&seen, "sdcard") == 1 && times(&seen, "mnt") == 1 && times(&seen, "data") == 1);
    read_dir("/mnt", &seen);
    CHECK(seen.count == 1 && times(&seen, "sdcard") == 1);
    CHECK(is_dir("/storage") && is_dir("/storage/emulated") && is_dir("/storage/self") && is_dir("/mnt") && is_dir("/"));
    read_dir("/storage/nothing", &seen);
    CHECK(seen.count == -1 && last_errno() == GE_NOENT);
    {
        t_stat fs;
        int fd = C.open("/storage/emulated", T_RDONLY | T_DIRECTORY, 0);
        CHECK(fd >= 0 && C.fstat(fd, &fs) == 0 && (fs.st_mode & T_IFDIR));
        if (fd >= 0) C.close(fd);
        CHECK_INT(open_errno("/storage/emulated", T_WRONLY), GE_ISDIR);
    }
    /* Made a name at a time, as "mkdir -p" makes a path: what is there is there, and is not refused. */
    CHECK(C.mkdir("/storage", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/storage/emulated", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir(SD, 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/sdcard/Android", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/sdcard/Android/data", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/sdcard/Android/data/" PKG, 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/sdcard/Android/data/" PKG "/cache", 0777) == 0);
    CHECK(C.rmdir("/storage/emulated") == -1 && last_errno() == GE_BUSY);
    CHECK(C.rmdir(SD) == -1 && last_errno() == GE_BUSY);

    /* A link: what is written through one name of it is in the phone's folder, and is read through another. */
    calls = g_calls;
    CHECK(put("/sdcard/Music/x.txt", "a song"));
    CHECK(g_calls > calls);
    p = mg_sprintf("%s/x.txt", g_roots[0]);
    text = text_of(p);
    CHECK(!strcmp(text, "a song"));
    free(text);
    free(p);
    CHECK(!strcmp(get(SD "/Music/x.txt"), "a song") && !strcmp(get("/storage/self/primary/Music/x.txt"), "a song"));
    CHECK_INT(size_of("/mnt/sdcard/Music/x.txt"), 6);
    /* Android's shared storage does not tell Music from music. */
    CHECK(!strcmp(get("/sdcard/music/x.txt"), "a song") && is_dir("/sdcard/MUSIC"));
    /* The folder of the shared storage's own that the link stands in front of is out of sight, and untouched. */
    CHECK(stat_errno("/sdcard/Music/hidden.txt") == GE_NOENT);
    p = mg_sprintf("%s/Music/hidden.txt", shared);
    CHECK(mg_path_stat(p, &st, NULL));
    free(p);
    {
        /* Through a descriptor: appended to, read from where it stands, and what fstat says. */
        t_stat fs;
        char buf[16];
        int fd = C.open("/sdcard/Music/x.txt", T_RDWR | T_APPEND, 0);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK_INT(C.write(fd, "!", 1), 1);
            CHECK(C.fstat(fd, &fs) == 0 && fs.st_size == 7 && (fs.st_mode & T_IFREG));
            C.close(fd);
        }
        fd = C.open("/sdcard/Music/x.txt", T_RDONLY, 0);
        CHECK(fd >= 0 && C.read(fd, buf, 16) == 7 && !memcmp(buf, "a song!", 7));
        if (fd >= 0) C.close(fd);
        CHECK_INT(open_errno("/sdcard/Music/x.txt", T_WRONLY | T_CREAT | T_EXCL), GE_EXIST);
    }

    /* Folders in it: made, nested, listed, and removed only when empty. */
    CHECK(C.mkdir("/sdcard/Music/a", 0777) == 0 && C.mkdir("/sdcard/Music/a/b", 0777) == 0);
    CHECK(C.mkdir("/sdcard/Music/a", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.mkdir("/sdcard/Music/no/such", 0777) == -1 && last_errno() == GE_NOENT);
    CHECK(put("/sdcard/Music/a/b/f.bin", "12345"));
    read_dir("/sdcard/Music", &seen);
    CHECK(seen.count == 2 && seen.dots && type_of(&seen, "a") == T_DT_DIR && type_of(&seen, "x.txt") == T_DT_REG);
    read_dir("/sdcard/Music/a", &seen);
    CHECK(seen.count == 1 && type_of(&seen, "b") == T_DT_DIR);
    read_dir("/sdcard/Music/a/b", &seen);
    CHECK(seen.count == 1 && type_of(&seen, "f.bin") == T_DT_REG);
    read_dir("/sdcard/Music/x.txt", &seen);
    CHECK(seen.count == -1 && last_errno() == GE_NOTDIR);
    CHECK(is_dir("/sdcard/Music/a/b") && size_of("/sdcard/Music/a/b/f.bin") == 5);
    CHECK_INT(open_errno("/sdcard/Music/a", T_WRONLY), GE_ISDIR);
    {
        t_stat fs;
        int fd = C.open("/sdcard/Music/a", T_RDONLY, 0), fd2 = C.open("/sdcard/Music/a/b", T_RDONLY | T_DIRECTORY, 0);
        CHECK(fd >= 0 && C.fstat(fd, &fs) == 0 && (fs.st_mode & T_IFDIR) && fd2 >= 0);
        if (fd >= 0) C.close(fd);
        if (fd2 >= 0) C.close(fd2);
        CHECK_INT(open_errno("/sdcard/Music/x.txt", T_RDONLY | T_DIRECTORY), GE_NOTDIR);
    }
    CHECK(C.rmdir("/sdcard/Music/a") == -1 && last_errno() == GE_NOTEMPTY);
    CHECK(C.unlink("/sdcard/Music/a/b") == -1 && last_errno() == GE_ISDIR);
    CHECK(C.rmdir("/sdcard/Music/a/b/f.bin") == -1 && last_errno() == GE_NOTDIR);
    CHECK(C.unlink("/sdcard/Music/a/b/f.bin") == 0 && C.rmdir("/sdcard/Music/a/b") == 0 && C.rmdir("/sdcard/Music/a") == 0);
    CHECK(stat_errno("/sdcard/Music/a") == GE_NOENT);

    /* What is not there. */
    CHECK_INT(open_errno("/sdcard/Music/none.txt", T_RDONLY), GE_NOENT);
    CHECK_INT(stat_errno("/sdcard/Music/none.txt"), GE_NOENT);
    CHECK(C.unlink("/sdcard/Music/none.txt") == -1 && last_errno() == GE_NOENT);
    CHECK(C.rmdir("/sdcard/Music/none") == -1 && last_errno() == GE_NOENT);
    CHECK(C.access("/sdcard/Music/none.txt", 0) == -1 && last_errno() == GE_NOENT);

    /* Renaming: in a link, from one link to another; between a link and the storage's own folders it is two file systems. */
    CHECK(C.rename("/sdcard/Music/x.txt", "/sdcard/Music/y.txt") == 0 && stat_errno("/sdcard/Music/x.txt") == GE_NOENT);
    CHECK(C.rename("/sdcard/Music/y.txt", "/sdcard/Pictures/z.txt") == 0);
    p = mg_sprintf("%s/z.txt", g_roots[1]);
    text = text_of(p);
    CHECK(!strcmp(text, "a song!"));
    free(text);
    free(p);
    CHECK(C.rename("/sdcard/Pictures/z.txt", "/sdcard/Download/z.txt") == -1 && last_errno() == GE_XDEV);
    CHECK(put("/sdcard/Download/d.txt", "down"));
    CHECK(C.rename("/sdcard/Download/d.txt", "/sdcard/Music/d.txt") == -1 && last_errno() == GE_XDEV);
    CHECK(size_of("/sdcard/Pictures/z.txt") == 7 && size_of("/sdcard/Download/d.txt") == 4 && stat_errno("/sdcard/Music/d.txt") == GE_NOENT);
    CHECK(C.rename("/sdcard/Download/d.txt", "/sdcard/Documents/d.txt") == 0 && size_of("/sdcard/Documents/d.txt") == 4);
    CHECK(C.rename("/sdcard/Music/none.txt", "/sdcard/Music/some.txt") == -1 && last_errno() == GE_NOENT);

    /* A link's own name is not the app's to make, remove or give away. */
    CHECK(C.mkdir("/sdcard/Music", 0777) == -1 && last_errno() == GE_EXIST);
    CHECK(C.rmdir("/sdcard/Music") == -1 && last_errno() == GE_BUSY);
    CHECK(C.unlink("/sdcard/Music") == -1 && last_errno() == GE_BUSY);
    CHECK(C.rename("/sdcard/Music", "/sdcard/Music2") == -1 && last_errno() == GE_BUSY);
    CHECK(C.rename("/sdcard/Documents", "/sdcard/Pictures") == -1 && last_errno() == GE_BUSY);
    CHECK(C.rename("/sdcard/Pictures/z.txt", "/sdcard/Music") == -1 && last_errno() == GE_BUSY);
    CHECK(is_dir("/sdcard/Music") && is_dir("/sdcard/Documents") && C.access("/sdcard/Music", 2) == 0);
    CHECK_INT(open_errno("/sdcard/Music", T_WRONLY), GE_ISDIR);
    CHECK(C.unlink("/sdcard/Pictures/z.txt") == 0 && C.unlink("/sdcard/Documents/d.txt") == 0);

    /* The app's own part: where it was before the storage was shared, with what it held; nobody else's is there. */
    CHECK(is_dir("/sdcard/Android/data/" PKG) && is_dir("/sdcard/Android/obb/" PKG) && is_dir("/sdcard/Android"));
    CHECK(!strcmp(get("/sdcard/Android/data/" PKG "/files/old-save.dat"), "saved"));
    CHECK(put("/sdcard/Android/data/" PKG "/files/save.dat", "new"));
    p = mg_sprintf("%s/Android/data/" PKG "/files/save.dat", own);
    CHECK(mg_path_stat(p, &st, NULL) && st.size == 3);
    free(p);
    p = mg_sprintf("%s/Android/data/" PKG, shared);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);
    read_dir("/sdcard/Android", &seen);
    CHECK(seen.count == 2 && times(&seen, "data") == 1 && times(&seen, "obb") == 1);
    read_dir("/sdcard/Android/data", &seen);
    CHECK(seen.count == 2 && times(&seen, PKG) == 1 && type_of(&seen, PKG) == T_DT_DIR && times(&seen, "com.other.game") == 1);
    CHECK_INT(stat_errno("/sdcard/Android/data/com.someone.else"), GE_NOENT);
    CHECK_INT(open_errno("/sdcard/Android/data/com.someone.else/files/x", T_RDONLY), GE_NOENT);
    CHECK(C.rmdir("/sdcard/Android/data/" PKG) == -1 && last_errno() == GE_BUSY);
    /*
     * Another app's own part is that app's folder, to read and to write, as Android 9 has it for an app with the storage
     * permission: what a file manager puts there is where the other app looks.
     */
    CHECK(!strcmp(get("/sdcard/Android/data/com.other.game/files/config.txt"), "SKIP=0"));
    CHECK(put("/sdcard/Android/data/com.other.game/files/qa.txt", "SKIP_VALIDATION=1"));
    p = mg_sprintf("%s/apps/com.other.game/sdcard/Android/data/com.other.game/files/qa.txt", data);
    text = text_of(p);
    CHECK(text && !strcmp(text, "SKIP_VALIDATION=1"));
    free(text);
    free(p);
    p = mg_sprintf("%s/Android/data/com.other.game", shared);
    CHECK(!mg_path_stat(p, &st, NULL));
    free(p);
    CHECK(put("/sdcard/Android/obb/com.other.game/qa.txt", "x"));
    read_dir("/sdcard/Android/obb", &seen);
    CHECK(seen.count == 2 && times(&seen, PKG) == 1 && times(&seen, "com.other.game") == 1);
    CHECK(C.rmdir("/sdcard/Android/data/com.other.game") == -1 && last_errno() == GE_BUSY);
    /* What the app kept outside its own part is where it was on the disk, and not in the shared storage (the log says so). */
    CHECK_INT(stat_errno("/sdcard/OldGame"), GE_NOENT);
    p = mg_sprintf("%s/OldGame", own);
    CHECK(mg_path_stat(p, &st, NULL) && st.is_dir);
    free(p);
    /* Beside its own part an app may make what Android lets it. */
    CHECK(C.mkdir("/sdcard/Android/media", 0777) == 0 && C.mkdir("/sdcard/Android/media/" PKG, 0777) == 0 && C.rmdir("/sdcard/Android/media/" PKG) == 0 &&
          C.rmdir("/sdcard/Android/media") == 0);

    /* A link that is only to be read: read, listed, and nothing else, whoever asks. */
    calls = g_calls;
    CHECK(!strcmp(get("/sdcard/Books/book.txt"), "a book") && size_of("/sdcard/Books/book.txt") == 6);
    read_dir("/sdcard/Books", &seen);
    CHECK(seen.count == 1 && times(&seen, "book.txt") == 1);
    CHECK(g_calls > calls);
    calls = g_calls;
    CHECK_INT(open_errno("/sdcard/Books/book.txt", T_WRONLY), GE_ACCES);
    CHECK_INT(open_errno("/sdcard/Books/book.txt", T_RDWR), GE_ACCES);
    CHECK_INT(open_errno("/sdcard/Books/book.txt", T_RDONLY | T_TRUNC), GE_ACCES);
    CHECK_INT(open_errno("/sdcard/Books/new.txt", T_WRONLY | T_CREAT), GE_ACCES);
    CHECK(C.mkdir("/sdcard/Books/shelf", 0777) == -1 && last_errno() == GE_ACCES);
    CHECK(C.unlink("/sdcard/Books/book.txt") == -1 && last_errno() == GE_ACCES);
    CHECK(C.rename("/sdcard/Books/book.txt", "/sdcard/Books/tome.txt") == -1 && last_errno() == GE_ACCES);
    CHECK(put("/sdcard/Music/in.txt", "in"));
    CHECK(C.rename("/sdcard/Music/in.txt", "/sdcard/Books/in.txt") == -1 && last_errno() == GE_ACCES);
    CHECK(C.rename("/sdcard/Books/book.txt", "/sdcard/Music/out.txt") == -1 && last_errno() == GE_ACCES);
    CHECK(C.access("/sdcard/Books/book.txt", 4) == 0 && C.access("/sdcard/Books/book.txt", 2) == -1 && last_errno() == GE_ACCES);
    CHECK(C.access("/sdcard/Books", 2) == -1 && C.access("/sdcard/Music/in.txt", 2) == 0);
    /* The same from a part of the layer that goes to the host layer itself (java.io.File's delete and mkdirs do). */
    p = mg_fs_host_path("/sdcard/Books/book.txt");
    CHECK(p && mg_path_is_foreign(p) && !mg_path_remove(p, NULL) && !mg_file_write_all(p, "x", 1));
    free(p);
    p = mg_sprintf("%s/book.txt", g_roots[2]);
    text = text_of(p);
    CHECK(!strcmp(text, "a book"));
    free(text);
    free(p);
    CHECK(C.unlink("/sdcard/Music/in.txt") == 0);
    /* The three links that could not be are not: their numbers reach nobody. */
    CHECK(stat_errno("/sdcard/a") == GE_NOENT && stat_errno("/sdcard/a/b") == GE_NOENT);
    {
        int err = 0;
        calls = g_calls;
        CHECK(!mg_path_stat(MG_FOREIGN_PREFIX "3", &st, &err) && err == MG_E_NOENT && !mg_path_stat(MG_FOREIGN_PREFIX "5/x", &st, &err) && err == MG_E_NOENT);
        CHECK(!mg_path_mkdir(MG_FOREIGN_PREFIX "4/made", &err) && err == MG_E_NOENT && !mg_path_stat(MG_FOREIGN_PREFIX "31", &st, &err));
        CHECK_INT(g_calls - calls, 0);
    }

    /* Room: the volume all of it is on, the same answer wherever the path is. */
    {
        t_statfs a, b;
        t_statvfs v;
        u64 total = 0, available = 0;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&v, 0, sizeof(v));
        CHECK(C.statfs("/sdcard/Music/y.txt", &a) == 0 && C.statfs("/data/data/" PKG, &b) == 0 && C.statvfs("/sdcard/Books", &v) == 0);
        CHECK(a.f_bsize == 4096 && a.f_blocks > 0 && a.f_bfree > 0 && a.f_bfree <= a.f_blocks && a.f_bavail == a.f_bfree && a.f_namelen == 255);
        CHECK(a.f_blocks == b.f_blocks && v.f_bsize == 4096 && v.f_frsize == 4096 && v.f_blocks > 0 && v.f_bavail <= v.f_blocks && v.f_namemax == 255);
        CHECK(mg_path_space(shared, &total, &available) && total / 4096 == a.f_blocks && available <= total);
        CHECK(!mg_path_space(MG_FOREIGN_PREFIX "0", &total, &available));
    }
    free(own);
    free(shared);
}

/* Two threads of an app in one link at once: each makes, writes, reads, lists and removes its own. */
typedef struct worker {
    int id, failures;
} worker;

/*
 * Names Windows refuses (host/mg_host.h): Android takes any character in a name but '/'. MX Player's Amazon SDK names a
 * database after an ARN, colons and all. In the app's own folder (the host layer's mapping) and in a link (the core's,
 * before the storage host is asked), each such name is made, read back, listed exactly as it was named, stat'ed, renamed,
 * and removed; and a name that needed nothing is still what it was on the host's own disk.
 */
static void test_odd_names(void)
{
    static const char *const odd[] = { "arn:aws:remote-config:us-west-2:455205533140:appConfig:apiz0o87_ConfigurationCache.db", "a<b>c", "say \"so\"", "back\\slash",
                                       "pipe|line", "what?", "star*", "dot.", "space ", "CON", "nul.txt", "Com1", "tab\there", "\001one" };
    static const char *const places[] = { "/data/data/" PKG "/files/odd", SD "/Music/odd" };
    dir_seen seen;
    size_t p, i;
    say("names Windows refuses: an app's own folder and a link");
    C.mkdir("/data/data/" PKG "/files", 0700);
    for (p = 0; p < MG_COUNT(places); p++) {
        bool made = true, read_back = true, stat_ok = true, renamed = true, gone = true, listed_once = true, moved_listed = true;
        char *path, *to;
        CHECK(C.mkdir(places[p], 0700) == 0 || last_errno() == GE_EXIST);
        for (i = 0; i < MG_COUNT(odd); i++) {
            path = mg_sprintf("%s/%s", places[p], odd[i]);
            made &= put(path, odd[i]);
            read_back &= !strcmp(get(path), odd[i]);
            stat_ok &= size_of(path) == (i64)strlen(odd[i]);
            free(path);
        }
        read_dir(places[p], &seen);
        for (i = 0; i < MG_COUNT(odd); i++) listed_once &= times(&seen, odd[i]) == 1;
        if (!listed_once || seen.count != (int)MG_COUNT(odd))
            for (i = 0; i < (size_t)seen.count && i < 48; i++) say("    listed: \"%s\"", seen.names[i]);
        CHECK(made && read_back && stat_ok);
        CHECK(listed_once && seen.count == (int)MG_COUNT(odd));
        for (i = 0; i < MG_COUNT(odd); i++) {
            path = mg_sprintf("%s/%s", places[p], odd[i]);
            to = mg_sprintf("%s/%s.", places[p], odd[i]);
            renamed &= C.rename(path, to) == 0 && stat_errno(path) == GE_NOENT && size_of(to) == (i64)strlen(odd[i]);
            free(path);
            free(to);
        }
        read_dir(places[p], &seen);
        for (i = 0; i < MG_COUNT(odd); i++) {
            to = mg_sprintf("%s.", odd[i]);
            moved_listed &= times(&seen, to) == 1 && times(&seen, odd[i]) == 0;
            free(to);
        }
        CHECK(renamed && moved_listed && seen.count == (int)MG_COUNT(odd));
        for (i = 0; i < MG_COUNT(odd); i++) {
            to = mg_sprintf("%s/%s.", places[p], odd[i]);
            gone &= C.unlink(to) == 0;
            free(to);
        }
        read_dir(places[p], &seen);
        CHECK(gone && seen.count == 0 && C.rmdir(places[p]) == 0);
    }
    {
        /* A name that needed nothing, on the host's own disk: as it was named. */
        char *host = mg_fs_host_path("/data/data/" PKG "/files/plain name.txt");
        listing l;
        CHECK(put("/data/data/" PKG "/files/plain name.txt", "plain"));
        memset(&l, 0, sizeof(l));
        if (host) {
            char *dir = mg_strdup(host);
            *strrchr(dir, '/') = 0;
            mg_path_list(dir, list_one, &l, NULL);
            free(dir);
        }
        CHECK(host && listed(&l, "plain name.txt") && C.unlink("/data/data/" PKG "/files/plain name.txt") == 0);
        free(host);
    }
}

/*
 * A shared writable mapping of a file, as MMKV and MX Player's mx-kv keep their stores: what is written into the mapping
 * reaches the file at msync, and at munmap, though the descriptor was closed right after mapping; and with neither, as the
 * shell settles the app's files when it is suspended (megler_settle_files), as TikTok Lite's KEVA needs (it does not msync).
 * (The pages are the layer's copy; Linux would write them back by itself.)
 */
static void test_shared_mapping(void)
{
    void *(*map)(void *, size_t, int, int, int, i32) = (void *(*)(void *, size_t, int, int, int, i32))mg_bionic_find("mmap");
    int (*unmap)(void *, size_t) = (int (*)(void *, size_t))mg_bionic_find("munmap");
    int (*sync)(void *, size_t, int) = (int (*)(void *, size_t, int))mg_bionic_find("msync");
    i32 (*pread_)(int, void *, u32, i32) = (i32 (*)(int, void *, u32, i32))mg_bionic_find("pread");
    static const char path[] = "/data/data/" PKG "/files/kv.bin";
    static char zeros[8192];
    char *p, back[8];
    int fd;
    say("a shared writable mapping of a file: msync and munmap write it back");
    CHECK(map && unmap && sync && pread_);
    if (!map || !unmap || !sync || !pread_) return;
    fd = C.open(path, T_RDWR | T_CREAT | T_TRUNC, 0600);
    CHECK(fd >= 0 && C.write(fd, zeros, sizeof(zeros)) == (i32)sizeof(zeros));
    p = (char *)map(NULL, sizeof(zeros), 3 /* PROT_READ | PROT_WRITE */, 1 /* MAP_SHARED */, fd, 0);
    C.close(fd);
    CHECK(p && p != (char *)-1);
    if (!p || p == (char *)-1) return;
    memcpy(p + 4096, "synced", 6);
    CHECK(sync(p + 4096, 6, 4 /* MS_SYNC */) == 0);
    fd = C.open(path, T_RDONLY, 0);
    memset(back, 0, sizeof(back));
    CHECK(fd >= 0 && pread_(fd, back, 6, 4096) == 6 && !memcmp(back, "synced", 6));
    C.close(fd);
    memcpy(p + 6000, "settled", 7);
    megler_settle_files();
    fd = C.open(path, T_RDONLY, 0);
    memset(back, 0, sizeof(back));
    CHECK(fd >= 0 && pread_(fd, back, 7, 6000) == 7 && !memcmp(back, "settled", 7));
    C.close(fd);
    memcpy(p, "unmapped", 8);
    CHECK(unmap(p, sizeof(zeros)) == 0);
    fd = C.open(path, T_RDONLY, 0);
    memset(back, 0, sizeof(back));
    CHECK(fd >= 0 && pread_(fd, back, 8, 0) == 8 && !memcmp(back, "unmapped", 8) && size_of(path) == (i64)sizeof(zeros));
    C.close(fd);
    CHECK(C.unlink(path) == 0);
}

/*
 * A file made shorter while it is mapped shared and writable stays so as its mapping is written back: Linux writes a mapping
 * back no further than the file's end (a page past it is a SIGBUS), and the layer's copy would have made the file whole again.
 */
static void test_shared_mapping_shortened(void)
{
    void *(*map)(void *, size_t, int, int, int, i32) = (void *(*)(void *, size_t, int, int, int, i32))mg_bionic_find("mmap");
    int (*unmap)(void *, size_t) = (int (*)(void *, size_t))mg_bionic_find("munmap");
    int (*trunc)(int, i32) = (int (*)(int, i32))mg_bionic_find("ftruncate");
    i32 (*pread_)(int, void *, u32, i32) = (i32 (*)(int, void *, u32, i32))mg_bionic_find("pread");
    static const char path[] = "/data/data/" PKG "/files/kv-short.bin";
    static char zeros[8192];
    char *p, back[8];
    int fd;
    say("a file made shorter while mapped shared: written back no further than its end");
    CHECK(map && unmap && trunc && pread_);
    if (!map || !unmap || !trunc || !pread_) return;
    fd = C.open(path, T_RDWR | T_CREAT | T_TRUNC, 0600);
    CHECK(fd >= 0 && C.write(fd, zeros, sizeof(zeros)) == (i32)sizeof(zeros));
    p = (char *)map(NULL, sizeof(zeros), 3 /* PROT_READ | PROT_WRITE */, 1 /* MAP_SHARED */, fd, 0);
    CHECK(p && p != (char *)-1);
    if (!p || p == (char *)-1) {
        C.close(fd);
        return;
    }
    CHECK(trunc(fd, 4096) == 0);
    C.close(fd);
    memcpy(p + 100, "kept", 4);
    memcpy(p + 6000, "beyond", 6);
    megler_settle_files();
    CHECK(size_of(path) == 4096);
    fd = C.open(path, T_RDONLY, 0);
    memset(back, 0, sizeof(back));
    CHECK(fd >= 0 && pread_(fd, back, 4, 100) == 4 && !memcmp(back, "kept", 4));
    C.close(fd);
    CHECK(unmap(p, sizeof(zeros)) == 0);
    CHECK(size_of(path) == 4096);
    CHECK(C.unlink(path) == 0);
}

static MG_HOSTCB void worker_run(void *arg)
{
    worker *w = (worker *)arg;
    char dir[96], file[128], buf[64], want[32];
    dir_seen *seen = (dir_seen *)mg_alloc(sizeof(*seen));
    int i;
    snprintf(dir, sizeof(dir), "/sdcard/Music/worker-%d", w->id);
    if (C.mkdir(dir, 0777) != 0) w->failures++;
    for (i = 0; i < 60; i++) {
        snprintf(file, sizeof(file), "%s/f-%d.txt", dir, i);
        snprintf(want, sizeof(want), "thread %d, file %d", w->id, i);
        if (!put(file, want)) w->failures++;
        if (strcmp(get_into(buf, sizeof(buf), file), want)) w->failures++;
        if (size_of(file) != (i64)strlen(want)) w->failures++;
        read_dir(dir, seen);
        if (seen->count != 1) w->failures++;
        if (i % 10 == 0) {
            /* (The whole storage: the links are in it once, whatever the other thread is doing.) */
            read_dir("/sdcard", seen);
            if (times(seen, "Music") != 1 || times(seen, "Pictures") != 1) w->failures++;
        }
        if (C.unlink(file) != 0) w->failures++;
    }
    if (C.rmdir(dir) != 0) w->failures++;
    free(seen);
}

static void test_threads(void)
{
    worker a = { 1, 0 }, b = { 2, 0 };
    mg_thread *ta, *tb;
    dir_seen seen;
    say("the shared storage: two threads in one link at once");
    ta = mg_thread_start(worker_run, &a, 0, "storage-1");
    tb = mg_thread_start(worker_run, &b, 0, "storage-2");
    CHECK(ta && tb);
    if (ta) mg_thread_join(ta);
    if (tb) mg_thread_join(tb);
    CHECK_INT(a.failures, 0);
    CHECK_INT(b.failures, 0);
    read_dir("/sdcard/Music", &seen);
    CHECK_INT(seen.count, 0);
}

/* With no link at all the storage is the layer's own folders and nothing is asked of anyone. */
static void test_no_links(const char *data)
{
    dir_seen seen;
    mg_stat st;
    char *p;
    i32 calls = g_calls;
    say("the shared storage: with no links");
    mg_run_storage(data, NULL);
    read_dir("/sdcard", &seen);
    CHECK(times(&seen, "Music") == 1 && times(&seen, "Pictures") == 1 && !times(&seen, "Books") && times(&seen, "Android") == 1);
    CHECK_INT(seen.count, 12);
    /* The folder the link stood in front of is back in sight. */
    CHECK(!strcmp(get("/sdcard/Music/hidden.txt"), "h"));
    CHECK(put("/sdcard/Pictures/p.txt", "plain") && C.mkdir("/sdcard/Pictures/album", 0777) == 0);
    p = mg_sprintf("%s/sdcard/Pictures/p.txt", data);
    CHECK(mg_path_stat(p, &st, NULL) && st.size == 5);
    free(p);
    CHECK(C.rename("/sdcard/Pictures/p.txt", "/sdcard/Music/p.txt") == 0 && C.unlink("/sdcard/Music/p.txt") == 0 && C.rmdir("/sdcard/Pictures/album") == 0);
    CHECK(C.rmdir("/sdcard/Music") == -1 && last_errno() == GE_NOTEMPTY);
    CHECK(is_dir("/sdcard/Android/data/" PKG) && !strcmp(get("/sdcard/Android/data/" PKG "/files/save.dat"), "new"));
    {
        /* The check goes through a folder of the layer's own as through a link, and says which it found. */
        static const char *const names[] = { "Pictures", "Download" };
        int before = t_failed;
        g_check_quiet = true;
        mg_storage_check_run(names, 2, t_sink, t_sink_user, NULL);
        g_check_quiet = false;
        CHECK_INT(t_failed - before, 0);
    }
    CHECK_INT(g_calls - calls, 0);
}

/* ------------------------------------------------- the check for a phone
 *
 * What an app does to files, done in a folder of the shared storage through
 * the C library, a line said for every step: mg_storage_check_run, which a
 * phone runs on its real libraries (megler_storage_check in megler.h) and the
 * tests run here on the stand-in's. Everything happens in a folder of the
 * check's own, megler-storage-check, made in the folder asked about and taken
 * away at the end: nothing else there is opened, changed or removed.
 */
#define CHECK_DIR "megler-storage-check"

static void heading(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_check_where, sizeof(g_check_where), fmt, ap);
    va_end(ap);
    if (!g_check_quiet) say("%s", g_check_where);
}

static void step_said(bool ok, int err, const char *fmt, ...)
{
    char text[420], why[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    t_checks++;
    if (!ok) t_failed++;
    snprintf(why, sizeof(why), " -- errno %d", err);
    if (!g_check_quiet) say("  %s %s%s", ok ? "ok  " : "FAIL", text, ok ? "" : why);
    else if (!ok) say("  FAIL %s: %s%s", g_check_where, text, why);
}

/* A step: whether it went as it must, and what it was. A failed one says the errno it was left with. */
#define STEP(cond, ...)                 \
    do {                                \
        bool ok_ = (cond);              \
        int err_ = last_errno();        \
        step_said(ok_, err_, __VA_ARGS__); \
    } while (0)

static void noted(const char *fmt, ...)
{
    char text[420];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (!g_check_quiet) say("       %s", text);
}

/* Takes the check's own folder away, by the names the check gives things and no others: after a run, and before one for what an earlier run left when it was cut short. */
static void check_sweep(const char *own)
{
    static const char *const files[] = { "a.txt", "b.txt", "c.txt", "x.txt", "sub/b.txt", "sub2/b.txt", "sub3/moved/b.txt" };
    static const char *const dirs[] = { "sub3/moved", "sub3", "sub2", "sub" };
    char path[400];
    size_t i;
    for (i = 0; i < MG_COUNT(files); i++) {
        snprintf(path, sizeof(path), "%s/%s", own, files[i]);
        C.unlink(path);
    }
    for (i = 0; i < MG_COUNT(dirs); i++) {
        snprintf(path, sizeof(path), "%s/%s", own, dirs[i]);
        C.rmdir(path);
    }
    C.rmdir(own);
}

static void check_dir(const char *dir, const char *alias)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    char own[300], a[340], b[340], c[340], sub[340], sub2[340], sub3[340], held[400], through[400], buf[64];
    dir_seen *seen = (dir_seen *)mg_alloc(sizeof(*seen));
    char *host = mg_fs_host_path(dir);
    bool link = host && mg_path_is_foreign(host);
    u64 began = mg_time_monotonic_ns(), t;
    int before, fd, i;
    free(host);
    snprintf(own, sizeof(own), "%s/" CHECK_DIR, dir);
    snprintf(a, sizeof(a), "%s/a.txt", own);
    snprintf(b, sizeof(b), "%s/b.txt", own);
    snprintf(c, sizeof(c), "%s/c.txt", own);
    snprintf(sub, sizeof(sub), "%s/sub", own);
    snprintf(sub2, sizeof(sub2), "%s/sub2", own);
    snprintf(sub3, sizeof(sub3), "%s/sub3", own);
    snprintf(through, sizeof(through), "%s/" CHECK_DIR "/a.txt", alias);
    heading("%s, which is %s", dir, link ? "a folder of the phone's own" : "a folder of the layer's own, not one of the phone's");
    STEP(is_dir(dir), "it is a folder");
    read_dir(dir, seen);
    before = seen->count;
    STEP(before >= 0 && seen->dots, "it is listed: %d entries", before);
    if (times(seen, CHECK_DIR)) {
        noted("an earlier check's folder was still there (a run cut short); it is taken away first");
        check_sweep(own);
        before--;
    }
    STEP(C.mkdir(own, 0777) == 0, "a folder is made in it: " CHECK_DIR);
    STEP(C.mkdir(own, 0777) == -1 && last_errno() == GE_EXIST, "made again it is refused, being there (EEXIST)");
    STEP(put(a, alphabet), "a file is written in the folder: a.txt, 26 bytes");
    STEP(!strcmp(get_into(buf, sizeof(buf), through), alphabet), "and read back through the storage's other name, %s", alias);
    STEP(size_of(through) == 26, "stat says 26 bytes and a file");
    {
        t_stat st;
        i64 now = (i64)(mg_time_realtime_ns() / 1000000000ull);
        bool ok = C.stat(a, &st) == 0;
        STEP(ok && st.mt_sec > now - 2 * 86400 && st.mt_sec < now + 2 * 86400, "its time of change is now, give or take (%lld s from the clock's)", ok ? (long long)(st.mt_sec - now) : 0ll);
    }
    {
        t_stat st;
        bool ok;
        memset(&st, 0, sizeof(st));
        fd = C.open(a, T_WRONLY | T_APPEND, 0);
        ok = fd >= 0 && C.write(fd, "0123", 4) == 4 && C.fstat(fd, &st) == 0 && st.st_size == 30;
        if (fd >= 0) C.close(fd);
        STEP(ok && !strcmp(get_into(buf, sizeof(buf), a), "abcdefghijklmnopqrstuvwxyz0123"), "four bytes appended: 30 bytes by fstat, the new ones at the end");
    }
    fd = C.open(a, T_WRONLY | T_TRUNC, 0);
    i = fd >= 0 && C.write(fd, "new", 3) == 3;
    if (fd >= 0) C.close(fd);
    STEP(i && size_of(a) == 3 && !strcmp(get_into(buf, sizeof(buf), a), "new"), "opened to be emptied, and written again: 3 bytes");
    STEP(open_errno(a, T_WRONLY | T_CREAT | T_EXCL) == GE_EXIST, "to make it anew when it is there is refused (EEXIST)");
    read_dir(own, seen);
    STEP(seen->count == 1 && times(seen, "a.txt") == 1 && type_of(seen, "a.txt") == T_DT_REG, "the folder lists one entry, the file");
    STEP(C.rename(a, b) == 0 && stat_errno(a) == GE_NOENT && size_of(b) == 3, "renamed where it is: a.txt is b.txt, and a.txt is gone");
    STEP(put(c, "replacement") && C.rename(c, b) == 0 && !strcmp(get_into(buf, sizeof(buf), b), "replacement") && stat_errno(c) == GE_NOENT,
         "a second file renamed onto it takes its place");
    snprintf(held, sizeof(held), "%s/b.txt", sub);
    STEP(C.mkdir(sub, 0777) == 0 && C.rename(b, held) == 0 && size_of(held) == 11 && stat_errno(b) == GE_NOENT, "a folder made in the folder, and the file moved into it by renaming");
    STEP(C.rmdir(sub) == -1 && last_errno() == GE_NOTEMPTY && size_of(held) == 11, "the folder is NOT removed while it holds the file (ENOTEMPTY), and the file is still in it");
    STEP(C.unlink(sub) == -1 && last_errno() == GE_ISDIR && is_dir(sub), "nor removed as if it were a file (EISDIR)");
    snprintf(held, sizeof(held), "%s/b.txt", sub2);
    STEP(C.rename(sub, sub2) == 0 && size_of(held) == 11 && stat_errno(sub) == GE_NOENT, "the folder renamed where it is, with what it holds");
    {
        /* A folder into another folder: by rename where the host can, and where it cannot the answer an app copies on. */
        char moved[400], inside[440];
        int r, e;
        snprintf(moved, sizeof(moved), "%s/moved", sub3);
        snprintf(inside, sizeof(inside), "%s/b.txt", moved);
        STEP(C.mkdir(sub3, 0777) == 0, "a second folder made beside it");
        r = C.rename(sub2, moved);
        e = last_errno();
        if (r == 0) {
            STEP(size_of(inside) == 11 && stat_errno(sub2) == GE_NOENT, "the first folder moved into the second by renaming, with what it holds");
            snprintf(held, sizeof(held), "%s", inside);
        } else {
            *mg_errno_ptr() = e;
            STEP(e == GE_XDEV && size_of(held) == 11 && stat_errno(moved) == GE_NOENT,
                 "the first folder is not moved into the second by renaming: EXDEV, as between two disks, which an app answers by copying; nothing was moved or lost");
        }
    }
    snprintf(through, sizeof(through), "%s/none.txt", own);
    STEP(open_errno(through, T_RDONLY) == GE_NOENT && stat_errno(through) == GE_NOENT, "a file that is not there is ENOENT, to open and to stat");
    /* What a call costs here: on a phone each is a question to Windows' broker. */
    t = mg_time_monotonic_ns();
    for (i = 0; i < 20; i++) size_of(held);
    noted("20 times stat of the file: %d ms", (int)((mg_time_monotonic_ns() - t) / 1000000ull));
    t = mg_time_monotonic_ns();
    for (i = 0; i < 20; i++) open_errno(held, T_RDONLY);
    noted("20 times the file opened and closed: %d ms", (int)((mg_time_monotonic_ns() - t) / 1000000ull));
    STEP(C.unlink(held) == 0 && stat_errno(held) == GE_NOENT, "the file is removed");
    STEP(C.unlink(held) == -1 && last_errno() == GE_NOENT, "removed again it is ENOENT");
    check_sweep(own);
    read_dir(dir, seen);
    STEP(stat_errno(own) == GE_NOENT && seen->count == before && !times(seen, CHECK_DIR), "the folders are removed, the check's own last: %s is as it was, %d entries", dir, before);
    noted("all of that took %d ms", (int)((mg_time_monotonic_ns() - began) / 1000000ull));
    free(seen);
}

/* A file from one folder of the storage to another by renaming: from one link to another on a phone. */
static void check_between(const char *from_dir, const char *to_dir)
{
    char from_own[300], to_own[300], from[340], to[340], buf[32];
    int r, e;
    snprintf(from_own, sizeof(from_own), "%s/" CHECK_DIR, from_dir);
    snprintf(to_own, sizeof(to_own), "%s/" CHECK_DIR, to_dir);
    snprintf(from, sizeof(from), "%s/x.txt", from_own);
    snprintf(to, sizeof(to), "%s/x.txt", to_own);
    heading("from %s to %s", from_dir, to_dir);
    check_sweep(from_own);
    check_sweep(to_own);
    STEP(C.mkdir(from_own, 0777) == 0 && C.mkdir(to_own, 0777) == 0 && put(from, "across"), "a folder of the check's in each, and a file in the first");
    r = C.rename(from, to);
    e = last_errno();
    if (r == 0) {
        STEP(!strcmp(get_into(buf, sizeof(buf), to), "across") && stat_errno(from) == GE_NOENT, "the file moved from the one to the other by renaming");
    } else {
        *mg_errno_ptr() = e;
        STEP(e == GE_XDEV && size_of(from) == 6 && stat_errno(to) == GE_NOENT, "the file is not moved from the one to the other by renaming: EXDEV, which an app answers by copying; it is where it was");
    }
    check_sweep(from_own);
    check_sweep(to_own);
    STEP(stat_errno(from_own) == GE_NOENT && stat_errno(to_own) == GE_NOENT, "and both folders of the check's are gone again");
}

int mg_storage_check_run(const char *const *names, int count, mg_test_sink sink, void *user, int *checks)
{
    int i, was_checks = t_checks, was_failed = t_failed, failed;
    mg_test_sink was_sink = t_sink;
    void *was_user = t_sink_user;
    t_sink = sink;
    t_sink_user = user;
    t_failed = 0;
    if (!libc_found()) {
        t_checks++;
        t_failed++;
        say("  FAIL the C library's file functions are not there: is the core started?");
    } else {
        t_statfs room;
        memset(&room, 0, sizeof(room));
        for (i = 0; i < count; i++) {
            char dir[200], alias[200];
            snprintf(dir, sizeof(dir), "/sdcard/%s", names[i]);
            snprintf(alias, sizeof(alias), SD "/%s", names[i]);
            check_dir(dir, alias);
        }
        if (count >= 2) {
            char from[200], to[200];
            snprintf(from, sizeof(from), "/sdcard/%s", names[0]);
            snprintf(to, sizeof(to), "/sdcard/%s", names[1]);
            check_between(from, to);
        }
        if (count > 0 && C.statfs("/sdcard", &room) == 0)
            noted("room, as statfs has it for every path: %llu MB free of %llu MB", (unsigned long long)(room.f_bavail * room.f_bsize >> 20),
                  (unsigned long long)(room.f_blocks * room.f_bsize >> 20));
    }
    failed = t_failed;
    if (checks) *checks += t_checks - was_checks;
    /* (The tests call this in the middle of their own counting, which goes on from here.) */
    t_failed = was_failed + failed;
    t_sink = was_sink;
    t_sink_user = was_user;
    return failed;
}

/* The check, on the links the layout's tests left: all its steps must go as they must, either way a folder's move is answered. */
static void test_check(void)
{
    static const char *const names[] = { "Music", "Pictures" };
    int before = t_failed;
    say("the shared storage: the check a phone runs on its libraries");
    g_check_quiet = true;
    mg_storage_check_run(names, 2, t_sink, t_sink_user, NULL);
    g_fake_moves_no_folder = true;
    mg_storage_check_run(names, 1, t_sink, t_sink_user, NULL);
    g_fake_moves_no_folder = false;
    g_check_quiet = false;
    CHECK_INT(t_failed - before, 0);
}

/*
 * All of it, in folders of its own under `base`: three for the roots and one
 * for the data. `make` NULL: the stand-in is asked; else whoever `make`
 * returns for the three folders, under the names the layout's tests link
 * them by.
 */
static int run_in(const char *base, mg_storage_host_maker make, mg_test_sink sink, void *user, int *checks)
{
    static const char *const names[FAKE_ROOTS] = { "Music", "Pictures", "Books" };
    char *data = mg_sprintf("%s/data", base);
    const megler_storage_host *made = NULL;
    int i;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    for (i = 0; i < FAKE_ROOTS; i++) {
        g_roots[i] = mg_sprintf("%s/root%d", base, i);
        mg_path_mkdirs(g_roots[i]);
        wipe(g_roots[i], 0);
    }
    mg_path_mkdirs(data);
    wipe(data, 0);
    memset(&g_inner, 0, sizeof(g_inner));
    if (make) {
        made = make(names, (const char *const *)g_roots, FAKE_ROOTS);
        CHECK(made && made->link_count == FAKE_ROOTS && made->open && made->make_folder && made->remove && made->rename);
        if (made) g_inner = *made;
    } else {
        g_inner.open = fake_open;
        g_inner.make_folder = fake_mkdir;
        g_inner.remove = fake_remove;
        g_inner.rename = fake_rename;
    }
    if (g_inner.open && g_inner.make_folder && g_inner.remove && g_inner.rename) {
        test_foreign_paths(base);
        CHECK(libc_found());
        if (libc_found()) {
            test_clear_data(data);
            test_layout(data);
            test_threads();
            test_odd_names();
            test_shared_mapping();
            test_shared_mapping_shortened();
            test_check();
            test_no_links(data);
            /* As the tests after this one found things: no links, nobody to ask, and the current directory where it was. */
            C.chdir("/");
        }
    }
    free(data);
    mg_foreign_set(NULL);
    for (i = 0; i < FAKE_ROOTS; i++) {
        free(g_roots[i]);
        g_roots[i] = NULL;
    }
    if (checks) *checks += t_checks;
    return t_failed;
}

int mg_storage_tests_run(const char *scratch, mg_test_sink sink, void *user, int *checks)
{
    char *base = mg_sprintf("%s/storage", scratch);
    int failed = run_in(base, NULL, sink, user, checks);
    free(base);
    return failed;
}

int mg_storage_tests_run_shell(const char *scratch, mg_storage_host_maker make, mg_test_sink sink, void *user, int *checks)
{
    char *base = mg_sprintf("%s/storage-shell", scratch);
    int failed = run_in(base, make, sink, user, checks);
    free(base);
    return failed;
}
