/*
 * The runtime and the JNI layer as one Java world.
 *
 * First from this side, on every host: the JNIEnv's own functions, called as
 * native code calls them (through the table, by their places in it), on
 * classes the fixtures define -- classes found, methods and fields looked up,
 * virtual and exact calls, objects made, strings and arrays both ways,
 * exceptions across the boundary -- and on a class no DEX file defines, which
 * stays the layer's own and whose objects bytecode can still hold.
 *
 * Then, where guest code can run, with a guest library (guest/t_jni.c): its
 * native methods called from bytecode and from here, and calling back.
 */
#include <stdio.h>

#include "../src/arm/mg_stubs.h"
#include "../src/bionic/mg_bionic.h"
#include "../src/dvm/mg_dvm_internal.h"
#include "../src/jni/mg_jni.h"
#include "../src/ld/mg_ld.h"
#include "mg_dvm_tests.h"

typedef void *ENV;
#define E ((ENV)mg_jni_env())
/* The n-th function of the JNIEnv. */
#define J(n, type) ((type)(*(void ***)mg_jni_env())[n])

static mg_jobj *find(const char *name) { return J(6, mg_jobj *(*)(ENV, const char *))(E, name); }
static void *method(mg_jobj *cls, const char *name, const char *sig) { return J(33, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, cls, name, sig); }
static void *smethod(mg_jobj *cls, const char *name, const char *sig) { return J(113, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, cls, name, sig); }
static void *field(mg_jobj *cls, const char *name, const char *sig) { return J(94, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, cls, name, sig); }
static mg_jobj *make(mg_jobj *cls, void *ctor, const mg_jvalue *args) { return J(30, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, cls, ctor, args); }
static i32 call_int(mg_jobj *o, void *m, const mg_jvalue *args) { return J(51, i32 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, o, m, args); }
static mg_jobj *call_obj(mg_jobj *o, void *m, const mg_jvalue *args) { return J(36, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, o, m, args); }
static i32 scall_int(mg_jobj *c, void *m, const mg_jvalue *args) { return J(131, i32 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, c, m, args); }
static bool check(void) { return J(228, u8 (*)(ENV))(E) != 0; }
static const char *utf(mg_jobj *s) { return J(169, const char *(*)(ENV, mg_jobj *, u8 *))(E, s, NULL); }

static void thing_answer(mg_jcall *c) { c->ret.i = c->self ? 42 + c->args[0].i : -1; }

static const mg_jhle k_thing[] = {
    { "megler/test/Thing", "answer", "(I)I", thing_answer },
    { NULL, NULL, NULL, NULL },
};

