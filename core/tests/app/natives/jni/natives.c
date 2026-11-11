/*
 * The native library of the test app `natives` (megler.test.natives.Lib): what
 * an app's own library does with JNI and the NDK, each thing small enough to
 * be checked from Java. Built by the NDK (tools/build-test-app.sh) against
 * its real headers, as an app's library is: armeabi-v7a, softfp.
 *
 * Some methods are bound by RegisterNatives in JNI_OnLoad, the others by
 * their Java_ names; Lib.java says which.
 */
#include <jni.h>

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define TAG "NativesLib"
#define LIB "megler/test/natives/Lib"

int natives_dep_answer(void);
long long natives_dep_scale(long long v, int by);

static JavaVM *g_vm;
static jclass g_lib;                   /* megler.test.natives.Lib, found in JNI_OnLoad and kept: the only way a thread of this library's own can have it */
static jmethodID g_on_thread;
static int g_onload_calls;
static jint g_after_detach = 12345;

/* --------------------------------------------------------- RegisterNatives */

static jint reg_add_ints(JNIEnv *env, jclass cls, jint a, jint b)
{
    (void)env; (void)cls;
    return a + b;
}

static jint reg_instance_times(JNIEnv *env, jobject self, jint k)
{
    jclass cls = (*env)->GetObjectClass(env, self);
    jfieldID factor = (*env)->GetFieldID(env, cls, "factor", "I");
    return (*env)->GetIntField(env, self, factor) * k;
}

static jstring reg_how(JNIEnv *env, jclass cls)
{
    (void)cls;
    return (*env)->NewStringUTF(env, "RegisterNatives");
}

static const JNINativeMethod k_registered[] = {
    { "addInts", "(II)I", (void *)reg_add_ints },
    { "instanceTimes", "(I)I", (void *)reg_instance_times },
    { "registeredHow", "()Ljava/lang/String;", (void *)reg_how },
};

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env = NULL;
    jclass lib;
    jfieldID f;
    (void)reserved;
    g_vm = vm;
    g_onload_calls++;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad call %d", g_onload_calls);
    /* A class of the app's, asked for here: found through the loader the library is loaded for. */
    lib = (*env)->FindClass(env, LIB);
    if (!lib) return JNI_ERR;
    g_lib = (jclass)(*env)->NewGlobalRef(env, lib);
    if ((*env)->RegisterNatives(env, lib, k_registered, 3) != JNI_OK) return JNI_ERR;
    g_on_thread = (*env)->GetMethodID(env, (*env)->FindClass(env, LIB "$ThreadReport"), "onThread", "(IIIZZZ)V");
    f = (*env)->GetStaticFieldID(env, lib, "onLoadCalls", "I");
    (*env)->SetStaticIntField(env, lib, f, g_onload_calls);
    f = (*env)->GetStaticFieldID(env, lib, "onLoadClass", "Ljava/lang/Class;");
    (*env)->SetStaticObjectField(env, lib, f, lib);
    return JNI_VERSION_1_6;
}

/* ---------------------------------------------------- by name: primitives */

JNIEXPORT jboolean JNICALL Java_megler_test_natives_Lib_flip(JNIEnv *env, jclass cls, jboolean v) { (void)env; (void)cls; return !v; }
JNIEXPORT jbyte JNICALL Java_megler_test_natives_Lib_nextByte(JNIEnv *env, jclass cls, jbyte v) { (void)env; (void)cls; return (jbyte)(v + 1); }
JNIEXPORT jchar JNICALL Java_megler_test_natives_Lib_nextChar(JNIEnv *env, jclass cls, jchar v) { (void)env; (void)cls; return (jchar)(v + 1); }
JNIEXPORT jshort JNICALL Java_megler_test_natives_Lib_nextShort(JNIEnv *env, jclass cls, jshort v) { (void)env; (void)cls; return (jshort)(v + 1); }
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_nextInt(JNIEnv *env, jclass cls, jint v) { (void)env; (void)cls; return (jint)((uint32_t)v + 1u); }
JNIEXPORT jlong JNICALL Java_megler_test_natives_Lib_nextLong(JNIEnv *env, jclass cls, jlong v) { (void)env; (void)cls; return (jlong)((uint64_t)v + 1u); }
JNIEXPORT jfloat JNICALL Java_megler_test_natives_Lib_half(JNIEnv *env, jclass cls, jfloat v) { (void)env; (void)cls; return v / 2; }
JNIEXPORT jdouble JNICALL Java_megler_test_natives_Lib_twice(JNIEnv *env, jclass cls, jdouble v) { (void)env; (void)cls; return v * 2; }

/* Ten arguments of every width: the first few in registers, the rest on the stack, each 64-bit one at an even word (softfp). */
JNIEXPORT jdouble JNICALL Java_megler_test_natives_Lib_mix(JNIEnv *env, jclass cls, jint a, jlong b, jdouble c, jint d, jlong e, jfloat f, jdouble g, jlong h, jint i, jdouble j)
{
    (void)env; (void)cls;
    return (double)a + (double)b + c + (double)d + (double)e + (double)f + g + (double)h + (double)i + j;
}

JNIEXPORT jlong JNICALL Java_megler_test_natives_Lib_mixLong(JNIEnv *env, jobject self, jlong a, jint b, jlong c, jbyte d, jlong e, jshort f, jchar g, jboolean h)
{
    (void)env; (void)self;
    return a * 3 + b * 5 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + (h ? 23 : 0);
}

/* Two methods of one name: the library exports each with its parameters in the name. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_over__I(JNIEnv *env, jclass cls, jint v) { (void)env; (void)cls; return v + 1; }
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_over__Ljava_lang_String_2(JNIEnv *env, jclass cls, jstring s) { (void)cls; return (*env)->GetStringLength(env, s); }

/* ------------------------------------------------------------------ arrays */

JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_sum(JNIEnv *env, jclass cls, jintArray a)
{
    jsize n = (*env)->GetArrayLength(env, a), i;
    jint *v = (*env)->GetIntArrayElements(env, a, NULL), total = 0;
    (void)cls;
    for (i = 0; i < n; i++) total += v[i];
    /* And written back: the caller sees every element negated. */
    for (i = 0; i < n; i++) v[i] = -v[i];
    (*env)->ReleaseIntArrayElements(env, a, v, 0);
    return total;
}

JNIEXPORT void JNICALL Java_megler_test_natives_Lib_fill(JNIEnv *env, jclass cls, jbyteArray a, jint from, jint count, jbyte v)
{
    jbyte buf[64];
    (void)cls;
    memset(buf, v, sizeof(buf));
    (*env)->SetByteArrayRegion(env, a, from, count, buf);
}

