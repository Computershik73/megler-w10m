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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Each bit one thing that held; 0x7FF is all of them. */
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
    (*env)->DeleteLocalRef(env, local);
    (*env)->DeleteWeakGlobalRef(env, weak);
    (*env)->DeleteGlobalRef(env, global);
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
