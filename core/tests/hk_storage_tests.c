/*
 * The phone's own folders in an app's storage, tried without a phone.
 *
 * On a phone the user's music, pictures and videos are files Windows will
 * not open by path for an app; its broker hands over handles for them, and
 * the shell passes those on (megler_storage_host). Here a stand-in does the
 * same with folders under the tests' scratch folder: it opens them with the
 * operating system's plain calls and hands the handles over. Everything
 * between that and an app -- the host layer's foreign paths -- is the code
 * a phone runs.
 *
 * What a stand-in cannot say is whether the broker's handles do all that a
 * plain one does. core/tests is not where that is found out.
 */
#include "hk_storage_tests.h"

#include <stdio.h>

#include "../src/hk_base.h"
#include "../src/host/hk_host.h"

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
static volatile i32 g_fake_calls;      /* how often the stand-in was asked for anything */

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
    hk_atomic_add(&g_fake_calls, 1);
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
    hk_atomic_add(&g_fake_calls, 1);
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
    hk_atomic_add(&g_fake_calls, 1);
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
    hk_atomic_add(&g_fake_calls, 1);
    if (!a || !b) *err = HK_E_NOENT;
    else ok = hk_path_rename(a, b, err);
    free(a);
    free(b);
    return ok;
}

static const hk_foreign k_fake = { NULL, fake_open, fake_mkdir, fake_remove, fake_rename };

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

    hk_foreign_set(&k_fake);
    CHECK(hk_path_stat(L0, &st, &err) && st.is_dir);
    CHECK(hk_path_stat(L1, &st, &err) && st.is_dir);
    /* A root the stand-in does not have, and a file that is not there. */
    CHECK(!hk_path_stat(HK_FOREIGN_PREFIX "7", &st, &err) && err == HK_E_NOENT);
    CHECK(!hk_path_stat(L0 "/nothing", &st, &err) && err == HK_E_NOENT);
    CHECK(hk_file_open(L0 "/nothing", HK_O_READ, &err) == NULL && err == HK_E_NOENT);

    /* Paths that could mean another place to the operating system never reach whoever answers. */
    calls = g_fake_calls;
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
    CHECK_INT(g_fake_calls - calls, 0);

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

int hk_storage_tests_run(const char *scratch, hk_test_sink sink, void *user, int *checks)
{
    int i;
    t_sink = sink;
    t_sink_user = user;
    t_checks = t_failed = 0;
    for (i = 0; i < FAKE_ROOTS; i++) {
        g_roots[i] = hk_sprintf("%s/storage/root%d", scratch, i);
        hk_path_mkdirs(g_roots[i]);
        wipe(g_roots[i], 0);
    }
    test_foreign_paths(scratch);
    hk_foreign_set(NULL);
    for (i = 0; i < FAKE_ROOTS; i++) {
        free(g_roots[i]);
        g_roots[i] = NULL;
    }
    if (checks) *checks += t_checks;
    return t_failed;
}
