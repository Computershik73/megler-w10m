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
 * the names it binds against (hk_bionic_find), with bionic's structures.
 *
 * What a stand-in cannot say is whether the broker's handles do all that a
 * plain one does. core/tests is not where that is found out.
 */
#include "hk_storage_tests.h"

#include <stdio.h>

#include "../include/megler.h"
#include "../src/bionic/hk_bionic.h"
#include "../src/hk_base.h"
#include "../src/host/hk_host.h"
#include "../src/run/hk_run.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static int t_checks, t_failed;
static hk_test_sink t_sink;
static void *t_sink_user;

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
    return rel[0] ? hk_sprintf("%s/%s", g_roots[root], rel) : hk_strdup(g_roots[root]);
}

#if defined(_WIN32)
static int fake_error(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return HK_E_NOENT;
    case ERROR_ACCESS_DENIED: return HK_E_ACCESS;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return HK_E_EXIST;
    case ERROR_DIRECTORY: return HK_E_NOTDIR;
    default: return HK_E_IO;
    }
}

static void *fake_handle(const char *path, int flags, int kind, int *err)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w = (wchar_t *)hk_alloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1)), *p;
    CREATEFILE2_EXTENDED_PARAMETERS ex;
    DWORD access = 0, disposition = OPEN_EXISTING;
    HANDLE h;
    void *out = NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    for (p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    memset(&ex, 0, sizeof(ex));
    ex.dwSize = sizeof(ex);
    ex.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (kind == HK_FOREIGN_FILE) {
        if (flags & HK_O_READ) access |= GENERIC_READ;
        if (flags & HK_O_WRITE) access |= GENERIC_WRITE;
        if (!access) access = GENERIC_READ;
        if ((flags & HK_O_CREATE) && (flags & HK_O_EXCL)) disposition = CREATE_NEW;
        else if ((flags & HK_O_CREATE) && (flags & HK_O_TRUNC)) disposition = CREATE_ALWAYS;
        else if (flags & HK_O_CREATE) disposition = OPEN_ALWAYS;
        else if (flags & HK_O_TRUNC) disposition = TRUNCATE_EXISTING;
    } else {
        /* The one way Windows opens a directory; a file opens this way too. */
        ex.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
        access = kind == HK_FOREIGN_DIR ? GENERIC_READ : FILE_READ_ATTRIBUTES;
    }
    h = CreateFile2(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, &ex);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        hk_stat st;
        *err = fake_error(e);
        if (e == ERROR_ACCESS_DENIED && kind == HK_FOREIGN_FILE && hk_path_stat(path, &st, NULL) && st.is_dir) *err = HK_E_ISDIR;
    } else if (kind == HK_FOREIGN_DIR) {
        FILE_STANDARD_INFO si;
        if (GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si)) && si.Directory) {
            out = h;
        } else {
            CloseHandle(h);
            *err = HK_E_NOTDIR;
        }
    } else {
        out = h;
    }
    return out;
}
#else
static void *fake_handle(const char *path, int flags, int kind, int *err)
{
    int o = O_RDONLY, fd;
    if (kind == HK_FOREIGN_DIR) {
        o = O_RDONLY | O_DIRECTORY;
    } else if (kind == HK_FOREIGN_FILE) {
        if ((flags & HK_O_READ) && (flags & HK_O_WRITE)) o = O_RDWR;
        else if (flags & HK_O_WRITE) o = O_WRONLY;
        if (flags & HK_O_CREATE) o |= O_CREAT;
        if (flags & HK_O_TRUNC) o |= O_TRUNC;
        if (flags & HK_O_APPEND) o |= O_APPEND;
        if (flags & HK_O_EXCL) o |= O_EXCL;
    }
    fd = open(path, o | O_CLOEXEC, 0666);
    if (fd < 0) {
        switch (errno) {
        case ENOENT: *err = HK_E_NOENT; break;
        case EACCES: case EPERM: *err = HK_E_ACCESS; break;
        case EEXIST: *err = HK_E_EXIST; break;
        case ENOTDIR: *err = HK_E_NOTDIR; break;
        case EISDIR: *err = HK_E_ISDIR; break;
        default: *err = HK_E_IO; break;
        }
        return NULL;
    }
    /* The descriptor plus one, so that no descriptor is NULL. */
    return (void *)(intptr_t)(fd + 1);
}
#endif

