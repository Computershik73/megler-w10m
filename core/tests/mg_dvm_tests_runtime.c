/*
 * The runtime around the bytecode: native methods and the way to the JNI
 * layer, monitors, two threads at once, the limits (how deep, how long), what
 * is refused (instructions not implemented, classes that cannot be linked,
 * files that are not DEX).
 */
#include <stdio.h>

#include "../src/dvm/mg_dvm_internal.h"
#include "mg_dvm_tests.h"
#include "zlib.h"

/* ------------------------------------------------------------------ natives */

static int g_all_seen;

static void N_all(mg_jcall *c)
{
    const mg_jvalue *a = c->args;
    /* Each argument in its own member, and the small ones whole in .i as well. */
    g_all_seen = a[0].z == 1 && a[0].i == 1 && a[1].b == -2 && a[1].i == -2 && a[2].c == 'A' && a[2].i == 'A' && a[3].s == -300 && a[3].i == -300
                 && a[4].i == 123456 && a[5].j == ((i64)1 << 40) && a[6].f == 1.5f && a[7].d == 2.5 && a[8].l && !strcmp(text_of(a[8].l), "nine")
                 && c->self == NULL && !strcmp(c->name, "all") && !strcmp(c->sig, "(ZBCSIJFDLjava/lang/String;)J");
    c->ret.j = 0x1122334455667788ll;
}

static void N_plus(mg_jcall *c)
{
    mg_jvalue base;
    base.j = 0;
    mg_dvm_get_field(c->self, "base", &base);
    c->ret.i = base.i + c->args[0].i;
}

/* Each returns more bits than its type has: only the type's own may reach the register. */
static void N_retBool(mg_jcall *c) { c->ret.i = 0x1234500 | (c->args[0].i != 0); }
static void N_retSmall(mg_jcall *c) { c->ret.i = c->args[0].i; }
static void N_retFloat(mg_jcall *c) { c->ret.f = c->args[0].f * 2; }
static void N_retDouble(mg_jcall *c) { c->ret.d = c->args[0].d * 2; }
static void N_retObject(mg_jcall *c) { c->ret.l = c->args[0].l; }
static void N_thrower(mg_jcall *c)
{
    (void)c;
    mg_dvm_throw("java/lang/IllegalStateException", "from a native method");
}

/* callback(n): 0 for 0, else what the bytecode method step(n) returns, which calls callback(n - 1). */
static void N_callback(mg_jcall *c)
{
    mg_jvalue arg, r;
    if (c->args[0].i <= 0) return;
    arg.j = 0;
    arg.i = c->args[0].i;
    if (mg_dvm_call_static("t/Natives", "step", "(I)I", &arg, &r)) c->ret.i = r.i;
}

/* A synchronized native method: the monitor is held while it runs. (Only the owner may notify.) */
static void N_locked(mg_jcall *c) { c->ret.z = mg_dvm_monitor_notify(c->self, false); }

static const mg_jhle k_test_natives[] = {
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
    mg_jobj *o, *s;
    mg_jvalue v;
    say("native methods");
    /* Before anything is registered, a native method is an error that says which. */
    call("t/Natives", "callAll", "()J");
    /* (ART's sentence: the method as source writes it, and the two names a library could have exported it under.) */
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for long t.Natives.all(boolean, byte, char, short, int, long, float, double, java.lang.String) "
                        "(tried Java_t_Natives_all and Java_t_Natives_all__ZBCSIJFDLjava_lang_String_2)");
    mg_dvm_register(k_test_natives);

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
    A[8].l = mg_dvm_new_string("nine");
    /* From C the values arrive as C gave them: only their own members are promised. */
    A[1].i = -2;
    A[3].i = -300;
    CHECK(call("t/Natives", "all", "(ZBCSIJFDLjava/lang/String;)J").j == 0x1122334455667788ll);
    CHECK_INT(g_all_seen, 1);

    /* an instance method, and what small types come back as */
    v.j = 0;
    v.i = 100;
    o = mg_dvm_new("t/Natives", "(I)V", &v);
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
    s = mg_dvm_new_string("the same object");
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
    CHECK(!mg_dvm_monitor_notify(o, false));

    call("t/Natives", "missing", "()V");
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for void t.Natives.missing() (tried Java_t_Natives_missing and Java_t_Natives_missing__)");

    /* A native method nothing implements, here or in any library. */
    A[0].i = 5;
    A[1].j = (i64)1 << 33;
    call("t/Natives", "viaJni", "(IJ)J");
    CHECK_STR(thrown(), "java.lang.UnsatisfiedLinkError: No implementation found for long t.Natives.viaJni(int, long) (tried Java_t_Natives_viaJni and Java_t_Natives_viaJni__IJ)");
}

