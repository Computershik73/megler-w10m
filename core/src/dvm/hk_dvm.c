/* The runtime's interface to the rest of the core: starting it, adding DEX files, calling into it from C. See hk_dvm.h. */
#include "hk_dvm_internal.h"

#include <stdio.h>

bool hk_dvm_running(void) { return g_dvm.running; }

static bool g_counting;
static void set_slow(void) { g_dvm.slow = (g_dvm.trace >= 3 || g_dvm.step_limit || g_counting || g_dvm.profiling) ? 1 : 0; }

/* ------------------------------------------------------------- the profile */

static void profile_clear(hk_dvm_class *c, void *user)
{
    int pass, i;
    (void)user;
    for (pass = 0; pass < 2; pass++) {
        hk_dvm_method *list = pass ? c->virt : c->direct;
        for (i = 0; i < (pass ? c->nvirt : c->ndirect); i++) {
            list[i].prof_self = list[i].prof_incl = list[i].prof_ns = list[i].prof_bytes = 0;
            list[i].prof_calls = list[i].prof_objects = 0;
        }
    }
}

void hk_dvm_profile(bool on)
{
    if (on) hk_dvm_each_linked(profile_clear, NULL);
    g_dvm.profiling = on ? 1 : 0;
    set_slow();
}

typedef struct profile_rows {
    hk_dvm_method **v;
    size_t n, cap;
    u64 instructions, objects, bytes, native_ns;
} profile_rows;

static void profile_collect(hk_dvm_class *c, void *user)
{
    profile_rows *r = (profile_rows *)user;
    int pass, i;
    for (pass = 0; pass < 2; pass++) {
        hk_dvm_method *list = pass ? c->virt : c->direct;
        for (i = 0; i < (pass ? c->nvirt : c->ndirect); i++) {
            hk_dvm_method *m = &list[i];
            if (!m->prof_calls && !m->prof_self && !m->prof_objects) continue;
            if (r->n == r->cap) {
                r->cap = r->cap ? r->cap * 2 : 1024;
                r->v = (hk_dvm_method **)hk_realloc(r->v, sizeof(*r->v) * r->cap);
            }
            r->v[r->n++] = m;
            r->instructions += m->prof_self;
            r->objects += m->prof_objects;
            r->bytes += m->prof_bytes;
            /* Native time is counted once where natives do not nest; where one calls back into bytecode that calls another, twice. */
            r->native_ns += m->prof_ns;
        }
    }
}

static int g_profile_key;
static int profile_order(const void *a, const void *b)
{
    const hk_dvm_method *x = *(hk_dvm_method *const *)a, *y = *(hk_dvm_method *const *)b;
    u64 p = g_profile_key == 0 ? x->prof_self : g_profile_key == 1 ? x->prof_incl : g_profile_key == 2 ? x->prof_ns : x->prof_bytes;
    u64 q = g_profile_key == 0 ? y->prof_self : g_profile_key == 1 ? y->prof_incl : g_profile_key == 2 ? y->prof_ns : y->prof_bytes;
    return p < q ? 1 : p > q ? -1 : 0;
}

void hk_dvm_profile_report(int top, void (*line)(const char *text, void *user), void *user)
{
    static const char *const titles[] = { "by the instructions executed in the method itself", "by the instructions executed in the method and under it (summed over its calls)",
                                          "native methods by the time spent in them", "by the bytes of objects made in the method" };
    profile_rows r;
    char text[400];
    size_t i;
    int key;
    memset(&r, 0, sizeof(r));
    hk_dvm_each_linked(profile_collect, &r);
    snprintf(text, sizeof(text), "profile: %llu thousand instructions in %u methods; %llu objects (%llu KB) made; %llu ms in native methods", (unsigned long long)(r.instructions / 1000),
             (unsigned)r.n, (unsigned long long)r.objects, (unsigned long long)(r.bytes >> 10), (unsigned long long)(r.native_ns / 1000000ull));
    line(text, user);
    for (key = 0; key < 4; key++) {
        g_profile_key = key;
        if (r.n) qsort(r.v, r.n, sizeof(*r.v), profile_order);
        snprintf(text, sizeof(text), "  %s:", titles[key]);
        line(text, user);
        for (i = 0; i < r.n && (int)i < top; i++) {
            const hk_dvm_method *m = r.v[i];
            u64 value = key == 0 ? m->prof_self : key == 1 ? m->prof_incl : key == 2 ? m->prof_ns : m->prof_bytes;
            if (!value) break;
            if (key == 2) snprintf(text, sizeof(text), "    %8llu ms  %7u calls  %s.%s", (unsigned long long)(value / 1000000ull), (unsigned)m->prof_calls, m->cls->name, m->name);
            else if (key == 3)
                snprintf(text, sizeof(text), "    %8llu KB  %7u objects  %s.%s", (unsigned long long)(value >> 10), (unsigned)m->prof_objects, m->cls->name, m->name);
            else
                snprintf(text, sizeof(text), "    %9llu k  %5.1f%%  %7u calls  %s.%s%s", (unsigned long long)(value / 1000), r.instructions ? 100.0 * (double)value / (double)r.instructions : 0.0,
                         (unsigned)m->prof_calls, m->cls->name, m->name, m->sig);
            line(text, user);
        }
    }
    free(r.v);
}