static void test_env(void)
{
    mg_jobj *rect_c, *square_c, *area_i, *arith, *wide, *exc, *rect, *square, *blob, *s, *a, *e, *thing_c, *thing;
    void *ctor, *area, *iarea, *describe, *w, *name, *sj, *m;
    mg_jvalue v[4];
    i32 n = 0;
    static const u16 wide_text[] = { 'z', 0x4E2D };
    static const i32 four[] = { 1, 2, 3, 4 };
    i32 back[4];
    say("the JNIEnv on the runtime's classes");
    memset(v, 0, sizeof(v));

    /* classes */
    rect_c = find("t/Rect");
    square_c = find("t/Square");
    area_i = find("t/HasArea");
    CHECK(rect_c && rect_c->kind == MG_J_CLASS && rect_c->dvm && rect_c == mg_dvm_class_object("t/Rect") && rect_c == mg_jni_class_object("t/Rect"));
    CHECK_STR(mg_jni_class_name(rect_c), "t/Rect");
    CHECK(J(10, mg_jobj *(*)(ENV, mg_jobj *))(E, square_c) == rect_c);                         /* GetSuperclass */
    CHECK(J(10, mg_jobj *(*)(ENV, mg_jobj *))(E, area_i) == NULL);
    CHECK(J(11, u8 (*)(ENV, mg_jobj *, mg_jobj *))(E, square_c, area_i) == 1);                 /* IsAssignableFrom */
    CHECK(J(11, u8 (*)(ENV, mg_jobj *, mg_jobj *))(E, rect_c, square_c) == 0);
    CHECK(find("[Lt/Rect;") == mg_dvm_class_object("[Lt/Rect;"));
    CHECK(!check());
    CHECK(find("t/NotThereAtAll") != NULL);                                                    /* nothing defines it: one of the layer's own, as ever */
    CHECK(find("t/Orphan") == NULL && check());                                                /* something defines it and it cannot be linked */
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Parent (needed by t/Orphan)");
    /* Finding a class does not initialise it (Android's FindClass does not either); asking for something of it does. This one's initialiser threw, long ago. */
    CHECK(find("t/Boom") != NULL && !check());
    CHECK(smethod(find("t/Boom"), "touch", "()I") == NULL && check());
    CHECK(!strncmp(thrown(), "java.lang.NoClassDefFoundError: Could not initialize class t/Boom | caused by java.lang.ExceptionInInitializerError", 115));

    /* objects and calls: virtual, exact, through an interface */
    ctor = method(rect_c, "<init>", "(II)V");
    area = method(rect_c, "area", "()I");
    CHECK(ctor && area);
    v[0].i = 3;
    v[1].i = 4;
    rect = make(rect_c, ctor, v);
    v[0].i = 4;
    square = make(square_c, method(square_c, "<init>", "(I)V"), v);
    v[0].i = 5;
    blob = make(find("t/Blob"), method(find("t/Blob"), "<init>", "(I)V"), v);
    CHECK(rect && square && blob && rect->dvm && !check());
    CHECK_INT(call_int(rect, area, NULL), 12);
    CHECK_INT(call_int(square, area, NULL), 1016);                                             /* Rect's id, Square's method */
    CHECK_INT(J(81, i32 (*)(ENV, mg_jobj *, mg_jobj *, void *, const mg_jvalue *))(E, square, rect_c, area, NULL), 16);   /* CallNonvirtualIntMethodA */
    iarea = method(area_i, "area", "()I");
    describe = method(area_i, "describe", "()Ljava/lang/String;");
    CHECK_INT(call_int(blob, iarea, NULL), 5);
    CHECK_INT(call_int(square, iarea, NULL), 1016);
    CHECK_STR(utf(call_obj(rect, describe, NULL)), "area 12");
    CHECK(J(31, mg_jobj *(*)(ENV, mg_jobj *))(E, square) == square_c);                         /* GetObjectClass */
    CHECK(J(32, u8 (*)(ENV, mg_jobj *, mg_jobj *))(E, square, area_i) == 1);                   /* IsInstanceOf */
    CHECK(J(32, u8 (*)(ENV, mg_jobj *, mg_jobj *))(E, blob, rect_c) == 0);
    arith = find("t/Arith");
    v[0].i = 2;
    v[1].i = 3;
    CHECK_INT(scall_int(arith, smethod(arith, "sum", "(II)I"), v), 5);
    wide = find("t/Wide");
    v[0].i = 7;
    v[1].j = 0x100000000ll;
    v[2].i = -3;
    v[3].j = 5;
    CHECK(J(134, i64 (*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, wide, smethod(wide, "mix", "(IJIJ)J"), v) == (7 + 0x100000000ll) * -3 - 5);
    CHECK(!check());

    /* what is not there is an exception and no id, as on Android */
    CHECK(method(rect_c, "nothing", "()V") == NULL && check());
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: no non-static method \"t/Rect.nothing()V\"");
    CHECK(smethod(rect_c, "area", "()I") == NULL);
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: no static method \"t/Rect.area()I\"");
    CHECK(field(rect_c, "w", "J") == NULL);
    CHECK_STR(thrown(), "java.lang.NoSuchFieldError: no \"J\" field \"w\" in class \"t/Rect\" or its superclasses");
    CHECK_INT(call_int(rect, NULL, NULL), 0);                                                  /* and a caller that goes on with no id gets zero */

    /* fields */
    w = field(rect_c, "w", "I");
    name = field(square_c, "name", "Ljava/lang/String;");                                      /* Shape's, asked of Square */
    CHECK(w && name);
    CHECK_INT(J(100, i32 (*)(ENV, mg_jobj *, void *))(E, rect, w), 3);                         /* GetIntField */
    J(109, void (*)(ENV, mg_jobj *, void *, i32))(E, rect, w, 7);                              /* SetIntField */
    CHECK_INT(call_int(rect, area, NULL), 28);
    CHECK_STR(utf(J(95, mg_jobj *(*)(ENV, mg_jobj *, void *))(E, square, name)), "square");    /* GetObjectField */
    sj = J(144, void *(*)(ENV, mg_jobj *, const char *, const char *))(E, wide, "sj", "J");    /* GetStaticFieldID */
    J(160, void (*)(ENV, mg_jobj *, void *, i64))(E, wide, sj, 0x1122334455667788ll);          /* SetStaticLongField */
    CHECK(J(151, i64 (*)(ENV, mg_jobj *, void *))(E, wide, sj) == 0x1122334455667788ll);       /* GetStaticLongField */
    CHECK(mg_dvm_get_static("t/Wide", "sj", &v[0]) && v[0].j == 0x1122334455667788ll);
    CHECK(mg_jni_get_field(rect, "h").i == 4);
    v[0].j = 0;
    v[0].i = 2;
    mg_jni_set_field(rect, "h", 'I', v[0]);
    CHECK_INT(call_int(rect, area, NULL), 14);

    /* strings: made here, read by bytecode; made by bytecode, read here */
    s = J(167, mg_jobj *(*)(ENV, const char *))(E, "h\xc3\xa9llo");                             /* NewStringUTF */
    CHECK(s && s->dvm && s->kind == MG_J_STRING);
    CHECK_INT(J(164, i32 (*)(ENV, mg_jobj *))(E, s), 5);                                       /* GetStringLength */
    CHECK_INT(J(165, const u16 *(*)(ENV, mg_jobj *, u8 *))(E, s, NULL)[1], 0xE9);              /* GetStringChars */
    CHECK_STR(utf(s), "h\xc3\xa9llo");
    CHECK_INT(J(168, i32 (*)(ENV, mg_jobj *))(E, s), 6);                                       /* GetStringUTFLength */
    CHECK_STR(mg_jni_string(s), "h\xc3\xa9llo");
    exc = find("t/Exc");
    v[0].j = 0;
    v[0].l = s;
    v[1].i = 1;
    CHECK_INT(scall_int(exc, smethod(exc, "charAt", "(Ljava/lang/String;I)I"), v), 0xE9);
    s = J(163, mg_jobj *(*)(ENV, const u16 *, i32))(E, wide_text, 2);                          /* NewString */
    CHECK_STR(utf(s), "z\xe4\xb8\xad");
    s = J(116, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, find("t/Strings"), smethod(find("t/Strings"), "grown", "()Ljava/lang/String;"), NULL);
    CHECK_INT(J(164, i32 (*)(ENV, mg_jobj *))(E, s), 40);
    CHECK_STR(utf(s), "0123456789012345678901234567890123456789");
    s = make(find("java/lang/String"), method(find("java/lang/String"), "<init>", "(Ljava/lang/String;)V"), v);   /* NewObject of a String */
    CHECK(s && s != v[0].l && s->kind == MG_J_STRING);
    CHECK_STR(utf(s), "h\xc3\xa9llo");

    /* arrays */
    a = J(179, mg_jobj *(*)(ENV, i32))(E, 4);                                                  /* NewIntArray */
    CHECK(a && a->dvm && a->kind == MG_J_PRIM_ARRAY && J(171, i32 (*)(ENV, mg_jobj *))(E, a) == 4);
    CHECK_STR(mg_jni_class_name(a), "[I");
    J(211, void (*)(ENV, mg_jobj *, i32, i32, const void *))(E, a, 0, 4, four);                /* SetIntArrayRegion */
    v[0].l = a;
    CHECK_INT(scall_int(find("t/Guest"), smethod(find("t/Guest"), "sumArray", "([I)I"), v), 10);
    J(203, void (*)(ENV, mg_jobj *, i32, i32, void *))(E, a, 1, 2, back);                      /* GetIntArrayRegion */
    CHECK(back[0] == 2 && back[1] == 3);
    CHECK(((i32 *)J(187, void *(*)(ENV, mg_jobj *, u8 *))(E, a, NULL))[3] == 4);                /* GetIntArrayElements */
    a = J(172, mg_jobj *(*)(ENV, i32, mg_jobj *, mg_jobj *))(E, 2, find("java/lang/String"), mg_jni_new_string("x"));   /* NewObjectArray */
    CHECK(a && a->dvm && a->kind == MG_J_OBJ_ARRAY && a->u.oarr.len == 2);
    CHECK_STR(mg_jni_class_name(a), "[Ljava/lang/String;");
    J(174, void (*)(ENV, mg_jobj *, i32, mg_jobj *))(E, a, 1, mg_jni_new_string("y"));         /* SetObjectArrayElement */
    v[0].l = a;
    CHECK_STR(utf(J(116, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, find("t/Guest"),
                                                                                  smethod(find("t/Guest"), "join", "([Ljava/lang/String;)Ljava/lang/String;"), v)), "x,y");
    CHECK_STR(utf(J(173, mg_jobj *(*)(ENV, mg_jobj *, i32))(E, a, 0)), "x");                   /* GetObjectArrayElement */
    CHECK(!check());
    J(174, void (*)(ENV, mg_jobj *, i32, mg_jobj *))(E, a, 0, rect);
    CHECK_STR(thrown(), "java.lang.ArrayStoreException: t/Rect cannot be stored in an array of type [Ljava/lang/String;");
    J(174, void (*)(ENV, mg_jobj *, i32, mg_jobj *))(E, a, 2, NULL);
    CHECK_STR(thrown(), "java.lang.ArrayIndexOutOfBoundsException: length=2; index=2");
    a = mg_jni_new_prim_array('D', 3);
    CHECK(a && a->dvm && a->u.arr.esz == 8 && a->u.arr.len == 3);
    a = mg_jni_new_object_array("t/Rect", 2);
    CHECK(a && a->dvm);
    CHECK_STR(mg_jni_class_name(a), "[Lt/Rect;");

    /* exceptions: bytecode's seen here, and one thrown here seen by the runtime */
    v[0].i = 7;
    v[1].i = 0;
    scall_int(exc, smethod(exc, "wrongCatch", "(II)I"), v);
    CHECK(check());
    e = J(15, mg_jobj *(*)(ENV))(E);                                                           /* ExceptionOccurred */
    CHECK(e && J(32, u8 (*)(ENV, mg_jobj *, mg_jobj *))(E, e, find("java/lang/RuntimeException")) == 1);
    J(17, void (*)(ENV))(E);                                                                   /* ExceptionClear */
    CHECK(!check() && mg_dvm_pending() == NULL);
    J(14, i32 (*)(ENV, mg_jobj *, const char *))(E, find("java/lang/IllegalStateException"), "from C");   /* ThrowNew */
    CHECK(check() && mg_dvm_pending() != NULL);
    CHECK_STR(thrown(), "java.lang.IllegalStateException: from C");
    J(13, i32 (*)(ENV, mg_jobj *))(E, e);                                                      /* Throw */
    CHECK_STR(thrown(), "java.lang.ArithmeticException: divide by zero");
    mg_jni_throw("java/lang/UnsupportedOperationException", "said the layer");
    CHECK(mg_jni_exception_clear() && !mg_jni_exception_clear());

    /* monitors */
    CHECK_INT(J(217, i32 (*)(ENV, mg_jobj *))(E, rect), 0);
    CHECK(mg_dvm_monitor_notify(rect, false));                                                 /* only the owner may */
    CHECK_INT(J(218, i32 (*)(ENV, mg_jobj *))(E, rect), 0);
    CHECK_INT(J(218, i32 (*)(ENV, mg_jobj *))(E, rect), -1);
    CHECK_STR(thrown(), "java.lang.IllegalMonitorStateException: MonitorExit on an object this thread has not locked");

    /* RegisterNatives: a method the class declares is bound; one it does not is said, and is not an error */
    {
        struct { mg_addr name, sig, fn; } table[2];
        mg_dvm_method *bound = mg_dvm_find_method(mg_dvm_find_class("t/Natives"), "viaJni", "(IJ)J", true);
        table[0].name = (mg_addr)"viaJni";
        table[0].sig = (mg_addr)"(IJ)J";
        table[0].fn = (mg_addr)0x1234;
        table[1].name = (mg_addr)"notDeclared";
        table[1].sig = (mg_addr)"()V";
        table[1].fn = (mg_addr)0x5678;
        if (sizeof(mg_addr) == 4) {
            /* The table is the guest's, of 32-bit words: it can only be made here where a pointer is one. */
            CHECK_INT(J(215, i32 (*)(ENV, mg_jobj *, const void *, i32))(E, find("t/Natives"), table, 2), 0);
            CHECK(bound && bound->nat && bound->nat->jni == (void *)(mg_addr)0x1234);
            if (bound && bound->nat) {
                bound->nat->jni = NULL;
                bound->nat->bound = false;
            }
        }
        CHECK(bound != NULL && !check());
    }
    /* RegisterNatives on a subclass for a native method its superclass declares: bound there, as ART looks up the superclasses
       (Reanimated registers NativeProxyCommon's installJSIBindings on NativeProxy; Discord drew nothing for the miss). */
    {
        struct { mg_addr name, sig, fn; } table[1];
        mg_dvm_method *inherited = mg_dvm_find_method(mg_dvm_find_class("t/NativeBase"), "installBindings", "()V", false);
        table[0].name = (mg_addr)"installBindings";
        table[0].sig = (mg_addr)"()V";
        table[0].fn = (mg_addr)0x4321;
        if (sizeof(mg_addr) == 4) {
            CHECK_INT(J(215, i32 (*)(ENV, mg_jobj *, const void *, i32))(E, find("t/NativeChild"), table, 1), 0);
            CHECK(inherited && inherited->nat && inherited->nat->jni == (void *)(mg_addr)0x4321);
            if (inherited && inherited->nat) {
                inherited->nat->jni = NULL;
                inherited->nat->bound = false;
            }
        }
        CHECK(inherited != NULL && !check());
    }

    /* A class no DEX file defines: the layer's own, implemented in C as before, and its objects are objects bytecode can hold. */
    mg_jni_register(k_thing);
    thing_c = find("megler/test/Thing");
    thing = mg_jni_new_object("megler/test/Thing");
    CHECK(thing_c && thing && thing->dvm && thing_c->dvm && thing_c == mg_jni_class_object("megler/test/Thing"));
    CHECK_STR(mg_jni_class_name(thing), "megler/test/Thing");
    m = method(thing_c, "answer", "(I)I");
    v[0].j = 0;
    v[0].i = 8;
    CHECK(m != NULL);
    CHECK_INT(call_int(thing, m, v), 50);
    CHECK(method(thing_c, "anythingElse", "()V") != NULL && !check());                         /* never refused: it logs when called */
    v[0].i = 9;
    mg_jni_set_field(thing, "n", 'I', v[0]);
    CHECK_INT(mg_jni_get_field(thing, "n").i, 9);
    CHECK_INT(J(100, i32 (*)(ENV, mg_jobj *, void *))(E, thing, field(thing_c, "n", "I")), 9);
    v[0].l = thing;
    CHECK_STR(utf(J(116, mg_jobj *(*)(ENV, mg_jobj *, void *, const mg_jvalue *))(E, find("t/Shapes"),
                                                                                  smethod(find("t/Shapes"), "className", "(Ljava/lang/Object;)Ljava/lang/String;"), v)),
              "megler.test.Thing");
    CHECK_INT(scall_int(find("t/Shapes"), smethod(find("t/Shapes"), "what", "(Ljava/lang/Object;)I"), v), 0);
    a = mg_jni_new_object_array("java/lang/Object", 1);
    J(174, void (*)(ENV, mg_jobj *, i32, mg_jobj *))(E, a, 0, thing);
    CHECK(a->u.oarr.v[0] == thing && !check());
    a = mg_jni_new_object_array("megler/test/Thing", 3);
    CHECK(a && a->dvm);
    CHECK_STR(mg_jni_class_name(a), "[Lmegler/test/Thing;");
    (void)n;
}

