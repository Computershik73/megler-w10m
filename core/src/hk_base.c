/* The log, the allocator wrappers, and the file helpers that need no OS call of their own. */
#include "hk_base.h"
#include "host/hk_host.h"

#include <stdio.h>

/* ------------------------------------------------------------------- log */

static hk_log_sink g_sink;
static void *g_sink_user;
static int g_min_level = HK_LOG_INFO;
static hk_lock g_log_lock = HK_LOCK_INIT;

void hk_log_set_sink(hk_log_sink sink, void *user)
{
    g_sink = sink;
    g_sink_user = user;
}

void hk_log_set_level(int min_level) { g_min_level = min_level; }

void hk_logv(int level, const char *fmt, va_list ap)
{
    char line[1024];
    if (level < g_min_level) return;
    vsnprintf(line, sizeof(line), fmt, ap);
    hk_lock_acquire(&g_log_lock);
    hk_host_print(line);
    if (g_sink) g_sink(level, line, g_sink_user);
    hk_lock_release(&g_log_lock);
}

void hk_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hk_logv(level, fmt, ap);
    va_end(ap);
}

void hk_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hk_logv(HK_LOG_ERROR, fmt, ap);
    va_end(ap);
    abort();
}

/* ---------------------------------------------------------------- the trace
 *
 * A ring of the last few hundred things the layer did for the app: cheap to
 * write (no lock, no file), read only when something has gone wrong, when the
 * crash report prints it. What a log line a second could never show -- the
 * hundred calls before a failure -- is here.
 */
#define TRACE_SLOTS 4096
#define TRACE_TEXT 108
static struct {
    u32 ms, thread;
    char text[TRACE_TEXT];
} g_trace[TRACE_SLOTS];
static volatile i32 g_trace_next;

void hk_trace(const char *fmt, ...)
{
    u32 slot = (u32)(hk_atomic_add(&g_trace_next, 1) - 1) % TRACE_SLOTS;
    va_list ap;
    g_trace[slot].ms = (u32)(hk_time_monotonic_ns() / 1000000ull);
    g_trace[slot].thread = hk_thread_self_id();
    va_start(ap, fmt);
    vsnprintf(g_trace[slot].text, TRACE_TEXT, fmt, ap);
    va_end(ap);
}

void hk_trace_dump(int level, int newest)
{
    u32 end = (u32)g_trace_next, count = end < TRACE_SLOTS ? end : TRACE_SLOTS, i;
    if (newest > 0 && (u32)newest < count) count = (u32)newest;
    hk_log(level, "trace: the last %u things done, oldest first (milliseconds, thread, what)", (unsigned)count);
    for (i = end - count; i != end; i++) {
        u32 slot = i % TRACE_SLOTS;
        hk_log(level, "trace: %u %u %s", (unsigned)g_trace[slot].ms, (unsigned)g_trace[slot].thread, g_trace[slot].text);
    }
}

/* Notes already made, by a hash of their text; a collision only loses a repeat of another note. */
#define NOTE_SLOTS 512
static u32 g_notes[NOTE_SLOTS];
static hk_lock g_note_lock = HK_LOCK_INIT;

void hk_note_once(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    u32 h = 2166136261u, i;
    const char *p;
    bool seen = false;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    for (p = line; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    if (!h) h = 1;
    hk_lock_acquire(&g_note_lock);
    for (i = 0; i < NOTE_SLOTS; i++) {
        u32 slot = (h + i) % NOTE_SLOTS;
        if (g_notes[slot] == h) { seen = true; break; }
        if (!g_notes[slot]) { g_notes[slot] = h; break; }
    }
    hk_lock_release(&g_note_lock);
    if (!seen) hk_log(HK_LOG_WARN, "%s", line);
}

/* ---------------------------------------------------------------- memory */

void *hk_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) hk_fatal("out of memory (%u bytes)", (unsigned)n);
    return p;
}

void *hk_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) hk_fatal("out of memory (%u bytes)", (unsigned)n);
    return q;
}

char *hk_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)hk_alloc(n);
    memcpy(d, s, n);
    return d;
}

char *hk_sprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    char *s;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    s = (char *)hk_alloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

size_t hk_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

/* ------------------------------------------------------------------ files */

/*
 * Foreign paths (host/hk_host.h): who answers for them, a path taken apart,
 * and what is the same on every host -- everything but what is done with a
 * handle once there is one.
 */
static hk_foreign g_foreign;
static volatile i32 g_foreign_set;

void hk_foreign_set(const hk_foreign *f)
{
    hk_atomic_xchg(&g_foreign_set, 0);
    if (!f) return;
    g_foreign = *f;
    hk_atomic_xchg(&g_foreign_set, 1);
}

bool hk_path_is_foreign(const char *path) { return path && !strncmp(path, HK_FOREIGN_PREFIX, sizeof(HK_FOREIGN_PREFIX) - 1); }

/* The root's number and what follows it ("" for the root itself); false for a path that is not a well-formed foreign one. */
static bool foreign_split(const char *path, int *root, const char **rel)
{
    const char *p = path + sizeof(HK_FOREIGN_PREFIX) - 1, *part;
    int n = 0, digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 6; p++, digits++) n = n * 10 + (*p - '0');
    if (!digits || (*p && *p != '/')) return false;
    *root = n;
    if (!*p) {
        *rel = p;
        return true;
    }
    *rel = ++p;
    /* Every part has something in it and is not "." or ".."; nothing that would mean another place to the operating system. */
    for (part = p;; p++) {
        if (*p == '\\' || *p == ':') return false;
        if (*p && *p != '/') continue;
        if (p == part || (p - part == 1 && part[0] == '.') || (p - part == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!*p) return true;
        part = p + 1;
    }
}