JNIEXPORT jlongArray JNICALL Java_megler_test_natives_Lib_squares(JNIEnv *env, jclass cls, jint n)
{
    jlongArray out = (*env)->NewLongArray(env, n);
    jlong v[32];
    jint i;
    (void)cls;
    for (i = 0; i < n && i < 32; i++) v[i] = (jlong)i * i * 1000000007LL;
    (*env)->SetLongArrayRegion(env, out, 0, n, v);
    return out;
}

JNIEXPORT jdouble JNICALL Java_megler_test_natives_Lib_sumCritical(JNIEnv *env, jclass cls, jdoubleArray a)
{
    jsize n = (*env)->GetArrayLength(env, a), i;
    jdouble *v = (jdouble *)(*env)->GetPrimitiveArrayCritical(env, a, NULL), total = 0;
    (void)cls;
    for (i = 0; i < n; i++) {
        total += v[i];
        v[i] += 0.5;
    }
    (*env)->ReleasePrimitiveArrayCritical(env, a, v, 0);
    return total;
}

/* One element of each of the other primitive kinds read by region and written back changed; the sum of what was read. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_others(JNIEnv *env, jclass cls, jbooleanArray z, jcharArray c, jshortArray s, jfloatArray f)
{
    jboolean zv;
    jchar cv;
    jshort sv;
    jfloat fv;
    (void)cls;
    (*env)->GetBooleanArrayRegion(env, z, 1, 1, &zv);
    (*env)->GetCharArrayRegion(env, c, 1, 1, &cv);
    (*env)->GetShortArrayRegion(env, s, 1, 1, &sv);
    (*env)->GetFloatArrayRegion(env, f, 1, 1, &fv);
    {
        jboolean nz = !zv;
        jchar nc = (jchar)(cv + 1);
        jshort ns = (jshort)(sv - 1);
        jfloat nf = fv * 2;
        (*env)->SetBooleanArrayRegion(env, z, 0, 1, &nz);
        (*env)->SetCharArrayRegion(env, c, 0, 1, &nc);
        (*env)->SetShortArrayRegion(env, s, 0, 1, &ns);
        (*env)->SetFloatArrayRegion(env, f, 0, 1, &nf);
    }
    return (zv ? 1000 : 0) + cv + sv + (jint)fv;
}

/* The words of a sentence: an array of strings made here. */
JNIEXPORT jobjectArray JNICALL Java_megler_test_natives_Lib_words(JNIEnv *env, jclass cls, jstring sentence)
{
    const char *text = (*env)->GetStringUTFChars(env, sentence, NULL);
    jclass string = (*env)->FindClass(env, "java/lang/String");
    jobjectArray out;
    char word[64];
    int count = 1, i, k = 0, at = 0;
    (void)cls;
    for (i = 0; text[i]; i++)
        if (text[i] == ' ') count++;
    out = (*env)->NewObjectArray(env, count, string, NULL);
    for (i = 0;; i++) {
        if (text[i] == ' ' || !text[i]) {
            jstring w;
            word[k] = 0;
            w = (*env)->NewStringUTF(env, word);
            (*env)->SetObjectArrayElement(env, out, at++, w);
            (*env)->DeleteLocalRef(env, w);
            k = 0;
            if (!text[i]) break;
        } else if (k < 63) {
            word[k++] = text[i];
        }
    }
    (*env)->ReleaseStringUTFChars(env, sentence, text);
    return out;
}

JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_total(JNIEnv *env, jclass cls, jobjectArray strings)
{
    jsize n = (*env)->GetArrayLength(env, strings), i;
    jint total = 0;
    (void)cls;
    for (i = 0; i < n; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, strings, i);
        if (s) total += (*env)->GetStringUTFLength(env, s);
        (*env)->DeleteLocalRef(env, s);
    }
    return total;
}

/* A region outside an array, an element outside one, a region outside a string: each an exception and nothing copied. A bit for each that was. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_bounds(JNIEnv *env, jclass cls, jintArray ints, jobjectArray objects, jstring s)
{
    jclass aioobe = (*env)->FindClass(env, "java/lang/ArrayIndexOutOfBoundsException"), sioobe = (*env)->FindClass(env, "java/lang/StringIndexOutOfBoundsException");
    jint v[8] = { 7, 7, 7, 7, 7, 7, 7, 7 }, bits = 0;
    jchar units[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    jthrowable t;
    (void)cls;
    (*env)->GetIntArrayRegion(env, ints, 2, 5, v);
    t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (t && (*env)->IsInstanceOf(env, t, aioobe) && v[0] == 7) bits |= 1;
    (*env)->SetIntArrayRegion(env, ints, -1, 2, v);
    t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (t && (*env)->IsInstanceOf(env, t, aioobe)) bits |= 2;
    if (!(*env)->GetObjectArrayElement(env, objects, (*env)->GetArrayLength(env, objects)) && (*env)->ExceptionCheck(env)) {
        t = (*env)->ExceptionOccurred(env);
        (*env)->ExceptionClear(env);
        if ((*env)->IsInstanceOf(env, t, aioobe)) bits |= 4;
    }
    (*env)->GetStringRegion(env, s, 1, (*env)->GetStringLength(env, s), units);
    t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (t && (*env)->IsInstanceOf(env, t, sioobe) && units[0] == 9) bits |= 8;
    /* And the last region that is inside each, to its end, is no exception. */
    (*env)->GetIntArrayRegion(env, ints, 2, (*env)->GetArrayLength(env, ints) - 2, v);
    (*env)->GetStringRegion(env, s, 1, (*env)->GetStringLength(env, s) - 1, units);
    if (!(*env)->ExceptionCheck(env)) bits |= 16;
    return bits;
}

/* ----------------------------------------------------------------- strings */

JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_greet(JNIEnv *env, jclass cls, jstring name)
{
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    char buf[200];
    (void)cls;
    snprintf(buf, sizeof(buf), "hello, %s", n);
    (*env)->ReleaseStringUTFChars(env, name, n);
    return (*env)->NewStringUTF(env, buf);
}