void hk_dvm_count_instructions(bool on)
{
    g_counting = on;
    set_slow();
}

void hk_dvm_set_trace(int level)
{
    g_dvm.trace = level;
    set_slow();
}

void hk_dvm_set_step_limit(u64 instructions)
{
    g_dvm.step_limit = instructions;
    set_slow();
}

static bool add_dex(const void *data, size_t size, const char *label, bool boot)
{
    hk_dvm_dex *d = hk_dvm_dex_open((const u8 *)data, size, label);
    if (!d) return false;
    d->boot = boot;
    d->app = g_dvm.app_follows;
    hk_dvm_classes_add_dex(d);
    return true;
}

bool hk_dvm_add_dex(const void *data, size_t size, const char *label) { return add_dex(data, size, label, !g_dvm.running); }

static bool add_dex_file(const char *path, bool boot)
{
    size_t size = 0;
    u8 *data = hk_file_read_all(path, &size);
    const char *slash = strrchr(path, '/');
    if (!data) {
        hk_warn("dvm: %s cannot be read", path);
        return false;
    }
    if (!add_dex(data, size, slash ? slash + 1 : path, boot)) {
        free(data);
        return false;
    }
    return true;
}

bool hk_dvm_add_dex_file(const char *path) { return add_dex_file(path, !g_dvm.running); }

/* A boot class path file is mapped where the host can map it: its pages are the file's, there when touched, and no copy of 11 MB is kept. */
static bool add_boot_file(const char *path)
{
    const char *slash = strrchr(path, '/');
    hk_map map;
    if (hk_map_open(&map, path) && map.size < 0x7FFFFFFFu && add_dex(map.data, (size_t)map.size, slash ? slash + 1 : path, true)) return true;
    return add_dex_file(path, true);
}

static bool g_libcore_natives;

bool hk_dvm_libcore_natives(void) { return g_libcore_natives; }

void hk_dvm_app_follows(void) { g_dvm.app_follows = true; }
bool hk_dvm_class_is_app(const hk_dvm_class *c) { return c && c->dex && c->dex->app; }

static void give_loader(hk_dvm_class *c, void *user)
{
    hk_jvalue v;
    v.j = 0;
    v.l = (hk_jobj *)user;
    if (c->dex && c->dex->app && c->mirror) hk_dvm_set_field(c->mirror, "classLoader", v);
}

void hk_dvm_set_app_loader(hk_jobj *loader)
{
    g_dvm.app_loader = loader;
    if (g_dvm.libcore) hk_dvm_each_linked(give_loader, loader);
}

bool hk_dvm_start_android(const char *root)
{
    static const char *const names[] = { "megler-libcore", "core-oj", "core-libart", "conscrypt", "okhttp", "bouncycastle", "apache-xml" };
    const char *paths[HK_COUNT(names)];
    char *made[HK_COUNT(names)], *crypto;
    hk_stat st;
    size_t i;
    int n = 0;
    bool ok;
    if (g_dvm.running) return g_dvm.libcore;
    /*
     * Whether conscrypt's native library is there decides who the security providers are (hk_dvm_art.c, "the security providers"):
     * there, and on a host that runs ARM code, which is where libcore's libraries load. On the PC's own build its providers could
     * not start, and with them none could.
     */
    crypto = hk_sprintf("%s/lib/libjavacrypto.so", root);
    g_dvm.conscrypt = HK_HOST_ARM32 && hk_path_stat(crypto, &st, NULL) && !st.is_dir;
    free(crypto);
    for (i = 0; i < HK_COUNT(names); i++) {
        made[i] = hk_sprintf("%s/framework/%s.dex", root, names[i]);
        if (hk_path_stat(made[i], &st, NULL) && !st.is_dir) paths[n++] = made[i];
    }
    g_dvm.android_root = hk_strdup(root);
    ok = n > 0 && hk_dvm_start(paths, n) && g_dvm.libcore;
    if (!ok) hk_err("dvm: Android's boot class path was not found in %s/framework (core-oj.dex and the rest)", root);
    /* libcore's own libraries, where they are and this host can run them. Without them libcore is its bytecode and what hk_dvm_art_host.c stands in with. */
    else g_libcore_natives = hk_dvm_load_libcore_natives(root);
    /* libjavacrypto.so stands on libjavacore.so and libnativehelper.so: without them it would not load when conscrypt asks for it. */
    if (g_dvm.conscrypt && !g_libcore_natives) {
        hk_warn("dvm: conscrypt's native library is in %s/lib, but libcore's did not load: the runtime's own security provider stands in", root);
        g_dvm.conscrypt = false;
    }
    for (i = 0; i < HK_COUNT(names); i++) free(made[i]);
    return ok;
}

