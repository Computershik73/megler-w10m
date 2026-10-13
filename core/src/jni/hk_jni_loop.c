/*
 * Android's message loops, for the native code that drives itself with them.
 *
 * Unity paces its frames with a Choreographer on a HandlerThread of its own,
 * and runs work on the UI thread through Handlers. None of it is optional:
 * the engine's main thread waits for the first vsync callback, so without a
 * loop that delivers one the game shows three frames and stops.
 *
 *   - A Looper is a queue of events in order of time, and a thread that runs
 *     them. A HandlerThread makes one with a thread of its own; the main
 *     looper's thread starts when something is first sent to it.
 *   - A Handler posts Runnables and Messages to its Looper; the Looper hands
 *     a Message to the Handler's Callback, and runs a Runnable.
 *   - A Choreographer posts a frame callback for the next 60 Hz boundary.
 *   - The callbacks are Java proxies: objects that JNIBridge makes for an
 *     interface implemented in native code (or in C#). Calling one is calling
 *     the native JNIBridge.invoke the library registered, with a
 *     java.lang.reflect.Method for the interface's method and the arguments
 *     boxed. The app's own Java classes do not run here, so a Runnable or a
 *     Handler that is one of those is noted and dropped.
 *
 * A loop's thread is a thread of the app's as far as its code can tell: the
 * collector may stop it like any other, so its waits are the guest's kind.
 */
#include "hk_jni_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bionic/hk_bionic.h"
#include "../host/hk_host.h"

#define NS 1000000000ll
#define VSYNC_NS (NS / 60)

#define PROXY "java/lang/reflect/Proxy"
#define BRIDGE "bitter/jnibridge/JNIBridge"
#define BRIDGE_INVOKE "(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;"

static i64 now_ns(void) { return (i64)hk_time_monotonic_ns(); }

static bool is_a(const hk_jobj *o, const char *cls) { return o && o->kind == HK_J_OBJECT && !strcmp(hk_jni_class_name(o), cls); }

/* ----------------------------------------------------------------- boxing */

static hk_jobj *box(const char *cls, char kind, hk_jvalue v)
{
    hk_jobj *o = hk_jni_new_object(cls);
    hk_jni_set_field(o, "value", kind, v);
    return o;
}

hk_jobj *hk_jni_box(const char *cls, char kind, hk_jvalue v) { return box(cls, kind, v); }

/* The class a signature returns: "(J)Ljava/lang/Long;" names java/lang/Long. */
static bool returned_class(const char *sig, char *out, size_t size)
{
    const char *r = strchr(sig, ')'), *end;
    if (!r || r[1] != 'L' || !(end = strchr(r, ';')) || (size_t)(end - r - 2) >= size) return false;
    memcpy(out, r + 2, (size_t)(end - r - 2));
    out[end - r - 2] = 0;
    return true;
}

/* new Long(5), new Float(1.5f)...: the constructor's own signature says which kind the value is. */
static void box_init(hk_jcall *c)
{
    if (c->sig[1] != 'L' && c->sig[1] != ')') hk_jni_set_field(c->self, "value", c->sig[1], c->args[0]);
}

static void box_value_of(hk_jcall *c)
{
    char cls[96];
    if (c->sig[1] == 'L' || c->sig[1] == ')' || !returned_class(c->sig, cls, sizeof(cls))) return;
    c->ret.l = box(cls, c->sig[1], c->args[0]);
}

static bool box_is(const hk_jobj *o, const char *short_name)
{
    const char *n = hk_jni_class_name(o);
    return !strncmp(n, "java/lang/", 10) && !strcmp(n + 10, short_name);
}

static i64 number_long(hk_jobj *o)
{
    hk_jvalue v = hk_jni_get_field(o, "value");
    if (box_is(o, "Long")) return v.j;
    if (box_is(o, "Float")) return (i64)v.f;
    if (box_is(o, "Double")) return (i64)v.d;
    if (box_is(o, "Short")) return v.s;
    if (box_is(o, "Byte")) return v.b;
    if (box_is(o, "Character")) return v.c;
    if (box_is(o, "Boolean")) return v.z != 0;
    return v.i;
}

static double number_double(hk_jobj *o)
{
    hk_jvalue v = hk_jni_get_field(o, "value");
    if (box_is(o, "Float")) return v.f;
    if (box_is(o, "Double")) return v.d;
    return (double)number_long(o);
}

static void number_int(hk_jcall *c) { c->ret.i = (i32)number_long(c->self); }
static void number_long_value(hk_jcall *c) { c->ret.j = number_long(c->self); }
static void number_short(hk_jcall *c) { c->ret.s = (i16)number_long(c->self); }
static void number_byte(hk_jcall *c) { c->ret.b = (i8)number_long(c->self); }
static void number_float(hk_jcall *c) { c->ret.f = (float)number_double(c->self); }
static void number_double_value(hk_jcall *c) { c->ret.d = number_double(c->self); }
static void boolean_value(hk_jcall *c) { c->ret.z = number_long(c->self) != 0; }
static void char_value(hk_jcall *c) { c->ret.c = (u16)number_long(c->self); }

/* ------------------------------------------------------------ reflection */