/* By UTF-16 code units, through the critical pair. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_reverse(JNIEnv *env, jclass cls, jstring s)
{
    jsize n = (*env)->GetStringLength(env, s), i;
    const jchar *in = (*env)->GetStringCritical(env, s, NULL);
    jchar out[64];
    (void)cls;
    if (n > 64) n = 64;
    for (i = 0; i < n; i++) out[i] = in[n - 1 - i];
    (*env)->ReleaseStringCritical(env, s, in);
    return (*env)->NewString(env, out, n);
}

/* The same stretch of a string taken both ways: as UTF-16, made a string again, and as UTF-8, whose bytes are counted into `utf8Bytes[0]`. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_region(JNIEnv *env, jclass cls, jstring s, jint start, jint len, jintArray utf8Bytes)
{
    jchar units[64];
    char utf8[200];
    jint n;
    (void)cls;
    memset(utf8, 0, sizeof(utf8));
    (*env)->GetStringRegion(env, s, start, len, units);
    (*env)->GetStringUTFRegion(env, s, start, len, utf8);
    n = (jint)strlen(utf8);
    (*env)->SetIntArrayRegion(env, utf8Bytes, 0, 1, &n);
    return (*env)->NewString(env, units, len);
}

/* --------------------------------------------- calls back, fields, objects */

JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_callBack(JNIEnv *env, jclass cls, jobject callback, jint v)
{
    jclass c = (*env)->GetObjectClass(env, callback);
    jmethodID on_value = (*env)->GetMethodID(env, c, "onValue", "(IJDLjava/lang/String;)I");
    jmethodID stat = (*env)->GetStaticMethodID(env, cls, "staticBack", "(Ljava/lang/String;F)Ljava/lang/String;");
    jstring text = (*env)->NewStringUTF(env, "from native"), back;
    jfieldID counter = (*env)->GetStaticFieldID(env, cls, "counter", "J");
    jint r = (*env)->CallIntMethod(env, callback, on_value, v, (jlong)v << 33, v / 4.0, text);
    /* A static method with a float among its arguments (passed to "..." as a double), and a static field of the class. */
    back = (jstring)(*env)->CallStaticObjectMethod(env, cls, stat, text, (jdouble)1.5f);
    (*env)->SetStaticLongField(env, cls, counter, (*env)->GetStaticLongField(env, cls, counter) + (*env)->GetStringLength(env, back));
    return r;
}

/* A new object of the app's own class, made by its constructor, with a field set after. */
JNIEXPORT jobject JNICALL Java_megler_test_natives_Lib_make(JNIEnv *env, jclass cls, jint factor, jstring label)
{
    jmethodID init = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
    jobject o = (*env)->NewObject(env, cls, init, factor);
    jfieldID f = (*env)->GetFieldID(env, cls, "label", "Ljava/lang/String;");
    (*env)->SetObjectField(env, o, f, label);
    return o;
}

/* -------------------------------------------------------------- exceptions */

JNIEXPORT void JNICALL Java_megler_test_natives_Lib_throwIt(JNIEnv *env, jclass cls, jstring message)
{
    const char *m = (*env)->GetStringUTFChars(env, message, NULL);
    (void)cls;
    (*env)->ThrowNew(env, (*env)->FindClass(env, "java/lang/IllegalStateException"), m);
    (*env)->ReleaseStringUTFChars(env, message, m);
}

/* An exception object of the app's own class, made here and thrown. */
JNIEXPORT void JNICALL Java_megler_test_natives_Lib_throwOwn(JNIEnv *env, jclass cls, jint code)
{
    jclass ex = (*env)->FindClass(env, LIB "$Failure");
    jmethodID init = (*env)->GetMethodID(env, ex, "<init>", "(I)V");
    (void)cls;
    (*env)->Throw(env, (jthrowable)(*env)->NewObject(env, ex, init, code));
}

/* Java throws inside a call made from here: seen, described, read and cleared, and its message handed back as an ordinary result. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_catchIt(JNIEnv *env, jclass cls, jobject runnable)
{
    jmethodID run = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, runnable), "run", "()V");
    jthrowable t;
    jstring message;
    (void)cls;
    (*env)->CallVoidMethod(env, runnable, run);
    if (!(*env)->ExceptionCheck(env)) return (*env)->NewStringUTF(env, "nothing was thrown");
    t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    message = (jstring)(*env)->CallObjectMethod(env, t, (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Throwable"), "getMessage", "()Ljava/lang/String;"));
    return (*env)->ExceptionCheck(env) ? NULL : message;
}

/* "found", or the name of the class of what FindClass threw: a class that is not there is an exception, not a class. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_find(JNIEnv *env, jclass cls, jstring name)
{
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    jclass found = (*env)->FindClass(env, n);
    jthrowable t;
    (void)cls;
    (*env)->ReleaseStringUTFChars(env, name, n);
    if (found) return (*env)->NewStringUTF(env, "found");
    t = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    if (!t) return (*env)->NewStringUTF(env, "null and no exception");
    return (jstring)(*env)->CallObjectMethod(env, (*env)->GetObjectClass(env, t),
                                             (*env)->GetMethodID(env, (*env)->FindClass(env, "java/lang/Class"), "getName", "()Ljava/lang/String;"));
}

/* ------------------------------------------------------------- references */

