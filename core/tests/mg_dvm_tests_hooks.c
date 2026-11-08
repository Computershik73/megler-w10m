/*
 * Methods an app hooks (core/src/dvm/mg_dvm_hook.c): Xposed's XposedBridge, its natives as Aliucord's Aliuhook declares
 * them, on libcore.
 *
 * The fixture (core/tests/dvm/hooks, hooks.dex: tools/dvm-hooks-fixture.sh) hooks with a context like Aliuhook's HookInfo
 * an instance method, static ones, a constructor, an interface's default method, a native method, and reports what the
 * callbacks saw and what the calls gave: called from bytecode, through reflection, by a subclass and an override's super
 * call, the original run by the backup, arguments and results changed, exceptions each way, unhooked, on two threads. Here
 * those reports are checked, and so are calls of hooked methods through JNI, calls from loops the JIT compiled (at its
 * threshold of 1, at their first jump back) before the first hook and after it, and a hook that nothing but the runtime
 * keeps, across collections. Where guest code runs, a library that stands in for Aliucord's libaliuhook.so is loaded
 * without its JNI_OnLoad, which fails, and none of its functions is called.
 */
#include <stdio.h>

#include "../src/dvm/mg_dvm_internal.h"
#include "../src/jni/mg_jni.h"
#include "../src/ld/mg_ld.h"
#include "mg_dvm_tests.h"

typedef void *ENV;
#define E ((ENV)mg_jni_env())
/* The n-th function of the JNIEnv. */
#define J(n, type) ((type)(*(void ***)mg_jni_env())[n])

/* Hook's modes (core/tests/dvm/hooks/src/hooks/Hook.java). */
enum { MODE_CALL, MODE_RESULT, MODE_ARGS, MODE_THROW, MODE_SKIP, MODE_WRONG, MODE_COUNT };

static void native_add(mg_jcall *c) { c->ret.i = c->args[0].i + c->args[1].i; }

static const mg_jhle k_hooks_natives[] = {
    { "hooks/Target", "nativeAdd", "(II)I", native_add },
    { NULL, NULL, NULL, NULL },
};

static void hook_on(const char *which, int mode)
{
    A[0].l = mg_dvm_new_string(which);
    A[1].i = mode;
    call("hooks/Hooks", "on", "(Ljava/lang/String;I)V");
}

static bool hook_off(const char *which)
{
    A[0].l = mg_dvm_new_string(which);
    return call("hooks/Hooks", "off", "(Ljava/lang/String;)Z").z != 0;
}

static i32 calls_of(const char *which)
{
    A[0].l = mg_dvm_new_string(which);
    return call("hooks/Hooks", "calls", "(Ljava/lang/String;)I").i;
}

static i32 loop(const char *name, mg_jobj *t, i32 n)
{
    A[0].l = t;
    A[1].i = n;
    return call("hooks/Target", name, "(Lhooks/Target;I)I").i;
}

static i32 sum_twice(i32 n)
{
    A[0].i = n;
    return call("hooks/Target", "sumTwice", "(I)I").i;
}

#if MG_GUEST_CODE
/* A library like libaliuhook.so: loaded, its JNI_OnLoad (which returns JNI_ERR) not run, its exported natives not called. */
static int *g_library_calls;

static void test_library(const char *guest_dir)
{
    mg_lib *lib;
    say("  a library like Aliucord's libaliuhook.so: loaded without its JNI_OnLoad, XposedBridge's natives the runtime's");
    mg_ld_add_search_dir(guest_dir);
    CHECK(mg_dvm_load_library("libt_xposed.so"));
    CHECK_STR(thrown(), "");
    lib = mg_ld_find_lib("libt_xposed.so");
    g_library_calls = lib ? (int *)mg_ld_sym(lib, "t_xposed_called") : NULL;
    CHECK(g_library_calls != NULL);
    /* The library exports this one too: the runtime's is called. */
    CHECK(call("de/robv/android/xposed/XposedBridge", "disableHiddenApiRestrictions", "()Z").z == 1);
    CHECK_STR(thrown(), "");
    if (g_library_calls) CHECK_INT(*g_library_calls, 0);
}
#endif