static void *fake_open(void *user, int root, const char *rel, int flags, int kind, int *err)
{
    char *path = fake_path(root, rel);
    void *h;
    (void)user;
    if (!path) {
        *err = HK_E_NOENT;
        return NULL;
    }
    h = fake_handle(path, flags, kind, err);
    free(path);
    return h;
}

static bool fake_mkdir(void *user, int root, const char *rel, int *err)
{
    char *path = fake_path(root, rel);
    bool ok;
    (void)user;
    if (!path) {
        *err = HK_E_NOENT;
        return false;
    }
    ok = hk_path_mkdir(path, err);
    free(path);
    return ok;
}

static bool fake_remove(void *user, int root, const char *rel, bool dir, int *err)
{
    char *path = fake_path(root, rel);
    bool ok;
    (void)user;
    if (!path) {
        *err = HK_E_NOENT;
        return false;
    }
    ok = dir ? hk_path_rmdir(path, err) : hk_path_remove(path, err);
    free(path);
    return ok;
}

static bool fake_rename(void *user, int from_root, const char *from, int to_root, const char *to, int *err)
{
    char *a = fake_path(from_root, from), *b = fake_path(to_root, to);
    bool ok = false;
    (void)user;
    if (!a || !b) *err = HK_E_NOENT;
    else ok = hk_path_rename(a, b, err);
    free(a);
    free(b);
    return ok;
}

/*
 * Who is asked: the stand-in, or the shell's own storage host where the
 * tests' program has it (hk_storage_tests_run_shell). Either way through
 * these, which count the calls: some checks are that nobody was asked.
 */
static megler_storage_host g_inner;
static volatile i32 g_calls;

static void *counted_open(void *user, int link, const char *path, int flags, int kind, int *err)
{
    (void)user;
    hk_atomic_add(&g_calls, 1);
    return g_inner.open(g_inner.user, link, path, flags, kind, err);
}

static bool counted_mkdir(void *user, int link, const char *path, int *err)
{
    (void)user;
    hk_atomic_add(&g_calls, 1);
    return g_inner.make_folder(g_inner.user, link, path, err);
}

static bool counted_remove(void *user, int link, const char *path, bool dir, int *err)
{
    (void)user;
    hk_atomic_add(&g_calls, 1);
    return g_inner.remove(g_inner.user, link, path, dir, err);
}

static bool counted_rename(void *user, int from_link, const char *from, int to_link, const char *to, int *err)
{
    (void)user;
    hk_atomic_add(&g_calls, 1);
    return g_inner.rename(g_inner.user, from_link, from, to_link, to, err);
}

static const hk_foreign k_counted = { NULL, counted_open, counted_mkdir, counted_remove, counted_rename };

/* ------------------------------------------------------- the host layer */

typedef struct listing {
    int count, dirs;
    char names[16][64];
} listing;

