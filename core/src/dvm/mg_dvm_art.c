/*
 * libcore's own classes as the boot class path: what Android's runtime does
 * for them inside itself.
 *
 * libcore's bytecode is interpreted like any other. Its native methods are of
 * two kinds. Most are in libcore's own libraries (libjavacore.so,
 * libopenjdk.so), which register theirs when they are loaded: guest code,
 * which the linker loads like an app's. The rest are the runtime's, because
 * they are about what only a runtime knows -- an object's class and identity,
 * the text of a string, the calling thread, the stack, fields by offset --
 * and this file is those, written against Android 9's own source
 * (the .cc files of art/runtime/native at android-9.0.0_r47): the same classes, the same
 * signatures, the same answers where an answer makes sense here.
 *
 * It is also what else the runtime owns of those classes: the fields of a
 * Class object that bytecode reads, the java.lang.Thread of a thread, and the
 * order things are started in (mg_dvm_art_start).
 *
 * What is not here is in docs/JAVA.md: reflection beyond what libcore itself
 * uses, annotations, class loaders, proxies. Nothing is collected, so the
 * natives that are about the collector (most of VMRuntime) do nothing.
 */
#include "mg_dvm_internal.h"

#include <math.h>
#include <stdio.h>

#include "../bionic/mg_bionic.h"

#define N(name) static void name(mg_jcall *c)

static bool npe(const char *what) { return mg_dvm_throw("java/lang/NullPointerException", "%s", what); }

/* A class name as Java prints it: dots for slashes. */
static mg_jobj *dotted(const char *name)
{
    char *text = mg_strdup(name), *p;
    mg_jobj *s;
    for (p = text; *p; p++)
        if (*p == '/') *p = '.';
    s = mg_dvm_new_string(text);
    free(text);
    return s;
}

static void set_int(mg_jobj *o, const char *name, i32 v)
{
    mg_jvalue x;
    x.j = 0;
    x.i = v;
    mg_dvm_set_field(o, name, x);
}

static void set_long(mg_jobj *o, const char *name, i64 v)
{
    mg_jvalue x;
    x.j = v;
    mg_dvm_set_field(o, name, x);
}

static void set_obj(mg_jobj *o, const char *name, mg_jobj *v)
{
    mg_jvalue x;
    x.j = 0;
    x.l = v;
    mg_dvm_set_field(o, name, x);
}

static mg_jvalue get(mg_jobj *o, const char *name)
{
    mg_jvalue x;
    x.j = 0;
    mg_dvm_get_field(o, name, &x);
    return x;
}

/* Service APK loaders are explicit: they share platform classes, never a client's libraries. */
static mg_jobj *g_service_loaders[16];
static char *g_service_apks[16]; /* the APK each one opened, by the name the app knows it under */
/*
 * Other loaders over a service's APK: microG makes a PathClassLoader over its own APK for each of its dynamite modules a client
 * loads (DynamiteContextFactory.createClassLoader), whose classes on Android are microG's, apart from the client's. Here such
 * a loader is the service loader's scope under another name: its classes are the service's, its native libraries the
 * service's (a class's loader, as Java and System.loadLibrary see it, is the service loader). Before, the APK's classes were
 * put on the client's one class path, where the client's own classes of the same names won (Play Store's
 * ScionActivityInfo, without the field microG's measurement module reads) and conscrypt's natives were looked for in the
 * client's APK (Sberbank's ProviderInstaller: UnsatisfiedLinkError).
 */
#define MAX_ALIASES 32
static mg_jobj *g_alias_loaders[MAX_ALIASES];
static int g_alias_scopes[MAX_ALIASES];
int mg_dvm_loader_scope(mg_jobj *loader) {
    int i;
    if (!loader) return 0;
    for (i = 1; i < 16; i++) if (g_service_loaders[i] == loader) return i;
    for (i = 0; i < MAX_ALIASES; i++) if (g_alias_loaders[i] == loader) return g_alias_scopes[i];
    return 0;
}
mg_jobj *mg_dvm_scope_loader(int scope) { return scope > 0 && scope < 16 ? g_service_loaders[scope] : NULL; }
void mg_dvm_loader_roots(void (*fn)(mg_jobj *, void *), void *user) {
    int i;
    for (i = 1; i < 16; i++) if (g_service_loaders[i]) fn(g_service_loaders[i], user);
    /* (Kept for good, as the table holds them by address: microG keeps each module's loader for good itself.) */
    for (i = 0; i < MAX_ALIASES; i++) if (g_alias_loaders[i]) fn(g_alias_loaders[i], user);
    for (i = 0; i < (int)mg_dvm_dex_count(); i++) {
        mg_dvm_dex *d = mg_dvm_dex_at((u32)i);
        if (d && d->loader) fn(d->loader, user);
    }
}
/* The scope of the service loader that opened this APK, or 0. */
static int service_apk_scope(const char *source)
{
    int i;
    for (i = 1; i < 16; i++) if (g_service_apks[i] && !strcmp(g_service_apks[i], source)) return i;
    return 0;
}
/* A loader made over a service's APK becomes another name of its scope: false when the table is full. */
static bool alias_loader(mg_jobj *loader, int scope, const char *source)
{
    mg_jvalue parent, merged, size;
    int i, free_slot = -1;
    bool known = false;
    mg_dvm_lock();
    for (i = 0; i < MAX_ALIASES && !known; i++) {
        if (g_alias_loaders[i] == loader) known = true;
        else if (!g_alias_loaders[i] && free_slot < 0) free_slot = i;
    }
    if (!known && free_slot >= 0) {
        g_alias_loaders[free_slot] = loader;
        g_alias_scopes[free_slot] = scope;
    }
    mg_dvm_unlock();
    if (known) return true;
    if (free_slot < 0) return false;
    mg_info("dvm: a %s over %s: the classes of its scope %d", mg_dvm_class_of(loader)->name, source, scope);
    /* microG's filter of the classes a module takes from its client (FilteredClassLoader, the module's MERGED_CLASSES and
       MERGED_PACKAGES: Cronet's API) is not followed: those classes are microG's own here. Said, for when one is wanted. */
    parent.l = merged.l = NULL;
    size.i = 0;
    if (mg_dvm_get_field(loader, "parent", &parent) && parent.l && !strcmp(mg_dvm_class_of(parent.l)->name, "com/google/android/gms/chimera/container/FilteredClassLoader")) {
        int n = 0;
        if (mg_dvm_get_field(parent.l, "allowedClasses", &merged) && merged.l && mg_dvm_call_virtual(merged.l, "size", "()I", NULL, &size)) n += size.i;
        size.i = 0;
        if (mg_dvm_get_field(parent.l, "allowedPackages", &merged) && merged.l && mg_dvm_call_virtual(merged.l, "size", "()I", NULL, &size)) n += size.i;
        mg_dvm_clear_pending();
        if (n) mg_warn("dvm: the module's loader over %s names %d classes and packages to take from the app; they are the service's own here", source, n);
    }
    return true;
}
N(ServiceClassLoader_openApk) {
    const char *path = mg_dvm_string_utf8(c->args[0].l);
    char *host;
    char *legacy;
    u8 *legacy_dex;
    size_t legacy_size = 0;
    int scope;
    if (!path) { npe("apk"); return; }
    mg_dvm_lock();
    for (scope = 1; scope < 16 && g_service_loaders[scope]; scope++);
    if (scope < 16) g_service_loaders[scope] = c->self;
    mg_dvm_unlock();
    if (scope == 16) { mg_dvm_throw("java/lang/IllegalStateException", "Too many service loaders"); return; }
    host = mg_fs_host_path(path);
    if (!host || (!mg_dvm_add_apk_scope(host, scope) || !mg_ld_add_service_apk(host, scope))) {
        free(host);
        mg_dvm_throw("java/io/IOException", "Cannot load service APK %s", path);
        return;
    }
    free(host);
    /* A separately loaded service may declare Android's optional Apache HTTP
       library even when its client does not. Keep that library in the service
       scope so it cannot resolve client classes or displace client libraries. */
    legacy = mg_sprintf("%s/framework/org.apache.http.legacy.dex", g_dvm.android_root ? g_dvm.android_root : ".");
    legacy_dex = mg_file_read_all(legacy, &legacy_size);
    if (legacy_dex && !mg_dvm_add_dex_scope(legacy_dex, legacy_size, "org.apache.http.legacy.dex", scope))
        free(legacy_dex);
    free(legacy);
    g_service_apks[scope] = mg_strdup(path);
    c->ret.i = scope;
}
N(ServiceClassLoader_findClassNative) {
    const char *given = mg_dvm_string_utf8(c->args[0].l);
    char *name, *p;
    mg_dvm_class *k;
    int scope = mg_dvm_loader_scope(c->self);
    if (!given) { npe("name"); return; }
    name = mg_strdup(given);
    for (p = name; *p; p++) if (*p == '.') *p = '/';
    if (!scope || !mg_dvm_class_exists_scope(name, scope)) mg_dvm_throw("java/lang/ClassNotFoundException", "%s", given);
    else { k = mg_dvm_find_class_scope(name, scope); if (k) c->ret.l = mg_dvm_mirror(k); }
    free(name);
}

/* ------------------------------------------------------------ Class objects */

/* Every interface a class's own list names, and theirs, each once. */
typedef struct iface_list {
    mg_dvm_class **v;
    int n, cap;
} iface_list;

static void flatten_interfaces(mg_dvm_class *c, iface_list *l)
{
    int i, k;
    for (i = 0; i < c->nifaces; i++) {
        mg_dvm_class *ic = c->ifaces[i];
        for (k = 0; k < l->n; k++)
            if (l->v[k] == ic) break;
        /* One already there has had its own interfaces taken too: going through them again only costs, and on a
           hierarchy where many interfaces share theirs it costs more at each level. */
        if (k < l->n) continue;
        if (l->n == l->cap) {
            int cap = l->cap ? l->cap * 2 : 64;
            mg_dvm_class **v = (mg_dvm_class **)realloc(l->v, sizeof(*v) * (size_t)cap);
            if (!v) return;
            l->v = v;
            l->cap = cap;
        }
        l->v[l->n++] = ic;
        flatten_interfaces(ic, l);
    }
}

/*
 * The fields of java.lang.Class that libcore's bytecode reads without asking
 * the runtime: what the class is (accessFlags, primitiveType), what is above
 * it (superClass, componentType), and every interface it implements, its
 * ancestors' included (ifTable: pairs, the interface and a table this
 * runtime has no use for), which is what Class.isAssignableFrom walks.
 */