/*
 * Loops calling a getter (done in place in their compiled code), a static method and an instance method (called the quick
 * way, into their compiled code), compiled before anything is hooked; then the first hook, which forgets the code that
 * does calls in place and keeps the rest; the loops again, hooked, each call seen by its hook; a getter hooked after the
 * code that does it in place was compiled with hooks about, the same code then calling the hook; and all unhooked.
 */
static void test_compiled(void)
{
    u32 was = g_dvm_jit_threshold, in_place = g_dvm_jit_in_place;
    mg_dvm_class *tc;
    mg_dvm_method *values, *values2, *twice, *adds;
    mg_jobj *t;
    mg_jvalue v;
    say("  hooked methods called from compiled loops");
    mg_dvm_jit_start(true, 1, false);
    v.j = 0;
    v.i = 3;
    t = mg_dvm_new("hooks/Target", "(I)V", &v);
    tc = mg_dvm_find_class("hooks/Target");
    values = tc ? mg_dvm_find_method(tc, "sumValues", "(Lhooks/Target;I)I", true) : NULL;
    values2 = tc ? mg_dvm_find_method(tc, "sumValues2", "(Lhooks/Target;I)I", true) : NULL;
    twice = tc ? mg_dvm_find_method(tc, "sumTwice", "(I)I", true) : NULL;
    adds = tc ? mg_dvm_find_method(tc, "sumAdds", "(Lhooks/Target;I)I", true) : NULL;
    CHECK(t && values && values2 && twice && adds);
    if (!t || !values || !values2 || !twice || !adds) {
        g_dvm_jit_threshold = was;
        return;
    }
    CHECK_INT(loop("sumValues", t, 2000), 6000);
    CHECK_INT(loop("sumValues2", t, 2000), 6000);
    CHECK_INT(sum_twice(2000), 3998000);
    CHECK_INT(loop("sumAdds", t, 2000), 2005000);
    CHECK_STR(thrown(), "");
#if MG_DVM_JIT
    CHECK(values->jit && values->jit->in_place && values2->jit && values2->jit->in_place && g_dvm_jit_in_place > in_place);
    CHECK(twice->jit && !twice->jit->in_place && adds->jit && !adds->jit->in_place);
#else
    (void)in_place;
#endif
    /* The first hooks: the code that does calls in place forgotten, the rest kept. */
    hook_on("getValue", MODE_RESULT);
    hook_on("twice", MODE_RESULT);
    hook_on("add", MODE_CALL);
    CHECK_STR(thrown(), "");
#if MG_DVM_JIT
    CHECK(!values->jit && !values2->jit && twice->jit && adds->jit);
#endif
    CHECK_INT(loop("sumValues", t, 2000), 2000 * (3 + 1000));
    CHECK_INT(calls_of("getValue"), 2000);
    CHECK_INT(sum_twice(2000), 3998000 + 2000 * 1000);
    CHECK_INT(calls_of("twice"), 2000);
    CHECK_INT(loop("sumAdds", t, 2000), 2005000);
    CHECK_INT(calls_of("add"), 2000);
    CHECK_STR(thrown(), "");
#if MG_DVM_JIT
    CHECK(values->jit != NULL);
#endif
    /* Compiled with hooks about, its getter not hooked: done in place, after a look at the hook. Then hooked: the same code. */
    CHECK_INT(loop("sumValues2", t, 2000), 6000);
    {
        mg_dvm_jit_code *code = values2->jit;
#if MG_DVM_JIT
        CHECK(code && code->in_place);
#endif
        hook_on("getValue2", MODE_RESULT);
        CHECK_INT(loop("sumValues2", t, 2000), 2000 * (3 + 1000));
        CHECK_INT(calls_of("getValue2"), 2000);
        CHECK(values2->jit == code);
    }
    /* All unhooked: the originals, and the hooks not called. */
    CHECK(hook_off("getValue") && hook_off("getValue2") && hook_off("twice") && hook_off("add"));
    CHECK_INT(loop("sumValues", t, 2000), 6000);
    CHECK_INT(loop("sumValues2", t, 2000), 6000);
    CHECK_INT(sum_twice(2000), 3998000);
    CHECK_INT(loop("sumAdds", t, 2000), 2005000);
    CHECK_INT(calls_of("getValue") + calls_of("getValue2") + calls_of("twice") + calls_of("add"), 8000);
    CHECK_STR(thrown(), "");
    g_dvm_jit_threshold = was;
}