#if MG_HOST_ARM32
/*
 * A native call under a guard (mg_jni_call_native_guarded, as the runtime calls the native methods of code Android runs in a
 * process of its own): guest code that faults beyond repair -- the stubs' udf, an undefined instruction -- ends the call,
 * which says it faulted, and the thread goes on: its guard is cleared, and the calls after it, guarded or not, are made.
 */
static void test_guarded_call(void)
{
    void *udf = mg_stub(MG_STUB_UDF), *read_tp = mg_stub(MG_STUB_READ_TPIDRURW);
    bool faulted = true;
    mg_jvalue v;
    int i;
    say("a guarded native call: a fault in it ends the call, not the process");
    CHECK(udf != NULL && read_tp != NULL);
    if (!udf || !read_tp) return;
    v = mg_jni_call_native_guarded(read_tp, NULL, "()I", NULL, &faulted);
    CHECK(!faulted);
    for (i = 0; i < 3; i++) {
        faulted = false;
        v = mg_jni_call_native_guarded(udf, NULL, "()I", NULL, &faulted);
        CHECK(faulted && v.j == 0);
        CHECK(mg_gthread_self()->fault_guard == NULL);
    }
    v = mg_jni_call_native_guarded(read_tp, NULL, "()I", NULL, &faulted);
    CHECK(!faulted);
    (void)mg_jni_call_native(read_tp, NULL, "()I", NULL);
}
#endif

