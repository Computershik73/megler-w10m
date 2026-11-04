/* The log, the allocator wrappers, and the file helpers that need no OS call of their own. */
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

/* ------------------------------------------------------------------ files */

/*
 * Foreign paths (host/mg_host.h): who answers for them, a path taken apart,
 * and what is the same on every host -- everything but what is done with a
 * handle once there is one.
 */
static mg_foreign g_foreign;
static volatile i32 g_foreign_set;

void mg_foreign_set(const mg_foreign *f)
{
    mg_atomic_xchg(&g_foreign_set, 0);
    if (!f) return;
    g_foreign = *f;
    mg_atomic_xchg(&g_foreign_set, 1);
}

bool mg_path_is_foreign(const char *path) { return path && !strncmp(path, MG_FOREIGN_PREFIX, sizeof(MG_FOREIGN_PREFIX) - 1); }

/* ------------------------------------------------ names Windows refuses (mg_host.h) */

static bool same3(const char *s, const char *upper)
{
    int i;
    for (i = 0; i < 3; i++)
        if ((s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]) != upper[i]) return false;
    return true;
}

/* CON, PRN, AUX, NUL, COM0-9, LPT0-9: what comes before the first dot, without the spaces at its end, in any case. */
static bool device_name(const char *s, size_t n)
{
    size_t stem = 0;
    while (stem < n && s[stem] != '.') stem++;
    while (stem > 0 && s[stem - 1] == ' ') stem--;
    if (stem == 3) return same3(s, "CON") || same3(s, "PRN") || same3(s, "AUX") || same3(s, "NUL");
    return stem == 4 && (same3(s, "COM") || same3(s, "LPT")) && s[3] >= '0' && s[3] <= '9';
}

static bool refused_char(unsigned char c, bool backslash)
{
    return (c >= 1 && c < 32) || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*' || (backslash && c == '\\');
}

static bool is_sep(char c, bool host) { return c == '/' || (host && c == '\\'); }

char *mg_name_to_host(const char *path, int how)
{
    size_t n = strlen(path), i = 0, start, len, k;
    bool host = how == MG_NAME_HOST;
    /* At most three bytes for each one. */
    char *out = (char *)mg_alloc(3 * n + 1), *o = out;
    if (host) {
        /* What names a place to Windows, not a file: \\?\ or \\.\, then a drive's letter and colon. */
        if (n >= 4 && is_sep(path[0], true) && is_sep(path[1], true) && (path[2] == '?' || path[2] == '.') && is_sep(path[3], true)) i = 4;
        if (n >= i + 2 && ((path[i] >= 'A' && path[i] <= 'Z') || (path[i] >= 'a' && path[i] <= 'z')) && path[i + 1] == ':' && (n == i + 2 || is_sep(path[i + 2], true)))
            i += 2;
        memcpy(o, path, i);
        o += i;
    }
    while (i <= n) {
        const char *s;
        bool dots, device;
        start = i;
        while (i < n && !is_sep(path[i], host)) i++;
        s = path + start;
        len = i - start;
        dots = (len == 1 && s[0] == '.') || (len == 2 && s[0] == '.' && s[1] == '.');
        device = !dots && len && device_name(s, len);
        for (k = 0; k < len; k++) {
            unsigned char c = (unsigned char)s[k];
            if (!dots && ((k == 0 && device) || refused_char(c, !host) || (k == len - 1 && (c == '.' || c == ' ')))) {
                unsigned cp = 0xF000u + c;
                *o++ = (char)0xEF;
                *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *o++ = (char)c;
            }
        }
        if (i < n) *o++ = path[i];
        i++;
    }
    *o = 0;
    return out;
}