/* Hooked methods called through JNI, as an app's native code calls them: virtually, exactly, statically, a constructor. */
static void test_jni_calls(void)
{
    mg_jobj *tc, *t;
    void *ctor, *add, *twice, *mix;
    mg_jvalue a[9];
    say("  hooked methods called through JNI");
    tc = J(6, mg_jobj *(*)(ENV, const char *))(E, "hooks/Target");                                          /* FindClass */
    CHECK(tc != NULL);
    if (!tc) return;
    ctor = J(33, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, tc, "<init>", "(I)V");           /* GetMethodID */
    add = J(33, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, tc, "add", "(I)I");
    twice = J(113, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, tc, "twice", "(I)I");          /* GetStaticMethodID */
    mix = J(113, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, tc, "mix", "(ZBCSIJFDLjava/lang/String;)J");
    CHECK(ctor && add && twice && mix);
    if (!ctor || !add || !twice || !mix) return;
    hook_on("init", MODE_CALL);
    hook_on("add", MODE_CALL);
    hook_on("twice", MODE_RESULT);
    hook_on("mix", MODE_CALL);
    memset(a, 0, sizeof(a));
    a[0].i = 21;
    t = J(30, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, tc, ctor, a);                      /* NewObjectA */
    CHECK(t != NULL);
    if (!t) return;
    CHECK(mg_dvm_get_field(t, "value", &a[1]) && a[1].i == 21);
    CHECK_INT(calls_of("init"), 1);
    a[0].i = 3;
    CHECK_INT(J(51, i32 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, t, add, a), 24);                 /* CallIntMethodA */
    a[0].i = 4;
    CHECK_INT(J(81, i32 (*)(ENV, mg_jobj *, mg_jobj *, void *, const mg_jvalue *))(E, t, tc, add, a), 25);  /* CallNonvirtualIntMethodA */
    CHECK_INT(calls_of("add"), 2);
    a[0].i = 5;
    CHECK_INT(J(131, i32 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, tc, twice, a), 1010);          /* CallStaticIntMethodA */
    /* Every kind of argument, each set as native code sets one (that member alone), boxed for the hook as it is. */
    memset(a, 0xA5, sizeof(a));
    a[0].z = 1;
    a[1].b = -2;
    a[2].c = 'c';
    a[3].s = -300;
    a[4].i = 70000;
    a[5].j = (i64)1 << 40;
    a[6].f = 2.5f;
    a[7].d = -1.5;
    a[8].l = mg_dvm_new_string("four");
    CHECK(J(134, i64 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, tc, mix, a) == ((i64)1 << 40) + 69803);   /* CallStaticLongMethodA */
    A[0].l = mg_dvm_new_string("mix");
    CHECK_STR(text_of(call("hooks/Hooks", "seen", "(Ljava/lang/String;)Ljava/lang/String;").l), "(Ztrue,B-2,Cc,S-300,I70000,J1099511627776,F25,D-15,Lfour)");
    CHECK_STR(thrown(), "");
    CHECK(hook_off("init") && hook_off("add") && hook_off("twice") && hook_off("mix"));
}