bool hk_dvm_start(const char *const *boot, int nboot)
{
    static const char *const first[] = { "java/lang/Object", "java/lang/Class", "java/lang/String", "java/lang/Throwable" };
    const u16 one = 1;
    char why[300];
    size_t k;
    int i;
    if (g_dvm.running) return true;
    /* The JNI layer first: every class linked from here on is declared there too, and objects carry that class where it looks. */
    hk_jni_init();
    hk_jni_dvm_join(&g_dvm.jni);
    /* Code units, array data and the bits of floats are read from the file as they lie, which is little-endian. So are all the hosts. */
    if (*(const u8 *)&one != 1) {
        hk_err("dvm: this host is big-endian, which the interpreter does not handle");
        return false;
    }
    if (nboot <= 0) {
        if (!hk_dvm_boot_add()) return false;
    } else {
        for (i = 0; i < nboot; i++)
            if (!add_boot_file(boot[i])) hk_warn("dvm: boot class path entry %s was not added", boot[i]);
        /* A boot class path with libcore's own String on it is libcore's: its natives are Android's runtime's, done in hk_dvm_art.c. */
        if (hk_dvm_class_exists("java/lang/StringFactory") && !hk_dvm_art_start()) return false;
    }
    if (!hk_dvm_class_exists("java/lang/Object")) {
        hk_err("dvm: no java.lang.Object on the boot class path");
        return false;
    }
    g_dvm.running = true;
    /* The classes everything else leans on, linked in the order they lean on each other. */
    for (k = 0; k < HK_COUNT(first); k++) {
        if (hk_dvm_find_class(first[k])) continue;
        hk_err("dvm: %s cannot be linked: %s", first[k], hk_dvm_describe_pending(why, sizeof(why)) ? why : "?");
        hk_dvm_clear_pending();
        g_dvm.running = false;
        return false;
    }
    hk_dvm_objects_start();
    if (g_dvm.libcore) hk_dvm_intrinsics_start();
    /* From here on allocation collects when enough has been made (hk_dvm_gc.c). */
    hk_dvm_gc_enable(true);
    return true;
}

bool hk_dvm_string_init(const char *sig, const hk_jvalue *args, hk_jobj **out)
{
    const char *factory, *close = strchr(sig, ')');
    hk_dvm_method *m;
    hk_jvalue r;
    char *fsig;
    *out = NULL;
    if (!g_dvm.c_string_factory) return hk_dvm_boot_string_init(sig, args, out);
    /*
     * libcore's String has no constructors that work: Android's runtime turns each into the method of
     * java.lang.StringFactory with the same parameters, named for what the string is made from.
     */
    if (!strcmp(sig, "()V")) factory = "newEmptyString";
    else if (!strncmp(sig, "([B", 3)) factory = "newStringFromBytes";
    else if (!strncmp(sig, "([C", 3) || !strcmp(sig, "(II[C)V")) factory = "newStringFromChars";
    else if (!strcmp(sig, "([III)V")) factory = "newStringFromCodePoints";
    else if (!strcmp(sig, "(Ljava/lang/String;)V")) factory = "newStringFromString";
    else if (!strcmp(sig, "(Ljava/lang/StringBuffer;)V")) factory = "newStringFromStringBuffer";
    else if (!strcmp(sig, "(Ljava/lang/StringBuilder;)V")) factory = "newStringFromStringBuilder";
    else return hk_dvm_throw("java/lang/NoSuchMethodError", "java/lang/String.<init>%s", sig);
    fsig = hk_sprintf("%.*s)Ljava/lang/String;", (int)(close ? close - sig : 0), sig);
    m = hk_dvm_find_method(g_dvm.c_string_factory, factory, fsig, true);
    free(fsig);
    if (!m) return hk_dvm_throw("java/lang/NoSuchMethodError", "java/lang/StringFactory.%s for String%s", factory, sig);
    if (!hk_dvm_ensure_init(g_dvm.c_string_factory) || !hk_dvm_invoke(m, NULL, args, &r)) return false;
    *out = r.l;
    return true;
}