/* Wherever guest code runs: the library is the build's own ABI's (tools/build-guest.sh <abi>). */
#if MG_GUEST_CODE
static void test_guest_natives(const char *guest_dir)
{
    mg_lib *lib;
    void *onload;
    mg_jobj *g, *sub, *made, *arr;
    mg_jvalue v;
    int i;
    say("native methods in a guest library");
    mg_ld_add_search_dir(guest_dir);
    lib = mg_ld_load("libt_jni.so");
    CHECK(lib != NULL);
    if (!lib) return;
    CHECK(mg_ld_init(lib));
    onload = mg_ld_sym(lib, "JNI_OnLoad");
    CHECK(onload != NULL);
    if (!onload) return;
    /* It registers eleven methods and a twelfth the class does not have, which is said and not fatal. */
    CHECK_INT(MG_GFN(i32 (*)(void *, void *), onload)(mg_jni_vm(), NULL), 0x10006);
    CHECK_STR(thrown(), "");

    /* called from here: by its exported name, and registered ones with every kind of argument */
    A[0].i = 40;
    A[1].i = 2;
    CHECK_INT(call("t/Guest", "add", "(II)I").i, 42);
    A[0].i = 7;
    A[1].j = (i64)1 << 40;
    A[2].d = 2.5;
    A[3].f = 1.5f;
    A[4].l = mg_dvm_new_string("abc");
    arr = mg_dvm_new_array("[I", 3);
    for (i = 0; i < 3; i++) ((i32 *)arr->u.arr.data)[i] = i + 1;
    A[5].l = arr;
    CHECK(call("t/Guest", "mix", "(IJDFLjava/lang/String;[I)J").j == 7 + ((i64)1 << 40) + 25 + 15 + 3 + 6);
    /* and from bytecode */
    CHECK(call("t/Guest", "runMix", "()J").j == 7 + ((i64)1 << 40) + 25 + 15 + 3 + 6);
    for (i = 0; i < 14; i++) A[i].j = ((i64)1 << 33) + i;
    CHECK(call("t/Guest", "many", "(JJJJJJJJJJJJJJ)J").j == 105 * ((i64)1 << 33) + 910);
    A[0].d = 1.5;
    A[1].f = 2.5f;
    CHECK(call("t/Guest", "fmul", "(DF)D").d == 3.75);

    /* strings and arrays, each way */
    A[0].l = mg_dvm_new_string("w\xc3\xb6rld");
    CHECK_STR(text_of(call("t/Guest", "greet", "(Ljava/lang/String;)Ljava/lang/String;").l), "hello, w\xc3\xb6rld!");
    CHECK_STR(text_of(call("t/Guest", "runArrays", "()Ljava/lang/String;").l), "30:one,two,three");

    /* the library calling back: a static method, a virtual one as the object's own class has it, fields */
    v.j = 0;
    v.i = 3;
    g = mg_dvm_new("t/Guest", "(I)V", &v);
    sub = mg_dvm_new("t/GuestSub", "(I)V", &v);
    A[0].i = 5;
    CHECK_INT(vcall(g, "callBack", "(I)I").i, 10 + 15);
    A[0].i = 5;
    CHECK_INT(vcall(sub, "callBack", "(I)I").i, 10 + 1015);
    CHECK(mg_dvm_get_field(g, "count", &v) && v.i == 4);
    CHECK(mg_dvm_get_static("t/Guest", "total", &v) && v.i == 10);
    A[0].i = 77;
    made = call("t/Guest", "make", "(I)Lt/Guest;").l;
    CHECK(made && mg_dvm_get_field(made, "count", &v) && v.i == 77);
    CHECK_STR(mg_dvm_class_name(made), "t/Guest");
    CHECK_STR(thrown(), "");

    /* exceptions each way: the library's caught by bytecode, bytecode's seen and cleared by the library */
    CHECK_INT(call("t/Guest", "runCatch", "()I").i, 10);
    CHECK_STR(thrown(), "");
    call("t/Guest", "thrower", "()V");
    CHECK_STR(thrown(), "java.lang.IllegalStateException: from guest");
    CHECK_INT(call("t/Guest", "catcher", "()I").i, 1);
    CHECK_STR(thrown(), "");
    CHECK_INT(vcall(g, "locked", "()I").i, 0);
    CHECK_STR(thrown(), "");
}
#endif