/* Each bit one thing that held; 0xFFFF is all of them. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_refs(JNIEnv *env, jclass cls, jobject o)
{
    jobject global = (*env)->NewGlobalRef(env, o), local = (*env)->NewLocalRef(env, o), kept;
    jweak weak = (*env)->NewWeakGlobalRef(env, o);
    jint bits = 0;
    jstring made;
    (void)cls;
    if (global && (*env)->IsSameObject(env, global, o)) bits |= 1;
    if (weak && (*env)->IsSameObject(env, weak, o) && !(*env)->IsSameObject(env, weak, NULL)) bits |= 2;
    if (local && (*env)->IsSameObject(env, local, global)) bits |= 4;
    if ((*env)->EnsureLocalCapacity(env, 64) == 0) bits |= 8;
    if ((*env)->PushLocalFrame(env, 32) == 0) bits |= 16;
    made = (*env)->NewStringUTF(env, "made in a frame");
    kept = (*env)->PopLocalFrame(env, made);
    if (kept && (*env)->GetStringUTFLength(env, (jstring)kept) == 15) bits |= 32;
    if ((*env)->IsInstanceOf(env, o, cls) && (*env)->IsAssignableFrom(env, (*env)->GetObjectClass(env, o), (*env)->FindClass(env, "java/lang/Object"))) bits |= 64;
    if ((*env)->IsSameObject(env, (*env)->GetSuperclass(env, cls), (*env)->FindClass(env, "java/lang/Object"))) bits |= 128;
    if ((*env)->GetVersion(env) >= JNI_VERSION_1_6) bits |= 256;
    {
        JavaVM *vm = NULL;
        if ((*env)->GetJavaVM(env, &vm) == 0 && vm == g_vm) bits |= 512;
    }
    if (!(*env)->ExceptionCheck(env)) bits |= 1024;
    /*
     * What kind of reference each is: the fresh string a local one, an object held globally a global one, nothing an
     * invalid one -- and this method's own parameter a local one though it is held globally too (`global`), as Chromium's
     * JavaParamRef checks every parameter of its native methods (Ozon's Cronet). A reference is the object here, so a
     * parameter is local while the method runs, whatever else refers to it.
     */
    {
        jobject held = (*env)->NewStringUTF(env, "held"), held_global = (*env)->NewGlobalRef(env, held);
        jweak held_weak = (*env)->NewWeakGlobalRef(env, held);
        if ((*env)->GetObjectRefType(env, held_global) == JNIGlobalRefType && (*env)->GetObjectRefType(env, kept) == JNILocalRefType
            && (*env)->GetObjectRefType(env, NULL) == JNIInvalidRefType && (*env)->GetObjectRefType(env, o) == JNILocalRefType) bits |= 2048;
        (*env)->DeleteGlobalRef(env, held_global);
        /* ...and once the global one is gone, the weak one is what holds it. */
        if ((*env)->GetObjectRefType(env, held_weak) == JNIWeakGlobalRefType) bits |= 4096;
        (*env)->DeleteWeakGlobalRef(env, held_weak);
    }
    /*
     * A reference JNI answers with is a local one though the object is held globally too -- a class found again, the class
     * of an object, what a call returns, an array's element, a new local reference to a global or a weak one -- for as
     * long as it is not deleted: Chromium checks each as it takes it into a ScopedJavaLocalRef, and Ozon's Cronet 112 ended
     * on that. Once deleted, the global reference is a global one again.
     */
    {
        jclass string = (*env)->FindClass(env, "java/lang/String"), again, of;
        jobject string_global = (*env)->NewGlobalRef(env, string), text = (*env)->NewStringUTF(env, "text"), text_global, same, element, fresh;
        jweak text_weak;
        jobjectArray array = (*env)->NewObjectArray(env, 1, string, NULL);
        int local_ok = 1, global_ok = 1;
        (*env)->DeleteLocalRef(env, string);
        global_ok &= (*env)->GetObjectRefType(env, string_global) == JNIGlobalRefType;
        again = (*env)->FindClass(env, "java/lang/String");
        local_ok &= (*env)->GetObjectRefType(env, again) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, again);
        global_ok &= (*env)->GetObjectRefType(env, string_global) == JNIGlobalRefType;
        of = (*env)->GetObjectClass(env, text);
        local_ok &= (*env)->GetObjectRefType(env, of) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, of);
        text_global = (*env)->NewGlobalRef(env, text);
        (*env)->SetObjectArrayElement(env, array, 0, text);
        (*env)->DeleteLocalRef(env, text);
        global_ok &= (*env)->GetObjectRefType(env, text_global) == JNIGlobalRefType;
        same = (*env)->CallObjectMethod(env, text_global, (*env)->GetMethodID(env, string_global, "toString", "()Ljava/lang/String;"));
        local_ok &= same && (*env)->IsSameObject(env, same, text_global) && (*env)->GetObjectRefType(env, same) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, same);
        global_ok &= (*env)->GetObjectRefType(env, text_global) == JNIGlobalRefType;
        element = (*env)->GetObjectArrayElement(env, array, 0);
        local_ok &= (*env)->GetObjectRefType(env, element) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, element);
        fresh = (*env)->NewLocalRef(env, text_global);
        local_ok &= (*env)->GetObjectRefType(env, fresh) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, fresh);
        global_ok &= (*env)->GetObjectRefType(env, text_global) == JNIGlobalRefType;
        text_weak = (*env)->NewWeakGlobalRef(env, text_global);
        (*env)->DeleteGlobalRef(env, text_global);
        global_ok &= (*env)->GetObjectRefType(env, text_weak) == JNIWeakGlobalRefType;
        fresh = (*env)->NewLocalRef(env, text_weak);
        local_ok &= (*env)->GetObjectRefType(env, fresh) == JNILocalRefType;
        (*env)->DeleteLocalRef(env, fresh);
        global_ok &= (*env)->GetObjectRefType(env, text_weak) == JNIWeakGlobalRefType;
        (*env)->DeleteWeakGlobalRef(env, text_weak);
        (*env)->DeleteGlobalRef(env, string_global);
        (*env)->DeleteLocalRef(env, array);
        if (local_ok) bits |= 16384;
        if (global_ok) bits |= 32768;
    }
    (*env)->DeleteLocalRef(env, local);
    (*env)->DeleteGlobalRef(env, global);
    (*env)->DeleteWeakGlobalRef(env, weak);
    if ((*env)->GetObjectRefType(env, o) == JNILocalRefType) bits |= 8192;
    return bits;
}

/* A Method object to its identifier and called through it; an identifier to a Method object, for Java to look at. */
JNIEXPORT jobject JNICALL Java_megler_test_natives_Lib_reflect(JNIEnv *env, jclass cls, jobject method, jobject field, jintArray out)
{
    jmethodID m = (*env)->FromReflectedMethod(env, method);
    jfieldID f = (*env)->FromReflectedField(env, field);
    jint v[2];
    v[0] = m ? (*env)->CallStaticIntMethod(env, cls, m, 20, 22) : -1;
    v[1] = f ? (jint)(*env)->GetStaticLongField(env, cls, f) : -1;
    (*env)->SetIntArrayRegion(env, out, 0, 2, v);
    (*env)->SetObjectArrayElement(env, (jobjectArray)(*env)->GetStaticObjectField(env, cls, (*env)->GetStaticFieldID(env, cls, "reflected", "[Ljava/lang/Object;")), 0,
                                  (*env)->ToReflectedField(env, cls, (*env)->GetFieldID(env, cls, "factor", "I"), JNI_FALSE));
    return (*env)->ToReflectedMethod(env, cls, (*env)->GetStaticMethodID(env, cls, "staticBack", "(Ljava/lang/String;F)Ljava/lang/String;"), JNI_TRUE);
}