static void fill_mirror(mg_jobj *m, mg_dvm_class *c)
{
    static const char prims[] = "ZBCSIJFDV";               /* Android numbers the primitive types from one in this order */
    iface_list all = { NULL, 0, 0 };
    mg_dvm_class *k;
    int i;
    set_int(m, "accessFlags", (i32)c->flags);
    set_int(m, "primitiveType", c->prim && strchr(prims, c->prim) ? (i32)(strchr(prims, c->prim) - prims) + 1 : 0);
    set_int(m, "dexClassDefIndex", c->dex ? (i32)c->def : 0xFFFF);
    set_int(m, "objectSize", (i32)(MG_DVM_HEADER + c->nslots * sizeof(mg_jvalue)));
    if (c->super) set_obj(m, "superClass", mg_dvm_mirror(c->super));
    if (c->component) set_obj(m, "componentType", mg_dvm_mirror(c->component));
    /* Each dynamically opened DEX names its defining loader; the original app uses its app loader. */
    if (c->scope) set_obj(m, "classLoader", mg_dvm_scope_loader(c->scope));
    else if (c->dex && c->dex->loader) set_obj(m, "classLoader", c->dex->loader);
    else if (c->dex && c->dex->app && g_dvm.app_loader) set_obj(m, "classLoader", g_dvm.app_loader);
    /* All of them, however many: a table of at most 128 left Avito's Dagger component, which has 826, not an instance of
       the 506th of its interface's 564 by Class.isInstance, and Avito ended at its start on a dependency it found missing. */
    for (k = c; k; k = k->super) flatten_interfaces(k, &all);
    if (all.n) {
        mg_jobj *table = mg_dvm_new_array("[Ljava/lang/Object;", all.n * 2);
        for (i = 0; table && i < all.n; i++) table->u.oarr.v[2 * i] = mg_dvm_mirror(all.v[i]);
        set_obj(m, "ifTable", table);
    }
    free(all.v);
}

/* ------------------------------------------------------------------ threads */

static mg_dvm_class *g_thread_class;
static volatile i32 g_thread_count;

/*
 * The java.lang.Thread of a thread Java did not start, made as ART makes one
 * for a thread that attaches (Thread::CreatePeer and Runtime::
 * AttachCurrentThread in art/runtime): the constructor the runtime keeps for
 * it, then the group is told it runs. `name` NULL is the first such thread's
 * "main" and "Thread-N" after; `group` NULL the main group. NULL with an
 * exception pending.
 */
static mg_jobj *make_peer(const char *name, mg_jobj *in_group, bool daemon)
{
    mg_dvm_thread *t = mg_dvm_self();
    mg_dvm_method *ctor;
    mg_jvalue args[4], group, v;
    char made[32];
    mg_jobj *peer;
    if (!g_thread_class) g_thread_class = mg_dvm_find_class("java/lang/Thread");
    if (!g_thread_class || !mg_dvm_ensure_init(g_thread_class) || !mg_dvm_get_static("java/lang/ThreadGroup", "mainThreadGroup", &group)) return NULL;
    if (in_group && mg_dvm_instance_of(in_group, "java/lang/ThreadGroup")) group.l = in_group;
    peer = mg_dvm_alloc_object(g_thread_class);
    ctor = mg_dvm_find_method(g_thread_class, "<init>", "(Ljava/lang/ThreadGroup;Ljava/lang/String;IZ)V", false);
    if (!peer || !ctor) return NULL;
    /* Before its constructor runs: the constructor asks for the current thread, and must be given this one. */
    t->peer = peer;
    set_long(peer, "nativePeer", (i64)(mg_addr)t);
    if (mg_atomic_add(&g_thread_count, 1) == 1 && !name) mg_strlcpy(made, "main", sizeof(made));
    else snprintf(made, sizeof(made), "Thread-%u", (unsigned)t->id);
    memset(args, 0, sizeof(args));
    args[0].l = group.l;
    args[1].l = mg_dvm_new_string(name ? name : made);
    args[2].i = 5;                                               /* Thread.NORM_PRIORITY */
    args[3].z = daemon;
    if (!mg_dvm_invoke(ctor, peer, args, NULL)) {
        t->peer = NULL;
        return NULL;
    }
    /* The constructor counted it among the group's threads that have not started; this is the start. */
    v.j = 0;
    v.l = peer;
    if (group.l && !mg_dvm_call_virtual(group.l, "add", "(Ljava/lang/Thread;)V", &v, NULL)) mg_dvm_clear_pending();
    return peer;
}

/* The java.lang.Thread of the calling thread; made, in the main group, for a thread Java did not start. NULL with an exception pending. */
static mg_jobj *current_thread(void)
{
    mg_dvm_thread *t = mg_dvm_self();
    return t->peer ? t->peer : make_peer(NULL, NULL, false);
}

static mg_dvm_thread *thread_of(mg_jobj *peer) { return peer ? (mg_dvm_thread *)(mg_addr)get(peer, "nativePeer").j : NULL; }

/* The end of a thread as Java sees it: the group forgets it, it is no longer alive, and join() -- a wait on the thread's lock for as long as it is alive -- is told. */
static void end_peer(mg_jobj *peer)
{
    mg_jvalue v;
    mg_jobj *lock;
    v.j = 0;
    v.l = peer;
    if (get(peer, "group").l && !mg_dvm_call_virtual(get(peer, "group").l, "threadTerminated", "(Ljava/lang/Thread;)V", &v, NULL)) mg_dvm_clear_pending();
    lock = get(peer, "lock").l;
    if (lock) mg_dvm_monitor_enter(lock);
    set_long(peer, "nativePeer", 0);
    if (lock) {
        mg_dvm_monitor_notify(lock, true);
        mg_dvm_monitor_exit(lock);
    }
}

bool mg_dvm_thread_attach(const char *name, mg_jobj *group, bool daemon)
{
    mg_dvm_thread *t = mg_dvm_self();
    if (!g_dvm.running || !g_dvm.libcore || t->peer) return true;
    if (make_peer(name, group, daemon)) return true;
    mg_dvm_clear_pending();
    return false;
}

void mg_dvm_thread_detach(void)
{
    mg_dvm_thread *t = mg_dvm_self();
    mg_jobj *peer = t->peer, *e;
    mg_jvalue v;
    if (!g_dvm.running || !g_dvm.libcore || !peer) return;
    /* An exception nothing caught goes where Java sends one of a thread that ends with it. */
    e = mg_dvm_pending();
    if (e) {
        mg_dvm_clear_pending();
        v.j = 0;
        v.l = e;
        if (!mg_dvm_call_virtual(peer, "dispatchUncaughtException", "(Ljava/lang/Throwable;)V", &v, NULL)) mg_dvm_clear_pending();
    }
    end_peer(peer);
    t->peer = NULL;
}

/* What a thread Java starts is given: its Thread, and whose code started it (mg_dvm_thread.owner_scope). */
typedef struct started_thread {
    mg_jobj *peer;
    int owner_scope;
} started_thread;

/*
 * The class scope of the service whose code is starting a thread on this one: that of the innermost frame of an app's own code
 * or a service's (a pool's worker is started under the platform's frames, by the code that gave the pool its first task); with
 * none, this thread's own owner.
 */
static int starting_scope(void)
{
    mg_dvm_thread *self = mg_dvm_self();
    mg_dvm_frame *f;
    for (f = self->top; f; f = f->prev) {
        if (!f->m || !f->m->cls) continue;
        if (f->m->cls->scope > 0) return f->m->cls->scope;
        if (mg_dvm_class_is_app(f->m->cls)) return 0;
    }
    return self->owner_scope;
}

/* A thread Java started: run(), what Java does with an exception nothing caught, and the word to whoever waits in join(). */
static void thread_main(void *arg)
{
    started_thread *s = (started_thread *)arg;
    mg_jobj *peer = s->peer, *e;
    mg_dvm_thread *t = mg_dvm_self();
    mg_jvalue v;
    t->owner_scope = s->owner_scope;
    free(s);
    /* One of the app's threads from the start: native methods run on it, and the guest's C library must know it. */
    mg_gthread_self();
    t->peer = peer;
    mg_atomic_add(&g_thread_count, 1);
    set_long(peer, "nativePeer", (i64)(mg_addr)t);
    if (!mg_dvm_call_virtual(peer, "run", "()V", NULL, NULL)) {
        e = mg_dvm_pending();
        mg_dvm_clear_pending();
        v.j = 0;
        v.l = e;
        if (!mg_dvm_call_virtual(peer, "dispatchUncaughtException", "(Ljava/lang/Throwable;)V", &v, NULL)) mg_dvm_clear_pending();
    }
    end_peer(peer);
}

N(Thread_currentThread) { c->ret.l = current_thread(); }

N(Thread_nativeCreate)
{
    mg_jobj *peer = c->args[0].l;
    i64 stack = c->args[1].j;
    mg_thread *th;
    started_thread *s = (started_thread *)mg_alloc(sizeof(*s));
    s->peer = peer;
    s->owner_scope = starting_scope();
    /* Alive from the moment start() returns, as Java promises; the thread puts its own state here when it runs. */
    set_long(peer, "nativePeer", 1);
    /* Java's frames are not on this stack, only what native methods need of it. */
    th = mg_thread_start(thread_main, s, stack > (1 << 20) ? (size_t)stack : (size_t)1 << 20, "java");
    if (!th) {
        free(s);
        set_long(peer, "nativePeer", 0);
        mg_dvm_throw("java/lang/OutOfMemoryError", "a thread could not be started");
        return;
    }
    mg_thread_detach(th);
}

N(Thread_interrupted)
{
    mg_dvm_thread *t = mg_dvm_self();
    c->ret.z = t->interrupted;
    t->interrupted = false;
}

N(Thread_isInterrupted)
{
    mg_dvm_thread *t = thread_of(c->self);
    c->ret.z = (mg_addr)t > 1 && t->interrupted;
}

N(Thread_nativeInterrupt)
{
    mg_dvm_thread *t = thread_of(c->self);
    mg_jobj *lock = get(c->self, "lock").l;
    if ((mg_addr)t > 1) t->interrupted = true;
    /* A thread asleep is waiting on its own lock (Thread.sleep): woken, it finds the flag. One waiting on another object finds it when that wait ends. */
    if (lock) {
        mg_dvm_monitor_enter(lock);
        mg_dvm_monitor_notify(lock, true);
        mg_dvm_monitor_exit(lock);
    }
}

N(Thread_nativeHoldsLock)
{
    /* Only the owner can notify: asked without disturbing anyone, since nobody waits for a signal that is not sent. */
    mg_jobj *o = c->args[0].l;
    if (!o) npe("holdsLock(null)");
    else if (mg_dvm_monitor_exit(o)) {
        mg_dvm_monitor_enter(o);
        c->ret.z = 1;
    }
}

N(Thread_nativeGetStatus) { c->ret.i = c->args[0].z ? 1 : 0; }  /* an index into Thread.State: NEW, or RUNNABLE once started */
N(Thread_nothing) { (void)c; }
N(Thread_yield)
{
    (void)c;
    mg_thread_yield();
}

/* Thread.sleep: a wait on the thread's own lock, which the caller holds, so that an interrupt can end it. */
N(Thread_sleep)
{
    mg_dvm_thread *t = mg_dvm_self();
    i64 ms = c->args[1].j + (c->args[2].i ? 1 : 0);
    if (!c->args[0].l || !mg_dvm_monitor_wait(c->args[0].l, ms > 0 ? ms : 1)) mg_sleep_ns((u64)(ms > 0 ? ms : 1) * 1000000ull);
    if (t->interrupted) {
        t->interrupted = false;
        mg_dvm_throw("java/lang/InterruptedException", NULL);
    }
}