/*
 * ThrowNew of a class of another class scope (a service's, megler.ServiceClassLoader: GmsCore's LevelDB throws its own
 * LevelDbIoErrorException) throws that class: a Throwable bytecode can call, not a stand-in made from the class's name, which
 * the app's scope may not have (logging GmsCore's ended Play Store with an IncompatibleClassChangeError) or have as another.
 * The d8 program's DEX file (mg_dvm_tests_d8.c) in a scope of its own, while the app's has none of its classes: its
 * exception Main$Oops is then of that scope alone, as GmsCore's is.
 */
static void test_scoped_throw(const char *fixtures)
{
    enum { SCOPE = 13 };
    static const char OOPS[] = "d8t/Main$Oops";
    char *path;
    static mg_map map;                                   /* (a DEX file is used where it lies: kept mapped) */
    mg_dvm_class *theirs;
    mg_jobj *e;
    say("JNI: ThrowNew of a class of another class scope");
    if (mg_dvm_class_exists(OOPS)) {
        say("  (the app's scope has %s already: the d8 program was run on this runtime first; not tested here)", OOPS);
        return;
    }
    path = mg_sprintf("%s/d8t.dex", fixtures);
    CHECK(mg_map_open(&map, path));
    free(path);
    if (!map.data) return;
    CHECK(mg_dvm_add_dex_scope(map.data, (size_t)map.size, "d8t.dex (scope 13)", SCOPE));
    theirs = mg_dvm_find_class_scope(OOPS, SCOPE);
    CHECK(theirs && theirs->scope == SCOPE && !mg_dvm_class_exists(OOPS));
    if (!theirs) return;
    J(14, i32 (*)(ENV, mg_jobj *, const char *))(E, mg_dvm_mirror(theirs), "from scope 13");   /* ThrowNew */
    e = mg_dvm_pending();
    CHECK(e && mg_dvm_class_of(e) == theirs && mg_dvm_is_instance(e, g_dvm.c_throwable));
    CHECK_STR(thrown(), "d8t.Main$Oops: from scope 13");
}

void test_jni(const char *guest_dir, const char *fixtures)
{
    test_env();
    if (fixtures) test_scoped_throw(fixtures);
#if MG_HOST_ARM32
    test_guarded_call();
#endif
#if MG_GUEST_CODE
    if (guest_dir) test_guest_natives(guest_dir);
    else say("(no guest libraries were named: native methods in a guest library are not tested)");
#else
    (void)guest_dir;
#endif
}