static void list_one(const char *name, bool is_dir, void *user)
{
    listing *l = (listing *)user;
    if (l->count < 16) hk_strlcpy(l->names[l->count], name, sizeof(l->names[0]));
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

static void wipe_one(const char *name, bool is_dir, void *user)
{
    wipe_list *w = (wipe_list *)user;
    if (w->count >= 64) return;
    w->names[w->count] = hk_strdup(name);
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
        if (!hk_path_list(dir, wipe_one, &w, NULL) || !w.count) return;
        for (i = 0; i < w.count; i++) {
            char *path = hk_sprintf("%s/%s", dir, w.names[i]);
            if (w.dirs[i]) {
                if (depth < 8) wipe(path, depth + 1);
                hk_path_rmdir(path, NULL);
            } else {
                hk_path_remove(path, NULL);
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
    u8 *all = hk_file_read_all(path, &n);
    return all ? (char *)all : hk_strdup("");
}

#define L0 HK_FOREIGN_PREFIX "0"
#define L1 HK_FOREIGN_PREFIX "1"

static void test_foreign_paths(const char *scratch)
{
    hk_stat st;
    hk_file *f;
    listing seen;
    char *text, *plain;
    int err = 0, i;
    i32 calls;
    say("foreign paths: files the host layer is handed, not allowed to open");

    /* Nobody answers yet: such a path is nowhere. */
    hk_foreign_set(NULL);
    CHECK(hk_path_is_foreign(L0) && hk_path_is_foreign(L0 "/a") && !hk_path_is_foreign(scratch) && !hk_path_is_foreign("megler-link"));
    CHECK(!hk_path_stat(L0, &st, &err) && err == HK_E_NOENT);
    CHECK(hk_file_open(L0 "/a.txt", HK_O_READ, &err) == NULL && err == HK_E_NOENT);
    CHECK(!hk_path_mkdir(L0 "/d", &err) && err == HK_E_NOENT);

    hk_foreign_set(&k_counted);
    CHECK(hk_path_stat(L0, &st, &err) && st.is_dir);
    CHECK(hk_path_stat(L1, &st, &err) && st.is_dir);
    /* A root the stand-in does not have, and a file that is not there. */
    CHECK(!hk_path_stat(HK_FOREIGN_PREFIX "7", &st, &err) && err == HK_E_NOENT);
    CHECK(!hk_path_stat(L0 "/nothing", &st, &err) && err == HK_E_NOENT);
    CHECK(hk_file_open(L0 "/nothing", HK_O_READ, &err) == NULL && err == HK_E_NOENT);

    /* Paths that could mean another place to the operating system never reach whoever answers. */
    calls = g_calls;
    {
        static const char *const bad[] = {
            L0 "/../x", L0 "/a/../x", L0 "/./x", L0 "/a/.", L0 "//x", L0 "/a//b", L0 "/a/", L0 "/a\\b", L0 "/c:x", L0 "x",
            HK_FOREIGN_PREFIX "x/a", HK_FOREIGN_PREFIX "", HK_FOREIGN_PREFIX "/a",
        };
        for (i = 0; i < (int)HK_COUNT(bad); i++) {
            err = 0;
            CHECK(!hk_path_stat(bad[i], &st, &err) && err == HK_E_INVAL);
            err = 0;
            CHECK(hk_file_open(bad[i], HK_O_WRITE | HK_O_CREATE, &err) == NULL && err == HK_E_INVAL);
        }
        err = 0;
        CHECK(!hk_path_mkdir(L0 "/../made", &err) && err == HK_E_INVAL);
        CHECK(!hk_path_remove(L0 "/a/../b", &err) && err == HK_E_INVAL);
        CHECK(!hk_path_rename(L0 "/a", L0 "/../b", &err) && err == HK_E_INVAL);
    }
    CHECK_INT(g_calls - calls, 0);

    /* A file written through the layer is in the root's folder, and reads back both ways. */
    CHECK(hk_file_write_all(L0 "/a.txt", "0123456789", 10));
    plain = hk_sprintf("%s/a.txt", g_roots[0]);
    text = text_of(plain);
    CHECK(!strcmp(text, "0123456789"));
    free(text);
    text = text_of(L0 "/a.txt");
    CHECK(!strcmp(text, "0123456789"));
    free(text);
    CHECK(hk_path_stat(L0 "/a.txt", &st, &err) && !st.is_dir && st.size == 10);
    CHECK(st.mtime_ns > 1600000000ll * 1000000000ll);

    /* The handle is a file like any other: positions, positioned reads, its length cut, what it says of itself. */
    f = hk_file_open(L0 "/a.txt", HK_O_READ | HK_O_WRITE, &err);
    CHECK(f != NULL && err == HK_E_OK);
    if (f) {
        char buf[16];
        CHECK_INT(hk_file_pread(f, buf, 4, 3, &err), 4);
        CHECK(!memcmp(buf, "3456", 4));
        CHECK_INT(hk_file_read(f, buf, 2, &err), 2);
        CHECK(!memcmp(buf, "01", 2));
        CHECK_INT(hk_file_seek(f, -2, 2, &err), 8);
        CHECK_INT(hk_file_write(f, "XYZ", 3, &err), 3);
        CHECK_INT(hk_file_pwrite(f, "ab", 2, 0, &err), 2);
        CHECK(hk_file_stat(f, &st) && st.size == 11 && !st.is_dir);
        CHECK(hk_file_truncate(f, 9, &err));
        CHECK(hk_file_sync(f));
        hk_file_close(f);
    }
    text = text_of(plain);
    CHECK(!strcmp(text, "ab234567X"));
    free(text);
    /* Appending, and the flags that decide what an open does to what is there. */
    f = hk_file_open(L0 "/a.txt", HK_O_WRITE | HK_O_APPEND, &err);
    CHECK(f != NULL);
    if (f) {
        CHECK_INT(hk_file_write(f, "+", 1, &err), 1);
        hk_file_close(f);
    }
    CHECK(hk_file_open(L0 "/a.txt", HK_O_WRITE | HK_O_CREATE | HK_O_EXCL, &err) == NULL && err == HK_E_EXIST);
    text = text_of(L0 "/a.txt");
    CHECK(!strcmp(text, "ab234567X+"));
    free(text);
    f = hk_file_open(L0 "/a.txt", HK_O_WRITE | HK_O_TRUNC, &err);
    CHECK(f != NULL);
    hk_file_close(f);
    CHECK(hk_path_stat(L0 "/a.txt", &st, &err) && st.size == 0);
    CHECK(hk_file_write_all(L0 "/a.txt", "mapped", 6));
    {
        hk_map map;
        CHECK(hk_map_open(&map, L0 "/a.txt") && map.size == 6 && !memcmp(map.data, "mapped", 6));
        hk_map_close(&map);
        CHECK(!hk_map_open(&map, L0 "/nothing"));
    }

    /* Folders: made one at a time and all at once, listed through a handle, in the way of a file. */
    CHECK(hk_path_mkdir(L0 "/dir", &err));
    CHECK(!hk_path_mkdir(L0 "/dir", &err) && err == HK_E_EXIST);
    CHECK(!hk_path_mkdir(L0, &err) && err == HK_E_EXIST);
    CHECK(!hk_path_mkdir(L0 "/no/such/parent", &err) && err == HK_E_NOENT);
    CHECK(hk_path_mkdirs(L0 "/deep/er/still"));
    free(plain);
    plain = hk_sprintf("%s/deep/er/still", g_roots[0]);
    CHECK(hk_path_stat(plain, &st, &err) && st.is_dir);
    CHECK(hk_path_stat(L0 "/deep/er", &st, &err) && st.is_dir);
    CHECK(hk_file_write_all(L0 "/deep/er/inner.bin", "\0\1\2", 3));
    CHECK(hk_file_open(L0 "/dir", HK_O_READ, &err) == NULL && err == HK_E_ISDIR);
    CHECK(hk_file_open(L0, HK_O_READ, &err) == NULL && err == HK_E_ISDIR);
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L0, list_one, &seen, &err));
    CHECK(seen.count == 3 && seen.dirs == 2 && listed(&seen, "a.txt") && listed(&seen, "dir") && listed(&seen, "deep"));
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L0 "/deep/er", list_one, &seen, &err) && seen.count == 2 && seen.dirs == 1 && listed(&seen, "still") && listed(&seen, "inner.bin"));
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L0 "/dir", list_one, &seen, &err) && seen.count == 0);
    CHECK(!hk_path_list(L0 "/a.txt", list_one, &seen, &err) && err == HK_E_NOTDIR);
    CHECK(!hk_path_list(L0 "/nothing", list_one, &seen, &err) && err == HK_E_NOENT);
    /* More entries than one bufferful of them (the Win32 host reads a directory 16 KB at a time). */
    CHECK(hk_path_mkdir(L1 "/many", &err));
    for (i = 0; i < 300; i++) {
        char *name = hk_sprintf(L1 "/many/a-file-with-a-name-long-enough-to-fill-the-buffer-%03d.dat", i);
        if (!hk_file_write_all(name, "x", 1)) break;
        free(name);
    }
    CHECK_INT(i, 300);
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L1 "/many", list_one, &seen, &err) && seen.count == 300 && seen.dirs == 0);
    for (i = 0; i < 300; i++) {
        char *name = hk_sprintf(L1 "/many/a-file-with-a-name-long-enough-to-fill-the-buffer-%03d.dat", i);
        if (!hk_path_remove(name, &err)) break;
        free(name);
    }
    CHECK_INT(i, 300);
    CHECK(hk_path_rmdir(L1 "/many", &err));

    /* Renaming: in a folder, to another, to another root, onto a file that is there; never a root, never out to an ordinary path. */
    CHECK(hk_path_rename(L0 "/a.txt", L0 "/b.txt", &err));
    CHECK(!hk_path_stat(L0 "/a.txt", &st, &err) && err == HK_E_NOENT && hk_path_stat(L0 "/b.txt", &st, &err) && st.size == 6);
    CHECK(hk_path_rename(L0 "/b.txt", L0 "/dir/c.txt", &err));
    CHECK(hk_path_rename(L0 "/dir/c.txt", L1 "/d.txt", &err));
    free(plain);
    plain = hk_sprintf("%s/d.txt", g_roots[1]);
    text = text_of(plain);
    CHECK(!strcmp(text, "mapped"));
    free(text);
    CHECK(hk_file_write_all(L1 "/e.txt", "newer", 5));
    CHECK(hk_path_rename(L1 "/e.txt", L1 "/d.txt", &err));
    text = text_of(L1 "/d.txt");
    CHECK(!strcmp(text, "newer") && !hk_path_stat(L1 "/e.txt", &st, NULL));
    free(text);
    CHECK(hk_path_rename(L0 "/deep", L0 "/dir/moved", &err) && hk_path_stat(L0 "/dir/moved/er/inner.bin", &st, &err) && st.size == 3);
    CHECK(!hk_path_rename(L0 "/nothing", L0 "/something", &err) && err == HK_E_NOENT);
    CHECK(!hk_path_rename(L0, L1 "/zero", &err) && err == HK_E_BUSY);
    CHECK(!hk_path_rename(L0 "/dir", L1, &err) && err == HK_E_BUSY);
    free(plain);
    plain = hk_sprintf("%s/outside.txt", scratch);
    CHECK(!hk_path_rename(L1 "/d.txt", plain, &err) && err == HK_E_XDEV);
    CHECK(hk_file_write_all(plain, "out", 3));
    CHECK(!hk_path_rename(plain, L1 "/inside.txt", &err) && err == HK_E_XDEV);
    CHECK(hk_path_stat(plain, &st, NULL) && hk_path_stat(L1 "/d.txt", &st, NULL) && !hk_path_stat(L1 "/inside.txt", &st, NULL));
    CHECK(hk_path_remove(plain, &err));

    /* Removing: a file as a file, a folder as a folder and only when it is empty; never a root. */
    CHECK(!hk_path_remove(L0 "/dir", &err) && err == HK_E_ISDIR);
    CHECK(!hk_path_rmdir(L0 "/dir", &err) && err == HK_E_NOTEMPTY);
    CHECK(!hk_path_rmdir(L1 "/d.txt", &err) && err == HK_E_NOTDIR);
    CHECK(!hk_path_remove(L1 "/nothing", &err) && err == HK_E_NOENT);
    CHECK(!hk_path_rmdir(L0, &err) && err == HK_E_BUSY);
    CHECK(!hk_path_remove(L0, &err) && err == HK_E_ISDIR);
    CHECK(hk_path_remove(L1 "/d.txt", &err) && !hk_path_stat(L1 "/d.txt", &st, NULL));
    CHECK(hk_path_remove(L0 "/dir/moved/er/inner.bin", &err));
    CHECK(hk_path_rmdir(L0 "/dir/moved/er/still", &err) && hk_path_rmdir(L0 "/dir/moved/er", &err) && hk_path_rmdir(L0 "/dir/moved", &err));
    CHECK(hk_path_rmdir(L0 "/dir", &err));
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L0, list_one, &seen, &err) && seen.count == 0);
    memset(&seen, 0, sizeof(seen));
    CHECK(hk_path_list(L1, list_one, &seen, &err) && seen.count == 0);
    free(plain);

    /* And when whoever answered is gone, so are the paths. */
    hk_foreign_set(NULL);
    CHECK(!hk_path_stat(L0, &st, &err) && err == HK_E_NOENT);
}