/* ------------------------------------------------------------------- Object */

N(Object_identityHashCodeNative) { c->ret.i = c->args[0].l ? (DVM_HASH_WORD(c->args[0].l) & 0x0FFFFFFF) : 0; }

N(Object_internalClone)
{
    mg_jobj *o = c->self, *copy;
    mg_dvm_class *k = mg_dvm_class_of(o);
    if (o->kind == MG_J_PRIM_ARRAY || o->kind == MG_J_OBJ_ARRAY) {
        copy = mg_dvm_alloc_array(k, mg_dvm_array_length(o));
        if (!copy) return;
        if (o->kind == MG_J_OBJ_ARRAY) memcpy(copy->u.oarr.v, o->u.oarr.v, sizeof(mg_jobj *) * o->u.oarr.len);
        else memcpy(copy->u.arr.data, o->u.arr.data, (size_t)o->u.arr.len * o->u.arr.esz);
    } else {
        copy = mg_dvm_alloc_object(k);
        if (!copy) return;
        memcpy(mg_dvm_slots(copy), mg_dvm_slots(o), sizeof(mg_jvalue) * k->nslots);
    }
    c->ret.l = copy;
}

static void not_owner(const char *what) { mg_dvm_throw("java/lang/IllegalMonitorStateException", "object not locked by thread before %s()", what); }

static void wait_on(mg_jcall *c, i64 ms)
{
    mg_dvm_thread *t = mg_dvm_self();
    if (!mg_dvm_monitor_wait(c->self, ms)) {
        not_owner("wait");
        return;
    }
    if (t->interrupted) {
        t->interrupted = false;
        mg_dvm_throw("java/lang/InterruptedException", NULL);
    }
}

N(Object_wait) { wait_on(c, 0); }
N(Object_waitJI) { wait_on(c, c->args[0].j + (c->args[1].i ? 1 : 0)); }
N(Object_notify) { if (!mg_dvm_monitor_notify(c->self, false)) not_owner("notify"); }
N(Object_notifyAll) { if (!mg_dvm_monitor_notify(c->self, true)) not_owner("notifyAll"); }

/* ------------------------------------------------------------------- String */

N(String_charAt)
{
    i32 i = c->args[0].i;
    if (i < 0 || i >= c->self->u.str.len16) mg_dvm_throw("java/lang/StringIndexOutOfBoundsException", "length=%d; index=%d", (int)c->self->u.str.len16, (int)i);
    else c->ret.c = c->self->u.str.u16[i];
}

N(String_compareTo)
{
    mg_jobj *o = c->args[0].l;
    const u16 *a = c->self->u.str.u16, *b;
    i32 na = c->self->u.str.len16, nb, i;
    if (!o) {
        npe("compareTo(null)");
        return;
    }
    b = o->u.str.u16;
    nb = o->u.str.len16;
    for (i = 0; i < na && i < nb; i++)
        if (a[i] != b[i]) {
            c->ret.i = (i32)a[i] - (i32)b[i];
            return;
        }
    c->ret.i = na - nb;
}

N(String_concat)
{
    mg_jobj *o = c->args[0].l, *s;
    i32 na = c->self->u.str.len16;
    if (!o) {
        npe("concat(null)");
        return;
    }
    if (!o->u.str.len16) {
        c->ret.l = c->self;
        return;
    }
    s = mg_dvm_alloc_string(NULL, na + o->u.str.len16);
    if (!s) return;
    memcpy(s->u.str.u16, c->self->u.str.u16, (size_t)na * 2);
    memcpy(s->u.str.u16 + na, o->u.str.u16, (size_t)o->u.str.len16 * 2);
    c->ret.l = s;
}

N(String_doReplace)
{
    mg_jobj *s = mg_dvm_alloc_string(c->self->u.str.u16, c->self->u.str.len16);
    i32 i;
    if (!s) return;
    for (i = 0; i < s->u.str.len16; i++)
        if (s->u.str.u16[i] == c->args[0].c) s->u.str.u16[i] = c->args[1].c;
    c->ret.l = s;
}

N(String_fastSubstring) { c->ret.l = mg_dvm_alloc_string(c->self->u.str.u16 + c->args[0].i, c->args[1].i); }

N(String_getCharsNoCheck)
{
    i32 start = c->args[0].i, end = c->args[1].i;
    mg_jobj *buf = c->args[2].l;
    memcpy((u16 *)buf->u.arr.data + c->args[3].i, c->self->u.str.u16 + start, (size_t)(end - start) * 2);
}

N(String_intern) { c->ret.l = mg_dvm_intern_string(c->self); }

N(String_toCharArray)
{
    mg_jobj *a = mg_dvm_new_array("[C", c->self->u.str.len16);
    if (a) memcpy(a->u.arr.data, c->self->u.str.u16, (size_t)c->self->u.str.len16 * 2);
    c->ret.l = a;
}

/* newStringFromBytes(byte[] data, int high, int offset, int count): each byte a character, under the same high byte. */
N(StringFactory_newStringFromBytes)
{
    mg_jobj *data = c->args[0].l, *s;
    i32 high = c->args[1].i, offset = c->args[2].i, count = c->args[3].i, i;
    if (!data) {
        npe("data == null");
        return;
    }
    if (offset < 0 || count < 0 || offset > (i32)data->u.arr.len - count) {
        mg_dvm_throw("java/lang/StringIndexOutOfBoundsException", "length=%d; regionStart=%d; regionLength=%d", (int)data->u.arr.len, (int)offset, (int)count);
        return;
    }
    s = mg_dvm_alloc_string(NULL, count);
    for (i = 0; s && i < count; i++) s->u.str.u16[i] = (u16)((high << 8) | ((u8 *)data->u.arr.data)[offset + i]);
    c->ret.l = s;
}

/* newStringFromChars(int offset, int count, char[] data): the caller has checked the range. */
N(StringFactory_newStringFromChars) { c->ret.l = mg_dvm_alloc_string((const u16 *)c->args[2].l->u.arr.data + c->args[0].i, c->args[1].i); }

N(StringFactory_newStringFromString)
{
    mg_jobj *o = c->args[0].l;
    if (!o) npe("toCopy == null");
    else c->ret.l = mg_dvm_alloc_string(o->u.str.u16, o->u.str.len16);
}

/* ------------------------------------------------ libcore.util.CharsetUtils */

static void bytes_to_chars(mg_jcall *c)
{
    const u8 *from = (const u8 *)c->args[0].l->u.arr.data + c->args[1].i;
    u16 *to = (u16 *)c->args[3].l->u.arr.data;
    i32 n = c->args[2].i, i;
    bool ascii = !strcmp(c->name, "asciiBytesToChars");
    for (i = 0; i < n; i++) to[i] = ascii && from[i] > 0x7F ? 0xFFFD : from[i];
}

static void chars_to_bytes(mg_jcall *c)
{
    const u16 *from = c->args[0].l->u.str.u16 + c->args[1].i;
    i32 n = c->args[2].i, i;
    u16 limit = !strcmp(c->name, "toAsciiBytes") ? 0x7F : 0xFF;
    mg_jobj *a = mg_dvm_new_array("[B", n);
    for (i = 0; a && i < n; i++) ((u8 *)a->u.arr.data)[i] = from[i] > limit ? '?' : (u8)from[i];
    c->ret.l = a;
}

