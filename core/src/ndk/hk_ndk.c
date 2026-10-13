#include "hk_ndk.h"

#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../bionic/hk_fd.h"
#include "../jni/hk_jni.h"
#include "../zip/hk_zip.h"

/* =========================================================== ANativeWindow */

hk_nwindow *hk_nwindow_create(int width, int height)
{
    hk_nwindow *w = (hk_nwindow *)hk_alloc(sizeof(*w));
    w->refs = 1;
    w->width = width;
    w->height = height;
    w->format = 1;                 /* WINDOW_FORMAT_RGBA_8888 */
    return w;
}

void hk_nwindow_acquire(hk_nwindow *w) { if (w) hk_atomic_add(&w->refs, 1); }
void hk_nwindow_release(hk_nwindow *w)
{
    if (w && hk_atomic_add(&w->refs, -1) == 0) free(w);
}

void hk_nwindow_buffer_size(const hk_nwindow *w, int *width, int *height)
{
    *width = w->buffer_width > 0 ? w->buffer_width : w->width;
    *height = w->buffer_height > 0 ? w->buffer_height : w->height;
}

static int g_nw_width(hk_nwindow *w) { int a, b; if (!w) return -1; hk_nwindow_buffer_size(w, &a, &b); return a; }
static int g_nw_height(hk_nwindow *w) { int a, b; if (!w) return -1; hk_nwindow_buffer_size(w, &a, &b); return b; }
static int g_nw_format(hk_nwindow *w) { return w ? w->format : -1; }

static int g_nw_set_geometry(hk_nwindow *w, int width, int height, int format)
{
    if (!w) return -1;
    /* Takes effect for surfaces made after it, which is when apps call it: before eglCreateWindowSurface. */
    w->buffer_width = width;
    w->buffer_height = height;
    if (format) w->format = format;
    return 0;
}

static int g_nw_lock(hk_nwindow *w, void *buffer, void *dirty)
{
    (void)w; (void)buffer; (void)dirty;
    hk_note_once("ANativeWindow_lock: drawing into the window without OpenGL ES is not supported yet");
    return -1;
}
static int g_nw_unlock(hk_nwindow *w) { (void)w; return -1; }
static int g_nw_ok(hk_nwindow *w) { (void)w; return 0; }
/* A Surface made by a driver carries its window (drivers/hk_unity.c); the caller owns a reference, as on Android. */
static hk_nwindow *g_nw_from_surface(void *env, hk_jobj *surface)
{
    hk_nwindow *w = surface ? (hk_nwindow *)surface->native : NULL;
    (void)env;
    if (w) hk_nwindow_acquire(w);
    return w;
}

/* ================================================================= ALooper */

#define POLL_WAKE (-1)
#define POLL_CALLBACK (-2)
#define POLL_TIMEOUT (-3)
#define POLL_ERROR (-4)

typedef struct looper_fd {
    int fd, ident, events;
    hk_addr callback;
    void *data;
} looper_fd;

typedef struct hk_looper {
    volatile i32 refs;
    int wake_fd;
    hk_lock lock;
    looper_fd *fds;
    int nfds, cap;
    looper_fd pending[32];
    int npending;
} hk_looper;

static HK_THREAD_LOCAL hk_looper *t_looper;
static HK_THREAD_LOCAL bool t_java_looper;     /* the thread runs a Java message loop, which polls its looper for it */

static hk_looper *g_looper_prepare(int opts)
{
    (void)opts;
    if (!t_looper) {
        hk_looper *l = (hk_looper *)hk_alloc(sizeof(*l));
        l->refs = 1;
        hk_lock_init(&l->lock);
        l->wake_fd = hk_fd_eventfd(0, 04000);      /* non-blocking */
        t_looper = l;
    }
    return t_looper;
}

/* A thread with a Java Looper has a looper for the asking; any other has one once it has prepared it. */
static hk_looper *g_looper_for_thread(void) { return t_looper || !t_java_looper ? t_looper : g_looper_prepare(0); }

void hk_looper_java_thread(void) { t_java_looper = true; }

static void g_looper_acquire(hk_looper *l) { if (l) hk_atomic_add(&l->refs, 1); }
static void g_looper_release(hk_looper *l) { if (l) hk_atomic_add(&l->refs, -1); }

static void g_looper_wake(hk_looper *l)
{
    u64 one = 1;
    if (l) hk_fd_write(l->wake_fd, &one, 8);
}

static int g_looper_remove_fd(hk_looper *l, int fd)
{
    int i, removed = 0;
    if (!l) return -1;
    hk_lock_acquire(&l->lock);
    for (i = 0; i < l->nfds; i++) {
        if (l->fds[i].fd == fd) {
            l->fds[i] = l->fds[--l->nfds];
            removed = 1;
            break;
        }
    }
    hk_lock_release(&l->lock);
    g_looper_wake(l);
    return removed;
}