/* The Class a type descriptor names, and how many characters of `d` it took. Primitives are classes named "int" and so on. */
static hk_jobj *class_for_desc(const char *d, int *len)
{
    static const struct { char c; const char *name; } prim[] = {
        { 'Z', "boolean" }, { 'B', "byte" }, { 'C', "char" }, { 'S', "short" }, { 'I', "int" }, { 'J', "long" },
        { 'F', "float" },   { 'D', "double" }, { 'V', "void" },
    };
    char name[200];
    size_t i;
    for (i = 0; i < HK_COUNT(prim); i++)
        if (d[0] == prim[i].c) {
            *len = 1;
            return hk_jni_class_object(prim[i].name);
        }
    if (d[0] == 'L') {
        const char *e = strchr(d, ';');
        size_t n = e ? (size_t)(e - d - 1) : 0;
        if (!e || n >= sizeof(name)) {
            *len = 1;
            return hk_jni_class_object("java/lang/Object");
        }
        memcpy(name, d + 1, n);
        name[n] = 0;
        *len = (int)(e - d) + 1;
        return hk_jni_class_object(name);
    }
    if (d[0] == '[') {
        int inner = 1;
        class_for_desc(d + 1, &inner);
        *len = 1 + inner;
        snprintf(name, sizeof(name), "%.*s", *len, d);
        return hk_jni_class_object(name);
    }
    *len = 1;
    return hk_jni_class_object("java/lang/Object");
}

static void method_name(hk_jcall *c)
{
    const char *n = hk_jni_reflected_name(c->self);
    c->ret.l = hk_jni_new_string(n ? n : "");
}

static void method_return_type(hk_jcall *c)
{
    const char *sig = hk_jni_reflected_sig(c->self), *r = sig ? strchr(sig, ')') : NULL;
    int n;
    c->ret.l = r ? class_for_desc(r + 1, &n) : hk_jni_class_object("void");
}

static void method_parameter_types(hk_jcall *c)
{
    const char *sig = hk_jni_reflected_sig(c->self), *p;
    hk_jobj *types[32], *arr;
    int nt = 0, i;
    if (sig && sig[0] == '(')
        for (p = sig + 1; *p && *p != ')' && nt < (int)HK_COUNT(types);) {
            int n = 1;
            types[nt++] = class_for_desc(p, &n);
            p += n;
        }
    arr = hk_jni_new_object_array("java/lang/Class", (u32)nt);
    for (i = 0; i < nt; i++) arr->u.oarr.v[i] = types[i];
    c->ret.l = arr;
}

/* --------------------------------------------------------------- proxies */

/*
 * Unity's own Java (bitter.jnibridge.JNIBridge's InterfaceProxy) calls the
 * native side of a proxy and disables a proxy under one lock, the proxy's:
 * disableInterfaceProxy does not return while a call through that proxy is
 * under way, and no call starts after it. The engine counts on that: it
 * disables a proxy and deletes the object behind it in the next breath.
 * Without the lock, a film skipped with a tap had its decoder thread's
 * message handler called through an object that had just been deleted (a jump
 * to nowhere, on the phone, the first time anyone skipped a film).
 *
 * The lock is a Java monitor there, so the thread inside may enter again (a
 * callback that disables its own proxy). The wait is one a stop-the-world
 * signal gets through, since the thread that holds the lock is in guest code.
 * (The few bytes are never freed: a thread may be waiting on them when the
 * object itself goes.)
 */
typedef struct proxy_state {
    volatile i32 busy;                 /* 1 while a thread is inside */
    u32 owner;                         /* that thread */
    int depth;
} proxy_state;

static proxy_state *proxy_enter(hk_jobj *proxy)
{
    proxy_state *s = (proxy_state *)proxy->native;
    u32 me = hk_thread_self_id();
    if (!s) return NULL;
    if (s->busy && s->owner == me) {
        s->depth++;
        return s;
    }
    while (hk_atomic_cas(&s->busy, 0, 1) != 0) hk_gwait(&s->busy, 1, 2000000ull);
    s->owner = me;
    s->depth = 1;
    return s;
}

static void proxy_leave(proxy_state *s)
{
    if (!s || --s->depth) return;
    s->owner = 0;
    hk_atomic_cas(&s->busy, 1, 0);
    hk_futex_wake(&s->busy, true);
}

static void bridge_new_proxy(hk_jcall *c)
{
    hk_jobj *p = hk_jni_new_object(PROXY);
    hk_jvalue h;
    h.j = c->args[0].j;
    p->native = hk_alloc(sizeof(proxy_state));
    hk_jni_set_field(p, "handle", 'J', h);
    c->ret.l = p;
}

/* The native side is about to go: nothing more is to be called through this one, and nothing is being. */
static void bridge_disable_proxy(hk_jcall *c)
{
    hk_jvalue none;
    proxy_state *s;
    none.j = 0;
    if (!is_a(c->args[0].l, PROXY)) return;
    s = proxy_enter(c->args[0].l);
    hk_jni_set_field(c->args[0].l, "handle", 'J', none);
    proxy_leave(s);
}

