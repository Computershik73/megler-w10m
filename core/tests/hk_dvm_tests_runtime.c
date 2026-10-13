/*
 * The runtime around the bytecode: native methods and the way to the JNI
 * layer, monitors, two threads at once, the limits (how deep, how long), what
 * is refused (instructions not implemented, classes that cannot be linked,
 * files that are not DEX).
 */
#include <stdio.h>

#include "../src/dvm/hk_dvm_internal.h"
#include "hk_dvm_tests.h"

/* ------------------------------------------------------------------ natives */

static int g_all_seen;

static void N_all(hk_jcall *c)
{
    const hk_jvalue *a = c->args;
    /* Each argument in its own member, and the small ones whole in .i as well. */
    g_all_seen = a[0].z == 1 && a[0].i == 1 && a[1].b == -2 && a[1].i == -2 && a[2].c == 'A' && a[2].i == 'A' && a[3].s == -300 && a[3].i == -300
                 && a[4].i == 123456 && a[5].j == ((i64)1 << 40) && a[6].f == 1.5f && a[7].d == 2.5 && a[8].l && !strcmp(text_of(a[8].l), "nine")
                 && c->self == NULL && !strcmp(c->name, "all") && !strcmp(c->sig, "(ZBCSIJFDLjava/lang/String;)J");
    c->ret.j = 0x1122334455667788ll;
}

static void N_plus(hk_jcall *c)
{
    hk_jvalue base;
    base.j = 0;
    hk_dvm_get_field(c->self, "base", &base);
    c->ret.i = base.i + c->args[0].i;
}

/* Each returns more bits than its type has: only the type's own may reach the register. */
static void N_retBool(hk_jcall *c) { c->ret.i = 0x1234500 | (c->args[0].i != 0); }
static void N_retSmall(hk_jcall *c) { c->ret.i = c->args[0].i; }
static void N_retFloat(hk_jcall *c) { c->ret.f = c->args[0].f * 2; }
static void N_retDouble(hk_jcall *c) { c->ret.d = c->args[0].d * 2; }
static void N_retObject(hk_jcall *c) { c->ret.l = c->args[0].l; }
static void N_thrower(hk_jcall *c)
{
    (void)c;
    hk_dvm_throw("java/lang/IllegalStateException", "from a native method");
}

/* callback(n): 0 for 0, else what the bytecode method step(n) returns, which calls callback(n - 1). */
static void N_callback(hk_jcall *c)
{
    hk_jvalue arg, r;
    if (c->args[0].i <= 0) return;
    arg.j = 0;
    arg.i = c->args[0].i;
    if (hk_dvm_call_static("t/Natives", "step", "(I)I", &arg, &r)) c->ret.i = r.i;
}

/* A synchronized native method: the monitor is held while it runs. (Only the owner may notify.) */
static void N_locked(hk_jcall *c) { c->ret.z = hk_dvm_monitor_notify(c->self, false); }

static const hk_jhle k_test_natives[] = {
    { "t/Natives", "all", "(ZBCSIJFDLjava/lang/String;)J", N_all },
    { "t/Natives", "plus", "(I)I", N_plus },
    { "t/Natives", "retBool", NULL, N_retBool },
    { "t/Natives", "retByte", NULL, N_retSmall },
    { "t/Natives", "retChar", NULL, N_retSmall },
    { "t/Natives", "retShort", NULL, N_retSmall },
    { "t/Natives", "retFloat", "(F)F", N_retFloat },
    { "t/Natives", "retDouble", "(D)D", N_retDouble },
    { "t/Natives", "retObject", NULL, N_retObject },
    { "t/Natives", "thrower", "()V", N_thrower },
    { "t/Natives", "callback", "(I)I", N_callback },
    { "t/Natives", "locked", "()Z", N_locked },
    { NULL, NULL, NULL, NULL },
};