bool hk_dvm_call_static(const char *cls, const char *name, const char *sig, const hk_jvalue *args, hk_jvalue *ret)
{
    hk_dvm_class *c = hk_dvm_find_class(cls);
    hk_dvm_method *m;
    if (!c) return false;
    m = hk_dvm_find_method(c, name, sig, true);
    if (!m) return hk_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s", cls, name, sig);
    /* The class that declares it, which may be above the one named. */
    if (!hk_dvm_ensure_init(m->cls)) return false;
    return hk_dvm_invoke(m, NULL, args, ret);
}

bool hk_dvm_call_virtual(hk_jobj *self, const char *name, const char *sig, const hk_jvalue *args, hk_jvalue *ret)
{
    hk_dvm_class *c;
    hk_dvm_method *m;
    if (!self || !self->dvm) return hk_dvm_throw("java/lang/NullPointerException", "Attempt to invoke virtual method '%s%s' on a null object reference", name, sig);
    c = hk_dvm_class_of(self);
    m = hk_dvm_find_virtual(c, hk_dvm_intern(name), hk_dvm_intern(sig));
    if (!m) m = hk_dvm_find_method(c, name, sig, false);        /* a private one */
    if (!m) return hk_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s", c->name, name, sig);
    return hk_dvm_invoke(m, self, args, ret);
}

hk_jobj *hk_dvm_new(const char *cls, const char *ctor_sig, const hk_jvalue *args)
{
    hk_dvm_class *c = hk_dvm_find_class(cls);
    hk_dvm_method *ctor;
    hk_jobj *o;
    if (!c || !hk_dvm_ensure_init(c)) return NULL;
    if (c == g_dvm.c_string) return hk_dvm_string_init(ctor_sig, args, &o) ? o : NULL;
    ctor = hk_dvm_find_method(c, "<init>", ctor_sig, false);
    if (!ctor) {
        hk_dvm_throw("java/lang/NoSuchMethodError", "%s.<init>%s", cls, ctor_sig);
        return NULL;
    }
    o = hk_dvm_alloc_object(c);
    if (!o || !hk_dvm_invoke(ctor, o, args, NULL)) return NULL;
    return o;
}

hk_jobj *hk_dvm_class_object(const char *cls)
{
    hk_dvm_class *c = hk_dvm_find_class(cls);
    return c ? hk_dvm_mirror(c) : NULL;
}

const char *hk_dvm_class_name(const hk_jobj *o) { return !o ? "null" : o->dvm ? hk_dvm_class_of(o)->name : hk_jni_class_name(o); }

bool hk_dvm_instance_of(hk_jobj *o, const char *cls)
{
    hk_dvm_class *c;
    if (!o || !o->dvm || !hk_dvm_class_exists(cls)) return false;
    c = hk_dvm_find_class(cls);
    if (!c) {
        hk_dvm_clear_pending();
        return false;
    }
    return hk_dvm_is_instance(o, c);
}

static hk_jvalue *static_slot(const char *cls, const char *name)
{
    hk_dvm_class *c = hk_dvm_find_class(cls);
    hk_dvm_field *f = c ? hk_dvm_find_field(c, name, true) : NULL;
    if (!f || !hk_dvm_ensure_init(f->cls)) return NULL;
    return &f->cls->statics[f->slot];
}

bool hk_dvm_get_static(const char *cls, const char *name, hk_jvalue *out)
{
    hk_jvalue *slot = static_slot(cls, name);
    if (slot) *out = *slot;
    return slot != NULL;
}

bool hk_dvm_set_static(const char *cls, const char *name, hk_jvalue v)
{
    hk_jvalue *slot = static_slot(cls, name);
    if (slot) *slot = v;
    return slot != NULL;
}

static hk_jvalue *instance_slot(hk_jobj *o, const char *name)
{
    hk_dvm_field *f = o && o->dvm ? hk_dvm_find_field(hk_dvm_class_of(o), name, false) : NULL;
    if (!f || f->slot >= hk_dvm_class_of(o)->nslots) return NULL;
    return &hk_dvm_slots(o)[f->slot];
}

bool hk_dvm_get_field(hk_jobj *o, const char *name, hk_jvalue *out)
{
    hk_jvalue *slot = instance_slot(o, name);
    if (slot) *out = *slot;
    return slot != NULL;
}

bool hk_dvm_set_field(hk_jobj *o, const char *name, hk_jvalue v)
{
    hk_jvalue *slot = instance_slot(o, name);
    if (slot) *slot = v;
    return slot != NULL;
}