/* The object's monitor held from here while Java is asked whether this thread holds it. */
JNIEXPORT jboolean JNICALL Java_megler_test_natives_Lib_monitor(JNIEnv *env, jclass cls, jobject o)
{
    jmethodID holds = (*env)->GetStaticMethodID(env, (*env)->FindClass(env, "java/lang/Thread"), "holdsLock", "(Ljava/lang/Object;)Z");
    jclass thread = (*env)->FindClass(env, "java/lang/Thread");
    jboolean during, after;
    (void)cls;
    if ((*env)->MonitorEnter(env, o) != JNI_OK) return JNI_FALSE;
    during = (*env)->CallStaticBooleanMethod(env, thread, holds, o);
    if ((*env)->MonitorExit(env, o) != JNI_OK) return JNI_FALSE;
    after = (*env)->CallStaticBooleanMethod(env, thread, holds, o);
    return during && !after;
}

/* ------------------------------------------------------------------ threads */

typedef struct job {
    jobject report;
    char name[40];
    int daemon;
} job;

/*
 * A thread of the library's own. It is not Java's until it attaches; then it
 * has a Thread of the name it gave, and what it tells Java is: what GetEnv
 * said before and after, whether FindClass found a class of the app's (on
 * Android it does not, here: the system class loader is asked), a class of
 * Android's, and whether the class kept from JNI_OnLoad still serves.
 */
static void *worker(void *arg)
{
    job *j = (job *)arg;
    JNIEnv *env = NULL;
    JavaVMAttachArgs args;
    jint before, rc, after;
    jclass app, string;
    jboolean kept;
    before = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    args.version = JNI_VERSION_1_6;
    args.name = j->name;
    args.group = NULL;
    rc = j->daemon ? (*g_vm)->AttachCurrentThreadAsDaemon(g_vm, &env, &args) : (*g_vm)->AttachCurrentThread(g_vm, &env, &args);
    if (rc != JNI_OK || !env) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "the thread could not attach: %d", (int)rc);
        free(j);
        return NULL;
    }
    after = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    app = (*env)->FindClass(env, LIB);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    string = (*env)->FindClass(env, "java/lang/String");
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    kept = (*env)->GetStaticMethodID(env, g_lib, "staticBack", "(Ljava/lang/String;F)Ljava/lang/String;") != NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->CallVoidMethod(env, j->report, g_on_thread, before, rc, after, (jboolean)(app != NULL), (jboolean)(string != NULL), kept);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteGlobalRef(env, j->report);
    (*g_vm)->DetachCurrentThread(g_vm);
    g_after_detach = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    free(j);
    return NULL;
}

JNIEXPORT jboolean JNICALL Java_megler_test_natives_Lib_startThread(JNIEnv *env, jclass cls, jobject report, jstring name, jboolean daemon)
{
    job *j = (job *)calloc(1, sizeof(*j));
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    pthread_t t;
    (void)cls;
    j->report = (*env)->NewGlobalRef(env, report);
    snprintf(j->name, sizeof(j->name), "%s", n);
    j->daemon = daemon;
    (*env)->ReleaseStringUTFChars(env, name, n);
    if (pthread_create(&t, NULL, worker, j) != 0) return JNI_FALSE;
    pthread_join(t, NULL);
    return JNI_TRUE;
}

/* What GetEnv said on the library's thread after it had detached. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_afterDetach(JNIEnv *env, jclass cls) { (void)env; (void)cls; return g_after_detach; }

/* ---------------------------------------------- the library this one needs */

JNIEXPORT jlong JNICALL Java_megler_test_natives_Lib_dep(JNIEnv *env, jclass cls, jlong v)
{
    (void)env; (void)cls;
    return natives_dep_scale(v, natives_dep_answer());
}

/* ------------------------------------------------------------- the NDK */

JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_readAsset(JNIEnv *env, jclass cls, jobject asset_manager, jstring name)
{
    AAssetManager *am = AAssetManager_fromJava(env, asset_manager);
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    AAsset *a = am ? AAssetManager_open(am, n, AASSET_MODE_BUFFER) : NULL;
    char buf[200];
    int got = 0;
    (void)cls;
    (*env)->ReleaseStringUTFChars(env, name, n);
    if (!a) return NULL;
    if (AAsset_getLength(a) < (off_t)sizeof(buf)) got = AAsset_read(a, buf, sizeof(buf) - 1);
    AAsset_close(a);
    buf[got > 0 ? got : 0] = 0;
    return (*env)->NewStringUTF(env, buf);
}

/*
 * Paints a Bitmap through libjnigraphics: the left half red, the right half
 * blue, a green square of a quarter of the width in the top left corner, and
 * the bottom right pixel half-transparent white (premultiplied, as the NDK
 * says the memory is). What it was told of the bitmap is the string returned.
 */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_paint(JNIEnv *env, jclass cls, jobject bitmap)
{
    AndroidBitmapInfo info;
    void *pixels = NULL;
    char text[120];
    uint32_t x, y;
    int rc;
    (void)cls;
    memset(&info, 0, sizeof(info));
    rc = AndroidBitmap_getInfo(env, bitmap, &info);
    if (rc == ANDROID_BITMAP_RESULT_SUCCESS && info.format == ANDROID_BITMAP_FORMAT_RGBA_8888) rc = AndroidBitmap_lockPixels(env, bitmap, &pixels);
    if (rc == ANDROID_BITMAP_RESULT_SUCCESS && pixels) {
        for (y = 0; y < info.height; y++) {
            uint8_t *row = (uint8_t *)pixels + (size_t)y * info.stride;
            for (x = 0; x < info.width; x++) {
                uint8_t *p = row + 4 * x;
                int green = x < info.width / 4 && y < info.height / 4;
                p[0] = !green && x < info.width / 2 ? 255 : 0;
                p[1] = green ? 255 : 0;
                p[2] = !green && x >= info.width / 2 ? 255 : 0;
                p[3] = 255;
            }
        }
        memset((uint8_t *)pixels + (size_t)(info.height - 1) * info.stride + 4 * (info.width - 1), 128, 4);
        rc = AndroidBitmap_unlockPixels(env, bitmap);
    }
    snprintf(text, sizeof(text), "%ux%u stride=%u format=%d rc=%d", (unsigned)info.width, (unsigned)info.height, (unsigned)info.stride, (int)info.format, rc);
    return (*env)->NewStringUTF(env, text);
}