N(CharsetUtils_toUtf8Bytes)
{
    const u16 *u = c->args[0].l->u.str.u16 + c->args[1].i;
    i32 n = c->args[2].i, i;
    size_t k = 0;
    u8 *out = (u8 *)mg_alloc((size_t)n * 3 + 4);
    mg_jobj *a;
    for (i = 0; i < n; i++) {
        u32 ch = u[i];
        if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            ch = 0x10000 + ((ch - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
            out[k++] = (u8)(0xF0 | (ch >> 18));
            out[k++] = (u8)(0x80 | ((ch >> 12) & 0x3F));
            out[k++] = (u8)(0x80 | ((ch >> 6) & 0x3F));
            out[k++] = (u8)(0x80 | (ch & 0x3F));
        } else if (ch >= 0xD800 && ch < 0xE000) {
            out[k++] = '?';                                      /* half a pair: Android writes a question mark */
        } else if (ch < 0x80) {
            out[k++] = (u8)ch;
        } else if (ch < 0x800) {
            out[k++] = (u8)(0xC0 | (ch >> 6));
            out[k++] = (u8)(0x80 | (ch & 0x3F));
        } else {
            out[k++] = (u8)(0xE0 | (ch >> 12));
            out[k++] = (u8)(0x80 | ((ch >> 6) & 0x3F));
            out[k++] = (u8)(0x80 | (ch & 0x3F));
        }
    }
    a = mg_dvm_new_array("[B", (i32)k);
    if (a) memcpy(a->u.arr.data, out, k);
    free(out);
    c->ret.l = a;
}

/* ------------------------------------------------------------------- System */

static void arraycopy_unchecked(mg_jcall *c)
{
    mg_jobj *src = c->args[0].l, *dst = c->args[2].l;
    memmove((u8 *)dst->u.arr.data + (size_t)c->args[3].i * dst->u.arr.esz, (const u8 *)src->u.arr.data + (size_t)c->args[1].i * src->u.arr.esz,
            (size_t)c->args[4].i * src->u.arr.esz);
}

N(System_arraycopy) { mg_dvm_arraycopy(c->args[0].l, c->args[1].i, c->args[2].l, c->args[3].i, c->args[4].i); }

/* ---------------------------------------------------------------- Throwable */

N(Throwable_nativeFillInStackTrace) { c->ret.l = mg_dvm_capture_stack(); }

/* The frames kept by nativeFillInStackTrace as StackTraceElements: class, method, file and line. */
static mg_jobj *stack_elements(mg_jobj *trace)
{
    mg_jobj *out;
    const i64 *v;
    i32 n, i;
    if (!trace || trace->kind != MG_J_PRIM_ARRAY || trace->u.arr.etype != 'J') return mg_dvm_new_array("[Ljava/lang/StackTraceElement;", 0);
    v = (const i64 *)trace->u.arr.data;
    n = (i32)(trace->u.arr.len / 2);
    out = mg_dvm_new_array("[Ljava/lang/StackTraceElement;", n);
    for (i = 0; out && i < n; i++) {
        const mg_dvm_method *m = (const mg_dvm_method *)(mg_addr)v[2 * i];
        const char *source = mg_dvm_dex_source(m->cls);
        mg_jvalue a[4];
        memset(a, 0, sizeof(a));
        a[0].l = dotted(m->cls->name);
        a[1].l = mg_dvm_new_string(m->name);
        a[2].l = source ? mg_dvm_new_string(source) : NULL;
        /* -2 is how a StackTraceElement says "a native method". */
        a[3].i = (m->flags & DVM_ACC_NATIVE) ? -2 : v[2 * i + 1] >= 0 ? mg_dvm_dex_line(m, (u32)v[2 * i + 1]) : -1;
        out->u.oarr.v[i] = mg_dvm_new("java/lang/StackTraceElement", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V", a);
        if (!out->u.oarr.v[i]) return NULL;
    }
    return out;
}

N(Throwable_nativeGetStackTrace) { c->ret.l = stack_elements(c->args[0].l); }

/* ------------------------------------------------------------------ VMStack */

N(VMStack_getStackClass2)
{
    /*
     * "The class of the caller's caller's caller" (art/runtime/native/dalvik_system_VMStack.cc walks three frames up from the
     * native method's own): asked by Reflection.getCallerClass() for the method that called it, to know who that one's caller is.
     */
    mg_dvm_frame *f = mg_dvm_self()->top;
    int up;
    for (up = 0; f && up < 3; up++) f = f->prev;
    c->ret.l = f ? mg_dvm_mirror(f->m->cls) : NULL;
}

/* The defining loader as Java sees it; boot classes have none. */
static mg_jobj *loader_of(const mg_dvm_class *k) {
    if (k->scope) return mg_dvm_scope_loader(k->scope);
    if (k->dex && k->dex->loader) return k->dex->loader;
    return mg_dvm_class_is_app(k) ? g_dvm.app_loader : NULL;
}

N(VMStack_getCallingClassLoader)
{
    /*
     * "The defining class loader of the caller's caller" (dalvik_system_VMStack.cc): two frames up from this native method's
     * own. System.loadLibrary asks it to know whose library is wanted, Class.forName whose classes.
     */
    mg_dvm_frame *f = mg_dvm_self()->top;
    int up;
    for (up = 0; f && up < 2; up++) f = f->prev;
    c->ret.l = f ? loader_of(f->m->cls) : NULL;
}

N(VMStack_getClosestUserClassLoader)
{
    /* The first loader up the stack that is not the boot class path's: the app's, if any code of the app is running. */
    mg_dvm_frame *f;
    for (f = mg_dvm_self()->top; f; f = f->prev) {
        if (loader_of(f->m->cls)) {
            c->ret.l = loader_of(f->m->cls);
            return;
        }
    }
    c->ret.l = NULL;
}

N(VMStack_getThreadStackTrace)
{
    mg_jobj *peer = c->args[0].l;
    /* Only of the calling thread: another's frames cannot be read while it runs. */
    c->ret.l = stack_elements(peer && peer == mg_dvm_self()->peer ? mg_dvm_capture_stack() : NULL);
}

/* ---------------------------------------------------------------- VMRuntime */

static const char *const k_boot_jars[] = { "core-oj", "core-libart", "conscrypt", "okhttp", "bouncycastle", "apache-xml" };

/*
 * The jars of the boot class path, as libcore knows them: /system/framework/core-oj.jar and the rest. Their bytecode is not read
 * from them (it is in the DEX files beside them); what libcore opens them for is what else they hold -- ICU4J's own
 * configuration is a file in core-libart.jar. Only those that are there are named.
 */
static int boot_jars(char out[][64], int cap)
{
    mg_stat st;
    size_t i;
    int n = 0;
    for (i = 0; i < MG_COUNT(k_boot_jars) && n < cap; i++) {
        char *host = mg_sprintf("%s/framework/%s.jar", g_dvm.android_root ? g_dvm.android_root : ".", k_boot_jars[i]);
        if (mg_path_stat(host, &st, NULL) && !st.is_dir) snprintf(out[n++], 64, "/system/framework/%s.jar", k_boot_jars[i]);
        free(host);
    }
    return n;
}

/* ------------------------------------------------------ the security providers */

#include "mg_dvm_art_dex.h"

/*
 * Android's list of security providers is conscrypt first ("AndroidOpenSSL":
 * digests, random numbers, ciphers, all over BoringSSL in its native
 * library), then libcore's own for certificate paths, then BouncyCastle, then
 * conscrypt's for TLS. libcore makes all of them when the first thing asks
 * for any, and takes one that cannot be made for a broken system: an
 * AssertionError in the static initialiser of sun.security.jca.Providers,
 * after which no MessageDigest, no SecureRandom and no UUID.randomUUID() can
 * be had for the life of the process.
 *
 * conscrypt's native library is not built. Until it is in the root, the
 * list is changed just before that initialiser runs: in conscrypt's first
 * place a provider of the runtime's own under conscrypt's name
 * (art/RuntimeProvider.smali says why the name: random bytes from the system,
 * BouncyCastle's digests, and SSLContexts whose connections fail with an
 * SSLException -- NoTlsContext.smali), then libcore's, then BouncyCastle, which has
 * MACs, ciphers and key derivation in Java. conscrypt's TLS provider is left
 * out, and with it the two properties that name its socket factories: there
 * is no TLS, and asking for it says so (NoSuchAlgorithmException) instead of
 * dying in a class that cannot start.
 */
static const char *g_providers_name, *g_timezone_name, *g_vmruntime_name, *g_conscrypt_platform_name;

/* Android 9 already negotiates ALPN, but its Platform bridge predates the
 * public SSLParameters API. Keep every original transfer (SNI, cipher order,
 * endpoint identification, etc.) and also transfer application protocols.
 * The additional public methods live in megler-libcore.dex. */
static mg_dvm_method g_ssl_parameters_original[4];

static void ssl_parameters_bridge(mg_jcall *c, int index)
{
    mg_jvalue protocols;
    mg_jobj *from, *to;
    if (!mg_dvm_invoke(&g_ssl_parameters_original[index], NULL, c->args, &c->ret)) return;
    from = c->args[(index & 1) ? 1 : 0].l;
    to = c->args[(index & 1) ? 0 : 1].l;
    protocols.j = 0;
    if (mg_dvm_call_virtual(from, "getApplicationProtocols", "()[Ljava/lang/String;", NULL, &protocols))
        mg_dvm_call_virtual(to, "setApplicationProtocols", "([Ljava/lang/String;)V", &protocols, NULL);
}

N(Platform_setSocketParameters) { ssl_parameters_bridge(c, 0); }
N(Platform_getSocketParameters) { ssl_parameters_bridge(c, 1); }
N(Platform_setEngineParameters) { ssl_parameters_bridge(c, 2); }
N(Platform_getEngineParameters) { ssl_parameters_bridge(c, 3); }

static void extend_ssl_parameters(mg_dvm_class *cls)
{
    static const mg_jhle_fn functions[] = {
        Platform_setSocketParameters, Platform_getSocketParameters,
        Platform_setEngineParameters, Platform_getEngineParameters
    };
    static const char *const signatures[] = {
        "(Ljavax/net/ssl/SSLParameters;Lcom/android/org/conscrypt/SSLParametersImpl;Lcom/android/org/conscrypt/AbstractConscryptSocket;)V",
        "(Ljavax/net/ssl/SSLParameters;Lcom/android/org/conscrypt/SSLParametersImpl;Lcom/android/org/conscrypt/ConscryptEngine;)V"
    };
    int i;
    mg_dvm_class *params = mg_dvm_find_class("javax/net/ssl/SSLParameters");
    if (!params || !mg_dvm_find_method(params, "getApplicationProtocols", "()[Ljava/lang/String;", false)) return;
    for (i = 0; i < 4; ++i) {
        mg_dvm_method *m = mg_dvm_find_method(cls, (i & 1) ? "getSSLParameters" : "setSSLParameters", signatures[i / 2], true);
        if (!m || !m->insns) continue;
        g_ssl_parameters_original[i] = *m;
        m->insns = NULL;
        m->ninsns = 0;
        m->flags |= DVM_ACC_NATIVE;
        m->native = functions[i];
        m->bound = true;
    }
}

static void set_security(mg_jobj *props, const char *key, const char *value)
{
    mg_jvalue a[2];
    memset(a, 0, sizeof(a));
    a[0].l = mg_dvm_new_string(key);
    a[1].l = value ? mg_dvm_new_string(value) : NULL;
    if (value) mg_dvm_call_virtual(props, "setProperty", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/Object;", a, NULL);
    else mg_dvm_call_virtual(props, "remove", "(Ljava/lang/Object;)Ljava/lang/Object;", a, NULL);
}

static void give_timezone_getter(void);

/*
 * VMRuntime.runFinalization(timeout) is Java in libcore: it waits on FinalizerReference's list for the FinalizerDaemon, which
 * Android's runtime fills as it makes every finalizable object (FinalizerReference.add) and this one does not -- its
 * collector keeps its own list and finalizes on its reference thread (mg_dvm_gc.c). So the method is made a native one,
 * before VMRuntime is first used, that waits for that thread instead. Without it, System.runFinalization() threw
 * "AssertionError: newly-created live Sentinel not on list!".
 */
N(VMRuntime_runFinalization)
{
    (void)c;
    mg_dvm_gc_run_finalization();
}

static void own_run_finalization(mg_dvm_class *c)
{
    mg_dvm_method *m = mg_dvm_find_method(c, "runFinalization", "(J)V", true);
    if (!m) return;
    m->insns = NULL;
    m->ninsns = 0;
    m->flags |= DVM_ACC_NATIVE;
    m->native = VMRuntime_runFinalization;
    m->bound = true;
}

static void before_init(mg_dvm_class *c)
{
    mg_jvalue props;
    if (c->name == g_conscrypt_platform_name) {
        extend_ssl_parameters(c);
        return;
    }
    if (c->name == g_timezone_name) {
        give_timezone_getter();
        return;
    }
    if (c->name == g_vmruntime_name) {
        own_run_finalization(c);
        return;
    }
    if (c->name != g_providers_name || g_dvm.conscrypt) return;
    props.j = 0;
    if (!mg_dvm_get_static("java/security/Security", "props", &props) || !props.l) {
        mg_dvm_clear_pending();
        return;
    }
    set_security(props.l, "security.provider.1", "megler.security.RuntimeProvider");
    set_security(props.l, "security.provider.2", "sun.security.provider.CertPathProvider");
    set_security(props.l, "security.provider.3", "com.android.org.bouncycastle.jce.provider.BouncyCastleProvider");
    set_security(props.l, "security.provider.4", NULL);
    set_security(props.l, "ssl.SocketFactory.provider", NULL);
    set_security(props.l, "ssl.ServerSocketFactory.provider", NULL);
    mg_dvm_clear_pending();
    mg_info("java: conscrypt's native library is not in the Android root: digests are BouncyCastle's, random numbers the system's, and there is no TLS"
            " (a connection that asks for it fails with an SSLException)");
}

N(SystemRandom_engineNextBytes)
{
    mg_jobj *bytes = c->args[0].l;
    if (!bytes) mg_dvm_throw("java/lang/NullPointerException", "bytes == null");
    else mg_random(bytes->u.arr.data, (size_t)bytes->u.arr.len);
}

/* ------------------------------------------------------------ the time zone */

/*
 * java.util.TimeZone's default zone is the phone's. libcore asks for its name in three places (TimeZone.getDefaultRef):
 * a TimezoneGetter if the runtime gave it one, the file /etc/timezone, and failing both it says GMT. Android gives it the
 * getter -- the framework's start installs one that reads the property persist.sys.timezone -- and that is what is done
 * here, just before TimeZone's static initialiser runs, which is before anything can ask: the getter is the runtime's own
 * class (art/PhoneTimeZone.smali) and its one method answers what the shell said (mg_timezone_id). With nothing said it
 * answers null and libcore goes on to GMT, as it did when there was no getter.
 *
 * A name tzdata does not have is GMT too -- TimeZone.getTimeZone never fails -- and nothing would tell. So the first time
 * the name is asked for it is tried, and a zone that came out as GMT without being asked for by that name is said in the
 * log. (TimeZone.getTimeZone is called from inside getDefaultRef here; both lock the class TimeZone, on the same thread.)
 */
static void give_timezone_getter(void)
{
    mg_jvalue arg;
    arg.j = 0;
    arg.l = mg_dvm_new("megler/runtime/PhoneTimeZone", "()V", NULL);
    if (!arg.l || !mg_dvm_call_static("org/apache/harmony/luni/internal/util/TimezoneGetter", "setInstance", "(Lorg/apache/harmony/luni/internal/util/TimezoneGetter;)V", &arg, NULL)) {
        mg_warn("java: libcore was not given the phone's time zone (%s); an app's times are GMT", mg_dvm_pending() ? mg_dvm_class_of(mg_dvm_pending())->name : "no getter");
        mg_dvm_clear_pending();
    }
}

N(PhoneTimeZone_getId)
{
    static char tried[64];
    const char *id = mg_timezone_id();
    mg_jvalue arg, zone, name;
    c->ret.l = NULL;
    if (!id[0]) return;
    c->ret.l = mg_dvm_new_string(id);
    if (!strcmp(tried, id)) return;
    mg_strlcpy(tried, id, sizeof(tried));
    arg.j = zone.j = name.j = 0;
    arg.l = c->ret.l;
    if (mg_dvm_call_static("java/util/TimeZone", "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;", &arg, &zone) && zone.l
        && mg_dvm_call_virtual(zone.l, "getID", "()Ljava/lang/String;", NULL, &name) && name.l) {
        const char *got = mg_dvm_string_utf8(name.l);
        if (got && !strcmp(got, "GMT") && strcmp(id, "GMT") != 0)
            mg_warn("java: the phone's time zone is given as \"%s\", which the Android root's tzdata does not have: an app's times are GMT", id);
        else mg_info("java: the time zone is %s", got ? got : id);
    }
    mg_dvm_clear_pending();
}

/* ------------------------------------------------------------------ DexFile */

/*
 * dalvik.system.DexFile is how libcore's class loaders reach the runtime: a
 * PathClassLoader opens each file of its path (openDexFileNative, which
 * hands back a "cookie") and asks for classes out of it by name
 * (defineClassNative). There is one class path here and no loading through a
 * loader, so opening a file means putting its classes on that path, if they
 * are not there, and a class asked of any cookie is the class of that name
 * wherever it was added. The files the app was started with are on the path
 * before the app's loader is made; mg_dvm_dex_source_added says which they
 * are, by the names the app knows them under.
 */
#define MAX_SOURCES 32
static char *g_sources[MAX_SOURCES];
static volatile i32 g_nsources;

void mg_dvm_dex_source_added(const char *guest_path)
{
    i32 slot = mg_atomic_add(&g_nsources, 1) - 1;
    if (slot < MAX_SOURCES) g_sources[slot] = mg_strdup(guest_path);
    else mg_atomic_add(&g_nsources, -1);
}

N(DexFile_openDexFileNative)
{
    const char *source = mg_dvm_string_utf8(c->args[0].l);
    mg_jobj *cookie;
    char *host;
    size_t n;
    i32 i, known = g_nsources;
    bool there = false;
    if (!source) {
        mg_dvm_throw("java/lang/NullPointerException", "sourceName == null");
        return;
    }
    for (i = 0; i < known && !there; i++) there = !strcmp(g_sources[i], source);
    /* openDexFileNative(sourceName, outputName, flags, loader, elements): a loader other than the app's over a service's APK
       is that service's scope under another name, and the APK's classes stay off the app's path. */
    if (!there && c->args[3].l && c->args[3].l != g_dvm.app_loader && service_apk_scope(source)) {
        if (!alias_loader(c->args[3].l, service_apk_scope(source), source)) {
            mg_dvm_throw("java/io/IOException", "too many class loaders over service APKs (%s)", source);
            return;
        }
        there = true;
    }
    if (!there) {
        /* A file the app names itself (a second DEX file it unpacked, a plug-in): its classes join the path, after everything there. */
        u32 before = mg_dvm_dex_count(), j;
        host = mg_fs_host_path(source);
        n = strlen(source);
        if (host && n > 4 && !strcmp(source + n - 4, ".dex")) there = mg_dvm_add_dex_file(host);
        else if (host) there = mg_dvm_add_apk(host) > 0;
        free(host);
        if (!there) {
            mg_dvm_throw("java/io/IOException", "No original dex files found for dex location %s", source);
            return;
        }
        mg_dvm_dex_source_added(source);
        /* DexFile opened these classes for this loader. Class.getClassLoader (and its
           resource and native-library lookup) must point back to that loader. */
        for (j = before; j < mg_dvm_dex_count(); j++) {
            mg_dvm_dex *d = mg_dvm_dex_at(j);
            if (d && d->scope == 0) d->loader = c->args[3].l;
        }
    }
    /* The cookie is opaque to libcore: an array of one long, as Android's is an array of pointers. */
    cookie = mg_dvm_new_array("[J", 1);
    if (cookie) ((i64 *)cookie->u.arr.data)[0] = 1;
    c->ret.l = cookie;
}

/*
 * A DEX file handed over in memory: dalvik.system.InMemoryDexClassLoader (Android 8 and later), which is how a library
 * that carries Java code inside its native one puts it on the path -- JUCE does, and comScore in MX Player is built with
 * it; Facebook's Audience Network loads its own DEX so. Its bytes are copied (the runtime keeps a DEX's bytes for good,
 * and the buffer is the app's), and its classes join the one path like a file's that the app opens.
 */
static void in_memory_dex(mg_jcall *c, const u8 *bytes, i64 limit, i32 start, i32 end)
{
    static volatile i32 count;
    char label[48];
    mg_jobj *cookie;
    u8 *copy;
    size_t n;
    if (!bytes || start < 0 || end <= start || end > limit) {
        mg_dvm_throw("java/io/IOException", "no DEX file in the buffer given (%d to %d)", (int)start, (int)end);
        return;
    }
    n = (size_t)(end - start);
    copy = (u8 *)mg_alloc(n);
    memcpy(copy, bytes + start, n);
    snprintf(label, sizeof(label), "in-memory DEX %d", (int)mg_atomic_add(&count, 1));
    if (!mg_dvm_add_dex(copy, n, label)) {
        free(copy);
        mg_dvm_throw("java/io/IOException", "the buffer is not a DEX file this runtime can read (%s, %u bytes)", label, (unsigned)n);
        return;
    }
    cookie = mg_dvm_new_array("[J", 1);
    if (cookie) ((i64 *)cookie->u.arr.data)[0] = 1;
    c->ret.l = cookie;
}

/* createCookieWithArray(byte[] buf, int start, int end) */
N(DexFile_createCookieWithArray)
{
    mg_jobj *a = c->args[0].l;
    if (!a) {
        mg_dvm_throw("java/lang/NullPointerException", "buf == null");
        return;
    }
    in_memory_dex(c, (const u8 *)a->u.arr.data, a->u.arr.len, c->args[1].i, c->args[2].i);
}

/* createCookieWithDirectBuffer(ByteBuffer buf, int start, int end): a direct buffer, whose memory is at its address field. */
N(DexFile_createCookieWithDirectBuffer)
{
    mg_jvalue address, capacity;
    address.j = capacity.j = 0;
    if (!c->args[0].l || !mg_dvm_get_field(c->args[0].l, "address", &address) || !mg_dvm_get_field(c->args[0].l, "capacity", &capacity)) {
        mg_dvm_throw("java/io/IOException", "a ByteBuffer that is not a direct one");
        return;
    }
    in_memory_dex(c, (const u8 *)(mg_addr)address.j, capacity.i, c->args[1].i, c->args[2].i);
}

N(DexFile_defineClassNative)
{
    const char *name = mg_dvm_string_utf8(c->args[0].l);
    char internal[400];
    mg_dvm_class *k;
    size_t i;
    int scope = mg_dvm_loader_scope(c->args[1].l); /* defineClassNative(name, loader, cookie, dexFile) */
    if (!name || strlen(name) >= sizeof(internal)) return;
    for (i = 0; name[i]; i++) internal[i] = name[i] == '.' ? '/' : name[i];
    internal[i] = 0;
    /* A class that is not there is null and no exception: the loader goes on to its next file, and throws ClassNotFoundException itself. */
    if (!mg_dvm_class_exists_scope(internal, scope)) return;
    k = mg_dvm_find_class_scope(internal, scope);
    c->ret.l = k ? mg_dvm_mirror(k) : NULL;
}

/*
 * VMClassLoader.findLoadedClass(loader, name). There is one class path, which ClassLoader.loadClass reaches through its parent,
 * so for a loader of that path the answer is null, as it always was. A loader of a scope of its own (another name of a
 * service's) has all the classes of its scope at once, the platform's among them: ART too finds a class of a loader's own DEX
 * files here, without asking the parent. The parent here would be the client's path, where a class of the same name may be
 * the client's.
 */
N(VMClassLoader_findLoadedClass)
{
    const char *given = c->args[1].l ? mg_dvm_string_utf8(c->args[1].l) : NULL;
    int scope = mg_dvm_loader_scope(c->args[0].l);
    char internal[400];
    mg_dvm_class *k;
    size_t i;
    if (!scope || !given || strlen(given) >= sizeof(internal) || strchr(given, '/')) return;
    for (i = 0; given[i]; i++) internal[i] = given[i] == '.' ? '/' : given[i];
    internal[i] = 0;
    if (!mg_dvm_class_exists_scope(internal, scope)) return;
    k = mg_dvm_find_class_scope(internal, scope);
    c->ret.l = k ? mg_dvm_mirror(k) : NULL;
}

/* getClassNameList(cookie): the classes of the app's own files (the cookie does not say which file; they are one path). */
N(DexFile_getClassNameList)
{
    u32 total = mg_dvm_defined_count(), i, n = 0;
    mg_jobj *out;
    for (i = 0; i < total; i++) {
        mg_dvm_dex *d = NULL;
        mg_dvm_defined_name(i, &d);
        if (d && d->app) n++;
    }
    out = mg_dvm_new_array("[Ljava/lang/String;", (i32)n);
    for (i = 0, n = 0; out && i < total; i++) {
        mg_dvm_dex *d = NULL;
        const char *name = mg_dvm_defined_name(i, &d);
        if (d && d->app) out->u.oarr.v[n++] = dotted(name);
    }
    c->ret.l = out;
}

N(DexFile_true) { c->ret.z = 1; }

N(VMRuntime_bootClassPath)
{
    char jars[8][64], text[600];
    size_t k = 0;
    int n = boot_jars(jars, 8), i;
    text[0] = 0;
    for (i = 0; i < n; i++) k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s", i ? ":" : "", jars[i]);
    c->ret.l = mg_dvm_new_string(text);
}

N(VMClassLoader_getBootClassPathEntries)
{
    char jars[8][64];
    int n = boot_jars(jars, 8), i;
    mg_jobj *a = mg_dvm_new_array("[Ljava/lang/String;", n);
    for (i = 0; a && i < n; i++) a->u.oarr.v[i] = mg_dvm_new_string(jars[i]);
    c->ret.l = a;
}

N(VMRuntime_classPath) { c->ret.l = mg_dvm_new_string("."); }
N(VMRuntime_vmVersion) { c->ret.l = mg_dvm_new_string("2.1.0"); }
N(VMRuntime_vmLibrary) { c->ret.l = mg_dvm_new_string("libart.so"); }
N(VMRuntime_instructionSet) { c->ret.l = mg_dvm_new_string("arm"); }
/*
 * The runtime's command line, -D properties libcore applies after its own (System.initUnchangeableSystemProperties): user.locale,
 * the phone's language and region, which Locale.getDefault() is made of first (mg_locale_tag). Here and not in the special
 * properties, which are libopenjdk.so's where it runs.
 */
N(VMRuntime_properties)
{
    char locale[64];
    mg_jobj *a = mg_dvm_new_array("[Ljava/lang/String;", 1);
    snprintf(locale, sizeof(locale), "user.locale=%s", mg_locale_tag());
    if (a) a->u.oarr.v[0] = mg_dvm_new_string(locale);
    c->ret.l = a;
}
N(VMRuntime_getTargetHeapUtilization) { c->ret.f = 0.75f; }

/* newNonMovableArray and newUnpaddedArray: an array of the class's elements. No array moves here, and none is padded. */
N(VMRuntime_newArray)
{
    mg_dvm_class *e = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    mg_dvm_class *a;
    char *name;
    if (!e) {
        npe("componentType == null");
        return;
    }
    name = e->prim ? mg_sprintf("[%c", e->prim) : e->name[0] == '[' ? mg_sprintf("[%s", e->name) : mg_sprintf("[L%s;", e->name);
    a = mg_dvm_find_class_scope(name, e->scope);
    free(name);
    if (a) c->ret.l = mg_dvm_alloc_array(a, c->args[1].i);
}

N(VMRuntime_addressOf)
{
    mg_jobj *a = c->args[0].l;
    if (!a || a->kind != MG_J_PRIM_ARRAY) mg_dvm_throw("java/lang/IllegalArgumentException", "not a primitive array");
    else c->ret.j = (i64)(mg_addr)a->u.arr.data;
}

/* A native method that is about something this runtime does not have -- a collector, a JIT, a debugger, a zygote: nothing, and zero. */
N(nothing) { (void)c; }

/* -------------------------------------------------------------------- Class */

N(Class_getNameNative)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self);
    c->ret.l = mg_dvm_intern_string(dotted(k->name));
}

N(Class_getPrimitiveClass)
{
    const char *name = mg_dvm_string_utf8(c->args[0].l);
    mg_dvm_class *k = name && mg_dvm_class_exists(name) && !strchr(name, '/') ? mg_dvm_find_class(name) : NULL;
    if (k && k->prim) c->ret.l = mg_dvm_mirror(k);
    else if (!mg_dvm_pending()) mg_dvm_throw("java/lang/ClassNotFoundException", "%s", name ? name : "null");
}

/* classForName: service loaders have their own namespace; existing loaders retain the app path. */
N(Class_classForName)
{
    const char *given = mg_dvm_string_utf8(c->args[0].l);
    mg_dvm_class *k;
    int scope = mg_dvm_loader_scope(c->args[2].l);
    char *name, *p;
    if (!given) {
        npe("name == null");
        return;
    }
    if (scope) {
        mg_jvalue out = {0}, args[2] = {{0}, {0}};
        args[0].l = c->args[0].l;
        if (mg_dvm_call_virtual(c->args[2].l, "loadClass", "(Ljava/lang/String;Z)Ljava/lang/Class;", args, &out)) {
            k = mg_dvm_mirror_class(out.l);
            if (k && (!c->args[1].z || mg_dvm_ensure_init(k))) c->ret.l = out.l;
        }
        return;
    }
    name = mg_strdup(given);
    for (p = name; *p; p++)
        if (*p == '.') *p = '/';
    /* A name with a slash in it is not a Java name, and a primitive's name is not a class to be looked up. */
    if (strchr(given, '/') || !mg_dvm_class_exists_scope(name, scope) || (name[0] != '[' && !strchr(name, '/') && mg_dvm_find_class_scope(name, scope) && mg_dvm_find_class_scope(name, scope)->prim)) {
        mg_dvm_clear_pending();
        mg_dvm_throw("java/lang/ClassNotFoundException", "%s", given);
    } else {
        k = mg_dvm_find_class_scope(name, scope);
        if (!k) {
            /* Named and not to be had: ClassNotFoundException, with what stopped it as the cause Java code looks for. */
            mg_jvalue a[2];
            mg_jobj *e;
            a[0].j = a[1].j = 0;
            a[1].l = mg_dvm_pending();
            mg_dvm_clear_pending();
            a[0].l = mg_dvm_new_string(given);
            e = mg_dvm_new("java/lang/ClassNotFoundException", "(Ljava/lang/String;Ljava/lang/Throwable;)V", a);
            if (e) mg_dvm_throw_object(e);
        } else if (!c->args[1].z || mg_dvm_ensure_init(k)) {
            c->ret.l = mg_dvm_mirror(k);
        }
    }
    free(name);
}

N(Class_getInterfacesInternal)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self);
    mg_jobj *a;
    int i;
    if (!k->nifaces) return;                                     /* null for none: the caller makes the empty array */
    a = mg_dvm_new_array("[Ljava/lang/Class;", k->nifaces);
    for (i = 0; a && i < k->nifaces; i++) a->u.oarr.v[i] = mg_dvm_mirror(k->ifaces[i]);
    c->ret.l = a;
}