static void test_natives(void)
{
    hk_jobj *o, *s;
    hk_jvalue v;
    say("native methods");
    /* Before anything is registered, a native method is an error that says which. */
    call("t/Natives", "callAll", "()J");
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for t/Natives.all(ZBCSIJFDLjava/lang/String;)J");
    hk_dvm_register(k_test_natives);

    /* every kind of argument, from bytecode (by range) and from C */
    g_all_seen = 0;
    CHECK(call("t/Natives", "callAll", "()J").j == 0x1122334455667788ll);
    CHECK_INT(g_all_seen, 1);
    g_all_seen = 0;
    A[0].z = 1;
    A[1].b = -2;
    A[2].c = 'A';
    A[3].s = -300;
    A[4].i = 123456;
    A[5].j = (i64)1 << 40;
    A[6].f = 1.5f;
    A[7].d = 2.5;
    A[8].l = hk_dvm_new_string("nine");
    /* From C the values arrive as C gave them: only their own members are promised. */
    A[1].i = -2;
    A[3].i = -300;
    CHECK(call("t/Natives", "all", "(ZBCSIJFDLjava/lang/String;)J").j == 0x1122334455667788ll);
    CHECK_INT(g_all_seen, 1);

    /* an instance method, and what small types come back as */
    v.j = 0;
    v.i = 100;
    o = hk_dvm_new("t/Natives", "(I)V", &v);
    CHECK(o != NULL);
    A[0].i = 23;
    CHECK_INT(vcall(o, "plus", "(I)I").i, 123);
    CHECK_INT(call("t/Natives", "rets", "()I").i, 1 - 128 + 65535 - 32768);
    A[0].i = 0x180;
    CHECK_INT(call("t/Natives", "retByte", "(I)B").i, -128);
    A[0].i = 0;
    CHECK_INT(call("t/Natives", "retBool", "(I)Z").i, 0);
    A[0].f = 1.25f;
    A[1].d = 2.5;
    CHECK(call("t/Natives", "floats", "(FD)D").d == 2.5 + 5.0);
    s = hk_dvm_new_string("the same object");
    A[0].l = s;
    CHECK(call("t/Natives", "retObject", "(Ljava/lang/Object;)Ljava/lang/Object;").l == s);
    CHECK_STR(thrown(), "");

    /* an exception thrown in C: caught by bytecode, or reaching C again */
    CHECK_INT(call("t/Natives", "catchNative", "()I").i, 1);
    CHECK_STR(thrown(), "");
    call("t/Natives", "thrower", "()V");
    CHECK_STR(thrown(), "java.lang.IllegalStateException: from a native method");

    /* C calling bytecode calling C, fifty deep; and far too deep, which is an error like any other and leaves everything usable */
    A[0].i = 50;
    CHECK_INT(call("t/Natives", "callback", "(I)I").i, 50);
    CHECK_STR(thrown(), "");
    A[0].i = 1000000;
    call("t/Natives", "callback", "(I)I");
    CHECK(!strncmp(thrown(), "java.lang.StackOverflowError: ", 30));
    A[0].i = 10;
    CHECK_INT(call("t/Natives", "callback", "(I)I").i, 10);
    CHECK_STR(thrown(), "");

    /* a synchronized native method holds its object's monitor, and lets go of it */
    CHECK_INT(vcall(o, "locked", "()Z").z, 1);
    CHECK(!hk_dvm_monitor_notify(o, false));

    call("t/Natives", "missing", "()V");
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for t/Natives.missing()V");

    /* A native method nothing implements, here or in any library. */
    A[0].i = 5;
    A[1].j = (i64)1 << 33;
    call("t/Natives", "viaJni", "(IJ)J");
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for t/Natives.viaJni(IJ)J");
}

/* ----------------------------------------------------------------- monitors */