static int g_looper_add_fd(hk_looper *l, int fd, int ident, int events, hk_addr callback, void *data)
{
    int i;
    if (!l || fd < 0) return -1;
    if (callback) ident = POLL_CALLBACK;
    else if (ident < 0) return -1;
    hk_lock_acquire(&l->lock);
    for (i = 0; i < l->nfds && l->fds[i].fd != fd; i++) { }
    if (i == l->nfds) {
        if (l->nfds == l->cap) {
            l->cap = l->cap ? l->cap * 2 : 8;
            l->fds = (looper_fd *)hk_realloc(l->fds, sizeof(looper_fd) * (size_t)l->cap);
        }
        l->nfds++;
    }
    l->fds[i].fd = fd;
    l->fds[i].ident = ident;
    l->fds[i].events = events;
    l->fds[i].callback = callback;
    l->fds[i].data = data;
    hk_lock_release(&l->lock);
    g_looper_wake(l);
    return 1;
}

static int looper_poll_once(hk_looper *l, int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    for (;;) {
        hk_pollfd pfds[65];
        looper_fd snapshot[64];
        int n, i, hits;
        bool woke = false, called = false;
        if (l->npending) {
            looper_fd p = l->pending[0];
            memmove(&l->pending[0], &l->pending[1], sizeof(looper_fd) * (size_t)--l->npending);
            if (out_fd) *out_fd = p.fd;
            if (out_events) *out_events = p.events;
            if (out_data) *out_data = p.data;
            return p.ident;
        }
        hk_lock_acquire(&l->lock);
        n = l->nfds > 64 ? 64 : l->nfds;
        memcpy(snapshot, l->fds, sizeof(looper_fd) * (size_t)n);
        hk_lock_release(&l->lock);
        pfds[0].fd = l->wake_fd;
        pfds[0].events = HK_POLLIN;
        for (i = 0; i < n; i++) {
            pfds[i + 1].fd = snapshot[i].fd;
            /* ALOOPER_EVENT_INPUT 1, OUTPUT 2 -> POLLIN 1, POLLOUT 4 */
            pfds[i + 1].events = (i16)(((snapshot[i].events & 1) ? HK_POLLIN : 0) | ((snapshot[i].events & 2) ? HK_POLLOUT : 0));
        }
        hits = hk_fd_poll(pfds, n + 1, timeout_ms);
        if (hits < 0) return POLL_ERROR;
        if (hits == 0) return POLL_TIMEOUT;
        if (pfds[0].revents) {
            u64 drain;
            hk_fd_read(l->wake_fd, &drain, 8);
            woke = true;
        }
        for (i = 0; i < n; i++) {
            int re = pfds[i + 1].revents, events;
            if (!re) continue;
            events = ((re & HK_POLLIN) ? 1 : 0) | ((re & HK_POLLOUT) ? 2 : 0) | ((re & HK_POLLERR) ? 4 : 0) | ((re & HK_POLLHUP) ? 8 : 0) | ((re & 0x20) ? 16 : 0);
            if (snapshot[i].callback) {
                called = true;
                if (!HK_GFN(int (*)(int, int, void *), snapshot[i].callback)(snapshot[i].fd, events, snapshot[i].data)) g_looper_remove_fd(l, snapshot[i].fd);
            } else if (l->npending < (int)HK_COUNT(l->pending)) {
                l->pending[l->npending] = snapshot[i];
                l->pending[l->npending].events = events;
                l->npending++;
            }
        }
        if (l->npending) continue;
        if (called) return POLL_CALLBACK;
        if (woke) return POLL_WAKE;
        /* Something changed that was not for us (a removed descriptor): wait again, without starting the clock over forever. */
        if (timeout_ms > 0) return POLL_TIMEOUT;
    }
}

static int g_looper_poll_once(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    hk_looper *l = t_looper;
    if (!l) return POLL_ERROR;
    return looper_poll_once(l, timeout_ms, out_fd, out_events, out_data);
}

static int g_looper_poll_all(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    hk_looper *l = t_looper;
    int r;
    if (!l) return POLL_ERROR;
    r = looper_poll_once(l, timeout_ms, out_fd, out_events, out_data);
    while (r == POLL_CALLBACK) r = looper_poll_once(l, 0, out_fd, out_events, out_data);
    return r;
}

bool hk_looper_poll_thread(void)
{
    if (!t_looper) return false;
    /* Callbacks of what is ready, and no waiting. A descriptor added without a callback has nobody to be reported to on such a thread. */
    while (g_looper_poll_all(0, NULL, NULL, NULL) >= 0) { }
    return true;
}