/* ------------------------------------------------- the storage an app sees
 *
 * The C library as an app's code has it. The structures are bionic's, as
 * hk_libc_io.c fills them in.
 */
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
    i32 at_sec, at_nsec, mt_sec, mt_nsec, ct_sec, ct_nsec;
    u64 st_ino;
} t_stat;

typedef struct t_dirent {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[256];
} t_dirent;

typedef struct t_statfs {
    u32 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    u32 f_namelen, f_frsize, f_flags, f_spare[4];
} t_statfs;

typedef struct t_statvfs {
    u32 f_bsize, f_frsize;
    u32 f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
} t_statvfs;

enum { T_RDONLY = 0, T_WRONLY = 1, T_RDWR = 2, T_CREAT = 0100, T_EXCL = 0200, T_TRUNC = 01000, T_APPEND = 02000, T_DIRECTORY = 040000 };
#define T_IFDIR 0040000u
#define T_IFREG 0100000u
#define T_DT_DIR 4
#define T_DT_REG 8

static struct {
    int (*open)(const char *, int, int);
    int (*close)(int);
    i32 (*read)(int, void *, u32);
    i32 (*write)(int, const void *, u32);
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
    for (i = 0; i < HK_COUNT(names); i++) {
        slot[i] = hk_bionic_find(names[i]);
        if (!slot[i]) return false;
    }
    return true;
}

