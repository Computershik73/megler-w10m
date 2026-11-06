/*
 * What the fixtures say about the bytecode itself: values of two registers,
 * switches, arrays, exceptions, dispatch, static initialisation, strings.
 * Each check names the method of a fixture it calls; the .smali beside it says
 * what the method does and why.
 */
#include <math.h>
#include <stdio.h>

#include "../src/dvm/mg_dvm_internal.h"
#include "mg_dvm_tests.h"

static void test_wide(void)
{
    mg_jvalue v;
    say("values of two registers");
    A[0].i = 7;
    A[1].j = 0x100000000ll;
    A[2].i = -3;
    A[3].j = 5;
    CHECK(call("t/Wide", "mix", "(IJIJ)J").j == (7 + 0x100000000ll) * -3 - 5);
    A[0].j = 0x123456789abcdef0ll;
    CHECK(call("t/Wide", "moves", "(J)J").j == 0x123456789abcdef0ll + 0x55 + 4);
    A[0].j = -1000000000000ll;
    A[1].d = 0.5;
    CHECK(call("t/Wide", "fields", "(JD)D").d == -1000000000000.0 + 0.5 + 123.0);
    CHECK(mg_dvm_get_static("t/Wide", "sj", &v) && v.j == -1000000000000ll);
    CHECK(mg_dvm_get_static("t/Wide", "sd", &v) && v.d == 0.5);
    A[0].j = 1000000000000ll;
    CHECK(call("t/Wide", "calls", "(J)J").j == ((3 + 1000000000000ll) * 5 - 100) * 2);
    A[0].d = 2.5;
    A[1].j = -4;
    CHECK(call("t/Wide", "five", "(DJ)D").d == -4 * 2.5 + 1.0);
    A[0].j = (i64)0x8000000000000001ull;
    CHECK(call("t/Wide", "twice", "(J)J").j == 2);
    CHECK_STR(thrown(), "");
}

static void test_switches(void)
{
    static const struct { i32 in, want; } packed[] = { { -2, -1 }, { -1, 100 }, { 0, 200 }, { 1, 300 }, { 2, -1 }, { INT32_MIN, -1 }, { INT32_MAX, -1 } };
    static const struct { i32 in, want; } sparse[] = { { -0x7fffffff, 1 }, { 1, 2 }, { 1000, 3 }, { 0x7fffffff, 4 }, { 0, 0 }, { 2, 0 }, { 999, 0 }, { INT32_MIN, 0 }, { -1, 0 } };
    static const struct { i32 in, want; } fall[] = { { 1, 1 }, { 2, 3 }, { 3, 6 }, { 0, 0 }, { 4, 0 } };
    size_t i;
    say("switches");
    for (i = 0; i < MG_COUNT(packed); i++) {
        A[0].i = packed[i].in;
        CHECK_INT(call("t/Switches", "packed", "(I)I").i, packed[i].want);
    }
    for (i = 0; i < MG_COUNT(sparse); i++) {
        A[0].i = sparse[i].in;
        CHECK_INT(call("t/Switches", "sparse", "(I)I").i, sparse[i].want);
    }
    for (i = 0; i < MG_COUNT(fall); i++) {
        A[0].i = fall[i].in;
        CHECK_INT(call("t/Switches", "fall", "(I)I").i, fall[i].want);
    }
    A[0].i = 10;
    CHECK_INT(call("t/Switches", "counted", "(I)I").i, 3);
    CHECK_STR(thrown(), "");
}