static unsigned char g_shared[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

/* A direct buffer of Java's making: read here, and written where Java will look. */
JNIEXPORT jlong JNICALL Java_megler_test_natives_Lib_bufferSum(JNIEnv *env, jclass cls, jobject buffer)
{
    unsigned char *p = (unsigned char *)(*env)->GetDirectBufferAddress(env, buffer);
    jlong n = (*env)->GetDirectBufferCapacity(env, buffer), i, total = 0;
    (void)cls;
    if (!p) return -1;
    for (i = 0; i < n; i++) total += p[i];
    p[n - 1] = 77;
    return total * 1000 + n;
}

/* And one of this library's memory, for Java to read and write. */
JNIEXPORT jobject JNICALL Java_megler_test_natives_Lib_sharedBuffer(JNIEnv *env, jclass cls) { (void)cls; return (*env)->NewDirectByteBuffer(env, g_shared, sizeof(g_shared)); }
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_sharedAt(JNIEnv *env, jclass cls, jint i) { (void)env; (void)cls; return g_shared[i & 7]; }

/* The device as the NDK describes it. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_configuration(JNIEnv *env, jclass cls, jobject asset_manager)
{
    AConfiguration *c = AConfiguration_new();
    char text[160], lang[3] = { 0, 0, 0 };
    (void)cls;
    AConfiguration_fromAssetManager(c, AAssetManager_fromJava(env, asset_manager));
    AConfiguration_getLanguage(c, lang);
    snprintf(text, sizeof(text), "density=%d sdk=%d orientation=%d w=%d h=%d lang=%s", (int)AConfiguration_getDensity(c), (int)AConfiguration_getSdkVersion(c),
             (int)AConfiguration_getOrientation(c), (int)AConfiguration_getScreenWidthDp(c), (int)AConfiguration_getScreenHeightDp(c), lang);
    AConfiguration_delete(c);
    return (*env)->NewStringUTF(env, text);
}

/* --------------------------------------------- the main thread's looper */

static int g_pipe[2] = { -1, -1 };
static jobject g_looper_report;

/* Runs between two messages of the thread whose looper it was added to; tells Java, and is not called again. */
static int on_readable(int fd, int events, void *data)
{
    JNIEnv *env = NULL;
    char byte = 0;
    (void)data;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return 0;
    if (read(fd, &byte, 1) == 1 && g_looper_report) {
        jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, g_looper_report), "onLooper", "(II)V");
        (*env)->CallVoidMethod(env, g_looper_report, m, (jint)byte, (jint)events);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, g_looper_report);
        g_looper_report = NULL;
    }
    close(g_pipe[0]);
    close(g_pipe[1]);
    return 0;
}

/* 0 when this thread has no looper; otherwise a pipe is watched on it, and a byte written: the callback comes with the thread's next turn. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_watchLooper(JNIEnv *env, jclass cls, jobject report)
{
    ALooper *looper = ALooper_forThread();
    (void)cls;
    if (!looper) return 0;
    if (pipe(g_pipe) != 0) return -1;
    g_looper_report = (*env)->NewGlobalRef(env, report);
    if (ALooper_addFd(looper, g_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_readable, NULL) != 1) return -2;
    return write(g_pipe[1], "L", 1) == 1 ? 1 : -3;
}

/* ------------------------------------------------------------- the sensors */

/*
 * The phone's sensors as native code has them, on the tests' stand-in (core/tests/mg_sensor_standin.c): the list and its
 * defaults described, then an event queue on this thread's looper, as NativeActivity's glue makes one, with the
 * accelerometer at 50 a second and the light. Its callback reads what comes until it has three of the accelerometer's and
 * one of the light's, checks them -- Android's values, the vector's accuracy, times on CLOCK_BOOTTIME (elapsedRealtimeNanos)
 * that go forward at about the rate asked for -- tells Java, and ends the queue.
 */
static ASensorManager *g_sensor_manager;
static ASensorEventQueue *g_sensor_queue;
static const ASensor *g_sensor_accel, *g_sensor_light;
static jobject g_sensor_report;
static int g_sensor_accels, g_sensor_lights, g_sensor_bad;
static int64_t g_sensor_last, g_sensor_gap = INT64_MAX;
static char g_sensor_why[160];

static int sensor_near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

static int64_t boottime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int on_sensors(int fd, int events, void *data)
{
    ASensorEvent e;
    JNIEnv *env = NULL;
    (void)fd;
    (void)events;
    (void)data;
    while (ASensorEventQueue_getEvents(g_sensor_queue, &e, 1) > 0) {
        int64_t now = boottime_ns();
        int good = e.version == (int)sizeof(ASensorEvent) && e.timestamp <= now && now - e.timestamp < 5000000000LL;
        if (e.type == ASENSOR_TYPE_ACCELEROMETER) {
            good = good && sensor_near(e.acceleration.x, -2.4516625f) && sensor_near(e.acceleration.y, 4.903325f) && sensor_near(e.acceleration.z, 7.3549875f)
                   && e.acceleration.status == ASENSOR_STATUS_ACCURACY_HIGH;
            if (g_sensor_accels > 0) {
                if (e.timestamp <= g_sensor_last) good = 0;
                if (e.timestamp - g_sensor_last < g_sensor_gap) g_sensor_gap = e.timestamp - g_sensor_last;
            }
            g_sensor_last = e.timestamp;
            g_sensor_accels++;
        } else if (e.type == ASENSOR_TYPE_LIGHT) {
            good = good && sensor_near(e.light, 321.5f);
            g_sensor_lights++;
        } else {
            good = 0;
        }
        if (!good && !g_sensor_bad++)
            snprintf(g_sensor_why, sizeof(g_sensor_why), "type %d values %g %g %g status %d at %lld ns", (int)e.type, e.data[0], e.data[1], e.data[2],
                     (int)e.acceleration.status, (long long)(boottime_ns() - e.timestamp));
    }
    if (g_sensor_accels < 3 || g_sensor_lights < 1) return 1;
    ASensorEventQueue_disableSensor(g_sensor_queue, g_sensor_accel);
    ASensorEventQueue_disableSensor(g_sensor_queue, g_sensor_light);
    ASensorManager_destroyEventQueue(g_sensor_manager, g_sensor_queue);
    g_sensor_queue = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK && g_sensor_report) {
        char text[300];
        jmethodID m = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, g_sensor_report), "onSensors", "(ZLjava/lang/String;)V");
        int ok = !g_sensor_bad && g_sensor_gap >= 16000000;
        snprintf(text, sizeof(text), "accelerometer %d light %d gap %lld us%s%s", g_sensor_accels, g_sensor_lights, (long long)(g_sensor_gap / 1000),
                 g_sensor_bad ? " bad: " : "", g_sensor_why);
        (*env)->CallVoidMethod(env, g_sensor_report, m, (jboolean)ok, (*env)->NewStringUTF(env, text));
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, g_sensor_report);
        g_sensor_report = NULL;
    }
    return 0;
}