/* Who answers for `path` and what to ask for; NULL with *err set when nobody does or it is not a path to ask about. */
static const hk_foreign *foreign_of(const char *path, int *root, const char **rel, int *err)
{
    if (!g_foreign_set) {
        if (err) *err = HK_E_NOENT;
        return NULL;
    }
    if (!foreign_split(path, root, rel)) {
        if (err) *err = HK_E_INVAL;
        return NULL;
    }
    return &g_foreign;
}

static bool foreign_fail(int *err, int e)
{
    if (err) *err = e ? e : HK_E_IO;
    return false;
}

void *hk_foreign_open(const char *path, int flags, int kind, int *err)
{
    int root = 0, e = HK_E_IO;
    const char *rel = "";
    const hk_foreign *f = foreign_of(path, &root, &rel, &e);
    void *h = NULL;
    if (!f) {
        /* (foreign_of has said why.) */
    } else if (!rel[0] && kind == HK_FOREIGN_FILE) {
        e = HK_E_ISDIR;
    } else if (!f->open) {
        e = HK_E_ACCESS;
    } else {
        e = HK_E_IO;
        h = f->open(f->user, root, rel, flags, kind, &e);
    }
    if (!h) {
        foreign_fail(err, e);
        return NULL;
    }
    if (err) *err = HK_E_OK;
    return h;
}

bool hk_foreign_mkdir(const char *path, int *err)
{
    int root = 0, e = HK_E_IO;
    const char *rel = "";
    const hk_foreign *f = foreign_of(path, &root, &rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!rel[0]) return foreign_fail(err, HK_E_EXIST);
    if (!f->mkdir) return foreign_fail(err, HK_E_ACCESS);
    e = HK_E_IO;
    return f->mkdir(f->user, root, rel, &e) || foreign_fail(err, e);
}

bool hk_foreign_remove(const char *path, bool dir, int *err)
{
    int root = 0, e = HK_E_IO;
    const char *rel = "";
    const hk_foreign *f = foreign_of(path, &root, &rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!rel[0]) return foreign_fail(err, dir ? HK_E_BUSY : HK_E_ISDIR);
    if (!f->remove) return foreign_fail(err, HK_E_ACCESS);
    e = HK_E_IO;
    return f->remove(f->user, root, rel, dir, &e) || foreign_fail(err, e);
}

bool hk_foreign_rename(const char *from, const char *to, int *err)
{
    int from_root = 0, to_root = 0, e = HK_E_IO;
    const char *from_rel = "", *to_rel = "";
    const hk_foreign *f;
    /* One of them is an ordinary path: another file system, as far as a rename goes. */
    if (!hk_path_is_foreign(from) || !hk_path_is_foreign(to)) return foreign_fail(err, HK_E_XDEV);
    f = foreign_of(from, &from_root, &from_rel, &e);
    if (f) f = foreign_of(to, &to_root, &to_rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!from_rel[0] || !to_rel[0]) return foreign_fail(err, HK_E_BUSY);
    if (!f->rename) return foreign_fail(err, HK_E_ACCESS);
    e = HK_E_IO;
    return f->rename(f->user, from_root, from_rel, to_root, to_rel, &e) || foreign_fail(err, e);
}

bool hk_path_mkdirs(const char *path)
{
    char *t = hk_strdup(path);
    char *p;
    hk_stat st;
    bool ok;
    for (p = t + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        /* "C:/" is not something to create. */
        if (p > t && p[-1] == ':') continue;
        *p = 0;
        hk_path_mkdir(t, NULL);
        *p = '/';
    }
    hk_path_mkdir(t, NULL);
    ok = hk_path_stat(t, &st, NULL) && st.is_dir;
    free(t);
    return ok;
}

u8 *hk_file_read_all(const char *path, size_t *size)
{
    hk_file *f = hk_file_open(path, HK_O_READ, NULL);
    hk_stat st;
    u8 *buf;
    size_t got = 0;
    if (!f) return NULL;
    if (!hk_file_stat(f, &st) || st.size > 0x7FFFFFFFu) {
        hk_file_close(f);
        return NULL;
    }
    buf = (u8 *)hk_alloc((size_t)st.size + 1);
    while (got < (size_t)st.size) {
        i64 r = hk_file_read(f, buf + got, (size_t)st.size - got, NULL);
        if (r <= 0) break;
        got += (size_t)r;
    }
    hk_file_close(f);
    buf[got] = 0;
    if (size) *size = got;
    return buf;
}

bool hk_file_write_all(const char *path, const void *data, size_t size)
{
    hk_file *f = hk_file_open(path, HK_O_WRITE | HK_O_CREATE | HK_O_TRUNC, NULL);
    size_t put = 0;
    if (!f) return false;
    while (put < size) {
        i64 r = hk_file_write(f, (const u8 *)data + put, size - put, NULL);
        if (r <= 0) break;
        put += (size_t)r;
    }
    hk_file_close(f);
    return put == size;
}