static void test_arrays(void)
{
    mg_jobj *a;
    say("arrays");
    A[0].i = 10;
    CHECK_INT(call("t/Arrays", "squares", "(I)I").i, 285);
    A[0].i = 1;
    CHECK_INT(call("t/Arrays", "bools", "(I)I").i, 1);
    A[0].i = 0x80;
    CHECK_INT(call("t/Arrays", "bytes", "(I)I").i, -128);
    A[0].i = 0x17f;
    CHECK_INT(call("t/Arrays", "bytes", "(I)I").i, 127);
    A[0].i = -1;
    CHECK_INT(call("t/Arrays", "chars", "(I)I").i, 65535);
    A[0].i = 0x12345;
    CHECK_INT(call("t/Arrays", "chars", "(I)I").i, 0x2345);
    A[0].i = 0x8000;
    CHECK_INT(call("t/Arrays", "shorts", "(I)I").i, -32768);
    A[0].i = 0x17fff;
    CHECK_INT(call("t/Arrays", "shorts", "(I)I").i, 32767);
    A[0].j = (i64)0xfedcba9876543210ull;
    CHECK(call("t/Arrays", "longs", "(J)J").j == (i64)0xfedcba9876543210ull);
    A[0].f = 1.25f;
    CHECK(call("t/Arrays", "floats", "(F)F").f == 1.25f);
    A[0].d = -1234.5e100;
    CHECK(call("t/Arrays", "doubles", "(D)D").d == -1234.5e100);
    CHECK(call("t/Arrays", "filled", "()J").j == (0x12345678ll + 127 + 0x1234) + 0x123456789abcdef0ll + 11 - 13);
    call("t/Arrays", "overfill", "()V");
    CHECK_STR(thrown(), "java.lang.ArrayIndexOutOfBoundsException: fill-array-data: 3 elements into an array of 2");
    A[0].i = 1;
    A[1].i = 2;
    A[2].i = 3;
    CHECK_INT(call("t/Arrays", "filledNew", "(III)I").i, 3123);
    CHECK_INT(call("t/Arrays", "filledObjects", "()I").i, 3);
    A[0].i = 3;
    A[1].i = 4;
    CHECK_INT(call("t/Arrays", "grid", "(II)I").i, 138);
    A[0].i = 0;
    CHECK_INT(call("t/Arrays", "at", "(I)I").i, 10);
    A[0].i = 2;
    CHECK_INT(call("t/Arrays", "at", "(I)I").i, 30);
    CHECK_STR(thrown(), "");
    A[0].i = 3;
    call("t/Arrays", "at", "(I)I");
    CHECK_STR(thrown(), "java.lang.ArrayIndexOutOfBoundsException: length=3; index=3");
    A[0].i = -1;
    call("t/Arrays", "at", "(I)I");
    CHECK_STR(thrown(), "java.lang.ArrayIndexOutOfBoundsException: length=3; index=-1");
    call("t/Arrays", "nullLength", "()I");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to get length of null array");
    call("t/Arrays", "nullRead", "()I");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to read from a null array");
    A[0].i = 5;
    CHECK_INT(call("t/Arrays", "sized", "(I)I").i, 5);
    A[0].i = 0;
    CHECK_INT(call("t/Arrays", "sized", "(I)I").i, 0);
    CHECK_STR(thrown(), "");
    A[0].i = -1;
    call("t/Arrays", "sized", "(I)I");
    CHECK_STR(thrown(), "java.lang.NegativeArraySizeException: -1");
    call("t/Arrays", "store", "()V");
    CHECK_STR(thrown(), "java.lang.ArrayStoreException: java/lang/Integer cannot be stored in an array of type [Ljava/lang/String;");
    CHECK_INT(call("t/Arrays", "copy", "()I").i, 1399);
    CHECK_INT(call("t/Arrays", "kinds", "()I").i, 1 | 2 | 8 | 16 | 64);
    CHECK_STR(thrown(), "");

    /* From C: an array is an mg_jobj whose elements native code reads where the JNI layer keeps them. */
    a = mg_dvm_new_array("[S", 4);
    CHECK(a && a->kind == MG_J_PRIM_ARRAY && a->u.arr.len == 4 && a->u.arr.esz == 2 && a->u.arr.etype == 'S' && a->permanent);
    CHECK(a && ((mg_addr)a->u.arr.data & 1) == 0 && ((i16 *)a->u.arr.data)[3] == 0);
    a = mg_dvm_new_array("[D", 2);
    CHECK(a && ((mg_addr)a->u.arr.data & 7) == 0);
    a = mg_dvm_new_array("[[Ljava/lang/String;", 3);
    CHECK(a && a->kind == MG_J_OBJ_ARRAY && a->u.oarr.len == 3 && a->u.oarr.v[2] == NULL);
    CHECK_STR(mg_dvm_class_name(a), "[[Ljava/lang/String;");
    CHECK(mg_dvm_instance_of(a, "[Ljava/lang/Object;") && mg_dvm_instance_of(a, "java/lang/Object") && !mg_dvm_instance_of(a, "[Ljava/lang/String;"));
    CHECK(mg_dvm_new_array("[I", -2) == NULL);
    CHECK_STR(thrown(), "java.lang.NegativeArraySizeException: -2");
    CHECK(mg_dvm_new_array("[Lno/such/Thing;", 1) == NULL);
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Thing (needed by [Lno/such/Thing;)");
}

static void test_exceptions(void)
{
    mg_jobj *e, *s, *integer;
    mg_jvalue v;
    char stack[600];
    say("exceptions");
    A[0].i = 7;
    A[1].i = 2;
    CHECK_INT(call("t/Exc", "divide", "(II)I").i, 3);
    A[0].i = 7;
    CHECK_INT(call("t/Exc", "divide", "(II)I").i, -1);
    CHECK_STR(thrown(), "");
    A[0].i = 1;
    CHECK(!mg_dvm_call_static("t/Exc", "wrongCatch", "(II)I", A, NULL));
    CHECK_INT(mg_dvm_pending_stack(stack, sizeof(stack)), 1);
    CHECK_STR(stack, "  at t.Exc.wrongCatch(pc 0)\n");
    CHECK_STR(thrown(), "java.lang.ArithmeticException: divide by zero");
    memset(A, 0, sizeof(A));
    A[0].i = 0;
    CHECK_INT(call("t/Exc", "which", "(I)I").i, 1);
    A[0].i = 1;
    CHECK_INT(call("t/Exc", "which", "(I)I").i, 2);
    A[0].i = 2;
    CHECK_INT(call("t/Exc", "which", "(I)I").i, 3);
    A[0].i = 3;
    CHECK_INT(call("t/Exc", "which", "(I)I").i, 0);
    CHECK_STR(thrown(), "");

    /* Through three frames, with a finally on the way, caught in the fourth. */
    A[0].i = 5;
    CHECK_STR(text_of(call("t/Exc", "trace", "(I)Ljava/lang/String;").l), "23fc5");
    A[0].i = 0;
    CHECK_STR(text_of(call("t/Exc", "trace", "(I)Ljava/lang/String;").l), "23xf0");
    CHECK_STR(thrown(), "");
    /* And caught by nothing: it reaches C, with where it was made and what it carries. */
    A[0].i = 9;
    CHECK(call("t/Exc", "uncaught", "(I)Ljava/lang/String;").l == NULL);
    e = mg_dvm_pending();
    CHECK(e != NULL);
    CHECK_STR(mg_dvm_class_name(e), "t/MyExc");
    CHECK(mg_dvm_instance_of(e, "java/lang/Exception") && mg_dvm_instance_of(e, "java/lang/Throwable") && !mg_dvm_instance_of(e, "java/lang/RuntimeException"));
    CHECK(mg_dvm_get_field(e, "code", &v) && v.i == 9);
    CHECK_INT(mg_dvm_pending_stack(stack, sizeof(stack)), 3);
    CHECK_STR(stack, "  at t.Exc.level3(Exc.java:30)\n  at t.Exc.level2(Exc.java:40)\n  at t.Exc.uncaught(Exc.java:70)\n");
    CHECK_STR(thrown(), "t.MyExc: deep");
    CHECK_STR(text_of(call("t/Exc", "logged", "()Ljava/lang/String;").l), "23f");
    /* One that another caused: after its own frames "Caused by" and the cause's, the frames both have said as Java says them. */
    A[0].i = 9;
    CHECK(!mg_dvm_call_static("t/Exc", "outer", "(I)V", A, NULL));
    CHECK_INT((int)mg_dvm_pending_stacks(stack, sizeof(stack)), (int)strlen(stack));
    CHECK_STR(stack, "  at t.Exc.wrapped(Exc.java:82)\n  at t.Exc.outer(Exc.java:90)\nCaused by: t.MyExc: deep\n  at t.Exc.level3(Exc.java:30)\n"
                     "  at t.Exc.level2(Exc.java:40)\n  at t.Exc.wrapped(Exc.java:80)\n  ... 1 more\n");
    CHECK_STR(thrown(), "java.lang.Throwable: wrapped | caused by t.MyExc: deep");

    A[0].i = 4;
    CHECK_INT(call("t/Exc", "nested", "(I)I").i, 26);
    A[0].i = 0;
    CHECK_INT(call("t/Exc", "nested", "(I)I").i, 999);
    CHECK_INT(call("t/Exc", "inHandler", "()I").i, 2);
    CHECK_STR(thrown(), "");
    call("t/Exc", "throwNull", "()V");
    CHECK_STR(thrown(), "java.lang.NullPointerException: throw with null exception");
    s = mg_dvm_new_string("abc");
    A[0].l = s;
    A[1].i = 1;
    CHECK_INT(call("t/Exc", "charAt", "(Ljava/lang/String;I)I").i, 'b');
    A[0].l = s;
    A[1].i = 3;
    CHECK_INT(call("t/Exc", "charAt", "(Ljava/lang/String;I)I").i, -1);
    CHECK_STR(text_of(call("t/Exc", "message", "()Ljava/lang/String;").l), "boom");
    CHECK_STR(thrown(), "");
    call("t/Exc", "caused", "()V");
    CHECK_STR(thrown(), "java.lang.RuntimeException: wrapped | caused by java.lang.ArithmeticException: divide by zero");

    A[0].l = s;
    CHECK(call("t/Exc", "cast", "(Ljava/lang/Object;)Ljava/lang/String;").l == s);
    CHECK(call("t/Exc", "cast", "(Ljava/lang/Object;)Ljava/lang/String;").l == NULL);
    CHECK_STR(thrown(), "");
    A[0].i = 5;
    integer = call("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;").l;
    A[0].l = integer;
    call("t/Exc", "cast", "(Ljava/lang/Object;)Ljava/lang/String;");
    CHECK_STR(thrown(), "java.lang.ClassCastException: java/lang/Integer cannot be cast to java/lang/String");
    call("t/Exc", "field", "(Lt/Exc;)I");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to read from field 't/Exc.value' on a null object reference");
    call("t/Exc", "store", "(Lt/Exc;)V");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to write to field 't/Exc.value' on a null object reference");
    e = mg_dvm_new("t/Exc", "()V", NULL);
    A[0].l = e;
    call("t/Exc", "store", "(Lt/Exc;)V");
    A[0].l = e;
    CHECK_INT(call("t/Exc", "field", "(Lt/Exc;)I").i, 5);
    A[0].j = 7;
    call("t/Exc", "longDivide", "(JJ)J");
    CHECK_STR(thrown(), "java.lang.ArithmeticException: divide by zero");
    A[0].j = INT64_MIN;
    A[1].j = -1;
    CHECK(call("t/Exc", "longDivide", "(JJ)J").j == INT64_MIN);
    CHECK_STR(thrown(), "");

    /* A static initialiser that throws: wrapped the first time, the class unusable ever after. */
    call("t/Exc", "boom", "()I");
    /* (libcore's ExceptionInInitializerError has no message and keeps what was thrown in a field of its own, exception, which
       the description follows as getCause() does.) */
    CHECK_STR(thrown(), g_libcore ? "java.lang.ExceptionInInitializerError | caused by java.lang.ArithmeticException: divide by zero"
                                  : "java.lang.ExceptionInInitializerError: java.lang.ArithmeticException: divide by zero | caused by java.lang.ArithmeticException: divide by zero");
    /* ... and every later use is NoClassDefFoundError, whose cause is what went wrong the first time (as in Java 17 and ART). */
    call("t/Exc", "boom", "()I");
    CHECK_STR(thrown(), g_libcore ? "java.lang.NoClassDefFoundError: Could not initialize class t/Boom | caused by java.lang.ExceptionInInitializerError | "
                                    "caused by java.lang.ArithmeticException: divide by zero"
                                  : "java.lang.NoClassDefFoundError: Could not initialize class t/Boom | caused by java.lang.ExceptionInInitializerError: "
                                    "java.lang.ArithmeticException: divide by zero | caused by java.lang.ArithmeticException: divide by zero");

    /* Thrown from C. */
    CHECK(!mg_dvm_throw("java/lang/IllegalStateException", "from C, %d", 5));
    CHECK_STR(thrown(), "java.lang.IllegalStateException: from C, 5");
    mg_dvm_throw("no/such/Exception", "what was meant");
    CHECK_STR(thrown(), "java.lang.NoClassDefFoundError: no/such/Exception (to throw it: what was meant)");
    CHECK(mg_dvm_pending() == NULL && mg_dvm_describe_pending(stack, sizeof(stack)) == NULL);
}

static mg_jobj *make(const char *cls, const char *sig, i32 a, i32 b)
{
    mg_jvalue args[2];
    args[0].j = args[1].j = 0;
    args[0].i = a;
    args[1].i = b;
    return mg_dvm_new(cls, sig, args);
}

static void test_dispatch(void)
{
    mg_jobj *rect, *square, *blob, *blob5, *blob9, *hollow, *sub, *all, *s;
    mg_jvalue v;
    i32 count;
    say("classes, interfaces, dispatch");
    rect = make("t/Rect", "(II)V", 3, 4);
    square = make("t/Square", "(I)V", 4, 0);
    blob = make("t/Blob", "(I)V", 5, 0);
    hollow = mg_dvm_new("t/Hollow", "()V", NULL);
    CHECK(rect && square && blob && hollow);
    CHECK_STR(thrown(), "");
    if (!rect || !square || !blob || !hollow) return;
    count = call("t/Shape", "count", "()I").i;
    CHECK_INT(count, 2);

    /* virtual: the object's class decides; super: the class above the caller's */
    CHECK_INT(vcall(rect, "area", "()I").i, 12);
    CHECK_INT(vcall(square, "area", "()I").i, 1016);
    /* a method the abstract class inherits from its interface without declaring it */
    CHECK_INT(vcall(rect, "twice", "()I").i, 24);
    CHECK_INT(vcall(square, "twice", "()I").i, 2032);
    A[0].l = rect;
    CHECK_STR(text_of(call("t/Shapes", "kind", "(Lt/Shape;)Ljava/lang/String;").l), "rect");
    A[0].l = square;
    CHECK_STR(text_of(call("t/Shapes", "kind", "(Lt/Shape;)Ljava/lang/String;").l), "square:rect");
    /* fields: the superclass's first, found by whichever class names them */
    CHECK_STR(text_of(vcall(square, "name", "()Ljava/lang/String;").l), "square");
    CHECK_STR(text_of(vcall(square, "nameViaSquare", "()Ljava/lang/String;").l), "square");
    CHECK(mg_dvm_get_field(rect, "w", &v) && v.i == 3);
    CHECK(mg_dvm_get_field(rect, "h", &v) && v.i == 4);
    CHECK(mg_dvm_get_field(square, "name", &v) && !strcmp(text_of(v.l), "square"));
    CHECK(!mg_dvm_get_field(rect, "nothing", &v));
    /* direct: a private method is not overridden by one of the same name below */
    CHECK_INT(vcall(rect, "viaPrivate", "()I").i, 7);
    CHECK_INT(vcall(square, "viaPrivate", "()I").i, 7);
    CHECK_INT(vcall(rect, "ownSecret", "()I").i, 9);

    /* interface: on three unrelated classes from one call site, twice over (the second time from what the first remembered) */
    all = mg_dvm_new_array("[Lt/HasArea;", 4);
    all->u.oarr.v[0] = rect;
    all->u.oarr.v[1] = square;
    all->u.oarr.v[2] = blob;
    all->u.oarr.v[3] = rect;
    A[0].l = all;
    CHECK_INT(call("t/Shapes", "total", "([Lt/HasArea;)I").i, 12 + 1016 + 5 + 12);
    A[0].l = all;
    CHECK_INT(call("t/Shapes", "total", "([Lt/HasArea;)I").i, 12 + 1016 + 5 + 12);
    CHECK_STR(thrown(), "");
    all->u.oarr.v[1] = hollow;
    A[0].l = all;
    call("t/Shapes", "total", "([Lt/HasArea;)I");
    CHECK_STR(thrown(), "java.lang.AbstractMethodError: t/HasArea.area()I");
    /* a default method, through the interface and through a class that inherits it */
    A[0].l = rect;
    CHECK_STR(text_of(call("t/Shapes", "describe", "(Lt/HasArea;)Ljava/lang/String;").l), "area 12");
    A[0].l = square;
    CHECK_STR(text_of(call("t/Shapes", "describe", "(Lt/HasArea;)Ljava/lang/String;").l), "area 1016");
    A[0].l = blob;
    CHECK_STR(text_of(call("t/Shapes", "describe", "(Lt/HasArea;)Ljava/lang/String;").l), "area 5");
    A[0].l = square;
    CHECK_STR(text_of(call("t/Shapes", "describeRect", "(Lt/Rect;)Ljava/lang/String;").l), "area 1016");
    CHECK_STR(text_of(vcall(blob, "describe", "()Ljava/lang/String;").l), "area 5");

    /* instance-of */
    A[0].l = rect;
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 1 | 2 | 4);
    A[0].l = square;
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 1 | 2 | 4 | 8);
    A[0].l = blob;
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 1 | 16);
    A[0].l = mg_dvm_new_string("a string");
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 16 | 32);
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 0);
    CHECK(mg_dvm_instance_of(square, "t/HasArea") && mg_dvm_instance_of(square, "t/Rect") && !mg_dvm_instance_of(rect, "t/Square") && !mg_dvm_instance_of(NULL, "t/Rect"));
    CHECK_STR(mg_dvm_class_name(square), "t/Square");

    /* a static method and an interface's constant, named through a class that inherits them */
    CHECK_INT(call("t/Shapes", "inherited", "()I").i, count * 1000 + 7);

    /* what Object gives, and what a class puts in its place */
    blob5 = make("t/Blob", "(I)V", 5, 0);
    blob9 = make("t/Blob", "(I)V", 9, 0);
    A[0].l = blob5;
    A[1].l = blob9;
    CHECK_INT(call("t/Shapes", "compare", "(Ljava/lang/Comparable;Ljava/lang/Object;)I").i, -4);
    A[0].l = blob5;
    A[1].l = rect;
    call("t/Shapes", "compare", "(Ljava/lang/Comparable;Ljava/lang/Object;)I");
    CHECK_STR(thrown(), "java.lang.ClassCastException: t/Rect cannot be cast to t/Blob");
    A[0].l = blob5;
    CHECK_INT(vcall(blob, "equals", "(Ljava/lang/Object;)Z").z, 1);
    A[0].l = blob9;
    CHECK_INT(vcall(blob, "equals", "(Ljava/lang/Object;)Z").z, 0);
    A[0].l = rect;
    CHECK_INT(vcall(rect, "equals", "(Ljava/lang/Object;)Z").z, 1);
    A[0].l = square;
    CHECK_INT(vcall(rect, "equals", "(Ljava/lang/Object;)Z").z, 0);
    CHECK_INT(vcall(blob9, "hashCode", "()I").i, 9);
    CHECK(vcall(rect, "hashCode", "()I").i == vcall(rect, "hashCode", "()I").i);
    A[0].l = blob;
    CHECK_STR(text_of(call("t/Shapes", "text", "(Ljava/lang/Object;)Ljava/lang/String;").l), "blob 5");
    CHECK_STR(text_of(call("t/Shapes", "text", "(Ljava/lang/Object;)Ljava/lang/String;").l), "null");
    A[0].l = rect;
    s = call("t/Shapes", "text", "(Ljava/lang/Object;)Ljava/lang/String;").l;
    CHECK(!strncmp(text_of(s), "t.Rect@", 7) && strlen(text_of(s)) > 7);
    A[0].l = rect;
    CHECK_STR(text_of(call("t/Shapes", "className", "(Ljava/lang/Object;)Ljava/lang/String;").l), "t.Rect");
    A[0].l = square;
    CHECK_STR(text_of(call("t/Shapes", "superName", "(Ljava/lang/Object;)Ljava/lang/String;").l), "t.Rect");
    A[0].l = rect;
    CHECK_STR(text_of(call("t/Shapes", "superName", "(Ljava/lang/Object;)Ljava/lang/String;").l), "t.Shape");
    A[0].l = all;
    CHECK_STR(text_of(call("t/Shapes", "className", "(Ljava/lang/Object;)Ljava/lang/String;").l), "[Lt.HasArea;");
    CHECK_STR(thrown(), "");
    call("t/Shapes", "makeAbstract", "()Ljava/lang/Object;");
    CHECK_STR(thrown(), "java.lang.InstantiationError: t/Shape");
    CHECK(mg_dvm_new("t/Rect", "(J)V", NULL) == NULL);
    CHECK_STR(thrown(), "java.lang.NoSuchMethodError: t/Rect.<init>(J)V");

    /* a class in the second file under classes of the first */
    CHECK_INT(call("u/Other", "subTwice", "()I").i, 20);
    sub = call("u/Other", "makeSub", "()Lu/Sub;").l;
    A[0].l = sub;
    CHECK_STR(text_of(call("t/Shapes", "kind", "(Lt/Shape;)Ljava/lang/String;").l), "sub");
    A[0].l = sub;
    CHECK_INT(call("t/Shapes", "what", "(Ljava/lang/Object;)I").i, 1 | 2 | 4);
    CHECK_STR(text_of(vcall(sub, "describe", "()Ljava/lang/String;").l), "area 10");
    CHECK_INT(call("t/Shape", "count", "()I").i, count + 2);
    CHECK_STR(thrown(), "");

    /* an enum */
    A[0].l = mg_dvm_new_string("GREEN");
    s = call("t/Color", "valueOf", "(Ljava/lang/String;)Lt/Color;").l;
    CHECK(s != NULL);
    CHECK(mg_dvm_get_static("t/Color", "GREEN", &v) && v.l == s);
    if (s) {
        CHECK_INT(vcall(s, "ordinal", "()I").i, 1);
        CHECK_STR(text_of(vcall(s, "name", "()Ljava/lang/String;").l), "GREEN");
        CHECK_STR(text_of(vcall(s, "toString", "()Ljava/lang/String;").l), "GREEN");
        mg_dvm_get_static("t/Color", "RED", &v);
        A[0].l = v.l;
        A[1].l = s;
        CHECK_INT(call("t/Shapes", "compare", "(Ljava/lang/Comparable;Ljava/lang/Object;)I").i, -1);
    }
    all = call("t/Color", "values", "()[Lt/Color;").l;
    CHECK(all && all->u.oarr.len == 2 && all->u.oarr.v[1] == s && all != call("t/Color", "values", "()[Lt/Color;").l);
    A[0].l = mg_dvm_new_string("BLUE");
    call("t/Color", "valueOf", "(Ljava/lang/String;)Lt/Color;");
    CHECK_STR(thrown(), g_libcore ? "java.lang.IllegalArgumentException: No enum constant t.Color.BLUE" : "java.lang.IllegalArgumentException: No enum constant t/Color.BLUE");
}