hk_jobj *hk_jni_proxy_call(hk_jobj *proxy, const char *iface, const char *name, const char *sig, hk_jobj **args, int nargs)
{
    static volatile i32 said_no_bridge, said_threw;
    hk_jvalue a[4], r;
    hk_jobj *arr;
    proxy_state *state;
    void *fn;
    i64 handle;
    int i;
    if (!is_a(proxy, PROXY)) return NULL;
    fn = hk_jni_native(BRIDGE, "invoke", BRIDGE_INVOKE);
    if (!fn) {
        if (hk_atomic_add(&said_no_bridge, 1) <= 3) hk_warn("looper: the app registered no JNIBridge.invoke; %s.%s cannot be called", iface, name);
        return NULL;
    }
    /* From here to the end of the call the proxy cannot be disabled by another thread (see proxy_state). */
    state = proxy_enter(proxy);
    handle = hk_jni_get_field(proxy, "handle").j;
    if (!handle) {
        proxy_leave(state);
        return NULL;
    }
    arr = hk_jni_new_object_array("java/lang/Object", (u32)nargs);
    for (i = 0; i < nargs; i++) arr->u.oarr.v[i] = hk_jni_ref(args[i]);
    memset(a, 0, sizeof(a));
    a[0].j = handle;
    a[1].l = hk_jni_class_object(iface);
    a[2].l = hk_jni_reflect_method(iface, name, sig, false);
    a[3].l = arr;
    r = hk_jni_call_native(fn, hk_jni_class_object(BRIDGE), BRIDGE_INVOKE, a);
    proxy_leave(state);
    if (hk_jni_exception_clear() && hk_atomic_add(&said_threw, 1) <= 8) hk_warn("looper: the app's %s.%s threw; carrying on", iface, name);
    hk_jni_unref(arr);
    return r.l;
}

static void run_runnable(hk_jobj *run)
{
    static volatile i32 said;
    if (!run) return;
    if (is_a(run, PROXY)) {
        hk_jni_unref(hk_jni_proxy_call(run, "java/lang/Runnable", "run", "()V", NULL, 0));
        return;
    }
    if (hk_atomic_add(&said, 1) <= 8) hk_warn("looper: a %s was posted; the app's own Java does not run here", hk_jni_class_name(run));
}

/* -------------------------------------------------------------- the queue */

typedef struct ev {
    struct ev *next;
    i64 when;
    hk_jobj *handler, *msg, *run, *frame;
    void (*call)(void *arg);           /* a function of this layer's, with its argument */
    void *call_arg;
} ev;

typedef struct looper {
    hk_lock lock;
    ev *head;
    volatile i32 seq;                  /* goes up whenever the queue changes: what the loop's thread waits on */
    volatile i32 ended;                /* 1 once the loop's thread has left the loop: what a join waits on */
    volatile i32 busy;                 /* 1 while the loop's thread is in a handler */
    bool started, quit;
    char name[48];
    hk_jobj *mirror;                   /* the android.os.Looper */
    hk_jobj *choreographer;
    hk_jobj *frame_time;               /* the Long a frame callback is handed, used over and over */
    hk_thread *thread;
    struct looper *next;
} looper;

typedef struct handler {
    looper *lp;
    hk_jobj *callback;
} handler;

static HK_THREAD_LOCAL looper *t_looper;
static looper *g_main, *g_loopers;
static hk_lock g_loop_lock = HK_LOCK_INIT;

static void looper_thread(void *arg);

static looper *looper_new(const char *name)
{
    looper *l = (looper *)hk_alloc(sizeof(*l));
    hk_lock_init(&l->lock);
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->mirror = hk_jni_new_object("android/os/Looper");
    l->mirror->native = l;
    l->mirror->permanent = true;
    hk_lock_acquire(&g_loop_lock);
    l->next = g_loopers;
    g_loopers = l;
    hk_lock_release(&g_loop_lock);
    return l;
}

static void looper_start(looper *l)
{
    bool go;
    hk_lock_acquire(&l->lock);
    go = !l->started && !l->quit;
    l->started = true;
    hk_lock_release(&l->lock);
    if (!go) return;
    l->thread = hk_thread_start(looper_thread, l, 2u * 1024 * 1024, l->name);
    if (!l->thread) hk_warn("looper: no thread for '%s'; nothing sent to it will run", l->name);
    else hk_info("looper: '%s' runs", l->name);
}

static looper *main_looper(void)
{
    looper *l;
    hk_lock_acquire(&g_loop_lock);
    l = g_main;
    hk_lock_release(&g_loop_lock);
    if (l) return l;
    l = looper_new("main");
    hk_lock_acquire(&g_loop_lock);
    if (!g_main) g_main = l;
    l = g_main;
    hk_lock_release(&g_loop_lock);
    return l;
}

hk_jobj *hk_jni_loop_main(void) { return main_looper()->mirror; }

static void ev_free(ev *e)
{
    hk_jni_unref(e->handler);
    hk_jni_unref(e->msg);
    hk_jni_unref(e->run);
    hk_jni_unref(e->frame);
    free(e);
}

static void enqueue(looper *l, ev *e)
{
    ev **pp;
    looper_start(l);
    hk_lock_acquire(&l->lock);
    for (pp = &l->head; *pp && (*pp)->when <= e->when;) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
    l->seq++;
    hk_lock_release(&l->lock);
    hk_futex_wake(&l->seq, true);
}

static void dispatch(looper *l, ev *e)
{
    static volatile i32 said;
    if (e->call) {
        e->call(e->call_arg);
    } else if (e->frame) {
        hk_jvalue t;
        t.j = e->when;
        if (!l->frame_time) {
            l->frame_time = hk_jni_new_object("java/lang/Long");
            l->frame_time->permanent = true;
        }
        hk_jni_set_field(l->frame_time, "value", 'J', t);
        hk_jni_unref(hk_jni_proxy_call(e->frame, "android/view/Choreographer$FrameCallback", "doFrame", "(J)V", &l->frame_time, 1));
    } else if (e->run) {
        run_runnable(e->run);
    } else if (e->msg) {
        handler *h = e->handler ? (handler *)e->handler->native : NULL;
        if (h && h->callback) {
            hk_jni_unref(hk_jni_proxy_call(h->callback, "android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z", &e->msg, 1));
        } else if (hk_atomic_add(&said, 1) <= 8) {
            hk_warn("looper: a message (what %d) went to a %s with no callback; dropped", (int)hk_jni_get_field(e->msg, "what").i,
                    hk_jni_class_name(e->handler));
        }
    }
}