static int last_errno(void) { return *hk_errno_ptr(); }

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
    char names[48][64];
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
                hk_strlcpy(s->names[s->count], e->d_name, sizeof(s->names[0]));
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
    hk_stat st;
    char *own = hk_sprintf("%s/apps/" PKG "/sdcard", data), *shared = hk_sprintf("%s/sdcard", data), *p, *text;
    char real[4096];
    size_t i;
    i32 calls;
    say("the shared storage: one for every app, the app's own part in it, and links to folders of the phone's");

    /* What an app left before the storage was shared: its own part, and something outside it. */
    p = hk_sprintf("%s/Android/data/" PKG "/files", own);
    CHECK(hk_path_mkdirs(p));
    free(p);
    p = hk_sprintf("%s/Android/data/" PKG "/files/old-save.dat", own);
    CHECK(hk_file_write_all(p, "saved", 5));
    free(p);
    p = hk_sprintf("%s/OldGame", own);
    CHECK(hk_path_mkdirs(p));
    free(p);
    /* And a folder of the shared storage's own by a name that is about to be a link. */
    p = hk_sprintf("%s/Music", shared);
    CHECK(hk_path_mkdirs(p));
    free(p);
    p = hk_sprintf("%s/Music/hidden.txt", shared);
    CHECK(hk_file_write_all(p, "h", 1));
    free(p);
    p = hk_sprintf("%s/book.txt", g_roots[2]);
    CHECK(hk_file_write_all(p, "a book", 6));
    free(p);

    memset(&host, 0, sizeof(host));
    host.links = k_links;
    host.link_count = (int)HK_COUNT(k_links);
    host.open = counted_open;
    host.make_folder = counted_mkdir;
    host.remove = counted_remove;
    host.rename = counted_rename;
    hk_run_files(data, PKG, &host);

    /* Five names, one place. */
    CHECK(put("/sdcard/one.txt", "one place"));
    for (i = 0; i < HK_COUNT(names); i++) {
        p = hk_sprintf("%s/one.txt", names[i]);
        CHECK(is_dir(names[i]) && !strcmp(get(p), "one place"));
        free(p);
    }
    p = hk_sprintf("%s/one.txt", shared);
    text = text_of(p);
    CHECK(!strcmp(text, "one place"));
    free(text);
    free(p);
    CHECK(C.realpath("/mnt/sdcard/one.txt", real) == real && !strcmp(real, SD "/one.txt"));
    CHECK(C.realpath("/storage/self/primary", real) == real && !strcmp(real, SD));
    p = hk_fs_host_path("/storage/sdcard0/Download/../one.txt");
    text = hk_sprintf("%s/one.txt", shared);
    CHECK(p && !strcmp(p, text));
    free(p);
    free(text);
    CHECK(!strcmp(hk_fs_external_dir(), SD));
    CHECK(stat_errno("/sdcard2") == GE_NOENT && stat_errno("/storage/emulated/1") == GE_NOENT);

    /* The standard folders are there from the start, each once, a link among them like any other. */
    read_dir("/sdcard", &seen);
    CHECK(seen.dots);
    for (i = 0; i < HK_COUNT(standard); i++) {
        CHECK_INT(times(&seen, standard[i]), 1);
        CHECK_INT(type_of(&seen, standard[i]), T_DT_DIR);
        p = hk_sprintf("/sdcard/%s", standard[i]);
        CHECK(is_dir(p) && C.access(p, 0) == 0);
        free(p);
    }
    CHECK(times(&seen, "Books") == 1 && times(&seen, "Android") == 1 && times(&seen, "one.txt") == 1 && type_of(&seen, "one.txt") == T_DT_REG);
    CHECK(!times(&seen, "music") && !times(&seen, "a") && !times(&seen, "OldGame"));
    CHECK_INT(seen.count, 13);
    /* Real folders in the shared storage's own folder, but for the links, which have none of their own to be. */
    p = hk_sprintf("%s/Download", shared);
    CHECK(hk_path_stat(p, &st, NULL) && st.is_dir);
    free(p);
    p = hk_sprintf("%s/Pictures", shared);
    CHECK(!hk_path_stat(p, &st, NULL));
    free(p);
    p = hk_sprintf("%s/Books", shared);
    CHECK(!hk_path_stat(p, &st, NULL));
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
    p = hk_sprintf("%s/x.txt", g_roots[0]);
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
    p = hk_sprintf("%s/Music/hidden.txt", shared);
    CHECK(hk_path_stat(p, &st, NULL));
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
    p = hk_sprintf("%s/z.txt", g_roots[1]);
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
    p = hk_sprintf("%s/Android/data/" PKG "/files/save.dat", own);
    CHECK(hk_path_stat(p, &st, NULL) && st.size == 3);
    free(p);
    p = hk_sprintf("%s/Android/data/" PKG, shared);
    CHECK(!hk_path_stat(p, &st, NULL));
    free(p);
    read_dir("/sdcard/Android", &seen);
    CHECK(seen.count == 2 && times(&seen, "data") == 1 && times(&seen, "obb") == 1);
    read_dir("/sdcard/Android/data", &seen);
    CHECK(seen.count == 1 && times(&seen, PKG) == 1 && type_of(&seen, PKG) == T_DT_DIR);
    CHECK_INT(stat_errno("/sdcard/Android/data/com.someone.else"), GE_NOENT);
    CHECK_INT(open_errno("/sdcard/Android/data/com.someone.else/files/x", T_RDONLY), GE_NOENT);
    CHECK(C.rmdir("/sdcard/Android/data/" PKG) == -1 && last_errno() == GE_BUSY);
    /* What the app kept outside its own part is where it was on the disk, and not in the shared storage (the log says so). */
    CHECK_INT(stat_errno("/sdcard/OldGame"), GE_NOENT);
    p = hk_sprintf("%s/OldGame", own);
    CHECK(hk_path_stat(p, &st, NULL) && st.is_dir);
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
    p = hk_fs_host_path("/sdcard/Books/book.txt");
    CHECK(p && hk_path_is_foreign(p) && !hk_path_remove(p, NULL) && !hk_file_write_all(p, "x", 1));
    free(p);
    p = hk_sprintf("%s/book.txt", g_roots[2]);
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
        CHECK(!hk_path_stat(HK_FOREIGN_PREFIX "3", &st, &err) && err == HK_E_NOENT && !hk_path_stat(HK_FOREIGN_PREFIX "5/x", &st, &err) && err == HK_E_NOENT);
        CHECK(!hk_path_mkdir(HK_FOREIGN_PREFIX "4/made", &err) && err == HK_E_NOENT && !hk_path_stat(HK_FOREIGN_PREFIX "31", &st, &err));
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
        CHECK(hk_path_space(shared, &total, &available) && total / 4096 == a.f_blocks && available <= total);
        CHECK(!hk_path_space(HK_FOREIGN_PREFIX "0", &total, &available));
    }
    free(own);
    free(shared);
}