static const char *taken(void) { return text_of(call("t/InitLog", "take", "()Ljava/lang/String;").l); }

static void test_init(void)
{
    mg_jvalue v;
    say("static initialisation");
    CHECK_STR(taken(), "");
    /* naming a class, or checking against it, does not initialise it */
    CHECK(call("t/Init", "classOnly", "()Ljava/lang/Class;").l != NULL);
    A[0].l = mg_dvm_new_string("not an InitB");
    CHECK_INT(call("t/Init", "checkOnly", "(Ljava/lang/Object;)Z").z, 0);
    CHECK(mg_dvm_class_object("t/InitB") != NULL);
    CHECK_STR(taken(), "");
    /* a field read through a subclass initialises the class that declares it, and no other */
    CHECK_INT(call("t/Init", "aViaB", "()I").i, 10);
    CHECK_STR(taken(), "A");
    /* a static method initialises its class, once */
    CHECK_INT(call("t/InitB", "b", "()I").i, 11);
    CHECK_STR(taken(), "B");
    CHECK_INT(call("t/InitB", "b", "()I").i, 11);
    CHECK_STR(taken(), "");
    /* constructors: the superclass's first */
    CHECK(call("t/Init", "makeB", "()Ljava/lang/Object;").l != NULL);
    CHECK_STR(taken(), "ab");
    /* a constructor the class does not declare is its superclass's, as ART finds it (R8 takes out ones that only pass on) */
    CHECK_INT(call("t/Init", "makeF", "()Z").z, 1);
    CHECK_STR(taken(), "a");
    /* two classes that need each other: the second sees the first half-made */
    CHECK_INT(call("t/InitC", "c", "()I").i, 6);
    CHECK_STR(taken(), "C1D1D2C2");
    CHECK(mg_dvm_get_static("t/InitD", "d", &v) && v.i == 5);
    CHECK_STR(taken(), "");

    /* values the file itself gives static fields, of every kind, there before the initialiser runs */
    CHECK(mg_dvm_get_static("t/InitE", "seen", &v) && v.i == -0x12345678 + 0x1234);
    CHECK(mg_dvm_get_static("t/InitE", "Z", &v) && v.z == 1);
    CHECK(mg_dvm_get_static("t/InitE", "B", &v) && v.b == -5);
    CHECK(mg_dvm_get_static("t/InitE", "S", &v) && v.s == 0x1234);
    CHECK(mg_dvm_get_static("t/InitE", "C", &v) && v.c == 'x');
    CHECK(mg_dvm_get_static("t/InitE", "I", &v) && v.i == -0x12345678);
    CHECK(mg_dvm_get_static("t/InitE", "J", &v) && v.j == 0x123456789abcdefll);
    CHECK(mg_dvm_get_static("t/InitE", "F", &v) && v.f == 1.5f);
    CHECK(mg_dvm_get_static("t/InitE", "D", &v) && v.d == -2.5);
    CHECK(mg_dvm_get_static("t/InitE", "STR", &v) && !strcmp(text_of(v.l), "preset"));
    CHECK(mg_dvm_get_static("t/InitE", "NUL", &v) && v.l == NULL);
    CHECK(mg_dvm_get_static("t/InitE", "SMALL", &v) && v.j == 0x7f);
    CHECK(mg_dvm_get_static("t/InitE", "WHOLE", &v) && v.d == 3.0);
    CHECK(mg_dvm_get_static("t/InitE", "plain", &v) && v.i == 0);
    CHECK(!mg_dvm_get_static("t/InitE", "nothing", &v));
    v.j = 0;
    v.i = 77;
    CHECK(mg_dvm_set_static("t/InitE", "plain", v) && mg_dvm_get_static("t/InitE", "plain", &v) && v.i == 77);
    /* and the built-in classes' own */
    CHECK(mg_dvm_get_static("java/lang/Integer", "MAX_VALUE", &v) && v.i == INT32_MAX);
    CHECK(mg_dvm_get_static("java/lang/Long", "MIN_VALUE", &v) && v.j == INT64_MIN);
    CHECK(mg_dvm_get_static("java/lang/Math", "PI", &v) && v.d == 3.141592653589793);
    CHECK(mg_dvm_get_static("java/lang/Float", "MAX_VALUE", &v) && v.f == 3.4028235e38f);
    CHECK(mg_dvm_get_static("java/lang/Double", "MIN_VALUE", &v) && v.d == 4.9e-324);
    CHECK(mg_dvm_get_static("java/lang/Character", "MAX_VALUE", &v) && v.c == 0xFFFF);
    CHECK_STR(thrown(), "");
}

