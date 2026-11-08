/* The log, the allocator wrappers and the trace. (The file helpers that need no OS call of their own are the host layer's: host/mg_host_common.c.) */
#include "mg_base.h"
#include "host/mg_host.h"

#include <stdio.h>

/* ------------------------------------------------------------------- log */

static mg_log_sink g_sink;
static void *g_sink_user;
static int g_min_level = MG_LOG_INFO;
static mg_lock g_log_lock = MG_LOCK_INIT;
/* Who holds g_log_lock (mg_log_holder): the host thread, since when, and the line, in its own frame while it writes it. */
static volatile u32 g_log_owner;
static volatile u64 g_log_owned_at;
static const char *volatile g_log_writing;

void mg_log_set_sink(mg_log_sink sink, void *user)
{
    g_sink = sink;
    g_sink_user = user;
}

mg_log_sink mg_log_get_sink(void **user)
{
    if (user) *user = g_sink_user;
    return g_sink;
}

void mg_log_set_level(int min_level) { g_min_level = min_level; }

void mg_logv(int level, const char *fmt, va_list ap)
{
    char line[1024];
    if (level < g_min_level) return;
    vsnprintf(line, sizeof(line), fmt, ap);
    if (g_log_owner && g_log_owner == mg_thread_self_id()) {
        /*
         * A fault or an exception on this thread while it writes a line, which the handler reports through the log too:
         * the lock is this thread's already, and taken again it would never come (the profiler's thread stood so, and
         * every thread that logged after it). The line goes out without it.
         */
        mg_host_print(line);
        if (g_sink) g_sink(level, line, g_sink_user);
        return;
    }
    mg_lock_acquire(&g_log_lock);
    g_log_owned_at = mg_time_monotonic_ns();
    g_log_writing = line;
    g_log_owner = mg_thread_self_id();
    mg_host_print(line);
    if (g_sink) g_sink(level, line, g_sink_user);
    g_log_owner = 0;
    mg_lock_release(&g_log_lock);
}

bool mg_log_holder(u32 *thread, u64 *since_ns, char *line, size_t size)
{
    u32 owner = g_log_owner;
    const char *writing = g_log_writing;
    if (!owner) return false;
    *thread = owner;
    *since_ns = g_log_owned_at;
    /* (The line is in the holder's frame, which stays while it holds the lock: read only while it still does.) */
    if (size) {
        line[0] = 0;
        if (writing && g_log_owner == owner) mg_strlcpy(line, writing, size);
    }
    return g_log_owner == owner;
}

void mg_log_unlocked(int level, const char *line)
{
    mg_host_print(line);
    if (g_sink) g_sink(level, line, g_sink_user);
}

void mg_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_logv(level, fmt, ap);
    va_end(ap);
}

void mg_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_logv(MG_LOG_ERROR, fmt, ap);
    va_end(ap);
    abort();
}

#if defined(MG_CORE_ELF)
/*
 * A line the host layer has written: on desktop Windows that layer is Megler.exe's (docs/DESKTOP.md), whose own mg_log
 * (host/win32/mg_host_base.c) formats with Windows' C library and hands the line here, to go where the core's lines go.
 */
MG_EXPORT void mg_log_line(int level, const char *line) { mg_log(level, "%s", line); }
#endif

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
/*
 * The flag trace-live: each thing done is said in the log too, as it is done, at most 30000 of them -- for an app
 * that ends its process itself, before any fault's report could say the trace (TikTok Lite's libmetasec_ov.so).
 * Read once the data folder is known (mg_flag is false before), and kept.
 */
static volatile i32 g_trace_live = -1, g_trace_live_lines;
const char *megler_data_dir(void);
bool mg_flag(const char *name);

void mg_trace(const char *fmt, ...)
{
    u32 slot = (u32)(mg_atomic_add(&g_trace_next, 1) - 1) % TRACE_SLOTS;
    va_list ap;
    g_trace[slot].ms = (u32)(mg_time_monotonic_ns() / 1000000ull);
    g_trace[slot].thread = mg_thread_self_id();
    va_start(ap, fmt);
    vsnprintf(g_trace[slot].text, TRACE_TEXT, fmt, ap);
    va_end(ap);
    if (g_trace_live < 0 && megler_data_dir()[0]) g_trace_live = mg_flag("trace-live") ? 1 : 0;
    if (g_trace_live > 0 && mg_atomic_add(&g_trace_live_lines, 1) <= 30000)
        mg_info("live: %u %s", (unsigned)g_trace[slot].thread, g_trace[slot].text);
}

void mg_trace_dump(int level, int newest)
{
    u32 end = (u32)g_trace_next, count = end < TRACE_SLOTS ? end : TRACE_SLOTS, i;
    if (newest > 0 && (u32)newest < count) count = (u32)newest;
    mg_log(level, "trace: the last %u things done, oldest first (milliseconds, thread, what)", (unsigned)count);
    for (i = end - count; i != end; i++) {
        u32 slot = i % TRACE_SLOTS;
        mg_log(level, "trace: %u %u %s", (unsigned)g_trace[slot].ms, (unsigned)g_trace[slot].thread, g_trace[slot].text);
    }
}

/* Notes already made, by a hash of their text; a collision only loses a repeat of another note. */
#define NOTE_SLOTS 512
static u32 g_notes[NOTE_SLOTS];
static mg_lock g_note_lock = MG_LOCK_INIT;

void mg_note_once(const char *fmt, ...)
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
    mg_lock_acquire(&g_note_lock);
    for (i = 0; i < NOTE_SLOTS; i++) {
        u32 slot = (h + i) % NOTE_SLOTS;
        if (g_notes[slot] == h) { seen = true; break; }
        if (!g_notes[slot]) { g_notes[slot] = h; break; }
    }
    mg_lock_release(&g_note_lock);
    if (!seen) mg_log(MG_LOG_WARN, "%s", line);
}

/* ---------------------------------------------------------------- memory */

void *mg_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) mg_fatal("out of memory (%u bytes)", (unsigned)n);
    return p;
}

void *mg_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) mg_fatal("out of memory (%u bytes)", (unsigned)n);
    return q;
}

char *mg_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)mg_alloc(n);
    memcpy(d, s, n);
    return d;
}

char *mg_sprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    char *s;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    s = (char *)mg_alloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

size_t mg_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}