static void looper_thread(void *arg)
{
    looper *l = (looper *)arg;
    t_looper = l;
    /* One of the app's threads from the start: it has a thread record before the app's code first asks whose thread this is. */
    hk_gthread_self();
    if (hk_app_threads_give_way()) hk_thread_runs_app_code();
    for (;;) {
        ev *e;
        i64 now, wait = 0;
        i32 seq;
        hk_lock_acquire(&l->lock);
        if (l->quit) {
            hk_lock_release(&l->lock);
            break;
        }
        e = l->head;
        seq = l->seq;
        now = now_ns();
        if (e && e->when <= now) {
            l->head = e->next;
        } else {
            if (e) wait = e->when - now;
            e = NULL;
        }
        hk_lock_release(&l->lock);
        if (e) {
            hk_atomic_xchg(&l->busy, 1);
            dispatch(l, e);
            ev_free(e);
            hk_atomic_xchg(&l->busy, 0);
            continue;
        }
        /* Until the queue changes, the next event is due, or a signal has been handled (the collector's stop is one). */
        hk_gwait(&l->seq, seq, wait > 0 ? (u64)wait : 0);
    }
    hk_atomic_xchg(&l->ended, 1);
    hk_futex_wake(&l->ended, true);
}

static void looper_quit(looper *l)
{
    if (!l) return;
    hk_lock_acquire(&l->lock);
    l->quit = true;
    l->seq++;
    hk_lock_release(&l->lock);
    hk_futex_wake(&l->seq, true);
}

void hk_jni_loop_shutdown(void)
{
    looper *l;
    hk_lock_acquire(&g_loop_lock);
    l = g_loopers;
    hk_lock_release(&g_loop_lock);
    for (; l; l = l->next) {
        hk_thread *t = l->thread;
        looper_quit(l);
        if (t && l != t_looper) {
            l->thread = NULL;
            hk_thread_join(t);
        }
    }
}

/* ----------------------------------------------------------------- Looper */

static looper *current_or_main(void) { return t_looper ? t_looper : main_looper(); }
static looper *looper_of(const hk_jobj *o) { return is_a(o, "android/os/Looper") ? (looper *)o->native : NULL; }

static void Looper_main(hk_jcall *c) { c->ret.l = main_looper()->mirror; }
static void Looper_mine(hk_jcall *c) { c->ret.l = t_looper ? t_looper->mirror : NULL; }
static void Looper_quit(hk_jcall *c) { looper_quit(looper_of(c->self)); }
static void Looper_is_current(hk_jcall *c) { c->ret.z = looper_of(c->self) && looper_of(c->self) == t_looper; }

/* A HandlerThread is a Thread whose run() is a Looper. The looper is there from construction; start() gives it a thread. */
static looper *thread_looper(const hk_jobj *o) { return is_a(o, "android/os/HandlerThread") ? (looper *)o->native : NULL; }

static void HT_init(hk_jcall *c)
{
    const char *name = c->sig[1] == 'L' ? hk_jni_string(c->args[0].l) : NULL;
    c->self->native = looper_new(name ? name : "HandlerThread");
}
static void HT_start(hk_jcall *c)
{
    looper *l = thread_looper(c->self);
    if (l) looper_start(l);
}
static void HT_looper(hk_jcall *c)
{
    looper *l = thread_looper(c->self);
    c->ret.l = l ? l->mirror : NULL;
}
static void HT_quit(hk_jcall *c)
{
    looper_quit(thread_looper(c->self));
    c->ret.z = 1;
}
static void HT_alive(hk_jcall *c)
{
    looper *l = thread_looper(c->self);
    c->ret.z = l && l->started && !l->ended;
}

/*
 * Thread.join: until the thread's loop has ended -- it was told to quit and
 * has finished what it was in the middle of. join(ms) gives up after that long.
 *
 * It has to wait. This is how Unity ends a decoder's thread when a film is
 * over or skipped: it posts one last message to the thread, whose handler quits
 * the loop from inside; joins the thread; and then deletes the handler's native
 * object. A join that returned at once had the thread call that handler while
 * it was being deleted ("Pure virtual function called!", or a jump to nowhere:
 * both seen on the phone, the first two times anyone skipped a film).
 *
 * And a join that waits has to end. The third time, the handler's way of
 * quitting was one this layer did not have (Message.getTarget returned null),
 * the loop was never told to quit, and the game stood still for good. So: a
 * loop that has sat with nothing to do and nothing in a handler for a second
 * while somebody waits for its thread to end, and has not been told to quit,
 * is ended here, loudly. Nobody joins a thread they mean to keep, and an idle
 * loop has no handler that could still be running when the caller goes on;
 * what is far likelier than an app that forgot to quit is another way of
 * quitting that is missing here, and the line in the log says to look for it.
 */
static bool looper_idle(looper *l)
{
    bool idle;
    hk_lock_acquire(&l->lock);
    idle = !l->busy && (!l->head || l->head->when > now_ns());
    hk_lock_release(&l->lock);
    return idle;
}