N(Class_newInstance)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self);
    mg_dvm_method *ctor = mg_dvm_find_method(k, "<init>", "()V", false);
    mg_jobj *o;
    if (k->prim || k->name[0] == '[' || (k->flags & (DVM_ACC_INTERFACE | DVM_ACC_ABSTRACT)) || !ctor) {
        mg_dvm_throw("java/lang/InstantiationException", "%s%s", k->name, ctor ? "" : " has no zero argument constructor");
        return;
    }
    if (!mg_dvm_ensure_init(k)) return;
    o = mg_dvm_alloc_object(k);
    if (o && mg_dvm_invoke(ctor, o, NULL, NULL)) c->ret.l = o;
}

N(Class_getInnerClassFlags) { c->ret.i = c->args[0].i; }      /* what the class file itself says: the InnerClass annotation is not read */
N(Class_false) { c->ret.z = 0; }
N(Class_null) { c->ret.l = NULL; }

/*
 * Natives libcore declares and Android's own libraries do not have either: on Android they are an UnsatisfiedLinkError
 * for whoever reaches them. Two can be reached by ordinary code, and get the answer that is true here instead.
 * Runtime.runFinalization0: nothing is collected, so no finalizer waits. Package.getSystemPackage0 and
 * getSystemPackages0, which Package.getPackage and getPackages ask when the caller is a class of the boot class path:
 * no package of the system's is known by a file it came from.
 */