/* A hook nothing keeps but the runtime, its context and its backup's Method: alive across collections. */
static void test_roots(void)
{
    int i;
    say("  a hook kept by the runtime alone, across collections");
    A[0].l = mg_dvm_new_string("twice");
    call("hooks/Hooks", "orphan", "(Ljava/lang/String;)V");
    CHECK_STR(thrown(), "");
    for (i = 0; i < 3; i++) {
        mg_dvm_gc("hooks test", NULL);
        A[0].i = 7;
        CHECK_INT(call("hooks/Target", "twice", "(I)I").i, 14);
    }
    CHECK_INT(call("hooks/Hooks", "counted", "()I").i, 3);
    A[0].l = mg_dvm_new_string("twice");
    CHECK(call("hooks/Hooks", "unhook", "(Ljava/lang/String;)Z").z != 0);
    CHECK_STR(thrown(), "");
}

void test_hooks(const char *fixtures, const char *guest_dir)
{
    char *path = mg_sprintf("%s/hooks.dex", fixtures);
    bool added;
    say("methods an app hooks (Xposed's XposedBridge, its natives as Aliucord's Aliuhook declares them)");
    mg_dvm_register(k_hooks_natives);
    added = mg_dvm_add_dex_file(path);
    CHECK(added);
    free(path);
    if (!added) return;
#if MG_GUEST_CODE
    if (guest_dir) test_library(guest_dir);
#else
    (void)guest_dir;
#endif
    /* (First: what is compiled before the first hook of the process.) */
    test_compiled();
    CHECK_STR(text_of(call("hooks/Hooks", "instance", "()Ljava/lang/String;").l), "8 9 3 50 15 true 4 (LT5,I3)(LT5,I4)(LT1,I2)(LT2,I3) true false false 8 4");
    CHECK_STR(thrown(), "");
    CHECK_STR(text_of(call("hooks/Hooks", "statics", "()Ljava/lang/String;").l), "1099511697579 (Ztrue,B-2,Cc,S-300,I70000,J1099511627776,F25,D-15,Lfour) 1008 8 1");
    CHECK_STR(thrown(), "");
    CHECK_STR(text_of(call("hooks/Hooks", "constructors", "()Ljava/lang/String;").l), "7 8 9 0 11 true 12 0 5 4 1 (LT0,I7)(LT0,I8)(LT0,I9)(LT0,I12)");
    CHECK_STR(thrown(), "");
    CHECK_STR(text_of(call("hooks/Hooks", "exceptions", "()Ljava/lang/String;").l),
              "ArithmeticException 3 from the hook java.lang.IllegalStateException NullPointerException ClassCastException ClassCastException x6");
    CHECK_STR(thrown(), "");
    CHECK_STR(text_of(call("hooks/Hooks", "more", "()Ljava/lang/String;").l),
              "hello! hello 1042 init hook 1005 5 2 1 1 102 (LT2,I1) true false true truetruetrue truetruefalsetrue");
    CHECK_STR(thrown(), "");
    /* The same with every instruction watched (the interpreter's other loop), which compiled code does not run under. */
    mg_dvm_set_step_limit(1000000000000ull);
    CHECK_STR(text_of(call("hooks/Hooks", "instance", "()Ljava/lang/String;").l), "8 9 3 50 15 true 4 (LT5,I3)(LT5,I4)(LT1,I2)(LT2,I3) true false false 8 4");
    mg_dvm_set_step_limit(0);
    CHECK_STR(thrown(), "");
    test_jni_calls();
    test_roots();
    CHECK_STR(text_of(call("hooks/Hooks", "threads", "()Ljava/lang/String;").l), "0 true");
    CHECK_STR(thrown(), "");
#if MG_GUEST_CODE
    if (g_library_calls) CHECK_INT(*g_library_calls, 0);
#endif
}