static void HT_join(hk_jcall *c)
{
    static volatile i32 said;
    looper *l = thread_looper(c->self);
    i64 limit_ms = c->sig && c->sig[1] == 'J' ? c->args[0].j : 0;
    i64 began = now_ns(), idle_since = 0;
    bool warned = false;
    if (!l || !l->started || l == t_looper) return;
    while (!l->ended) {
        i64 now = now_ns(), waited = now - began;
        if (limit_ms > 0 && waited >= limit_ms * 1000000) return;
        if (!l->quit && looper_idle(l)) {
            if (!idle_since) idle_since = now;
            if (now - idle_since > 1000000000ll) {
                hk_warn("looper: '%s' is being waited for to end, was never told to quit and has had nothing to do for a second: it is ended here. "
                        "The app's own way of quitting it most likely used something this layer lacks (look for \"is not implemented\" above)", l->name);
                looper_quit(l);
            }
        } else {
            idle_since = 0;
        }
        if (!warned && waited > 3000000000ll) {
            warned = true;
            if (hk_atomic_add(&said, 1) <= 4)
                hk_warn("looper: join has waited three seconds for '%s' to end, and waits on; its loop %s", l->name,
                        l->quit ? "was told to quit and is still in a handler" : "is busy and has not been told to quit");
        }
        hk_gwait(&l->ended, 0, 5000000ull);
    }
}

/* ---------------------------------------------------------------- Message */

static hk_jobj *new_message(hk_jobj *target, int what)
{
    hk_jobj *m = hk_jni_new_object("android/os/Message");
    hk_jvalue v;
    v.j = 0;
    hk_jni_set_field(m, "arg1", 'I', v);
    hk_jni_set_field(m, "arg2", 'I', v);
    hk_jni_set_field(m, "obj", 'L', v);
    v.i = what;
    hk_jni_set_field(m, "what", 'I', v);
    v.j = 0;
    v.l = target;
    hk_jni_set_field(m, "target", 'L', v);
    return m;
}

static void set_int(hk_jobj *o, const char *name, i32 i)
{
    hk_jvalue v;
    v.j = 0;
    v.i = i;
    hk_jni_set_field(o, name, 'I', v);
}

static void set_obj(hk_jobj *o, const char *name, hk_jobj *l)
{
    hk_jvalue v;
    v.j = 0;
    v.l = l;
    hk_jni_set_field(o, name, 'L', v);
}

/* Message.obtain(), obtain(Handler), obtain(Handler, int). */
static void Message_obtain(hk_jcall *c)
{
    bool has_target = !strncmp(c->sig, "(Landroid/os/Handler;", 21);
    bool has_what = has_target && c->sig[21] == 'I';
    c->ret.l = new_message(has_target ? c->args[0].l : NULL, has_what ? c->args[1].i : 0);
}

/* (A Handler may be of a class of the app's own, which says nothing here: what makes it one is that Handler's constructor ran for it.) */
static handler *handler_of(const hk_jobj *o) { return o && o->kind == HK_J_OBJECT ? (handler *)o->native : NULL; }

static void post_message(hk_jobj *handler_obj, hk_jobj *msg, i64 delay_ns)
{
    handler *h = handler_of(handler_obj);
    ev *e;
    if (!h || !msg) return;
    /* The message's target is whoever it is sent to (Handler.sendMessage on a message obtained elsewhere). */
    if (hk_jni_get_field(msg, "target").l != handler_obj) {
        hk_jvalue v;
        v.j = 0;
        v.l = handler_obj;
        hk_jni_set_field(msg, "target", 'L', v);
    }
    e = (ev *)hk_alloc(sizeof(*e));
    e->when = now_ns() + delay_ns;
    e->handler = hk_jni_ref(handler_obj);
    e->msg = hk_jni_ref(msg);
    enqueue(h->lp, e);
}

static void Message_send(hk_jcall *c) { post_message(hk_jni_get_field(c->self, "target").l, c->self, 0); }
/* Unity's last message to a decoder's thread ends the thread with message.getTarget().getLooper().quit(). */
static void Message_target(hk_jcall *c) { c->ret.l = hk_jni_get_field(c->self, "target").l; }
static void Message_set_target(hk_jcall *c)
{
    hk_jvalue v;
    v.j = 0;
    v.l = c->args[0].l;
    hk_jni_set_field(c->self, "target", 'L', v);
}

/* ---------------------------------------------------------------- Handler */

/* Handler(), Handler(Looper), Handler(Callback), Handler(Looper, Callback): the looper first wherever there is one. */
static void Handler_init(hk_jcall *c)
{
    handler *h = (handler *)hk_alloc(sizeof(*h));
    int i = 0;
    if (strstr(c->sig, "Landroid/os/Looper;")) h->lp = looper_of(c->args[i++].l);
    if (strstr(c->sig, "Landroid/os/Handler$Callback;")) h->callback = hk_jni_ref(c->args[i++].l);
    if (!h->lp) h->lp = current_or_main();
    c->self->native = h;
}

static void Handler_looper(hk_jcall *c)
{
    handler *h = handler_of(c->self);
    c->ret.l = h ? h->lp->mirror : NULL;
}

static bool post_runnable(hk_jobj *self, hk_jobj *run, i64 delay_ns)
{
    handler *h = handler_of(self);
    ev *e;
    if (!h || !run) return false;
    e = (ev *)hk_alloc(sizeof(*e));
    e->when = now_ns() + delay_ns;
    e->handler = hk_jni_ref(self);
    e->run = hk_jni_ref(run);
    enqueue(h->lp, e);
    return true;
}

static void Handler_post(hk_jcall *c) { c->ret.z = post_runnable(c->self, c->args[0].l, 0); }
static void Handler_post_delayed(hk_jcall *c) { c->ret.z = post_runnable(c->self, c->args[0].l, c->args[1].j * 1000000); }