/* ============================================================= AInputQueue */

#define MAX_POINTERS 10
#define EV_KEY 1
#define EV_MOTION 2

typedef struct g_input_event {
    int type;
    i64 event_time, down_time;         /* nanoseconds, the monotonic clock */
    int action, flags, meta;
    int keycode, repeat;
    int pointer_count;
    int ids[MAX_POINTERS];
    float xs[MAX_POINTERS], ys[MAX_POINTERS];
} g_input_event;

struct hk_input_queue {
    hk_lock lock;
    int event_fd;
    g_input_event *events[256];
    int head, count;
    /* the fingers that are down */
    int pointer_count;
    int ids[MAX_POINTERS];
    float xs[MAX_POINTERS], ys[MAX_POINTERS];
    i64 down_time;
};

hk_input_queue *hk_input_queue_create(void)
{
    hk_input_queue *q = (hk_input_queue *)hk_alloc(sizeof(*q));
    hk_lock_init(&q->lock);
    /*
     * A semaphore eventfd: readable exactly while events are queued. Its
     * count and the queue's change together, under the queue's lock. Apart,
     * an event taken between being queued and being counted leaves the
     * descriptor readable with nothing behind it, for good -- and an app
     * that polls without waiting then never leaves its event loop to draw.
     */
    q->event_fd = hk_fd_eventfd(0, 1 | 04000);
    return q;
}

/* Called with the queue's lock held; the event is the queue's from here, or freed. */
static void queue_push_locked(hk_input_queue *q, g_input_event *e)
{
    u64 one = 1;
    if (q->count == (int)HK_COUNT(q->events)) {
        /* An app that has stopped taking its input: the newest is what goes, and it is said once. */
        hk_note_once("input: the app's queue is full (%d events not taken); new ones are dropped", q->count);
        free(e);
        return;
    }
    q->events[(q->head + q->count) % (int)HK_COUNT(q->events)] = e;
    q->count++;
    hk_fd_write(q->event_fd, &one, 8);
}

void hk_input_queue_touch(hk_input_queue *q, int phase, int pointer_id, float x, float y)
{
    g_input_event *e;
    int i, index = -1, action;
    i64 now = (i64)hk_time_monotonic_ns();
    if (!q) return;
    hk_lock_acquire(&q->lock);
    for (i = 0; i < q->pointer_count; i++)
        if (q->ids[i] == pointer_id) index = i;
    if (phase == 0) {
        if (index < 0) {
            if (q->pointer_count == MAX_POINTERS) {
                hk_lock_release(&q->lock);
                return;
            }
            index = q->pointer_count++;
            q->ids[index] = pointer_id;
        }
        if (q->pointer_count == 1) q->down_time = now;
    } else if (index < 0) {
        hk_lock_release(&q->lock);
        return;
    }
    q->xs[index] = x;
    q->ys[index] = y;
    /* ACTION_DOWN 0, UP 1, MOVE 2, CANCEL 3, POINTER_DOWN 5, POINTER_UP 6; the pointer's index goes in bits 8-15. */
    if (phase == 0) action = q->pointer_count == 1 ? 0 : (5 | (index << 8));
    else if (phase == 1) action = 2;
    else if (phase == 2) action = q->pointer_count == 1 ? 1 : (6 | (index << 8));
    else action = 3;
    if (action == 2 && q->count) {
        /*
         * A move while the move before it has not been taken yet: that one
         * is brought up to date instead (Android batches them the same way).
         * A screen reports each finger a hundred times a second, an app looks
         * once a frame, and only where the fingers are now matters to it.
         */
        g_input_event *last = q->events[(q->head + q->count - 1) % (int)HK_COUNT(q->events)];
        if (last->type == EV_MOTION && last->action == 2 && last->pointer_count == q->pointer_count) {
            last->event_time = now;
            memcpy(last->xs, q->xs, sizeof(q->xs));
            memcpy(last->ys, q->ys, sizeof(q->ys));
            hk_lock_release(&q->lock);
            return;
        }
    }
    e = (g_input_event *)hk_alloc(sizeof(*e));
    e->type = EV_MOTION;
    e->event_time = now;
    e->down_time = q->down_time;
    e->action = action;
    e->pointer_count = q->pointer_count;
    memcpy(e->ids, q->ids, sizeof(q->ids));
    memcpy(e->xs, q->xs, sizeof(q->xs));
    memcpy(e->ys, q->ys, sizeof(q->ys));
    if (phase == 2) {
        for (i = index; i + 1 < q->pointer_count; i++) {
            q->ids[i] = q->ids[i + 1];
            q->xs[i] = q->xs[i + 1];
            q->ys[i] = q->ys[i + 1];
        }
        q->pointer_count--;
    } else if (phase == 3) {
        q->pointer_count = 0;
    }
    queue_push_locked(q, e);
    hk_lock_release(&q->lock);
}

