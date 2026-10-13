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