/* obtainMessage(), (int), (int, Object), (int, int, int), (int, int, int, Object). */
static void Handler_obtain(hk_jcall *c)
{
    const char *p = c->sig + 1;
    hk_jobj *m = new_message(c->self, *p == 'I' ? c->args[0].i : 0);
    if (!strncmp(p, "III", 3)) {
        set_int(m, "arg1", c->args[1].i);
        set_int(m, "arg2", c->args[2].i);
        if (p[3] == 'L') set_obj(m, "obj", hk_jni_ref(c->args[3].l));
    } else if (p[0] == 'I' && p[1] == 'L') {
        set_obj(m, "obj", hk_jni_ref(c->args[1].l));
    }
    c->ret.l = m;
}

static void Handler_send(hk_jcall *c)
{
    bool delayed = strstr(c->sig, ";J)") != NULL;
    if (!c->args[0].l) return;
    set_obj(c->args[0].l, "target", c->self);
    post_message(c->self, c->args[0].l, delayed ? c->args[1].j * 1000000 : 0);
    c->ret.z = 1;
}

static void Handler_send_empty(hk_jcall *c)
{
    hk_jobj *m = new_message(c->self, c->args[0].i);
    post_message(c->self, m, c->sig[2] == 'J' ? c->args[1].j * 1000000 : 0);
    hk_jni_unref(m);
    c->ret.z = 1;
}

/* Takes out what is queued for this handler: by message code, by runnable, or all of it. */
static bool remove_events(hk_jobj *handler_obj, int what, hk_jobj *run, bool all)
{
    handler *h = handler_of(handler_obj);
    looper *l;
    ev **pp, *gone = NULL;
    if (!h) return false;
    l = h->lp;
    hk_lock_acquire(&l->lock);
    for (pp = &l->head; *pp;) {
        ev *e = *pp;
        bool hit = e->handler == handler_obj && !e->frame &&
                   (all || (run && e->run == run) || (!run && what >= 0 && e->msg && hk_jni_get_field(e->msg, "what").i == what));
        if (hit) {
            *pp = e->next;
            e->next = gone;
            gone = e;
        } else {
            pp = &e->next;
        }
    }
    hk_lock_release(&l->lock);
    all = gone != NULL;
    while (gone) {
        ev *e = gone;
        gone = e->next;
        ev_free(e);
    }
    return all;
}

static void Handler_remove_callbacks(hk_jcall *c) { remove_events(c->self, -1, c->args[0].l, false); }
static void Handler_remove_messages(hk_jcall *c) { remove_events(c->self, c->args[0].i, NULL, false); }
static void Handler_remove_all(hk_jcall *c) { remove_events(c->self, -1, NULL, true); }

static void Handler_has_messages(hk_jcall *c)
{
    handler *h = handler_of(c->self);
    ev *e;
    if (!h) return;
    hk_lock_acquire(&h->lp->lock);
    for (e = h->lp->head; e; e = e->next)
        if (e->handler == c->self && e->msg && hk_jni_get_field(e->msg, "what").i == c->args[0].i) c->ret.z = 1;
    hk_lock_release(&h->lp->lock);
}

void hk_jni_loop_call(hk_jobj *handler_obj, void (*fn)(void *arg), void *arg)
{
    handler *h = handler_of(handler_obj);
    ev *e = (ev *)hk_alloc(sizeof(*e));
    e->when = now_ns();
    e->call = fn;
    e->call_arg = arg;
    enqueue(h ? h->lp : main_looper(), e);
}

/* The UI thread runs what is posted to it; from the UI thread itself it runs at once. */
static void Activity_run_on_ui(hk_jcall *c)
{
    looper *m = main_looper();
    ev *e;
    if (!c->args[0].l) return;
    if (t_looper == m) {
        run_runnable(c->args[0].l);
        return;
    }
    e = (ev *)hk_alloc(sizeof(*e));
    e->when = now_ns();
    e->run = hk_jni_ref(c->args[0].l);
    enqueue(m, e);
}

/* ----------------------------------------------------------- Choreographer */

static void Choreo_instance(hk_jcall *c)
{
    looper *l = current_or_main();
    hk_lock_acquire(&l->lock);
    if (!l->choreographer) {
        l->choreographer = hk_jni_new_object("android/view/Choreographer");
        l->choreographer->native = l;
        l->choreographer->permanent = true;
    }
    c->ret.l = l->choreographer;
    hk_lock_release(&l->lock);
}

static looper *choreo_looper(const hk_jobj *o) { return is_a(o, "android/view/Choreographer") ? (looper *)o->native : NULL; }

static void post_frame(hk_jobj *self, hk_jobj *cb, i64 delay_ns)
{
    looper *l = choreo_looper(self);
    i64 t = now_ns() + delay_ns;
    ev *e;
    if (!l || !cb) return;
    e = (ev *)hk_alloc(sizeof(*e));
    e->when = (t / VSYNC_NS + 1) * VSYNC_NS;       /* the next 60 Hz boundary after t */
    e->frame = hk_jni_ref(cb);
    enqueue(l, e);
}

static void Choreo_post(hk_jcall *c) { post_frame(c->self, c->args[0].l, 0); }
static void Choreo_post_delayed(hk_jcall *c) { post_frame(c->self, c->args[0].l, c->args[1].j * 1000000); }

static void Choreo_remove(hk_jcall *c)
{
    looper *l = choreo_looper(c->self);
    ev **pp, *gone = NULL;
    if (!l) return;
    hk_lock_acquire(&l->lock);
    for (pp = &l->head; *pp;) {
        ev *e = *pp;
        if (e->frame && e->frame == c->args[0].l) {
            *pp = e->next;
            e->next = gone;
            gone = e;
        } else {
            pp = &e->next;
        }
    }
    hk_lock_release(&l->lock);
    while (gone) {
        ev *e = gone;
        gone = e->next;
        ev_free(e);
    }
}