void hk_input_queue_key(hk_input_queue *q, int keycode, bool down)
{
    g_input_event *e;
    if (!q) return;
    e = (g_input_event *)hk_alloc(sizeof(*e));
    e->type = EV_KEY;
    e->event_time = e->down_time = (i64)hk_time_monotonic_ns();
    e->action = down ? 0 : 1;
    e->keycode = keycode;
    hk_lock_acquire(&q->lock);
    queue_push_locked(q, e);
    hk_lock_release(&q->lock);
}

static void g_iq_attach(hk_input_queue *q, hk_looper *l, int ident, hk_addr callback, void *data)
{
    if (q && l) g_looper_add_fd(l, q->event_fd, ident, 1, callback, data);
}
static void g_iq_detach(hk_input_queue *q)
{
    if (q && t_looper) g_looper_remove_fd(t_looper, q->event_fd);
}
static int g_iq_has_events(hk_input_queue *q) { return q ? (q->count > 0) : -1; }

static int g_iq_get_event(hk_input_queue *q, g_input_event **out)
{
    g_input_event *e = NULL;
    u64 one;
    if (!q) return -1;
    hk_lock_acquire(&q->lock);
    if (q->count) {
        e = q->events[q->head];
        q->head = (q->head + 1) % (int)HK_COUNT(q->events);
        q->count--;
        hk_fd_read(q->event_fd, &one, 8);
    }
    hk_lock_release(&q->lock);
    if (!e) return -1;
    *out = e;
    return 0;
}

static int g_iq_pre_dispatch(hk_input_queue *q, g_input_event *e) { (void)q; (void)e; return 0; }
static void g_iq_finish(hk_input_queue *q, g_input_event *e, int handled)
{
    (void)q; (void)handled;
    free(e);
}

static int g_ev_type(const g_input_event *e) { return e->type; }
static int g_ev_device(const g_input_event *e) { (void)e; return 1; }
static int g_ev_source(const g_input_event *e) { return e->type == EV_MOTION ? 0x1002 : 0x101; }   /* TOUCHSCREEN, KEYBOARD */
static int g_mo_action(const g_input_event *e) { return e->action; }
static int g_mo_flags(const g_input_event *e) { return e->flags; }
static int g_mo_meta(const g_input_event *e) { return e->meta; }
static int g_mo_zero(const g_input_event *e) { (void)e; return 0; }
static i64 g_mo_down_time(const g_input_event *e) { return e->down_time; }
static i64 g_mo_event_time(const g_input_event *e) { return e->event_time; }
static u32 g_mo_count(const g_input_event *e) { return (u32)e->pointer_count; }
static int g_mo_pointer_id(const g_input_event *e, u32 i) { return i < (u32)e->pointer_count ? e->ids[i] : -1; }
/* Floats go back to the guest as bits. */
static u32 g_mo_x(const g_input_event *e, u32 i) { return hk_fb(i < (u32)e->pointer_count ? e->xs[i] : 0); }
static u32 g_mo_y(const g_input_event *e, u32 i) { return hk_fb(i < (u32)e->pointer_count ? e->ys[i] : 0); }
static u32 g_mo_one(const g_input_event *e, u32 i) { (void)e; (void)i; return hk_fb(1.0f); }
static u32 g_mo_fzero(const g_input_event *e, u32 i) { (void)e; (void)i; return hk_fb(0.0f); }
static u32 g_mo_fzero0(const g_input_event *e) { (void)e; return hk_fb(0.0f); }
static u32 g_mo_fone0(const g_input_event *e) { (void)e; return hk_fb(1.0f); }
static u32 g_mo_axis(const g_input_event *e, int axis, u32 i)
{
    if (axis == 0) return g_mo_x(e, i);
    if (axis == 1) return g_mo_y(e, i);
    return hk_fb(axis == 2 ? 1.0f : 0.0f);         /* AXIS_PRESSURE */
}
static u32 g_mo_hist_size(const g_input_event *e) { (void)e; return 0; }
static u32 g_mo_hist_f(const g_input_event *e, u32 i, u32 h) { (void)h; return g_mo_x(e, i); }
static u32 g_mo_hist_fy(const g_input_event *e, u32 i, u32 h) { (void)h; return g_mo_y(e, i); }
static i64 g_mo_hist_time(const g_input_event *e, u32 h) { (void)h; return e->event_time; }
static int g_mo_tool(const g_input_event *e, u32 i) { (void)e; (void)i; return 1; }   /* TOOL_TYPE_FINGER */
static int g_key_action(const g_input_event *e) { return e->action; }
static int g_key_code(const g_input_event *e) { return e->keycode; }
static int g_key_repeat(const g_input_event *e) { return e->repeat; }