void mg_name_from_host(char *name)
{
    char *r = name, *w = name;
    while (*r) {
        unsigned char a = (unsigned char)r[0], b = (unsigned char)r[1], c = b ? (unsigned char)r[2] : 0;
        if (a == 0xEF && (b == 0x80 || b == 0x81) && (c & 0xC0) == 0x80) {
            unsigned v = ((unsigned)(b & 0x3F) << 6) | (c & 0x3Fu);
            if (v >= 1 && v < 0x80) {
                *w++ = (char)v;
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/* The root's number and what follows it ("" for the root itself); false for a path that is not a well-formed foreign one. */
static bool foreign_split(const char *path, int *root, const char **rel)
{
    const char *p = path + sizeof(MG_FOREIGN_PREFIX) - 1, *part;
    int n = 0, digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 6; p++, digits++) n = n * 10 + (*p - '0');
    if (!digits || (*p && *p != '/')) return false;
    *root = n;
    if (!*p) {
        *rel = p;
        return true;
    }
    *rel = ++p;
    /* Every part has something in it and is not "." or "..". (A backslash or a colon is a name's, and mapped before anyone is asked: mg_name_to_host.) */
    for (part = p;; p++) {
        if (*p && *p != '/') continue;
        if (p == part || (p - part == 1 && part[0] == '.') || (p - part == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!*p) return true;
        part = p + 1;
    }
}

/* Who answers for `path` and what to ask for; NULL with *err set when nobody does or it is not a path to ask about. */
static const mg_foreign *foreign_of(const char *path, int *root, const char **rel, int *err)
{
    if (!g_foreign_set) {
        if (err) *err = MG_E_NOENT;
        return NULL;
    }
    if (!foreign_split(path, root, rel)) {
        if (err) *err = MG_E_INVAL;
        return NULL;
    }
    return &g_foreign;
}

static bool foreign_fail(int *err, int e)
{
    if (err) *err = e ? e : MG_E_IO;
    return false;
}

void *mg_foreign_open(const char *path, int flags, int kind, int *err)
{
    int root = 0, e = MG_E_IO;
    const char *rel = "";
    const mg_foreign *f = foreign_of(path, &root, &rel, &e);
    void *h = NULL;
    if (!f) {
        /* (foreign_of has said why.) */
    } else if (!rel[0] && kind == MG_FOREIGN_FILE) {
        e = MG_E_ISDIR;
    } else if (!f->open) {
        e = MG_E_ACCESS;
    } else {
        char *mapped = mg_name_to_host(rel, MG_NAME_FOREIGN);
        e = MG_E_IO;
        h = f->open(f->user, root, mapped, flags, kind, &e);
        free(mapped);
    }
    if (!h) {
        foreign_fail(err, e);
        return NULL;
    }
    if (err) *err = MG_E_OK;
    return h;
}

bool mg_foreign_mkdir(const char *path, int *err)
{
    int root = 0, e = MG_E_IO;
    const char *rel = "";
    const mg_foreign *f = foreign_of(path, &root, &rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!rel[0]) return foreign_fail(err, MG_E_EXIST);
    if (!f->mkdir) return foreign_fail(err, MG_E_ACCESS);
    e = MG_E_IO;
    {
        char *mapped = mg_name_to_host(rel, MG_NAME_FOREIGN);
        bool ok = f->mkdir(f->user, root, mapped, &e);
        free(mapped);
        return ok || foreign_fail(err, e);
    }
}

bool mg_foreign_remove(const char *path, bool dir, int *err)
{
    int root = 0, e = MG_E_IO;
    const char *rel = "";
    const mg_foreign *f = foreign_of(path, &root, &rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!rel[0]) return foreign_fail(err, dir ? MG_E_BUSY : MG_E_ISDIR);
    if (!f->remove) return foreign_fail(err, MG_E_ACCESS);
    e = MG_E_IO;
    {
        char *mapped = mg_name_to_host(rel, MG_NAME_FOREIGN);
        bool ok = f->remove(f->user, root, mapped, dir, &e);
        free(mapped);
        return ok || foreign_fail(err, e);
    }
}

bool mg_foreign_rename(const char *from, const char *to, int *err)
{
    int from_root = 0, to_root = 0, e = MG_E_IO;
    const char *from_rel = "", *to_rel = "";
    const mg_foreign *f;
    /* One of them is an ordinary path: another file system, as far as a rename goes. */
    if (!mg_path_is_foreign(from) || !mg_path_is_foreign(to)) return foreign_fail(err, MG_E_XDEV);
    f = foreign_of(from, &from_root, &from_rel, &e);
    if (f) f = foreign_of(to, &to_root, &to_rel, &e);
    if (!f) return foreign_fail(err, e);
    if (!from_rel[0] || !to_rel[0]) return foreign_fail(err, MG_E_BUSY);
    if (!f->rename) return foreign_fail(err, MG_E_ACCESS);
    e = MG_E_IO;
    {
        char *from_mapped = mg_name_to_host(from_rel, MG_NAME_FOREIGN), *to_mapped = mg_name_to_host(to_rel, MG_NAME_FOREIGN);
        bool ok = f->rename(f->user, from_root, from_mapped, to_root, to_mapped, &e);
        free(from_mapped);
        free(to_mapped);
        return ok || foreign_fail(err, e);
    }
}

bool mg_path_mkdirs(const char *path)
{
    char *t = mg_strdup(path);
    char *p;
    mg_stat st;
    bool ok;
    for (p = t + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        /* "C:/" is not something to create. */
        if (p > t && p[-1] == ':') continue;
        *p = 0;
        mg_path_mkdir(t, NULL);
        *p = '/';
    }
    mg_path_mkdir(t, NULL);
    ok = mg_path_stat(t, &st, NULL) && st.is_dir;
    free(t);
    return ok;
}

u8 *mg_file_read_all(const char *path, size_t *size)
{
    mg_file *f = mg_file_open(path, MG_O_READ, NULL);
    mg_stat st;
    u8 *buf;
    size_t got = 0;
    if (!f) return NULL;
    if (!mg_file_stat(f, &st) || st.size > 0x7FFFFFFFu) {
        mg_file_close(f);
        return NULL;
    }
    buf = (u8 *)mg_alloc((size_t)st.size + 1);
    while (got < (size_t)st.size) {
        i64 r = mg_file_read(f, buf + got, (size_t)st.size - got, NULL);
        if (r <= 0) break;
        got += (size_t)r;
    }
    mg_file_close(f);
    buf[got] = 0;
    if (size) *size = got;
    return buf;
}

bool mg_file_write_all(const char *path, const void *data, size_t size)
{
    mg_file *f = mg_file_open(path, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, NULL);
    size_t put = 0;
    if (!f) return false;
    while (put < size) {
        i64 r = mg_file_write(f, (const u8 *)data + put, size - put, NULL);
        if (r <= 0) break;
        put += (size_t)r;
    }
    mg_file_close(f);
    return put == size;
}