static void Choreo_frame_time(hk_jcall *c) { c->ret.j = now_ns() / VSYNC_NS * VSYNC_NS; }
static void Choreo_interval(hk_jcall *c) { c->ret.j = VSYNC_NS; }

static void ret_void(hk_jcall *c) { (void)c; }

/* Integer.parseInt, Long.parseLong (with a radix or without), Float.parseFloat, Double.parseDouble, Boolean.parseBoolean. */
static void parse_number(hk_jcall *c)
{
    const char *text = hk_jni_string(c->args[0].l), *ret = strchr(c->sig, ')');
    int radix = !strncmp(c->sig, "(Ljava/lang/String;I)", 21) ? c->args[1].i : 10;
    char kind = ret ? ret[1] : 'I';
    char *end = NULL;
    if (!text) text = "";
    while (*text == ' ') text++;
    if (kind == 'Z') {
        c->ret.z = !strcmp(text, "true") || !strcmp(text, "TRUE") || !strcmp(text, "True");
        return;
    }
    if (kind == 'F') c->ret.f = (float)strtod(text, &end);
    else if (kind == 'D') c->ret.d = strtod(text, &end);
    else if (kind == 'J') c->ret.j = strtoll(text, &end, radix);
    else c->ret.i = (i32)strtol(text, &end, radix);
    if (end == text || (end && *end)) {
        char msg[120];
        snprintf(msg, sizeof(msg), "For input string: \"%.60s\"", text);
        hk_jni_throw("java/lang/NumberFormatException", msg);
    }
}

/* Integer.toString(int), Long.toString(long), String.valueOf(int/long/boolean/float/double). */
static void number_text(hk_jcall *c)
{
    char text[64];
    switch (c->sig[1]) {
    case 'J': snprintf(text, sizeof(text), "%lld", (long long)c->args[0].j); break;
    case 'Z': snprintf(text, sizeof(text), "%s", c->args[0].z ? "true" : "false"); break;
    case 'F': snprintf(text, sizeof(text), "%g", (double)c->args[0].f); break;
    case 'D': snprintf(text, sizeof(text), "%g", c->args[0].d); break;
    case 'L': snprintf(text, sizeof(text), "%s", hk_jni_string(c->args[0].l) ? hk_jni_string(c->args[0].l) : "null"); break;
    default: snprintf(text, sizeof(text), "%d", (int)c->args[0].i); break;
    }
    c->ret.l = hk_jni_new_string(text);
}

/* ---------------------------------------------------------------- install */

static const struct { const char *name, *super; } k_classes[] = {
    { "java/lang/Number", "java/lang/Object" },      { "java/lang/Long", "java/lang/Number" },
    { "java/lang/Integer", "java/lang/Number" },     { "java/lang/Short", "java/lang/Number" },
    { "java/lang/Byte", "java/lang/Number" },        { "java/lang/Float", "java/lang/Number" },
    { "java/lang/Double", "java/lang/Number" },      { "java/lang/Boolean", "java/lang/Object" },
    { "java/lang/Character", "java/lang/Object" },   { "java/lang/Thread", "java/lang/Object" },
    { "android/os/HandlerThread", "java/lang/Thread" }, { "android/os/Looper", "java/lang/Object" },
    { "android/os/Handler", "java/lang/Object" },    { "android/os/Message", "java/lang/Object" },
    { "android/os/Handler$Callback", "java/lang/Object" }, { "android/view/Choreographer", "java/lang/Object" },
    { "android/view/Choreographer$FrameCallback", "java/lang/Object" }, { "java/lang/Runnable", "java/lang/Object" },
    { PROXY, "java/lang/Object" },                   { BRIDGE, "java/lang/Object" },
    { "java/lang/reflect/Method", "java/lang/Object" },
};

static const char *const k_boxes[] = { "java/lang/Long",  "java/lang/Integer", "java/lang/Short",   "java/lang/Byte",
                                       "java/lang/Float", "java/lang/Double",  "java/lang/Boolean", "java/lang/Character" };

static hk_jhle g_box_table[HK_COUNT(k_boxes) * 2 + 1];