/* =========================================================== AAssetManager */

typedef struct g_asset {
    const u8 *data;        /* the whole asset in memory, once something needs it there */
    u8 *owned;
    u64 size, pos;
    u64 file_offset;       /* of an asset stored without compression: where it is in the APK */
    bool stored;
    const hk_zip *zip;
    const hk_zip_entry *entry;
} g_asset;

/* The asset's bytes in memory: read now if they have not been (a stored asset is read from the APK piece by piece until then). */
static const u8 *asset_bytes(g_asset *a)
{
    size_t n = 0;
    if (a->data) return a->data;
    a->owned = hk_zip_read(a->zip, a->entry, 1u << 30, &n);
    a->data = a->owned;
    return a->data;
}

static int g_asset_manager_tag;
void *hk_asset_manager(void) { return &g_asset_manager_tag; }

static g_asset *g_am_open(void *mgr, const char *name, int mode)
{
    char path[512];
    int i;
    (void)mgr; (void)mode;
    if (!name) return NULL;
    while (*name == '/') name++;
    snprintf(path, sizeof(path), "assets/%s", name);
    for (i = 0;; i++) {
        const hk_zip *z = hk_ld_apk_at(i);
        const hk_zip_entry *e;
        g_asset *a;
        if (!z) return NULL;
        e = hk_zip_find(z, path);
        if (!e) continue;
        a = (g_asset *)hk_alloc(sizeof(*a));
        a->zip = z;
        a->entry = e;
        a->size = e->usize;
        a->stored = hk_zip_stored_at(z, e, &a->file_offset);
        if (!a->stored && !asset_bytes(a)) {
            hk_warn("asset %s could not be read", name);
            free(a);
            return NULL;
        }
        return a;
    }
}

static void g_asset_close(g_asset *a)
{
    if (!a) return;
    free(a->owned);
    free(a);
}

static int g_asset_read(g_asset *a, void *buf, size_t n)
{
    u64 left;
    if (!a) return -1;
    left = a->size - a->pos;
    if (n > left) n = (size_t)left;
    if (a->data) memcpy(buf, a->data + a->pos, n);
    else if (!hk_zip_pread(a->zip, a->file_offset + a->pos, buf, n)) return -1;
    a->pos += n;
    return (int)n;
}

static i64 g_asset_seek64(g_asset *a, i64 offset, int whence)
{
    i64 at;
    if (!a) return -1;
    at = whence == 0 ? offset : whence == 1 ? (i64)a->pos + offset : (i64)a->size + offset;
    if (at < 0 || (u64)at > a->size) return -1;
    a->pos = (u64)at;
    return at;
}
static i32 g_asset_seek(g_asset *a, i32 offset, int whence) { return (i32)g_asset_seek64(a, offset, whence); }
static i32 g_asset_length(g_asset *a) { return a ? (i32)a->size : 0; }
static i64 g_asset_length64(g_asset *a) { return a ? (i64)a->size : 0; }
static i32 g_asset_remaining(g_asset *a) { return a ? (i32)(a->size - a->pos) : 0; }
static i64 g_asset_remaining64(g_asset *a) { return a ? (i64)(a->size - a->pos) : 0; }
static const void *g_asset_buffer(g_asset *a) { return a ? asset_bytes(a) : NULL; }
static int g_asset_is_allocated(g_asset *a) { return a && !a->stored ? 1 : 0; }

/* A descriptor on the APK and where the asset is in it: only for assets stored without compression. */
static int g_asset_open_fd64(g_asset *a, i64 *start, i64 *length)
{
    int fd;
    if (!a || !a->stored) return -1;
    fd = hk_fd_open_host(a->zip->path, HK_O_READ);
    if (fd < 0) return -1;
    *start = (i64)a->file_offset;
    *length = (i64)a->size;
    return fd;
}
static int g_asset_open_fd(g_asset *a, i32 *start, i32 *length)
{
    i64 s = 0, l = 0;
    int fd = g_asset_open_fd64(a, &s, &l);
    if (fd >= 0) {
        *start = (i32)s;
        *length = (i32)l;
    }
    return fd;
}

typedef struct g_asset_dir {
    char **names;
    int count, cap, at;
    size_t skip;
} g_asset_dir;