/* 1 when the list is as the stand-in's and the queue runs; a negative number for the first thing that was not. */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_watchSensors(JNIEnv *env, jclass cls, jobject report)
{
    /* As NativeActivity's sample glue asks for the manager: by name, for the package where Android has that (API 26). */
    typedef ASensorManager *(*for_package_fn)(const char *);
    void *android = dlopen("libandroid.so", RTLD_NOW);
    for_package_fn for_package = android ? (for_package_fn)dlsym(android, "ASensorManager_getInstanceForPackage") : NULL;
    const ASensor *proximity;
    ASensorList list = NULL;
    ALooper *looper;
    int n;
    (void)cls;
    if (!for_package) return -1;
    g_sensor_manager = for_package("megler.test.natives");
    if (!g_sensor_manager || g_sensor_manager != ASensorManager_getInstance()) return -2;
    n = ASensorManager_getSensorList(g_sensor_manager, &list);
    if (n != 11 || !list || ASensor_getType(list[0]) != ASENSOR_TYPE_ACCELEROMETER) return -3;
    g_sensor_accel = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_ACCELEROMETER);
    g_sensor_light = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_LIGHT);
    if (!g_sensor_accel || !g_sensor_light || g_sensor_accel != list[0]) return -4;
    if (strcmp(ASensor_getName(g_sensor_accel), "Test Accelerometer") || strcmp(ASensor_getVendor(g_sensor_accel), "Megler Tests")
        || strcmp(ASensor_getStringType(g_sensor_accel), "android.sensor.accelerometer") || ASensor_getMinDelay(g_sensor_accel) != 10000
        || !sensor_near(ASensor_getResolution(g_sensor_accel), 0.0023942f) || ASensor_getReportingMode(g_sensor_accel) != AREPORTING_MODE_CONTINUOUS
        || ASensor_isWakeUpSensor(g_sensor_accel))
        return -5;
    /* The proximity's default wakes the device; one that does not, there is none. */
    proximity = ASensorManager_getDefaultSensor(g_sensor_manager, ASENSOR_TYPE_PROXIMITY);
    if (!proximity || !ASensor_isWakeUpSensor(proximity) || ASensorManager_getDefaultSensorEx(g_sensor_manager, ASENSOR_TYPE_PROXIMITY, false)
        || ASensor_getMinDelay(proximity) != 0 || ASensor_getReportingMode(proximity) != AREPORTING_MODE_ON_CHANGE)
        return -6;
    looper = ALooper_forThread();
    if (!looper) return -7;
    g_sensor_report = (*env)->NewGlobalRef(env, report);
    g_sensor_queue = ASensorManager_createEventQueue(g_sensor_manager, looper, ALOOPER_POLL_CALLBACK, on_sensors, NULL);
    if (!g_sensor_queue) return -8;
    if (ASensorEventQueue_enableSensor(g_sensor_queue, g_sensor_accel) != 0 || ASensorEventQueue_setEventRate(g_sensor_queue, g_sensor_accel, 20000) != 0
        || ASensorEventQueue_enableSensor(g_sensor_queue, g_sensor_light) != 0)
        return -9;
    return 1;
}

/* ---------------------------------------------------- objects held across a collection */

static jobject g_held;

/*
 * Objects a native method holds while the collector runs: a string and an array of strings held only in this function's
 * locals (local references, which the collector finds on the stack), and a string held by a global reference made in an
 * earlier call. System.gc() is called through JNI in between, after garbage enough to be worth freeing; everything must read
 * as it was. 1 when it does; what went wrong otherwise.
 */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_holdAcrossGc(JNIEnv *env, jclass cls)
{
    jclass system = (*env)->FindClass(env, "java/lang/System"), string = (*env)->FindClass(env, "java/lang/String");
    jmethodID gc = (*env)->GetStaticMethodID(env, system, "gc", "()V");
    jstring local = (*env)->NewStringUTF(env, "held by a local reference");
    jobjectArray array = (*env)->NewObjectArray(env, 3, string, NULL);
    const char *text;
    char word[32];
    int i, ok;
    (void)cls;
    if (!g_held) g_held = (*env)->NewGlobalRef(env, (*env)->NewStringUTF(env, "held by a global reference"));
    for (i = 0; i < 3; i++) {
        snprintf(word, sizeof(word), "element %d", i);
        (*env)->SetObjectArrayElement(env, array, i, (*env)->NewStringUTF(env, word));
    }
    /* Garbage, each a local reference let go at once. */
    for (i = 0; i < 2000; i++) (*env)->DeleteLocalRef(env, (*env)->NewByteArray(env, 1000));
    (*env)->CallStaticVoidMethod(env, system, gc);
    if ((*env)->ExceptionCheck(env)) return -1;
    text = (*env)->GetStringUTFChars(env, local, NULL);
    ok = text && !strcmp(text, "held by a local reference");
    if (text) (*env)->ReleaseStringUTFChars(env, local, text);
    if (!ok) return -2;
    text = (*env)->GetStringUTFChars(env, (jstring)g_held, NULL);
    ok = text && !strcmp(text, "held by a global reference");
    if (text) (*env)->ReleaseStringUTFChars(env, (jstring)g_held, text);
    if (!ok) return -3;
    for (i = 0; i < 3; i++) {
        jstring e = (jstring)(*env)->GetObjectArrayElement(env, array, i);
        snprintf(word, sizeof(word), "element %d", i);
        text = e ? (*env)->GetStringUTFChars(env, e, NULL) : NULL;
        ok = text && !strcmp(text, word);
        if (text) (*env)->ReleaseStringUTFChars(env, e, text);
        if (!ok) return -4 - i;
    }
    return 1;
}

/* ---------------------------------------------------------- the system's ICU */

/* A function of an ICU library by its name and the version ICU gives its names ("u_strlen_60"), the first version that has it. */
static void *icu_function(void *lib, const char *name, int *version)
{
    char versioned[64];
    int v;
    for (v = 44; v <= 80; v++) {
        void *f;
        snprintf(versioned, sizeof(versioned), "%s_%d", name, v);
        f = dlsym(lib, versioned);
        if (f) {
            *version = v;
            return f;
        }
    }
    return NULL;
}