/* ----------------------------------------------------------------- monitors */

static void test_monitors(void)
{
    mg_jobj *s = mg_dvm_new("t/Sync", "()V", NULL);
    mg_jvalue v;
    u64 t0;
    say("monitors");
    CHECK(s != NULL);
    if (!s) return;
    CHECK_INT(vcall(s, "bump", "()I").i, 1);
    CHECK_INT(vcall(s, "twice", "()I").i, 3);
    CHECK_INT(vcall(s, "bump", "()I").i, 4);
    CHECK_STR(thrown(), "");
    /* each left the monitor free: this thread does not hold it */
    CHECK(!mg_dvm_monitor_exit(s));
    vcall(s, "throwing", "()V");
    CHECK_STR(thrown(), "java.lang.IllegalStateException: thrown with the lock held");
    CHECK(!mg_dvm_monitor_exit(s));
    A[0].l = s;
    call("t/Sync", "badExit", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: monitor-exit on an object this thread has not locked");
    A[0].l = s;
    call("t/Sync", "badNotify", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: object not locked by thread before notify()");
    A[0].l = s;
    call("t/Sync", "badWait", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: object not locked by thread before wait()");
    t0 = mg_time_monotonic_ns();
    A[0].l = s;
    call("t/Sync", "goodWait", "(Ljava/lang/Object;)V");
    CHECK_STR(thrown(), "");
    /* A wait of five milliseconds that nobody ends lasts five, by the clock (the host's own timed wait may end sooner; the monitor waits again). */
    CHECK(mg_time_monotonic_ns() - t0 >= 5000000ull);
    CHECK(!mg_dvm_monitor_exit(s));
    CHECK(mg_dvm_get_field(s, "count", &v) && v.i == 4);
}

/* ------------------------------------------------------------------ threads */

typedef struct worker {
    int id;
    mg_jobj *sync;
    i32 slow;
    bool ok;
    mg_jobj *mirrors[64], *strings[200];
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
    mg_jvalue v, r;
    char text[32];
    size_t i, n = MG_COUNT(k_race_classes);
    int k;
    w->ok = true;
    /* a class both threads ask for at once, whose initialiser takes a while: it runs once, and both see what it set */
    if (!mg_dvm_call_static("t/Slow", "get", "()I", NULL, &r)) w->ok = false;
    w->slow = r.i;
    /* the same classes, each thread in its own order: one Class object each, whoever linked it, and whole when it is had --
       on libcore, whose Class has the fields: its size, and an array's component type, are there (they were filled in after
       the object was published, and a thread could have one without them) */
    for (i = 0; i < n; i++) {
        size_t at = w->id ? n - 1 - i : i;
        w->mirrors[at] = mg_dvm_class_object(k_race_classes[at]);
        if (!w->mirrors[at]) w->ok = false;
        else {
            mg_jvalue size, component;
            size.j = component.j = 0;
            if (mg_dvm_get_field(w->mirrors[at], "objectSize", &size) && size.i <= 0) w->ok = false;
            if (k_race_classes[at][0] == '[' && mg_dvm_get_field(w->mirrors[at], "componentType", &component) && !component.l) w->ok = false;
        }
    }
    /* the same strings interned by both: one object each */
    for (k = 0; k < (int)MG_COUNT(w->strings); k++) {
        snprintf(text, sizeof(text), "interned by both %d", w->id ? (int)MG_COUNT(w->strings) - 1 - k : k);
        w->strings[w->id ? (int)MG_COUNT(w->strings) - 1 - k : k] = mg_dvm_intern_string(mg_dvm_new_string(text));
    }
    /* a counter both add to under its monitor */
    v.j = 0;
    v.i = 50000;
    if (!mg_dvm_call_virtual(w->sync, "bumpMany", "(I)V", &v, NULL)) w->ok = false;
    /* an exception is the thread's own: thrown here, seen here, and nowhere else */
    mg_dvm_throw("java/lang/IllegalStateException", "thread %d", w->id);
    if (!mg_dvm_describe_pending(w->own, sizeof(w->own))) w->own[0] = 0;
    mg_dvm_clear_pending();
}

static void test_threads(void)
{
    static worker w[2];
    mg_thread *t[2];
    mg_jvalue v;
    size_t i;
    int k;
    bool same = true;
    u64 executed_before, executed_after;
    say("two threads");
    w[0].sync = w[1].sync = mg_dvm_new("t/Sync", "()V", NULL);
    executed_before = mg_dvm_executed_all();
    for (k = 0; k < 2; k++) {
        w[k].id = k;
        t[k] = mg_thread_start(work, &w[k], 1 << 20, "dvm-test");
        CHECK(t[k] != NULL);
    }
    for (k = 0; k < 2; k++)
        if (t[k]) mg_thread_join(t[k]);
    CHECK(w[0].ok && w[1].ok);
    /*
     * What the two executed stays in the total once they have ended: t/Slow's initialiser summed 200000 numbers in one of
     * them, at least an instruction a number. (The total once fell back when a thread ended, and the meter's differences
     * went negative.)
     */
    executed_after = mg_dvm_executed_all();
    CHECK(executed_after >= executed_before + 200000ull);
    CHECK_INT(w[0].slow, (i32)(u32)(200000ull * 200001ull / 2));
    CHECK_INT(w[1].slow, w[0].slow);
    /* Slow's initialiser counts itself in Sync.scount: once. */
    CHECK(mg_dvm_get_static("t/Sync", "scount", &v) && v.i == 1);
    CHECK_INT(call("t/Sync", "sbump", "()I").i, 2);
    for (i = 0; i < MG_COUNT(k_race_classes); i++)
        if (!w[0].mirrors[i] || w[0].mirrors[i] != w[1].mirrors[i]) same = false;
    CHECK(same);
    same = true;
    for (i = 0; i < MG_COUNT(w[0].strings); i++)
        if (!w[0].strings[i] || w[0].strings[i] != w[1].strings[i]) same = false;
    CHECK(same);
    CHECK(mg_dvm_get_field(w[0].sync, "count", &v));
    CHECK_INT(v.i, 100000);
    CHECK_STR(w[0].own, "java.lang.IllegalStateException: thread 0");
    CHECK_STR(w[1].own, "java.lang.IllegalStateException: thread 1");
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------------- limits */

/* A thread the app's native code made and attached: its frame stack, and what it can do on it. */
typedef struct attached_run {
    size_t words;
    i32 down;
    char overflow[48];
    i32 after;
} attached_run;

static void attached_work(void *arg)
{
    attached_run *r = (attached_run *)arg;
    r->words = mg_dvm_self_attached()->stack_words;
    A[0].i = 1000;
    r->down = call("t/Deep", "down", "(I)I").i;
    call("t/Deep", "forever", "(I)I");
    mg_strlcpy(r->overflow, thrown(), sizeof(r->overflow));
    A[0].i = 100;
    r->after = call("t/Deep", "down", "(I)I").i;
}

static void test_limits(void)
{
    mg_dvm_stats before, after;
    say("limits");
    /* Ten thousand calls each inside the last cost ten thousand frames of the interpreter's own stack, and none of C's. */
    A[0].i = 10000;
    CHECK_INT(call("t/Deep", "down", "(I)I").i, 10000);
    CHECK_STR(thrown(), "");
    /* Which it committed as they needed it, more than the first part of it. */
    CHECK(mg_dvm_self()->stack_committed > DVM_STACK_FIRST_WORDS);
    call("t/Deep", "forever", "(I)I");
    CHECK(!strncmp(thrown(), "java.lang.StackOverflowError: ", 30));
    /* Without end, the whole of it, the reserve with it. */
    CHECK(mg_dvm_self()->stack_committed == DVM_STACK_WORDS && mg_dvm_self()->stack_words == DVM_STACK_WORDS);
    {
        /* A thread native code attached has the smaller one: a thousand calls deep fit, one that does not end is the
           error there too, and the thread goes on. */
        static attached_run r;
        mg_thread *th = mg_thread_start(attached_work, &r, 1 << 20, "dvm-attached");
        CHECK(th != NULL);
        if (th) mg_thread_join(th);
        CHECK(r.words == DVM_STACK_ATTACHED_WORDS && r.down == 1000 && r.after == 100);
        CHECK(!strncmp(r.overflow, "java.lang.StackOverflowError: ", 30));
    }
    CHECK_INT(call("t/Deep", "caught", "()I").i, 1);
    CHECK_STR(thrown(), "");
    A[0].i = 100;
    CHECK_INT(call("t/Deep", "down", "(I)I").i, 100);

    /* Code that does not end, ended: by a limit on instructions, which nothing in the code can catch. */
    mg_dvm_get_stats(&before);
    mg_dvm_set_step_limit(50000);
    call("t/Misc", "spin", "()V");
    CHECK_STR(thrown(), "java.lang.InternalError: the step limit is used up in t/Misc.spin");
    /* (It ran out wherever execution stood, which may be inside the constructor of the exception this method keeps catching.) */
    call("t/Misc", "stubborn", "()V");
    CHECK(!strncmp(thrown(), "java.lang.InternalError: the step limit is used up in ", 54));
    /* The limit is for each call from C: what fits within it is not disturbed. */
    A[0].i = 1000;
    CHECK_INT(call("t/Arith", "sumTo", "(I)I").i, 500500);
    CHECK_STR(thrown(), "");
    mg_dvm_set_step_limit(0);
    mg_dvm_get_stats(&after);
    CHECK(after.instructions - before.instructions > 100000 && after.instructions - before.instructions < 200000);
    A[0].i = 100000;
    CHECK_INT(call("t/Arith", "sumTo", "(I)I").i, (i32)(u32)(100000ull * 100001ull / 2));
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------- what is refused */

static void test_refused(const char *fixtures)
{
    char *path = mg_sprintf("%s/a.dex", fixtures);
    size_t size = 0, i;
    u8 *file = mg_file_read_all(path, &size), *copy;
    mg_dvm_stats stats;
    u32 dexes_classes;
    say("what is refused");
    free(path);

    /* instructions that are not implemented: an exception that names them, which the code can catch; the rest of the file runs */
    A[0].l = mg_dvm_new_string("x");
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
    CHECK(mg_dvm_class_object("t/Loop") == NULL);
    CHECK_STR(thrown(), "java.lang.ClassCircularityError: t/Loop");
    CHECK(mg_dvm_class_object("t/BadSuper") == NULL);
    CHECK_STR(thrown(), "java.lang.IncompatibleClassChangeError: t/BadSuper has the interface t/HasArea for its superclass");
    call("t/Orphan", "touch", "()I");
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/Orphan)");
    CHECK(mg_dvm_class_object("t/OrphanChild") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/OrphanChild)");
    CHECK(mg_dvm_class_object("[[Lt/Orphan;") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by [[Lt/Orphan;)");
    /* and asking again gives the same answer, not a half-linked class */
    CHECK(mg_dvm_class_object("t/OrphanChild") == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/OrphanChild)");

    /* files that are not DEX files, or not whole: refused when they are added, and nothing of them is kept */
    mg_dvm_get_stats(&stats);
    dexes_classes = stats.classes_defined;
    CHECK(file != NULL && size > 0x70);
    if (file && size > 0x70) {
        copy = (u8 *)mg_alloc(size + 8);
        CHECK(!mg_dvm_add_dex("this is not a DEX file, whatever its name says, and it is long enough to hold a header that it does not have....................", 120, "junk"));
        memcpy(copy + 1, file, size);
        CHECK(!mg_dvm_add_dex(copy + 1, size, "at an odd address"));
        memcpy(copy, file, size);
        CHECK(!mg_dvm_add_dex(copy, 0x60, "cut short"));
        CHECK(!mg_dvm_add_dex(copy, size - 1, "one byte missing"));
        copy[6] = '4';
        CHECK(!mg_dvm_add_dex(copy, size, "version 034"));
        copy[6] = '5';
        copy[size - 9] ^= 0x40;
        CHECK(!mg_dvm_add_dex(copy, size, "one bit changed"));
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
            CHECK(!mg_dvm_add_dex(copy, size, "a table outside the file"));
            memcpy(copy + i, saved, 4);
        }
        free(copy);
    }
    free(file);
    CHECK(!mg_dvm_add_dex_file("no/such/file.dex"));
    CHECK_INT(mg_dvm_add_apk("no/such/file.apk"), 0);
    mg_dvm_get_stats(&stats);
    CHECK_INT(stats.classes_defined, dexes_classes);
}

static void test_misc(void)
{
    mg_dvm_stats stats;
    mg_jobj *o;
    say("the rest");
    A[0].i = 5;
    CHECK_INT(call("t/Misc", "jumps", "(I)I").i, 8);
    CHECK_INT(call("t/Misc", "callMany", "()I").i, 385);
    o = call("t/Misc", "arrayClass", "()Ljava/lang/Class;").l;
    CHECK(o && o->kind == MG_J_CLASS && o == mg_dvm_class_object("[I"));
    CHECK_STR(mg_dvm_class_name(o), "java/lang/Class");
    CHECK_STR(text_of(vcall(o, "getName", "()Ljava/lang/String;").l), "[I");
    CHECK_STR(text_of(vcall(mg_dvm_class_object("[[Ljava/lang/String;"), "getSimpleName", "()Ljava/lang/String;").l), "String[][]");
    CHECK_STR(text_of(vcall(mg_dvm_class_object("t/HasArea"), "toString", "()Ljava/lang/String;").l), "interface t.HasArea");
    A[0].l = mg_dvm_new_string("t.Rect");
    o = call("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;").l;
    CHECK(o && o == mg_dvm_class_object("t/Rect"));
    A[0].l = mg_dvm_new_string("t.NotThere");
    call("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;");
    CHECK_STR(thrown(), "java.lang.ClassNotFoundException: t.NotThere");
    CHECK(mg_dvm_running());
    mg_dvm_get_stats(&stats);
    CHECK(stats.classes_linked > 60 && stats.classes_initialised > 20 && stats.objects > 500 && stats.object_bytes > stats.objects * 32 && stats.strings_interned > 50);
    CHECK_STR(thrown(), "");
}

/* ------------------------------------------------------------ DEX files viewed when needed */

static void le16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void le32(u8 *p, u32 v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }

/* An APK of one file, stored as it is on a four-byte boundary (as zipalign puts it, with its extra field); where its data starts. */
static bool write_stored_apk(const char *path, const char *name, const u8 *data, size_t size, size_t *data_at)
{
    size_t n = strlen(name), local = 30 + n + 7, total = local + size + 46 + n + 22;
    u8 *z = (u8 *)calloc(1, total), *c, *e;
    u32 crc = (u32)crc32(crc32(0L, Z_NULL, 0), data, (uInt)size);
    bool ok;
    if (!z) return false;
    le32(z, 0x04034b50u);
    le16(z + 4, 20);
    le32(z + 14, crc);
    le32(z + 18, (u32)size);
    le32(z + 22, (u32)size);
    le16(z + 26, (u32)n);
    le16(z + 28, 7);
    memcpy(z + 30, name, n);
    le16(z + 30 + n, 0xD935);          /* zipalign's: the alignment, and the padding */
    le16(z + 30 + n + 2, 3);
    le16(z + 30 + n + 4, 4);
    memcpy(z + local, data, size);
    c = z + local + size;
    le32(c, 0x02014b50u);
    le16(c + 4, 20);
    le16(c + 6, 20);
    le32(c + 16, crc);
    le32(c + 20, (u32)size);
    le32(c + 24, (u32)size);
    le16(c + 28, (u32)n);
    memcpy(c + 46, name, n);
    e = c + 46 + n;
    le32(e, 0x06054b50u);
    le16(e + 8, 1);
    le16(e + 10, 1);
    le32(e + 12, (u32)(46 + n));
    le32(e + 16, (u32)(local + size));
    ok = mg_file_write_all(path, z, total);
    free(z);
    *data_at = local;
    return ok;
}

/*
 * An APK's DEX file that it stores as it is: registered by its classes' names, not viewed until the first of its classes is
 * linked, and viewed then. One that changed on the disk after its classes were registered (here: a byte of it, in a scope of
 * its own, as a service's module's) is not taken: its class is a NoClassDefFoundError, and nothing else is the worse for it.
 */
void test_lazy_dex(const char *fixtures, const char *scratch)
{
    /* A fixture whose class nothing has defined yet (on libcore the benchmark's is, and the in-memory test's may be). */
    static const char *const files[][2] = { { "bench.dex", "bench/Bench" }, { "m.dex", "t/FromMemory" } };
    char *bench = NULL, *bad = mg_sprintf("%s/lazy-bad.apk", scratch), *good = mg_sprintf("%s/lazy-good.apk", scratch);
    const char *cls = NULL;
    size_t size = 0, at = 0, k;
    u8 *dex = NULL;
    u64 read0 = 0, viewed0 = 0, read1 = 0, viewed1 = 0;
    say("DEX files viewed once a class of them is linked");
    for (k = 0; k < MG_COUNT(files) && !dex; k++) {
        if (mg_dvm_class_exists(files[k][1]) || mg_dvm_class_exists_scope(files[k][1], 15)) continue;
        free(bench);
        bench = mg_sprintf("%s/%s", fixtures, files[k][0]);
        dex = mg_file_read_all(bench, &size);
        cls = files[k][1];
    }
    if (!dex) {
        say("  (no fixture whose class is not defined already: left out here)");
        goto done;
    }
    mg_path_mkdirs(scratch);
    CHECK(write_stored_apk(bad, "classes.dex", dex, size, &at));
    CHECK_INT(mg_dvm_add_apk_scope(bad, 15), 1);
    {
        u8 *apk = NULL;
        size_t apk_size = 0;
        apk = mg_file_read_all(bad, &apk_size);
        CHECK(apk && apk_size > at + size / 2);
        if (apk && apk_size > at + size / 2) {
            apk[at + size / 2] ^= 0x5A;
            CHECK(mg_file_write_all(bad, apk, apk_size));
        }
        free(apk);
    }
    CHECK(mg_dvm_find_class_scope(cls, 15) == NULL);
    CHECK(!strncmp(thrown(), "java.lang.NoClassDefFoundError: ", 32));
    CHECK(write_stored_apk(good, "classes.dex", dex, size, &at));
    mg_dvm_dex_memory(&read0, &viewed0);
    CHECK_INT(mg_dvm_add_apk(good), 1);
    mg_dvm_dex_memory(&read1, &viewed1);
    CHECK(read1 == read0 && viewed1 == viewed0);
    CHECK(mg_dvm_class_exists(cls));
    mg_dvm_dex_memory(&read1, &viewed1);
    CHECK(viewed1 == viewed0);
    CHECK(mg_dvm_find_class(cls) != NULL);
    CHECK_STR(thrown(), "");
    mg_dvm_dex_memory(&read1, &viewed1);
    CHECK(read1 == read0 && viewed1 >= viewed0 + size);
done:
    free(dex);
    free(good);
    free(bad);
    free(bench);
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