/* Two threads of an app in one link at once: each makes, writes, reads, lists and removes its own. */
typedef struct worker {
    int id, failures;
} worker;

static void worker_run(void *arg)
{
    worker *w = (worker *)arg;
    char dir[96], file[128], buf[64], want[32];
    dir_seen *seen = (dir_seen *)hk_alloc(sizeof(*seen));
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
    hk_thread *ta, *tb;
    dir_seen seen;
    say("the shared storage: two threads in one link at once");
    ta = hk_thread_start(worker_run, &a, 0, "storage-1");
    tb = hk_thread_start(worker_run, &b, 0, "storage-2");
    CHECK(ta && tb);
    if (ta) hk_thread_join(ta);
    if (tb) hk_thread_join(tb);
    CHECK_INT(a.failures, 0);
    CHECK_INT(b.failures, 0);
    read_dir("/sdcard/Music", &seen);
    CHECK_INT(seen.count, 0);
}

/* With no link at all the storage is the layer's own folders and nothing is asked of anyone. */
static void test_no_links(const char *data)
{
    dir_seen seen;
    hk_stat st;
    char *p;
    i32 calls = g_calls;
    say("the shared storage: with no links");
    hk_run_storage(data, NULL);
    read_dir("/sdcard", &seen);
    CHECK(times(&seen, "Music") == 1 && times(&seen, "Pictures") == 1 && !times(&seen, "Books") && times(&seen, "Android") == 1);
    CHECK_INT(seen.count, 12);
    /* The folder the link stood in front of is back in sight. */
    CHECK(!strcmp(get("/sdcard/Music/hidden.txt"), "h"));
    CHECK(put("/sdcard/Pictures/p.txt", "plain") && C.mkdir("/sdcard/Pictures/album", 0777) == 0);
    p = hk_sprintf("%s/sdcard/Pictures/p.txt", data);
    CHECK(hk_path_stat(p, &st, NULL) && st.size == 5);
    free(p);
    CHECK(C.rename("/sdcard/Pictures/p.txt", "/sdcard/Music/p.txt") == 0 && C.unlink("/sdcard/Music/p.txt") == 0 && C.rmdir("/sdcard/Pictures/album") == 0);
    CHECK(C.rmdir("/sdcard/Music") == -1 && last_errno() == GE_NOTEMPTY);
    CHECK(is_dir("/sdcard/Android/data/" PKG) && !strcmp(get("/sdcard/Android/data/" PKG "/files/save.dat"), "new"));
    CHECK_INT(g_calls - calls, 0);
}