/*
 * A string made as VK mp3 mod's app-lock library makes one: AllocObject on java.lang.String, then the constructor
 * String(byte[], String) called on it with CallVoidMethodA -- and the same with CallNonvirtualVoidMethod and String(char[])
 * --, the allocated object then used as the string. On Android the runtime turns such a call into StringFactory's and the
 * reference into the new string. The two strings joined with a '|'; null if a call threw.
 */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_allocatedString(JNIEnv *env, jclass cls, jbyteArray bytes, jcharArray chars)
{
    jclass string = (*env)->FindClass(env, "java/lang/String");
    jmethodID from_bytes = (*env)->GetMethodID(env, string, "<init>", "([BLjava/lang/String;)V");
    jmethodID from_chars = (*env)->GetMethodID(env, string, "<init>", "([C)V");
    jmethodID concat = (*env)->GetMethodID(env, string, "concat", "(Ljava/lang/String;)Ljava/lang/String;");
    jobject a, b, joined;
    jvalue args[2];
    (void)cls;
    a = (*env)->AllocObject(env, string);
    args[0].l = bytes;
    args[1].l = (*env)->NewStringUTF(env, "UTF-8");
    (*env)->CallVoidMethodA(env, a, from_bytes, args);
    if ((*env)->ExceptionCheck(env)) return NULL;
    b = (*env)->AllocObject(env, string);
    (*env)->CallNonvirtualVoidMethod(env, b, string, from_chars, chars);
    if ((*env)->ExceptionCheck(env)) return NULL;
    joined = (*env)->CallObjectMethod(env, a, concat, (*env)->NewStringUTF(env, "|"));
    return (jstring)(*env)->CallObjectMethod(env, joined, concat, b);
}

/*
 * The system's ICU, opened by name as Android 7 to 9 let an app open it (public.libraries.android.txt lists libicuuc.so and
 * libicui18n.so), and a function of each found as MX Player's player library finds them, trying the versions: u_strlen,
 * called, and ucsdet_open. The version of both, -1 when a library did not open, -2 when a function was not found, -3 when
 * u_strlen counted wrong or the two disagree.
 */
/* A system property as native code reads it (__system_property_get): "" when there is none. */
JNIEXPORT jstring JNICALL Java_megler_test_natives_Lib_systemProperty(JNIEnv *env, jclass cls, jstring name)
{
    char value[PROP_VALUE_MAX] = "";
    const char *n = (*env)->GetStringUTFChars(env, name, NULL);
    (void)cls;
    if (n) {
        __system_property_get(n, value);
        (*env)->ReleaseStringUTFChars(env, name, n);
    }
    return (*env)->NewStringUTF(env, value);
}

/*
 * A library the app wrote itself, opened by native code by its path: dlopen loads that file, as bionic's does, its constructor
 * run. The step dlopen-own-file hands a copy of libnatives_dep.so under a name no APK has, in a folder no library came from
 * before. Its natives_dep_answer (4242 once its constructor has run), or -1 when it did not open, which is logged.
 */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_dlopenAnswer(JNIEnv *env, jclass cls, jstring path)
{
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    void *h = p ? dlopen(p, RTLD_NOW) : NULL;
    int (*answer)(void) = h ? (int (*)(void))dlsym(h, "natives_dep_answer") : NULL;
    (void)cls;
    if (!h) __android_log_print(ANDROID_LOG_INFO, "NativesLib", "dlopen(%s): %s", p ? p : "(null)", dlerror());
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return answer ? answer() : -1;
}

JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_icuVersion(JNIEnv *env, jclass cls)
{
    static const uint16_t text[] = { 'M', 'e', 'g', 'l', 'e', 'r', 0 };
    void *uc = dlopen("libicuuc.so", RTLD_NOW), *i18n = dlopen("libicui18n.so", RTLD_NOW);
    int32_t (*u_strlen)(const uint16_t *);
    int v = 0, w = 0;
    (void)env; (void)cls;
    if (!uc || !i18n) return -1;
    u_strlen = (int32_t (*)(const uint16_t *))icu_function(uc, "u_strlen", &v);
    if (!u_strlen || !icu_function(i18n, "ucsdet_open", &w)) return -2;
    return u_strlen(text) == 6 && v == w ? v : -3;
}

/*
 * Java called back with 78 arguments, 45 ints and 33 objects, as WhatsApp's native code calls JniBridge.jnidispatch: through
 * the "..." call and through the jvalue one. Both answers the same, or -1.
 */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_manyArgs(JNIEnv *env, jclass cls, jobjectArray names)
{
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "manyBack", "(IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIILjava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)I");
    jobject o[33];
    jvalue v[78];
    jint a, b;
    int k;
    if (!m) return -2;
    for (k = 0; k < 33; k++) o[k] = (*env)->GetObjectArrayElement(env, names, k);
    a = (*env)->CallStaticIntMethod(env, cls, m, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
                                    o[0], o[1], o[2], o[3], o[4], o[5], o[6], o[7], o[8], o[9], o[10], o[11], o[12], o[13], o[14], o[15], o[16], o[17], o[18], o[19], o[20], o[21], o[22], o[23], o[24], o[25], o[26], o[27], o[28], o[29], o[30], o[31], o[32]);
    for (k = 0; k < 45; k++) v[k].i = k + 1;
    for (k = 0; k < 33; k++) v[45 + k].l = o[k];
    b = (*env)->CallStaticIntMethodA(env, cls, m, v);
    return a == b ? a : -1;
}

/*
 * One level of a layout measured by native code that calls back into Java for each child, as Meta's FlexLayout does
 * (FlexLayoutNative.jni_calculateLayout <-> FlexLayoutNativeMeasureCallback.measureNative): Lib.down(n - 1), from a frame
 * of a native layout's size, holding a string of its own that only this frame refers to while Java runs below it
 * (where the collector runs: Lib.deepest). A million more when the string is not what it was after.
 */
JNIEXPORT jint JNICALL Java_megler_test_natives_Lib_pingPong(JNIEnv *env, jclass cls, jint n)
{
    volatile char frame[384];
    char want[32];
    const char *got;
    jmethodID down = (*env)->GetStaticMethodID(env, cls, "down", "(I)I");
    jstring mine;
    jint r;
    int same;
    frame[0] = (char)n;
    frame[sizeof(frame) - 1] = (char)n;
    if (!down) return -1;
    snprintf(want, sizeof(want), "level %d of the layout", (int)n);
    mine = (*env)->NewStringUTF(env, want);
    r = (*env)->CallStaticIntMethod(env, cls, down, n - 1);
    if ((*env)->ExceptionCheck(env)) return -1;
    got = (*env)->GetStringUTFChars(env, mine, NULL);
    same = got && !strcmp(got, want);
    if (got) (*env)->ReleaseStringUTFChars(env, mine, got);
    return r + (frame[0] == frame[sizeof(frame) - 1] && same ? 0 : 1000000);
}