/* runFinalization: until the objects the last collections found to finalize have been finalized (mg_dvm_gc.c's reference thread). */
N(Runtime_runFinalization0)
{
    (void)c;
    mg_dvm_gc_run_finalization();
}
/* Runtime.gc(), which System.gc() calls: a collection, as ART does one. */
N(Runtime_gc)
{
    (void)c;
    mg_dvm_gc("System.gc", NULL);
}
/* Native memory a Java object keeps alive (NativeAllocationRegistry registers it so): it counts toward the next collection. */
N(VMRuntime_registerNativeAllocation) { mg_dvm_gc_native(c->args[0].i); }
N(VMRuntime_registerNativeFree) { mg_dvm_gc_native(-(i64)c->args[0].i); }

/*
 * UNIXProcess.forkAndExec, which is Runtime.exec and ProcessBuilder.start. An app's process cannot start another here,
 * and there is no /system/bin to start anything from: no sh, no getprop, no su. The answer is the one Android gives for a
 * program that is not on the phone, ENOENT, which libcore turns into "IOException: Cannot run program "getprop": error=2,
 * No such file or directory" -- what an app that probes for su already expects and catches. (libcore's own native would try
 * to fork, and say "error=38, Function not implemented", which no Android phone ever says.)
 */
N(UNIXProcess_forkAndExec)
{
    mg_dvm_throw("java/io/IOException", "error=2, No such file or directory");
    c->ret.i = -1;
}
N(Package_getSystemPackages0) { c->ret.l = mg_dvm_new_array("[Ljava/lang/String;", 0); }

/* ---------------------------------------------------------------- the rest */

N(AtomicLong_VMSupportsCS8) { c->ret.z = 1; }

N(Reference_getReferent) { c->ret.l = get(c->self, "referent").l; }
N(Reference_clearReferent) { set_obj(c->self, "referent", NULL); }

N(Array_createObjectArray)
{
    mg_dvm_class *e = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    mg_dvm_class *a;
    char *name;
    if (!e) {
        npe("componentType == null");
        return;
    }
    name = e->name[0] == '[' ? mg_sprintf("[%s", e->name) : mg_sprintf("[L%s;", e->name);
    a = mg_dvm_find_class_scope(name, e->scope);
    free(name);
    if (a) c->ret.l = mg_dvm_alloc_array(a, c->args[1].i);
}

/* An array of `dims` dimensions, each element of each made: new int[2][3] by another road. */
static mg_jobj *multi_array(mg_dvm_class *array_class, const i32 *dims, i32 ndims)
{
    mg_jobj *a = mg_dvm_alloc_array(array_class, dims[0]);
    i32 i;
    if (!a || ndims == 1) return a;
    for (i = 0; i < dims[0]; i++) {
        a->u.oarr.v[i] = multi_array(array_class->component, dims + 1, ndims - 1);
        if (!a->u.oarr.v[i]) return NULL;
    }
    return a;
}

N(Array_createMultiArray)
{
    mg_dvm_class *e = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    mg_jobj *dims = c->args[1].l;
    mg_dvm_class *a;
    char name[600];
    size_t k = 0;
    u32 i;
    if (!e || !dims || !dims->u.arr.len || dims->u.arr.len > 255) {
        mg_dvm_throw("java/lang/IllegalArgumentException", "Bad number of dimensions");
        return;
    }
    for (i = 0; i < dims->u.arr.len; i++) {
        if (((i32 *)dims->u.arr.data)[i] < 0) {
            mg_dvm_throw("java/lang/NegativeArraySizeException", "Dimension %u: %d", (unsigned)i, (int)((i32 *)dims->u.arr.data)[i]);
            return;
        }
        name[k++] = '[';
    }
    if (e->prim) snprintf(name + k, sizeof(name) - k, "%c", e->prim);
    else if (e->name[0] == '[') snprintf(name + k, sizeof(name) - k, "%s", e->name);
    else snprintf(name + k, sizeof(name) - k, "L%s;", e->name);
    a = mg_dvm_find_class_scope(name, e->scope);
    if (a) c->ret.l = multi_array(a, (const i32 *)dims->u.arr.data, (i32)dims->u.arr.len);
}