static void test_monitors(void)
{
    hk_jobj *s = hk_dvm_new("t/Sync", "()V", NULL);
    hk_jvalue v;
    u64 t0;
    say("monitors");
    CHECK(s != NULL);
    if (!s) return;
    CHECK_INT(vcall(s, "bump", "()I").i, 1);
    CHECK_INT(vcall(s, "twice", "()I").i, 3);
    CHECK_INT(vcall(s, "bump", "()I").i, 4);
    CHECK_STR(thrown(), "");
    /* each left the monitor free: this thread does not hold it */
    CHECK(!hk_dvm_monitor_exit(s));
    vcall(s, "throwing", "()V");
    CHECK_STR(thrown(), "java.lang.IllegalStateException: thrown with the lock held");
    CHECK(!hk_dvm_monitor_exit(s));
    A[0].l = s;
    call("t/Sync", "badExit", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: monitor-exit on an object this thread has not locked");
    A[0].l = s;
    call("t/Sync", "badNotify", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: object not locked by thread before notify()");
    A[0].l = s;
    call("t/Sync", "badWait", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: object not locked by thread before wait()");
    t0 = hk_time_monotonic_ns();
    A[0].l = s;
    call("t/Sync", "goodWait", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "");
    /* A wait of five milliseconds that nobody ends lasts five, by the clock (the host's own timed wait may end sooner; the monitor waits again). */
    CHECK(hk_time_monotonic_ns() - t0 >= 5000000ull);
    CHECK(!hk_dvm_monitor_exit(s));
    CHECK(hk_dvm_get_field(s, "count", &v) && v.i == 4);
}

/* ------------------------------------------------------------------ threads */

typedef struct worker {
    int id;
    hk_jobj *sync;
    i32 slow;
    bool ok;
    hk_jobj *mirrors[64], *strings[200];
    char own[200];
} worker;

static const char *const k_race_classes[] = {
    "java/lang/ArrayStoreException", "java/lang/CloneNotSupportedException", "java/lang/InterruptedException", "java/lang/NoSuchFieldException",
    "java/lang/NoSuchMethodException", "java/lang/IllegalAccessException", "java/lang/ClassFormatError", "java/lang/BootstrapMethodError",
    "java/lang/IllegalAccessError", "java/lang/AssertionError", "java/lang/Runnable", "java/lang/AutoCloseable", "java/lang/Short", "java/lang/Byte",
    "java/lang/Float", "[Ljava/lang/Runnable;", "[[Ljava/lang/Short;", "[Z", "[[[B", "java/lang/NumberFormatException", "t/Deep", "t/Misc", "u/Sub",
};

static void work(void *arg)
{
    worker *w = (worker *)arg;
    hk_jvalue v, r;
    char text[32];
    size_t i, n = HK_COUNT(k_race_classes);
    int k;
    w->ok = true;
    /* a class both threads ask for at once, whose initialiser takes a while: it runs once, and both see what it set */
    if (!hk_dvm_call_static("t/Slow", "get", "()I", NULL, &r)) w->ok = false;
    w->slow = r.i;
    /* the same classes, each thread in its own order: one Class object each, whoever linked it */
    for (i = 0; i < n; i++) {
        size_t at = w->id ? n - 1 - i : i;
        w->mirrors[at] = hk_dvm_class_object(k_race_classes[at]);
        if (!w->mirrors[at]) w->ok = false;
    }
    /* the same strings interned by both: one object each */
    for (k = 0; k < (int)HK_COUNT(w->strings); k++) {
        snprintf(text, sizeof(text), "interned by both %d", w->id ? (int)HK_COUNT(w->strings) - 1 - k : k);
        w->strings[w->id ? (int)HK_COUNT(w->strings) - 1 - k : k] = hk_dvm_intern_string(hk_dvm_new_string(text));
    }
    /* a counter both add to under its monitor */
    v.j = 0;
    v.i = 50000;
    if (!hk_dvm_call_virtual(w->sync, "bumpMany", "(I)V", &v, NULL)) w->ok = false;
    /* an exception is the thread's own: thrown here, seen here, and nowhere else */
    hk_dvm_throw("java/lang/IllegalStateException", "thread %d", w->id);
    if (!hk_dvm_describe_pending(w->own, sizeof(w->own))) w->own[0] = 0;
    hk_dvm_clear_pending();
}

static void test_threads(void)
{
    static worker w[2];
    hk_thread *t[2];
    hk_jvalue v;
    size_t i;
    int k;
    bool same = true;
    say("two threads");
    w[0].sync = w[1].sync = hk_dvm_new("t/Sync", "()V", NULL);
    for (k = 0; k < 2; k++) {
        w[k].id = k;
        t[k] = hk_thread_start(work, &w[k], 1 << 20, "dvm-test");
        CHECK(t[k] != NULL);
    }
    for (k = 0; k < 2; k++)
        if (t[k]) hk_thread_join(t[k]);
    CHECK(w[0].ok && w[1].ok);
    CHECK_INT(w[0].slow, (i32)(u32)(200000ull * 200001ull / 2));
    CHECK_INT(w[1].slow, w[0].slow);
    /* Slow's initialiser counts itself in Sync.scount: once. */
    CHECK(hk_dvm_get_static("t/Sync", "scount", &v) && v.i == 1);
    CHECK_INT(call("t/Sync", "sbump", "()I").i, 2);
    for (i = 0; i < HK_COUNT(k_race_classes); i++)
        if (!w[0].mirrors[i] || w[0].mirrors[i] != w[1].mirrors[i]) same = false;
    CHECK(same);
    same = true;
    for (i = 0; i < HK_COUNT(w[0].strings); i++)
        if (!w[0].strings[i] || w[0].strings[i] != w[1].strings[i]) same = false;
    CHECK(same);
    CHECK(hk_dvm_get_field(w[0].sync, "count", &v));
    CHECK_INT(v.i, 100000);
    CHECK_STR(w[0].own, "java.lang.IllegalStateException: thread 0");
    CHECK_STR(w[1].own, "java.lang.IllegalStateException: thread 1");
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------------- limits */

static void test_limits(void)
{
    hk_dvm_stats before, after;
    say("limits");
    /* Ten thousand calls each inside the last cost ten thousand frames of the interpreter's own stack, and none of C's. */
    A[0].i = 10000;
    CHECK_INT(call("t/Deep", "down", "(I)I").i, 10000);
    CHECK_STR(thrown(), "");
    call("t/Deep", "forever", "(I)I");
    CHECK(!strncmp(thrown(), "java.lang.StackOverflowError: ", 30));
    CHECK_INT(call("t/Deep", "caught", "()I").i, 1);
    CHECK_STR(thrown(), "");
    A[0].i = 100;
    CHECK_INT(call("t/Deep", "down", "(I)I").i, 100);

    /* Code that does not end, ended: by a limit on instructions, which nothing in the code can catch. */
    hk_dvm_get_stats(&before);
    hk_dvm_set_step_limit(50000);
    call("t/Misc", "spin", "()V");
    CHECK_STR(thrown(), "java.lang.InternalError: the step limit is used up in t/Misc.spin");
    /* (It ran out wherever execution stood, which may be inside the constructor of the exception this method keeps catching.) */
    call("t/Misc", "stubborn", "()V");
    CHECK(!strncmp(thrown(), "java.lang.InternalError: the step limit is used up in ", 54));
    /* The limit is for each call from C: what fits within it is not disturbed. */
    A[0].i = 1000;
    CHECK_INT(call("t/Arith", "sumTo", "(I)I").i, 500500);
    CHECK_STR(thrown(), "");
    hk_dvm_set_step_limit(0);
    hk_dvm_get_stats(&after);
    CHECK(after.instructions - before.instructions > 100000 && after.instructions - before.instructions < 200000);
    A[0].i = 100000;
    CHECK_INT(call("t/Arith", "sumTo", "(I)I").i, (i32)(u32)(100000ull * 100001ull / 2));
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------- what is refused */

static void test_refused(const char *fixtures)
{
    char *path = hk_sprintf("%s/a.dex", fixtures);
    size_t size = 0, i;
    u8 *file = hk_file_read_all(path, &size), *copy;
    hk_dvm_stats stats;
    u32 dexes_classes;
    say("what is refused");
    free(path);

    /* instructions that are not implemented: an exception that names them, which the code can catch; the rest of the file runs */
    A[0].l = hk_dvm_new_string("x");
    call("t/Refuse", "polymorphic", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.UnsupportedOperationException: invoke-polymorphic (a MethodHandle or VarHandle call) is not implemented, in t/Refuse.polymorphic");
    call("t/Refuse", "polymorphicRange", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.UnsupportedOperationException: invoke-polymorphic (a MethodHandle or VarHandle call) is not implemented, in t/Refuse.polymorphicRange");
    call("t/Refuse", "custom", "(I)V");
    CHECK_STR(thrown(), "java.lang.UnsupportedOperationException: invoke-custom (a call site bound at run time) is not implemented, in t/Refuse.custom");
    call("t/Refuse", "customRange", "(I)V");
    CHECK_STR(thrown(), "java.lang.UnsupportedOperationException: invoke-custom (a call site bound at run time) is not implemented, in t/Refuse.customRange");
    call("t/Refuse", "methodType", "()V");
    CHECK_STR(thrown(), "java.lang.UnsupportedOperationException: const-method-type is not implemented, in t/Refuse.methodType");
    CHECK_INT(call("t/Refuse", "caught", "()I").i, 1);
    A[0].i = 41;
    CHECK_INT(call("t/Refuse", "plain", "(I)I").i, 42);
    CHECK_STR(thrown(), "");

    /* what code names and nothing has */
    CHECK_INT(call("t/Misc", "nulls", "()I").i, 0);
    CHECK_STR(thrown(), "");
    call("t/Misc", "missing", "()Ljava/lang/Object;");
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Class");
    call("t/Misc", "missingMethod", "()V");
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: t/Misc.notThere()V");
    call("t/Misc", "missingField", "()I");
    CHECK_STR(thrown(), "java.lang.NoSuchFieldError: t/Misc.notThere:I");
    call("t/Misc", "wrongKind", "()V");
    CHECK_STR(thrown(), "java.lang.IncompatibleClassChangeError: t/Misc.spin()V is static");

    /* classes that cannot be linked */
    CHECK(hk_dvm_class_object("t/Loop") == NULL);
    CHECK_STR(thrown(), "java.lang.ClassCircularityError: t/Loop");
    CHECK(hk_dvm_class_object("t/BadSuper") == NULL);
    CHECK_STR(thrown(), "java.lang.IncompatibleClassChangeError: t/BadSuper has the interface t/HasArea for its superclass");
    call("t/Orphan", "touch", "()I");
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/Orphan)");
    CHECK(hk_dvm_class_object("t/OrphanChild") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/OrphanChild)");
    CHECK(hk_dvm_class_object("[[Lt/Orphan;") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by [[Lt/Orphan;)");
    /* and asking again gives the same answer, not a half-linked class */
    CHECK(hk_dvm_class_object("t/OrphanChild") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/OrphanChild)");

    /* files that are not DEX files, or not whole: refused when they are added, and nothing of them is kept */
    hk_dvm_get_stats(&stats);
    dexes_classes = stats.classes_defined;
    CHECK(file != NULL && size > 0x70);
    if (file && size > 0x70) {
        copy = (u8 *)hk_alloc(size + 8);
        CHECK(!hk_dvm_add_dex("this is not a DEX file, whatever its name says, and it is long enough to hold a header that it does not have....................", 120, "junk"));
        memcpy(copy + 1, file, size);
        CHECK(!hk_dvm_add_dex(copy + 1, size, "at an odd address"));
        memcpy(copy, file, size);
        CHECK(!hk_dvm_add_dex(copy, 0x60, "cut short"));
        CHECK(!hk_dvm_add_dex(copy, size - 1, "one byte missing"));
        copy[6] = '4';
        CHECK(!hk_dvm_add_dex(copy, size, "version 034"));
        copy[6] = '5';
        copy[size - 9] ^= 0x40;
        CHECK(!hk_dvm_add_dex(copy, size, "one bit changed"));
        copy[size - 9] ^= 0x40;
        /* every offset in the header pointing past the end, one at a time, under a checksum made right again */
        for (i = 0x3c; i <= 0x64; i += 8) {
            u8 saved[4];
            u32 a = 1, b = 0, k, far = (u32)size + 4;
            memcpy(saved, copy + i, 4);
            memcpy(copy + i, &far, 4);
            for (k = 12; k < size; k++) {
                a = (a + copy[k]) % 65521u;
                b = (b + a) % 65521u;
            }
            a |= b << 16;
            memcpy(copy + 8, &a, 4);
            CHECK(!hk_dvm_add_dex(copy, size, "a table outside the file"));
            memcpy(copy + i, saved, 4);
        }
        free(copy);
    }
    free(file);
    CHECK(!hk_dvm_add_dex_file("no/such/file.dex"));
    CHECK_INT(hk_dvm_add_apk("no/such/file.apk"), 0);
    hk_dvm_get_stats(&stats);
    CHECK_INT(stats.classes_defined, dexes_classes);
}

static void test_misc(void)
{
    hk_dvm_stats stats;
    hk_jobj *o;
    say("the rest");
    A[0].i = 5;
    CHECK_INT(call("t/Misc", "jumps", "(I)I").i, 8);
    CHECK_INT(call("t/Misc", "callMany", "()I").i, 385);
    o = call("t/Misc", "arrayClass", "()Ljava/lang/Class;").l;
    CHECK(o && o->kind == HK_J_CLASS && o == hk_dvm_class_object("[I"));
    CHECK_STR(hk_dvm_class_name(o), "java/lang/Class");
    CHECK_STR(text_of(vcall(o, "getName", "()Ljava/lang/String;").l), "[I");
    CHECK_STR(text_of(vcall(hk_dvm_class_object("[[Ljava/lang/String;"), "getSimpleName", "()Ljava/lang/String;").l), "String[][]");
    CHECK_STR(text_of(vcall(hk_dvm_class_object("t/HasArea"), "toString", "()Ljava/lang/String;").l), "interface t.HasArea");
    A[0].l = hk_dvm_new_string("t.Rect");
    o = call("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;").l;
    CHECK(o && o == hk_dvm_class_object("t/Rect"));
    A[0].l = hk_dvm_new_string("t.NotThere");
    call("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;");
    CHECK_STR(thrown(), "java.lang.ClassNotFoundException: t.NotThere");
    CHECK(hk_dvm_running());
    hk_dvm_get_stats(&stats);
    CHECK(stats.classes_linked > 60 && stats.classes_initialised > 20 && stats.objects > 500 && stats.object_bytes > stats.objects * 32 && stats.strings_interned > 50);
    CHECK_STR(thrown(), "");
}

void test_runtime(const char *fixtures)
{
    test_natives();
    test_monitors();
    test_threads();
    test_limits();
    test_refused(fixtures);
    test_misc();
}