static void dir_collect(const hk_zip_entry *e, void *user)
{
    g_asset_dir *d = (g_asset_dir *)user;
    const char *rest = e->name + d->skip;
    int i;
    if (!*rest || strchr(rest, '/')) return;       /* files of this folder only, as Android lists them */
    for (i = 0; i < d->count; i++)
        if (!strcmp(d->names[i], rest)) return;
    if (d->count == d->cap) {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->names = (char **)hk_realloc(d->names, sizeof(char *) * (size_t)d->cap);
    }
    d->names[d->count++] = hk_strdup(rest);
}

static g_asset_dir *g_am_open_dir(void *mgr, const char *name)
{
    g_asset_dir *d = (g_asset_dir *)hk_alloc(sizeof(*d));
    char prefix[512];
    int i;
    (void)mgr;
    while (name && *name == '/') name++;
    if (name && *name) snprintf(prefix, sizeof(prefix), "assets/%s/", name);
    else snprintf(prefix, sizeof(prefix), "assets/");
    d->skip = strlen(prefix);
    for (i = 0; hk_ld_apk_at(i); i++) hk_zip_each(hk_ld_apk_at(i), prefix, dir_collect, d);
    return d;
}
static const char *g_ad_next(g_asset_dir *d) { return d && d->at < d->count ? d->names[d->at++] : NULL; }
static void g_ad_rewind(g_asset_dir *d) { if (d) d->at = 0; }
static void g_ad_close(g_asset_dir *d)
{
    int i;
    if (!d) return;
    for (i = 0; i < d->count; i++) free(d->names[i]);
    free(d->names);
    free(d);
}
static void *g_am_from_java(void *env, void *obj) { (void)env; (void)obj; return hk_asset_manager(); }

/* ========================================================== AConfiguration */

typedef struct g_config {
    int width, height, density;
} g_config;
static g_config g_device = { 1280, 720, 320 };

void hk_configuration_set(int width, int height, int density_dpi)
{
    g_device.width = width;
    g_device.height = height;
    g_device.density = density_dpi > 0 ? density_dpi : 320;
}

static g_config *g_cfg_new(void)
{
    g_config *c = (g_config *)hk_g_malloc(sizeof(*c));
    if (c) *c = g_device;
    return c;
}
static void g_cfg_delete(g_config *c) { hk_g_free(c); }
static void g_cfg_from_am(g_config *c, void *am) { (void)am; if (c) *c = g_device; }
static void g_cfg_copy(g_config *d, const g_config *s) { if (d && s) *d = *s; }
static int g_cfg_zero(const g_config *c) { (void)c; return 0; }
static void g_cfg_set(g_config *c, int v) { (void)c; (void)v; }
static void g_cfg_language(const g_config *c, char *out) { (void)c; out[0] = 'e'; out[1] = 'n'; }
static void g_cfg_country(const g_config *c, char *out) { (void)c; out[0] = 'U'; out[1] = 'S'; }
static void g_cfg_set_str(g_config *c, const char *v) { (void)c; (void)v; }
static int g_cfg_orientation(const g_config *c) { return c->width >= c->height ? 2 : 1; }   /* LAND, PORT */
static int g_cfg_touchscreen(const g_config *c) { (void)c; return 3; }                      /* FINGER */
static int g_cfg_density(const g_config *c) { return c->density; }
static int g_cfg_one(const g_config *c) { (void)c; return 1; }                              /* NOKEYS, NONAV, *_NO, UI_MODE_TYPE_NORMAL */
static int g_cfg_sdk(const g_config *c) { (void)c; return hk_sdk_level(); }
static int g_cfg_screen_size(const g_config *c) { (void)c; return 2; }                      /* NORMAL */
static int g_cfg_screen_long(const g_config *c) { return (c->width > c->height ? c->width * 3 > c->height * 5 : c->height * 3 > c->width * 5) ? 2 : 1; }
static int g_cfg_width_dp(const g_config *c) { return c->width * 160 / c->density; }
static int g_cfg_height_dp(const g_config *c) { return c->height * 160 / c->density; }
static int g_cfg_smallest_dp(const g_config *c) { return (c->width < c->height ? c->width : c->height) * 160 / c->density; }
static int g_cfg_diff(const g_config *a, const g_config *b) { return memcmp(a, b, sizeof(*a)) != 0; }

/* ============================================================== the rest */