static const hk_jhle k_table[] = {
    { "java/lang/Number", "intValue", "()I", number_int },
    { "java/lang/Number", "longValue", "()J", number_long_value },
    { "java/lang/Number", "shortValue", "()S", number_short },
    { "java/lang/Number", "byteValue", "()B", number_byte },
    { "java/lang/Number", "floatValue", "()F", number_float },
    { "java/lang/Number", "doubleValue", "()D", number_double_value },
    { "java/lang/Boolean", "booleanValue", "()Z", boolean_value },
    { "java/lang/Integer", "parseInt", NULL, parse_number },
    { "java/lang/Long", "parseLong", NULL, parse_number },
    { "java/lang/Float", "parseFloat", NULL, parse_number },
    { "java/lang/Double", "parseDouble", NULL, parse_number },
    { "java/lang/Boolean", "parseBoolean", NULL, parse_number },
    { "java/lang/Integer", "toString", "(I)Ljava/lang/String;", number_text },
    { "java/lang/Long", "toString", "(J)Ljava/lang/String;", number_text },
    { "java/lang/String", "valueOf", NULL, number_text },
    { "java/lang/Character", "charValue", "()C", char_value },

    { "java/lang/reflect/Method", "getName", "()Ljava/lang/String;", method_name },
    { "java/lang/reflect/Method", "getReturnType", "()Ljava/lang/Class;", method_return_type },
    { "java/lang/reflect/Method", "getParameterTypes", "()[Ljava/lang/Class;", method_parameter_types },

    { BRIDGE, "newInterfaceProxy", "(J[Ljava/lang/Class;)Ljava/lang/Object;", bridge_new_proxy },
    { BRIDGE, "disableInterfaceProxy", "(Ljava/lang/Object;)V", bridge_disable_proxy },

    { "android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", Looper_main },
    { "android/os/Looper", "myLooper", "()Landroid/os/Looper;", Looper_mine },
    { "android/os/Looper", "quit", "()V", Looper_quit },
    { "android/os/Looper", "quitSafely", "()V", Looper_quit },
    { "android/os/Looper", "isCurrentThread", "()Z", Looper_is_current },
    { "android/content/Context", "getMainLooper", "()Landroid/os/Looper;", Looper_main },
    { "android/os/HandlerThread", "<init>", NULL, HT_init },
    { "android/os/HandlerThread", "start", "()V", HT_start },
    { "android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", HT_looper },
    { "android/os/HandlerThread", "quit", "()Z", HT_quit },
    { "android/os/HandlerThread", "quitSafely", "()Z", HT_quit },
    { "android/os/HandlerThread", "isAlive", "()Z", HT_alive },
    { "android/os/HandlerThread", "join", NULL, HT_join },
    { "android/os/HandlerThread", "setPriority", NULL, ret_void },
    { "android/os/HandlerThread", "setName", NULL, ret_void },

    { "android/os/Message", "obtain", "()Landroid/os/Message;", Message_obtain },
    { "android/os/Message", "obtain", "(Landroid/os/Handler;)Landroid/os/Message;", Message_obtain },
    { "android/os/Message", "obtain", "(Landroid/os/Handler;I)Landroid/os/Message;", Message_obtain },
    { "android/os/Message", "sendToTarget", "()V", Message_send },
    { "android/os/Message", "getTarget", "()Landroid/os/Handler;", Message_target },
    { "android/os/Message", "setTarget", "(Landroid/os/Handler;)V", Message_set_target },
    { "android/os/Message", "recycle", "()V", ret_void },

    { "android/os/Handler", "<init>", NULL, Handler_init },
    { "android/os/Handler", "getLooper", "()Landroid/os/Looper;", Handler_looper },
    { "android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", Handler_post },
    { "android/os/Handler", "postAtFrontOfQueue", "(Ljava/lang/Runnable;)Z", Handler_post },
    { "android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", Handler_post_delayed },
    { "android/os/Handler", "obtainMessage", NULL, Handler_obtain },
    { "android/os/Handler", "sendMessage", "(Landroid/os/Message;)Z", Handler_send },
    { "android/os/Handler", "sendMessageDelayed", "(Landroid/os/Message;J)Z", Handler_send },
    { "android/os/Handler", "sendEmptyMessage", "(I)Z", Handler_send_empty },
    { "android/os/Handler", "sendEmptyMessageDelayed", "(IJ)Z", Handler_send_empty },
    { "android/os/Handler", "removeCallbacks", "(Ljava/lang/Runnable;)V", Handler_remove_callbacks },
    { "android/os/Handler", "removeMessages", "(I)V", Handler_remove_messages },
    { "android/os/Handler", "removeCallbacksAndMessages", "(Ljava/lang/Object;)V", Handler_remove_all },
    { "android/os/Handler", "hasMessages", "(I)Z", Handler_has_messages },
    { "android/app/Activity", "runOnUiThread", "(Ljava/lang/Runnable;)V", Activity_run_on_ui },

    { "android/view/Choreographer", "getInstance", "()Landroid/view/Choreographer;", Choreo_instance },
    { "android/view/Choreographer", "postFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", Choreo_post },
    { "android/view/Choreographer", "postFrameCallbackDelayed", "(Landroid/view/Choreographer$FrameCallback;J)V", Choreo_post_delayed },
    { "android/view/Choreographer", "removeFrameCallback", "(Landroid/view/Choreographer$FrameCallback;)V", Choreo_remove },
    { "android/view/Choreographer", "getFrameTimeNanos", "()J", Choreo_frame_time },
    { "android/view/Choreographer", "getFrameIntervalNanos", "()J", Choreo_interval },
    { NULL, NULL, NULL, NULL }
};

void hk_jni_loop_install(void)
{
    static bool done;
    size_t i;
    if (done) return;
    done = true;
    for (i = 0; i < HK_COUNT(k_classes); i++) hk_jni_class(k_classes[i].name, k_classes[i].super);
    /* A class named for each primitive type, which is what a Method's parameter types are when they are not objects. */
    {
        static const char *const prim[] = { "int", "long", "boolean", "float", "double", "byte", "char", "short", "void" };
        for (i = 0; i < HK_COUNT(prim); i++) hk_jni_class(prim[i], "java/lang/Object");
    }
    for (i = 0; i < HK_COUNT(k_boxes); i++) {
        g_box_table[i * 2].cls = k_boxes[i];
        g_box_table[i * 2].name = "<init>";
        g_box_table[i * 2].fn = box_init;
        g_box_table[i * 2 + 1].cls = k_boxes[i];
        g_box_table[i * 2 + 1].name = "valueOf";
        g_box_table[i * 2 + 1].fn = box_value_of;
    }
    hk_jni_register(g_box_table);
    hk_jni_register(k_table);
}