static mg_jobj *value_of(const char *sig, mg_jvalue v)
{
    A[0] = v;
    return call("java/lang/String", "valueOf", sig).l;
}

static void test_strings(void)
{
    static const u16 unicode[] = { 0xE9, 0x4E2D, 0xD83D, 0xDE00, 0, 'z' };
    mg_jobj *s, *t, *blob, *box;
    const u16 *u;
    mg_jvalue v;
    i32 n = 0;
    say("strings");
    /* one literal, one object: in one method, in two files */
    CHECK_INT(call("t/Strings", "same", "()Z").z, 1);
    s = call("t/Strings", "shared", "()Ljava/lang/String;").l;
    CHECK(s && s == call("u/Other", "shared", "()Ljava/lang/String;").l);
    CHECK(s && s->kind == MG_J_STRING && s->permanent && s->u.str.len16 == 23);
    CHECK_STR(text_of(call("t/Strings", "jumbo", "()Ljava/lang/String;").l), "jumbo");

    /* concatenation as the compiler writes it */
    A[0].l = mg_dvm_new_string("n=");
    A[1].i = 42;
    A[2].j = -7;
    A[3].c = 'x';
    A[4].z = 1;
    A[5].d = 1.5;
    CHECK_STR(text_of(call("t/Strings", "concat", "(Ljava/lang/String;IJCZDLjava/lang/Object;)Ljava/lang/String;").l), "n=42-7xtrue1.5null");
    blob = make("t/Blob", "(I)V", 5, 0);
    A[0].l = NULL;
    A[1].i = INT32_MIN;
    A[2].j = INT64_MAX;
    A[3].c = 0x4E2D;
    A[5].d = -0.001;
    A[6].l = blob;
    CHECK_STR(text_of(call("t/Strings", "concat", "(Ljava/lang/String;IJCZDLjava/lang/Object;)Ljava/lang/String;").l),
              "null-21474836489223372036854775807\xe4\xb8\xad" "false-0.001blob 5");
    CHECK_STR(text_of(call("t/Strings", "grown", "()Ljava/lang/String;").l), "0123456789012345678901234567890123456789");

    /* text outside ASCII: as the file holds it (modified UTF-8), as Java sees it (UTF-16), as C is given it (UTF-8) */
    s = call("t/Strings", "unicode", "()Ljava/lang/String;").l;
    u = mg_dvm_string_utf16(s, &n);
    CHECK_INT(n, 6);
    CHECK(u && n == 6 && !memcmp(u, unicode, sizeof(unicode)));
    CHECK_STR(mg_dvm_string_utf8(s), "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80\xc0\x80z");
    CHECK_INT(vcall(s, "length", "()I").i, 6);
    A[0].i = 2;
    CHECK_INT(vcall(s, "charAt", "(I)C").c, 0xD83D);
    t = mg_dvm_new_string("h\xc3\xa9\xf0\x9f\x98\x80!");
    u = mg_dvm_string_utf16(t, &n);
    CHECK(n == 5 && u[0] == 'h' && u[1] == 0xE9 && u[2] == 0xD83D && u[3] == 0xDE00 && u[4] == '!');
    CHECK_STR(mg_dvm_string_utf8(t), "h\xc3\xa9\xf0\x9f\x98\x80!");
    t = mg_dvm_new_string_utf16(unicode, 2);
    CHECK_STR(mg_dvm_string_utf8(t), "\xc3\xa9\xe4\xb8\xad");
    CHECK(mg_dvm_string_utf8(NULL) == NULL && mg_dvm_string_utf16(NULL, &n) == NULL && n == 0);

    CHECK_INT(call("t/Strings", "chars", "()I").i, 5101);
    CHECK_INT(call("t/Strings", "hash", "()I").i, 99162322);
    CHECK_STR(text_of(call("t/Strings", "fromChars", "()Ljava/lang/String;").l), "abc");
    CHECK_INT(call("t/Strings", "identity", "()I").i, 7);
    CHECK_INT(call("t/Strings", "compare", "()I").i, -99);
    CHECK_INT(call("t/Strings", "lengths", "()I").i, 46);
    CHECK_STR(thrown(), "");

    /* String's own methods, called on an object from C */
    s = mg_dvm_new_string("Hello, world");
    A[0].i = 7;
    CHECK_STR(text_of(vcall(s, "substring", "(I)Ljava/lang/String;").l), "world");
    A[0].i = 1;
    A[1].i = 5;
    CHECK_STR(text_of(vcall(s, "substring", "(II)Ljava/lang/String;").l), "ello");
    A[0].i = 5;
    A[1].i = 99;
    vcall(s, "substring", "(II)Ljava/lang/String;");
    CHECK_STR(thrown(), g_libcore ? "java.lang.StringIndexOutOfBoundsException: length=12; index=99" : "java.lang.StringIndexOutOfBoundsException: begin 5, end 99, length 12");
    A[0].i = 'o';
    CHECK_INT(vcall(s, "indexOf", "(I)I").i, 4);
    A[0].i = 'o';
    A[1].i = 5;
    CHECK_INT(vcall(s, "indexOf", "(II)I").i, 8);
    A[0].i = 'z';
    CHECK_INT(vcall(s, "indexOf", "(I)I").i, -1);
    A[0].l = mg_dvm_new_string("world");
    CHECK_INT(vcall(s, "indexOf", "(Ljava/lang/String;)I").i, 7);
    A[0].l = mg_dvm_new_string("Hello");
    CHECK_INT(vcall(s, "startsWith", "(Ljava/lang/String;)Z").z, 1);
    A[0].l = mg_dvm_new_string("word");
    CHECK_INT(vcall(s, "endsWith", "(Ljava/lang/String;)Z").z, 0);
    A[0].l = mg_dvm_new_string("!");
    CHECK_STR(text_of(vcall(s, "concat", "(Ljava/lang/String;)Ljava/lang/String;").l), "Hello, world!");
    CHECK_STR(text_of(vcall(s, "toUpperCase", "()Ljava/lang/String;").l), "HELLO, WORLD");
    CHECK_STR(text_of(vcall(mg_dvm_new_string("  two words \n"), "trim", "()Ljava/lang/String;").l), "two words");
    A[0].l = mg_dvm_new_string("hello, WORLD");
    CHECK_INT(vcall(s, "equalsIgnoreCase", "(Ljava/lang/String;)Z").z, 1);
    CHECK_INT(vcall(s, "equals", "(Ljava/lang/Object;)Z").z, 0);
    A[0].l = mg_dvm_new_string("Hello, world");
    CHECK_INT(vcall(s, "equals", "(Ljava/lang/Object;)Z").z, 1);
    t = vcall(s, "toCharArray", "()[C").l;
    CHECK(t && t->kind == MG_J_PRIM_ARRAY && t->u.arr.len == 12 && ((u16 *)t->u.arr.data)[11] == 'd');
    if (!g_libcore || mg_dvm_libcore_natives()) {
        /* Text to bytes and back: on libcore that is ICU's converter, in its native libraries. */
        t = vcall(call("t/Strings", "unicode", "()Ljava/lang/String;").l, "getBytes", "()[B").l;
        CHECK(t && t->u.arr.len == 11 && !memcmp(t->u.arr.data, "\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80\0z", 11));
        A[0].l = t;
        t = mg_dvm_new("java/lang/String", "([B)V", A);
        CHECK(t && t->u.str.len16 == 6 && !memcmp(t->u.str.u16, unicode, sizeof(unicode)));
    }
    mg_dvm_clear_pending();
    memset(A, 0, sizeof(A));
    vcall(NULL, "length", "()I");
    CHECK_STR(thrown(), "java.lang.NullPointerException: Attempt to invoke virtual method 'length()I' on a null object reference");

    /* numbers as text and back */
    v.j = 0;
    v.i = -2147483647 - 1;
    CHECK_STR(text_of(value_of("(I)Ljava/lang/String;", v)), "-2147483648");
    v.j = 1234567890123456789ll;
    CHECK_STR(text_of(value_of("(J)Ljava/lang/String;", v)), "1234567890123456789");
    v.d = 1.0;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "1.0");
    v.d = 0.1;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "0.1");
    v.d = 1e7;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "1.0E7");
    v.d = 9999999.0;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "9999999.0");
    v.d = 1.5e-5;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "1.5E-5");
    v.d = 0.001;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "0.001");
    v.d = 123456.789;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "123456.789");
    v.d = -0.0;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "-0.0");
    v.j = 0x7ff8000000000000ll;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "NaN");
    v.j = (i64)0xfff0000000000000ull;
    CHECK_STR(text_of(value_of("(D)Ljava/lang/String;", v)), "-Infinity");
    v.j = 0;
    v.f = 0.1f;
    CHECK_STR(text_of(value_of("(F)Ljava/lang/String;", v)), "0.1");
    v.f = 1.0e10f;
    CHECK_STR(text_of(value_of("(F)Ljava/lang/String;", v)), "1.0E10");
    v.f = 16777216.0f;
    CHECK_STR(text_of(value_of("(F)Ljava/lang/String;", v)), "1.6777216E7");
    A[0].l = mg_dvm_new_string("-123");
    CHECK_INT(call("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I").i, -123);
    A[0].l = mg_dvm_new_string("2147483647");
    CHECK_INT(call("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I").i, INT32_MAX);
    CHECK_STR(thrown(), "");
    A[0].l = mg_dvm_new_string("2147483648");
    call("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");
    CHECK_STR(thrown(), "java.lang.NumberFormatException: For input string: \"2147483648\"");
    A[0].l = mg_dvm_new_string("12x");
    call("java/lang/Integer", "parseInt", "(Ljava/lang/String;)I");
    CHECK_STR(thrown(), "java.lang.NumberFormatException: For input string: \"12x\"");

    /* the boxes */
    A[0].i = 5;
    box = call("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;").l;
    CHECK_INT(vcall(box, "intValue", "()I").i, 5);
    CHECK(vcall(box, "doubleValue", "()D").d == 5.0);
    CHECK_STR(text_of(vcall(box, "toString", "()Ljava/lang/String;").l), "5");
    A[0].i = 5;
    A[0].l = call("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;").l;
    CHECK_INT(vcall(box, "equals", "(Ljava/lang/Object;)Z").z, 1);
    A[0].j = 5;
    A[0].l = call("java/lang/Long", "valueOf", "(J)Ljava/lang/Long;").l;
    CHECK_INT(vcall(box, "equals", "(Ljava/lang/Object;)Z").z, 0);
    A[0].j = 0x123456789ll;
    box = call("java/lang/Long", "valueOf", "(J)Ljava/lang/Long;").l;
    CHECK_INT(vcall(box, "hashCode", "()I").i, 0x23456789 ^ 1);
    CHECK_INT(vcall(box, "intValue", "()I").i, 0x23456789);
    CHECK_INT(vcall(box, "byteValue", "()B").b, (i8)0x89);
    A[0].d = -2.5;
    box = call("java/lang/Double", "valueOf", "(D)Ljava/lang/Double;").l;
    CHECK_INT(vcall(box, "intValue", "()I").i, -2);
    CHECK(vcall(box, "floatValue", "()F").f == -2.5f);
    CHECK_STR(text_of(vcall(box, "toString", "()Ljava/lang/String;").l), "-2.5");
    A[0].z = 1;
    box = call("java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;").l;
    CHECK(mg_dvm_get_static("java/lang/Boolean", "TRUE", &v) && v.l == box);
    CHECK_INT(vcall(box, "hashCode", "()I").i, 1231);
    A[0].c = 'q';
    box = call("java/lang/Character", "valueOf", "(C)Ljava/lang/Character;").l;
    CHECK_STR(text_of(vcall(box, "toString", "()Ljava/lang/String;").l), "q");
    A[0].i = -3;
    A[1].i = 8;
    CHECK_INT(call("java/lang/Math", "max", "(II)I").i, 8);
    A[0].d = 2.0;
    CHECK(call("java/lang/Math", "sqrt", "(D)D").d == sqrt(2.0));
    CHECK_STR(thrown(), "");
}

void test_language(void)
{
    test_wide();
    test_switches();
    test_arrays();
    test_exceptions();
    test_dispatch();
    test_init();
    test_strings();
}