/*
 * All of it, in folders of its own under `base`: three for the roots and one
 * for the data. `make` NULL: the stand-in is asked; else whoever `make`
 * returns for the three folders, under the names the layout's tests link
 * them by.
 */
static int run_in(const char *base, hk_storage_host_maker make, hk_test_sink sink, void *user, int *checks)
{
    static const char *const names[FAKE_ROOTS] = { "Music", "Pictures", "Books" };
    char *data = hk_sprintf("%s/data", base);
    const megler_storage_host *made = NULL;
    int i;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    for (i = 0; i < FAKE_ROOTS; i++) {
        g_roots[i] = hk_sprintf("%s/root%d", base, i);
        hk_path_mkdirs(g_roots[i]);
        wipe(g_roots[i], 0);
    }
    hk_path_mkdirs(data);
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
            test_layout(data);
            test_threads();
            test_no_links(data);
            /* As the tests after this one found things: no links, nobody to ask, and the current directory where it was. */
            C.chdir("/");
        }
    }
    free(data);
    hk_foreign_set(NULL);
    for (i = 0; i < FAKE_ROOTS; i++) {
        free(g_roots[i]);
        g_roots[i] = NULL;
    }
    if (checks) *checks += t_checks;
    return t_failed;
}

int hk_storage_tests_run(const char *scratch, hk_test_sink sink, void *user, int *checks)
{
    char *base = hk_sprintf("%s/storage", scratch);
    int failed = run_in(base, NULL, sink, user, checks);
    free(base);
    return failed;
}

int hk_storage_tests_run_shell(const char *scratch, hk_storage_host_maker make, hk_test_sink sink, void *user, int *checks)
{
    char *base = hk_sprintf("%s/storage-shell", scratch);
    int failed = run_in(base, make, sink, user, checks);
    free(base);
    return failed;
}