/* Sensors: none. An app that asks gets "no such sensor" and plays without. */
static int g_sensor_manager_tag;
static void *g_sensor_manager(void) { return &g_sensor_manager_tag; }
static void *g_sensor_manager_pkg(const char *pkg) { (void)pkg; return &g_sensor_manager_tag; }
static void *g_sensor_null(void *m, int type) { (void)m; (void)type; return NULL; }
static int g_sensor_list(void *m, void **list) { (void)m; if (list) *list = NULL; return 0; }
static void *g_sensor_queue(void *m, void *looper, int ident, hk_addr cb, void *data) { (void)m; (void)looper; (void)ident; (void)cb; (void)data; return &g_sensor_manager_tag; }
static int g_sensor_int(void *a, void *b) { (void)a; (void)b; return -1; }
/* There are no sensors, so no ASensor to ask; these answer for a NULL one. */
static int g_sensor_zero(const void *s) { (void)s; return 0; }
static const char *g_sensor_text(const void *s) { (void)s; return ""; }
static u32 g_sensor_fzero(const void *s) { (void)s; return hk_fb(0.0f); }
static int g_sensor_events(void *q, void *events, size_t n) { (void)q; (void)events; (void)n; return 0; }

/* Tracing: off. */
static int g_trace_enabled(void) { return 0; }
static void g_trace_begin(const char *name) { (void)name; }
static void g_trace_end(void) { }

#define N(n, f) HK_WRAP(n, f)