/* ------------------------------------------------------------------- Unsafe
 *
 * Fields by offset. An offset is counted in bytes from the object itself: an
 * instance field's is its slot's place after the header (what
 * java.lang.reflect.Field.getOffset says, mg_dvm_art_reflect.c), an array
 * element's is the header and then elements of the array's own size. A null
 * object means the offset is an address.
 */
static void *at(mg_jobj *o, i64 offset) { return o ? (void *)((u8 *)o + (size_t)offset) : (void *)(mg_addr)offset; }

#define UNSAFE_GET(Name, type, member)                                                      \
    N(Unsafe_get##Name) { c->ret.member = *(type *)at(c->args[0].l, c->args[1].j); }        \
    N(Unsafe_put##Name) { *(type *)at(c->args[0].l, c->args[1].j) = c->args[2].member; }    \
    N(Unsafe_get##Name##At) { c->ret.member = *(type *)(mg_addr)c->args[0].j; }             \
    N(Unsafe_put##Name##At) { *(type *)(mg_addr)c->args[0].j = c->args[1].member; }
UNSAFE_GET(Boolean, u8, z)
UNSAFE_GET(Byte, i8, b)
UNSAFE_GET(Char, u16, c)
UNSAFE_GET(Short, i16, s)
UNSAFE_GET(Int, i32, i)
UNSAFE_GET(Float, float, f)
UNSAFE_GET(Double, double, d)

/* 64 bits are read and written whole, also where the processor moves them as two halves: by the instruction pair that compares and swaps. */
static i64 load64(volatile i64 *p)
{
    i64 seen = *p;
    for (;;) {
        i64 now = mg_atomic_cas64(p, seen, seen);
        if (now == seen) return seen;
        seen = now;
    }
}

static void store64(volatile i64 *p, i64 v)
{
    i64 seen = *p;
    for (;;) {
        i64 now = mg_atomic_cas64(p, seen, v);
        if (now == seen) return;
        seen = now;
    }
}

N(Unsafe_getLong) { c->ret.j = load64((volatile i64 *)at(c->args[0].l, c->args[1].j)); }
N(Unsafe_putLong) { store64((volatile i64 *)at(c->args[0].l, c->args[1].j), c->args[2].j); }
N(Unsafe_getLongAt) { memcpy(&c->ret.j, (void *)(mg_addr)c->args[0].j, 8); }
N(Unsafe_putLongAt) { memcpy((void *)(mg_addr)c->args[0].j, &c->args[1].j, 8); }
N(Unsafe_getObject) { c->ret.l = *(mg_jobj **)at(c->args[0].l, c->args[1].j); }
N(Unsafe_putObject) { *(mg_jobj **)at(c->args[0].l, c->args[1].j) = c->args[2].l; }

N(Unsafe_compareAndSwapInt) { c->ret.z = mg_atomic_cas((volatile i32 *)at(c->args[0].l, c->args[1].j), c->args[2].i, c->args[3].i) == c->args[2].i; }
N(Unsafe_compareAndSwapLong) { c->ret.z = mg_atomic_cas64((volatile i64 *)at(c->args[0].l, c->args[1].j), c->args[2].j, c->args[3].j) == c->args[2].j; }
N(Unsafe_compareAndSwapObject)
{
    c->ret.z = mg_atomic_cas_ptr((void *volatile *)at(c->args[0].l, c->args[1].j), c->args[2].l, c->args[3].l) == (void *)c->args[2].l;
}

N(Unsafe_arrayBaseOffset) { c->ret.i = (i32)MG_DVM_HEADER; }
N(Unsafe_arrayIndexScale)
{
    mg_dvm_class *e = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    switch (e ? e->prim : 0) {
    case 'Z': case 'B': c->ret.i = 1; break;
    case 'C': case 'S': c->ret.i = 2; break;
    case 'I': case 'F': c->ret.i = 4; break;
    case 'J': case 'D': c->ret.i = 8; break;
    default: c->ret.i = (i32)sizeof(mg_jobj *); break;
    }
}
N(Unsafe_addressSize) { c->ret.i = (i32)sizeof(void *); }
N(Unsafe_pageSize) { c->ret.i = 4096; }
N(Unsafe_allocateMemory)
{
    void *p = c->args[0].j > 0 && c->args[0].j < 0x7FFF0000 ? malloc((size_t)c->args[0].j) : NULL;
    if (!p && c->args[0].j) mg_dvm_throw("java/lang/OutOfMemoryError", "%lld bytes outside the heap", (long long)c->args[0].j);
    c->ret.j = (i64)(mg_addr)p;
}
N(Unsafe_freeMemory) { free((void *)(mg_addr)c->args[0].j); }
N(Unsafe_setMemory) { memset((void *)(mg_addr)c->args[0].j, c->args[2].b, (size_t)c->args[1].j); }
N(Unsafe_copyMemory) { memmove((void *)(mg_addr)c->args[1].j, (void *)(mg_addr)c->args[0].j, (size_t)c->args[2].j); }
/* copyMemoryToPrimitiveArray(long src, Object dst, long dstOffset, long bytes): the offset is into the array's elements. */
N(Unsafe_copyMemoryToArray) { memcpy((u8 *)c->args[1].l->u.arr.data + (size_t)c->args[2].j, (void *)(mg_addr)c->args[0].j, (size_t)c->args[3].j); }
N(Unsafe_copyMemoryFromArray) { memcpy((void *)(mg_addr)c->args[2].j, (const u8 *)c->args[0].l->u.arr.data + (size_t)c->args[1].j, (size_t)c->args[3].j); }
N(Unsafe_allocateInstance)
{
    mg_dvm_class *k = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    if (k && mg_dvm_ensure_init(k)) c->ret.l = mg_dvm_alloc_object(k);
}

/* ------------------------------------------------------------------ the tables */

#define ARRAYCOPY(letter, name) { "java/lang/System", name, "([" letter "I[" letter "II)V", arraycopy_unchecked }
#define UNSAFE(Name, letter)                                                                        \
    { "sun/misc/Unsafe", "get" #Name, "(Ljava/lang/Object;J)" letter, Unsafe_get##Name },          \
    { "sun/misc/Unsafe", "put" #Name, "(Ljava/lang/Object;J" letter ")V", Unsafe_put##Name },      \
    { "sun/misc/Unsafe", "get" #Name, "(J)" letter, Unsafe_get##Name##At },                        \
    { "sun/misc/Unsafe", "put" #Name, "(J" letter ")V", Unsafe_put##Name##At }

/* Whole classes of methods that do nothing here: registered first, so that the table after it has the last word on any one of them. */
static const mg_jhle k_art_nothing[] = {
    { "dalvik/system/VMRuntime", NULL, NULL, nothing },
    { "dalvik/system/VMDebug", NULL, NULL, nothing },
    { "dalvik/system/ZygoteHooks", NULL, NULL, nothing },
    { "org/apache/harmony/dalvik/ddmc/DdmServer", NULL, NULL, nothing },
    { "org/apache/harmony/dalvik/ddmc/DdmVmInternal", NULL, NULL, nothing },
    { NULL, NULL, NULL, NULL },
};

static const mg_jhle k_art[] = {
    { "java/lang/Object", "identityHashCodeNative", NULL, Object_identityHashCodeNative },
    { "java/lang/Object", "internalClone", NULL, Object_internalClone },
    { "java/lang/Object", "wait", "()V", Object_wait },
    { "java/lang/Object", "wait", "(JI)V", Object_waitJI },
    { "java/lang/Object", "notify", NULL, Object_notify },
    { "java/lang/Object", "notifyAll", NULL, Object_notifyAll },

    { "java/lang/String", "charAt", NULL, String_charAt },
    { "java/lang/String", "compareTo", "(Ljava/lang/String;)I", String_compareTo },
    { "java/lang/String", "concat", NULL, String_concat },
    { "java/lang/String", "doReplace", NULL, String_doReplace },
    { "java/lang/String", "fastSubstring", NULL, String_fastSubstring },
    { "java/lang/String", "getCharsNoCheck", NULL, String_getCharsNoCheck },
    { "java/lang/String", "intern", NULL, String_intern },
    { "java/lang/String", "toCharArray", NULL, String_toCharArray },
    { "java/lang/StringFactory", "newStringFromBytes", "([BIII)Ljava/lang/String;", StringFactory_newStringFromBytes },
    { "java/lang/StringFactory", "newStringFromChars", "(II[C)Ljava/lang/String;", StringFactory_newStringFromChars },
    { "java/lang/StringFactory", "newStringFromString", "(Ljava/lang/String;)Ljava/lang/String;", StringFactory_newStringFromString },
    { "libcore/util/CharsetUtils", "asciiBytesToChars", NULL, bytes_to_chars },
    { "libcore/util/CharsetUtils", "isoLatin1BytesToChars", NULL, bytes_to_chars },
    { "libcore/util/CharsetUtils", "toAsciiBytes", NULL, chars_to_bytes },
    { "libcore/util/CharsetUtils", "toIsoLatin1Bytes", NULL, chars_to_bytes },
    { "libcore/util/CharsetUtils", "toUtf8Bytes", NULL, CharsetUtils_toUtf8Bytes },

    { "java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", System_arraycopy },
    ARRAYCOPY("C", "arraycopyCharUnchecked"), ARRAYCOPY("B", "arraycopyByteUnchecked"), ARRAYCOPY("S", "arraycopyShortUnchecked"),
    ARRAYCOPY("I", "arraycopyIntUnchecked"), ARRAYCOPY("J", "arraycopyLongUnchecked"), ARRAYCOPY("F", "arraycopyFloatUnchecked"),
    ARRAYCOPY("D", "arraycopyDoubleUnchecked"), ARRAYCOPY("Z", "arraycopyBooleanUnchecked"),

    { "java/lang/Thread", "currentThread", NULL, Thread_currentThread },
    { "java/lang/Thread", "nativeCreate", NULL, Thread_nativeCreate },
    { "java/lang/Thread", "interrupted", NULL, Thread_interrupted },
    { "java/lang/Thread", "isInterrupted", NULL, Thread_isInterrupted },
    { "java/lang/Thread", "nativeInterrupt", NULL, Thread_nativeInterrupt },
    { "java/lang/Thread", "nativeHoldsLock", NULL, Thread_nativeHoldsLock },
    { "java/lang/Thread", "nativeGetStatus", NULL, Thread_nativeGetStatus },
    { "java/lang/Thread", "nativeSetName", NULL, Thread_nothing },
    { "java/lang/Thread", "nativeSetPriority", NULL, Thread_nothing },
    { "java/lang/Thread", "sleep", "(Ljava/lang/Object;JI)V", Thread_sleep },
    { "java/lang/Thread", "yield", NULL, Thread_yield },

    { "java/lang/Throwable", "nativeFillInStackTrace", NULL, Throwable_nativeFillInStackTrace },
    { "java/lang/Throwable", "nativeGetStackTrace", NULL, Throwable_nativeGetStackTrace },
    { "dalvik/system/VMStack", "getStackClass2", NULL, VMStack_getStackClass2 },
    { "dalvik/system/VMStack", "getThreadStackTrace", NULL, VMStack_getThreadStackTrace },
    { "dalvik/system/VMStack", "getCallingClassLoader", NULL, VMStack_getCallingClassLoader },
    { "java/lang/Runtime", "runFinalization0", NULL, Runtime_runFinalization0 },
    { "java/lang/Runtime", "gc", NULL, Runtime_gc },
    { "dalvik/system/VMRuntime", "registerNativeAllocation", NULL, VMRuntime_registerNativeAllocation },
    { "dalvik/system/VMRuntime", "registerNativeFree", NULL, VMRuntime_registerNativeFree },
    { "java/lang/UNIXProcess", "forkAndExec", NULL, UNIXProcess_forkAndExec },
    { "java/lang/Package", "getSystemPackage0", NULL, Class_null },
    { "java/lang/Package", "getSystemPackages0", NULL, Package_getSystemPackages0 },
    { "dalvik/system/VMStack", "getClosestUserClassLoader", NULL, VMStack_getClosestUserClassLoader },
    { "dalvik/system/VMStack", "fillStackTraceElements", NULL, nothing },
    { "dalvik/system/VMStack", "getAnnotatedThreadStackTrace", NULL, Class_null },

    { "dalvik/system/VMRuntime", "bootClassPath", NULL, VMRuntime_bootClassPath },
    { "dalvik/system/VMRuntime", "classPath", NULL, VMRuntime_classPath },
    { "dalvik/system/VMRuntime", "vmVersion", NULL, VMRuntime_vmVersion },
    { "dalvik/system/VMRuntime", "vmLibrary", NULL, VMRuntime_vmLibrary },
    { "dalvik/system/VMRuntime", "vmInstructionSet", NULL, VMRuntime_instructionSet },
    { "dalvik/system/VMRuntime", "getCurrentInstructionSet", NULL, VMRuntime_instructionSet },
    { "dalvik/system/VMRuntime", "properties", NULL, VMRuntime_properties },
    { "dalvik/system/VMRuntime", "getTargetHeapUtilization", NULL, VMRuntime_getTargetHeapUtilization },
    { "dalvik/system/VMRuntime", "newNonMovableArray", NULL, VMRuntime_newArray },
    { "dalvik/system/VMRuntime", "newUnpaddedArray", NULL, VMRuntime_newArray },
    { "dalvik/system/VMRuntime", "addressOf", NULL, VMRuntime_addressOf },

    { "java/lang/Class", "getNameNative", NULL, Class_getNameNative },
    { "java/lang/Class", "getPrimitiveClass", NULL, Class_getPrimitiveClass },
    { "megler/ServiceClassLoader", "openApk", NULL, ServiceClassLoader_openApk },
    { "megler/ServiceClassLoader", "findClassNative", NULL, ServiceClassLoader_findClassNative },
    { "java/lang/Class", "classForName", NULL, Class_classForName },
    { "java/lang/Class", "getInterfacesInternal", NULL, Class_getInterfacesInternal },
    { "java/lang/Class", "newInstance", NULL, Class_newInstance },
    { "java/lang/Class", "getInnerClassFlags", NULL, Class_getInnerClassFlags },
    { "java/lang/Class", "isAnonymousClass", NULL, Class_false },
    { "java/lang/Class", "isDeclaredAnnotationPresent", NULL, Class_false },
    { "java/lang/Class", "getDeclaredAnnotation", NULL, Class_null },
    { "java/lang/Class", "getSignatureAnnotation", NULL, Class_null },
    { "java/lang/Class", "getEnclosingClass", NULL, Class_null },
    { "java/lang/Class", "getDeclaringClass", NULL, Class_null },
    { "java/lang/Class", "getEnclosingMethodNative", NULL, Class_null },
    { "java/lang/Class", "getEnclosingConstructorNative", NULL, Class_null },
    { "java/lang/Class", "getInnerClassName", NULL, Class_null },
    { "megler/security/SystemRandom", "engineNextBytes", NULL, SystemRandom_engineNextBytes },
    { "megler/runtime/PhoneTimeZone", "getId", NULL, PhoneTimeZone_getId },
    { "dalvik/system/DexFile", "openDexFileNative", NULL, DexFile_openDexFileNative },
    { "dalvik/system/DexFile", "createCookieWithArray", NULL, DexFile_createCookieWithArray },
    { "dalvik/system/DexFile", "createCookieWithDirectBuffer", NULL, DexFile_createCookieWithDirectBuffer },
    { "dalvik/system/DexFile", "defineClassNative", NULL, DexFile_defineClassNative },
    { "dalvik/system/DexFile", "getClassNameList", NULL, DexFile_getClassNameList },
    { "dalvik/system/DexFile", "closeDexFile", NULL, DexFile_true },
    /* Nothing is compiled ahead of time or optimised here: every other question about that is answered no, none, zero. */
    { "dalvik/system/DexFile", NULL, NULL, Class_null },
    { "java/lang/VMClassLoader", "findLoadedClass", NULL, VMClassLoader_findLoadedClass },
    { "java/lang/VMClassLoader", "getBootClassPathEntries", NULL, VMClassLoader_getBootClassPathEntries },

    { "java/util/concurrent/atomic/AtomicLong", "VMSupportsCS8", NULL, AtomicLong_VMSupportsCS8 },
    { "java/lang/ref/Reference", "getReferent", NULL, Reference_getReferent },
    { "java/lang/ref/Reference", "clearReferent", NULL, Reference_clearReferent },
    { "java/lang/ref/FinalizerReference", "getReferent", NULL, Reference_getReferent },
    { "java/lang/ref/FinalizerReference", "makeCircularListIfUnenqueued", NULL, Class_false },
    { "java/lang/reflect/Array", "createObjectArray", NULL, Array_createObjectArray },
    { "java/lang/reflect/Array", "createMultiArray", NULL, Array_createMultiArray },

    UNSAFE(Boolean, "Z"), UNSAFE(Byte, "B"), UNSAFE(Char, "C"), UNSAFE(Short, "S"), UNSAFE(Int, "I"), UNSAFE(Float, "F"), UNSAFE(Double, "D"),
    { "sun/misc/Unsafe", "getLong", "(Ljava/lang/Object;J)J", Unsafe_getLong },
    { "sun/misc/Unsafe", "putLong", "(Ljava/lang/Object;JJ)V", Unsafe_putLong },
    { "sun/misc/Unsafe", "getLong", "(J)J", Unsafe_getLongAt },
    { "sun/misc/Unsafe", "putLong", "(JJ)V", Unsafe_putLongAt },
    { "sun/misc/Unsafe", "getLongVolatile", NULL, Unsafe_getLong },
    { "sun/misc/Unsafe", "putLongVolatile", NULL, Unsafe_putLong },
    { "sun/misc/Unsafe", "putOrderedLong", NULL, Unsafe_putLong },
    { "sun/misc/Unsafe", "getIntVolatile", NULL, Unsafe_getInt },
    { "sun/misc/Unsafe", "putIntVolatile", NULL, Unsafe_putInt },
    { "sun/misc/Unsafe", "putOrderedInt", NULL, Unsafe_putInt },
    { "sun/misc/Unsafe", "getObject", NULL, Unsafe_getObject },
    { "sun/misc/Unsafe", "getObjectVolatile", NULL, Unsafe_getObject },
    { "sun/misc/Unsafe", "putObject", NULL, Unsafe_putObject },
    { "sun/misc/Unsafe", "putObjectVolatile", NULL, Unsafe_putObject },
    { "sun/misc/Unsafe", "putOrderedObject", NULL, Unsafe_putObject },
    { "sun/misc/Unsafe", "compareAndSwapInt", NULL, Unsafe_compareAndSwapInt },
    { "sun/misc/Unsafe", "compareAndSwapLong", NULL, Unsafe_compareAndSwapLong },
    { "sun/misc/Unsafe", "compareAndSwapObject", NULL, Unsafe_compareAndSwapObject },
    { "sun/misc/Unsafe", "getArrayBaseOffsetForComponentType", NULL, Unsafe_arrayBaseOffset },
    { "sun/misc/Unsafe", "getArrayIndexScaleForComponentType", NULL, Unsafe_arrayIndexScale },
    { "sun/misc/Unsafe", "addressSize", NULL, Unsafe_addressSize },
    { "sun/misc/Unsafe", "pageSize", NULL, Unsafe_pageSize },
    { "sun/misc/Unsafe", "allocateMemory", NULL, Unsafe_allocateMemory },
    { "sun/misc/Unsafe", "freeMemory", NULL, Unsafe_freeMemory },
    { "sun/misc/Unsafe", "setMemory", NULL, Unsafe_setMemory },
    { "sun/misc/Unsafe", "copyMemory", NULL, Unsafe_copyMemory },
    { "sun/misc/Unsafe", "copyMemoryToPrimitiveArray", NULL, Unsafe_copyMemoryToArray },
    { "sun/misc/Unsafe", "copyMemoryFromPrimitiveArray", NULL, Unsafe_copyMemoryFromArray },
    { "sun/misc/Unsafe", "allocateInstance", NULL, Unsafe_allocateInstance },
    { "sun/misc/Unsafe", "loadFence", NULL, nothing },
    { "sun/misc/Unsafe", "storeFence", NULL, nothing },
    { "sun/misc/Unsafe", "fullFence", NULL, nothing },
    { NULL, NULL, NULL, NULL },
};

bool mg_dvm_art_start(void)
{
    g_dvm.libcore = true;
    g_dvm.fill_mirror = fill_mirror;
    /* The runtime's own three classes, after libcore's on the boot class path (this is before the runtime counts as running: they are boot classes). */
    mg_dvm_add_dex(k_dvm_art_dex, k_dvm_art_dex_size, "megler-runtime.dex");
    g_providers_name = mg_dvm_intern("sun/security/jca/Providers");
    g_timezone_name = mg_dvm_intern("java/util/TimeZone");
    g_vmruntime_name = mg_dvm_intern("dalvik/system/VMRuntime");
    g_conscrypt_platform_name = mg_dvm_intern("com/android/org/conscrypt/Platform");
    g_dvm.before_init = before_init;
    mg_dvm_register(k_art_nothing);
    mg_dvm_register(k_art);
    mg_dvm_art_reflect_start();
    mg_dvm_art_annot_start();
    mg_dvm_art_host_start();
    g_dvm.c_string_factory = mg_dvm_find_class("java/lang/StringFactory");
    if (!g_dvm.c_string_factory) {
        mg_err("dvm: the boot class path has java.lang.StringFactory and it cannot be linked");
        mg_dvm_clear_pending();
        return false;
    }
    return true;
}