const hk_sym hk_tab_ndk[] = {
    N("ANativeWindow_acquire", hk_nwindow_acquire), N("ANativeWindow_release", hk_nwindow_release),
    N("ANativeWindow_fromSurface", g_nw_from_surface),
    N("ANativeWindow_getWidth", g_nw_width), N("ANativeWindow_getHeight", g_nw_height), N("ANativeWindow_getFormat", g_nw_format),
    N("ANativeWindow_setBuffersGeometry", g_nw_set_geometry), N("ANativeWindow_lock", g_nw_lock),
    N("ANativeWindow_unlockAndPost", g_nw_unlock), N("ANativeWindow_setBuffersTransform", g_nw_ok), N("ANativeWindow_setFrameRate", g_nw_ok),
    N("ALooper_forThread", g_looper_for_thread), N("ALooper_prepare", g_looper_prepare), N("ALooper_acquire", g_looper_acquire),
    N("ALooper_release", g_looper_release), N("ALooper_pollOnce", g_looper_poll_once), N("ALooper_pollAll", g_looper_poll_all),
    N("ALooper_wake", g_looper_wake), N("ALooper_addFd", g_looper_add_fd), N("ALooper_removeFd", g_looper_remove_fd),
    N("AInputQueue_attachLooper", g_iq_attach), N("AInputQueue_detachLooper", g_iq_detach), N("AInputQueue_hasEvents", g_iq_has_events),
    N("AInputQueue_getEvent", g_iq_get_event), N("AInputQueue_preDispatchEvent", g_iq_pre_dispatch), N("AInputQueue_finishEvent", g_iq_finish),
    N("AInputEvent_getType", g_ev_type), N("AInputEvent_getDeviceId", g_ev_device), N("AInputEvent_getSource", g_ev_source),
    N("AMotionEvent_getAction", g_mo_action), N("AMotionEvent_getFlags", g_mo_flags), N("AMotionEvent_getMetaState", g_mo_meta),
    N("AMotionEvent_getButtonState", g_mo_zero), N("AMotionEvent_getEdgeFlags", g_mo_zero), N("AMotionEvent_getDownTime", g_mo_down_time),
    N("AMotionEvent_getEventTime", g_mo_event_time), N("AMotionEvent_getPointerCount", g_mo_count), N("AMotionEvent_getPointerId", g_mo_pointer_id),
    N("AMotionEvent_getToolType", g_mo_tool), N("AMotionEvent_getX", g_mo_x), N("AMotionEvent_getY", g_mo_y),
    N("AMotionEvent_getRawX", g_mo_x), N("AMotionEvent_getRawY", g_mo_y), N("AMotionEvent_getPressure", g_mo_one),
    N("AMotionEvent_getSize", g_mo_one), N("AMotionEvent_getTouchMajor", g_mo_one), N("AMotionEvent_getTouchMinor", g_mo_one),
    N("AMotionEvent_getToolMajor", g_mo_one), N("AMotionEvent_getToolMinor", g_mo_one), N("AMotionEvent_getOrientation", g_mo_fzero),
    N("AMotionEvent_getAxisValue", g_mo_axis), N("AMotionEvent_getXOffset", g_mo_fzero0), N("AMotionEvent_getYOffset", g_mo_fzero0),
    N("AMotionEvent_getXPrecision", g_mo_fone0), N("AMotionEvent_getYPrecision", g_mo_fone0), N("AMotionEvent_getHistorySize", g_mo_hist_size),
    N("AMotionEvent_getHistoricalX", g_mo_hist_f), N("AMotionEvent_getHistoricalY", g_mo_hist_fy),
    N("AMotionEvent_getHistoricalEventTime", g_mo_hist_time),
    N("AKeyEvent_getAction", g_key_action), N("AKeyEvent_getKeyCode", g_key_code), N("AKeyEvent_getRepeatCount", g_key_repeat),
    N("AKeyEvent_getFlags", g_mo_flags), N("AKeyEvent_getMetaState", g_mo_meta), N("AKeyEvent_getScanCode", g_mo_zero),
    N("AKeyEvent_getDownTime", g_mo_down_time), N("AKeyEvent_getEventTime", g_mo_event_time),
    N("AAssetManager_open", g_am_open), N("AAssetManager_openDir", g_am_open_dir), N("AAssetManager_fromJava", g_am_from_java),
    N("AAsset_close", g_asset_close), N("AAsset_read", g_asset_read), N("AAsset_seek", g_asset_seek), N("AAsset_seek64", g_asset_seek64),
    N("AAsset_getLength", g_asset_length), N("AAsset_getLength64", g_asset_length64), N("AAsset_getRemainingLength", g_asset_remaining),
    N("AAsset_getRemainingLength64", g_asset_remaining64), N("AAsset_getBuffer", g_asset_buffer), N("AAsset_isAllocated", g_asset_is_allocated),
    N("AAsset_openFileDescriptor", g_asset_open_fd), N("AAsset_openFileDescriptor64", g_asset_open_fd64),
    N("AAssetDir_getNextFileName", g_ad_next), N("AAssetDir_rewind", g_ad_rewind), N("AAssetDir_close", g_ad_close),
    N("AConfiguration_new", g_cfg_new), N("AConfiguration_delete", g_cfg_delete), N("AConfiguration_fromAssetManager", g_cfg_from_am),
    N("AConfiguration_copy", g_cfg_copy), N("AConfiguration_diff", g_cfg_diff), N("AConfiguration_getMcc", g_cfg_zero),
    N("AConfiguration_getMnc", g_cfg_zero), N("AConfiguration_getLanguage", g_cfg_language), N("AConfiguration_getCountry", g_cfg_country),
    N("AConfiguration_getOrientation", g_cfg_orientation), N("AConfiguration_getTouchscreen", g_cfg_touchscreen),
    N("AConfiguration_getDensity", g_cfg_density), N("AConfiguration_getKeyboard", g_cfg_one), N("AConfiguration_getNavigation", g_cfg_one),
    N("AConfiguration_getKeysHidden", g_cfg_one), N("AConfiguration_getNavHidden", g_cfg_one), N("AConfiguration_getSdkVersion", g_cfg_sdk),
    N("AConfiguration_getScreenSize", g_cfg_screen_size), N("AConfiguration_getScreenLong", g_cfg_screen_long),
    N("AConfiguration_getUiModeType", g_cfg_one), N("AConfiguration_getUiModeNight", g_cfg_one),
    N("AConfiguration_getScreenWidthDp", g_cfg_width_dp), N("AConfiguration_getScreenHeightDp", g_cfg_height_dp),
    N("AConfiguration_getSmallestScreenWidthDp", g_cfg_smallest_dp), N("AConfiguration_getLayoutDirection", g_cfg_zero),
    N("AConfiguration_setOrientation", g_cfg_set), N("AConfiguration_setDensity", g_cfg_set), N("AConfiguration_setSdkVersion", g_cfg_set),
    N("AConfiguration_setLanguage", g_cfg_set_str), N("AConfiguration_setCountry", g_cfg_set_str),
    N("ASensorManager_getInstance", g_sensor_manager), N("ASensorManager_getInstanceForPackage", g_sensor_manager_pkg),
    N("ASensorManager_getDefaultSensor", g_sensor_null), N("ASensorManager_getSensorList", g_sensor_list),
    N("ASensorManager_createEventQueue", g_sensor_queue), N("ASensorManager_destroyEventQueue", g_sensor_int),
    N("ASensorEventQueue_enableSensor", g_sensor_int), N("ASensorEventQueue_disableSensor", g_sensor_int),
    N("ASensorEventQueue_setEventRate", g_sensor_int), N("ASensorEventQueue_getEvents", g_sensor_events),
    N("ASensorEventQueue_hasEvents", g_sensor_int),
    N("ASensor_getType", g_sensor_zero), N("ASensor_getMinDelay", g_sensor_zero), N("ASensor_getResolution", g_sensor_fzero),
    N("ASensor_getName", g_sensor_text), N("ASensor_getVendor", g_sensor_text), N("ASensor_getStringType", g_sensor_text),
    N("ATrace_isEnabled", g_trace_enabled), N("ATrace_beginSection", g_trace_begin), N("ATrace_endSection", g_trace_end),
    HK_END
};
