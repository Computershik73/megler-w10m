/*
 * A Dalvik runtime: an interpreter for DEX bytecode, so that apps whose code is
 * Java or Kotlin can run. See docs/JAVA.md for the design and for what is and
 * is not here yet.
 *
 * Android's own runtime cannot be loaded as the app's native libraries are: it
 * wants the kernel's system calls, signals and memory layout. This one is
 * portable C over the host layer (core/src/host), like the rest of the core,
 * and runs the same on the phone (32-bit ARM), under qemu-arm, and on a PC.
 *
 * Its objects are the JNI layer's: every object here begins with an hk_jobj
 * (jni/hk_jni.h), so a pointer to one is a jobject as native code knows it,
 * and a native method written for the interpreter has the shape of a
 * driver's (hk_jhle, hk_jcall). Once the runtime is started the two are one
 * Java world: jni/hk_jni.c sends what native code asks about a class that a
 * DEX file defines here, makes its strings and arrays here, and a native
 * method of such a class that nothing registered below is looked for in the
 * app's libraries. A class no DEX file defines stays what it was, implemented
 * in C or not at all. Until it is started the runtime costs nothing.
 *
 * What makes up java.lang without libcore is in hk_dvm_boot.c: enough of
 * Object, Class, String, StringBuilder, Throwable, the exceptions the
 * interpreter raises, Enum and the boxes for bytecode to mean something. The
 * plan is libcore's own bytecode in its place (docs/JAVA.md).
 *
 * Nothing is collected yet: an object lives as long as the process.
 */
#ifndef HK_DVM_H
#define HK_DVM_H

#include "../hk_base.h"
#include "../jni/hk_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ setup */

/*
 * Starts the runtime. `boot` lists .dex files that make up the boot class
 * path, searched in order before anything added later; with none, the built-in
 * java.lang of hk_dvm_boot.c is the boot class path. False when
 * java/lang/Object cannot be found in it. Call once. It starts the JNI layer
 * (hk_jni_init) if nothing has.
 */
bool hk_dvm_start(const char *const *boot, int nboot);
/*
 * Starts it on Android's own libcore: `root` is a folder laid out like
 * Android's /system, with the boot class path as DEX files in
 * `framework` (core-oj.dex, core-libart.dex and what else of conscrypt,
 * okhttp, bouncycastle and apache-xml is there), libcore's native libraries
 * in `lib`, ICU's data in `usr/icu` and the time zones in `usr/share/zoneinfo`.
 * java.lang is then libcore's, with what Android's runtime does for it done
 * in hk_dvm_art.c. The libraries are loaded where guest code can run
 * (hk_dvm_load_libcore_natives); without them libcore works as far as its
 * bytecode goes. False when the files are not there.
 */
bool hk_dvm_start_android(const char *root);
/*
 * Class loaders. There is one class path and no loading through a loader:
 * every class is found by name wherever it was added. But an app's classes
 * must say they come from the app's loader (Class.getClassLoader: resources
 * and services are found through it), and the system's that they come from
 * none. hk_dvm_app_follows marks the line: DEX files added after it are the
 * app's. hk_dvm_set_app_loader gives their classes the loader object, those
 * linked already and those linked later.
 */
void hk_dvm_app_follows(void);
void hk_dvm_set_app_loader(hk_jobj *loader);
/*
 * Says that a file whose classes are on the class path already is known to
 * the app under this (guest) path: when one of the app's class loaders opens
 * it (dalvik.system.DexFile), nothing is read again. A file the app opens
 * that was not named here is read then, and its classes join the path.
 */
void hk_dvm_dex_source_added(const char *guest_path);

/* Whether libcore's own native libraries are loaded (hk_dvm_start_android loads them where they are and can run). */
bool hk_dvm_libcore_natives(void);
/*
 * Loads one of the app's libraries of native methods by its file name
 * ("libfoo.so"; a path's last part is what is looked for in the APK) through
 * the layer's linker and runs its JNI_OnLoad: what System.loadLibrary comes
 * to for a class of the app's. False, the reason in the log, when it could
 * not be loaded, or on a host that cannot run it.
 */
bool hk_dvm_load_library(const char *name_or_path);
bool hk_dvm_running(void);

/*
 * Adds a DEX file's classes after those already there: where a class is
 * defined twice, the first definition wins, as on Android's class path. The
 * bytes are used where they are, for good, and must be aligned to four bytes
 * (anything from malloc is). False when it is not a DEX file this runtime can
 * read; the log says why.
 */
bool hk_dvm_add_dex(const void *data, size_t size, const char *label);
/* The same for a .dex file read whole. */
bool hk_dvm_add_dex_file(const char *path);
/* Every classesN.dex of an APK or a jar (hk_dvm_apk.c; needs zip/hk_zip.c). Returns how many were added. */
int hk_dvm_add_apk(const char *path);

/*
 * Native methods implemented in C, by class, name and signature: a table
 * ended by an entry with cls == NULL, as hk_jni_register takes. sig NULL
 * matches any signature. Later tables win over earlier ones. A method is bound
 * when its class is linked, or at its first call if nothing was registered
 * for it by then.
 *
 * The function receives its parameters in declaration order, one hk_jvalue
 * each (a long or a double is one), and writes its result to the member of
 * `ret` for its return type. It reports failure by throwing (hk_dvm_throw)
 * and returning. An object it returns needs no reference counting.
 */
void hk_dvm_register(const hk_jhle *table);

/* ---------------------------------------------------------------- calling */

/*
 * Calls a static method by name (class as "java/lang/String", signature as
 * "(I)V"), initialising its class first. `args` holds one value per
 * parameter; `ret` may be NULL. False when it threw: the exception is then
 * pending on the calling thread (hk_dvm_describe_pending, hk_dvm_clear_pending).
 */
bool hk_dvm_call_static(const char *cls, const char *name, const char *sig, const hk_jvalue *args, hk_jvalue *ret);
/* Calls a method on an object as invoke-virtual would: the object's own class decides which. */
bool hk_dvm_call_virtual(hk_jobj *self, const char *name, const char *sig, const hk_jvalue *args, hk_jvalue *ret);
/* A new object, made with the constructor of that signature ("(I)V"); NULL when it threw. */
hk_jobj *hk_dvm_new(const char *cls, const char *ctor_sig, const hk_jvalue *args);

/* ---------------------------------------------------------------- objects */

/* The java.lang.Class object of a class, loading it if need be; NULL, with NoClassDefFoundError pending, when nothing defines it. */
hk_jobj *hk_dvm_class_object(const char *cls);
/* The class of an object as a JNI name ("java/lang/String", "[I"); "null" for NULL. */
const char *hk_dvm_class_name(const hk_jobj *o);
bool hk_dvm_instance_of(hk_jobj *o, const char *cls);

hk_jobj *hk_dvm_new_string(const char *utf8);
hk_jobj *hk_dvm_new_string_utf16(const u16 *chars, i32 len);
/* The string's text: UTF-16 as it is kept, or UTF-8 made on first asking and kept with the string. NULL for NULL or not a string. */
const u16 *hk_dvm_string_utf16(hk_jobj *s, i32 *len);
const char *hk_dvm_string_utf8(hk_jobj *s);
/* An array of `len` zeros or nulls; `array_class` is "[I", "[Ljava/lang/String;", "[[D". Its elements are at u.arr.data / u.oarr.v. */
hk_jobj *hk_dvm_new_array(const char *array_class, i32 len);

/* A static field's value; false (nothing pending) when the class has no such field. The class is initialised first. */
bool hk_dvm_get_static(const char *cls, const char *name, hk_jvalue *out);
bool hk_dvm_set_static(const char *cls, const char *name, hk_jvalue v);
/* An instance field's value, looked for by name in the object's class and above. */
bool hk_dvm_get_field(hk_jobj *o, const char *name, hk_jvalue *out);
bool hk_dvm_set_field(hk_jobj *o, const char *name, hk_jvalue v);

/* ------------------------------------------------------------- exceptions */

/* Makes an exception of that class with the message and leaves it pending on the calling thread. Always returns false. */
bool hk_dvm_throw(const char *cls, const char *fmt, ...) HK_PRINTF(2, 3);
hk_jobj *hk_dvm_pending(void);
void hk_dvm_clear_pending(void);
/*
 * The pending exception as text, for a log: "java.lang.Foo: message", then
 * " | caused by ..." for each cause. NULL when nothing is pending.
 */
const char *hk_dvm_describe_pending(char *buf, size_t n);
/*
 * Where the pending exception was made: one line per frame, innermost first,
 * "  at some.Class.method(File.java:12)" (or "(pc 7)" where the DEX has no
 * line numbers, "(native)" for a native method). Returns the number of frames.
 */
int hk_dvm_pending_stack(char *buf, size_t n);

/* ------------------------------------------------------------ diagnostics */

/* 0: quiet. 1: classes linked and initialised, natives bound, exceptions thrown. 2: every call. 3: every instruction. */
void hk_dvm_set_trace(int level);
/*
 * A limit on the instructions one call from C may execute, for running code
 * that is not trusted to end: when it is used up, java.lang.InternalError is
 * thrown where execution stands, and nothing in the code can catch it. While
 * a limit is set, Object.wait() returns at once (Java lets any wait end
 * early), so that code which waits cannot outlast it either. 0: no limit.
 */
void hk_dvm_set_step_limit(u64 instructions);
/* Has every instruction counted (hk_dvm_stats.instructions) without limiting any: for measuring. It slows the interpreter by the counting. */
void hk_dvm_count_instructions(bool on);
/*
 * A profile, for finding where a slow step spends itself. While it is on,
 * every method counts the instructions executed in it, those executed under
 * it (from each call of it to its return), its calls, and the objects made
 * while it was the innermost method; a native method counts the time spent
 * in it. Turning it on clears the counts. The report gives the totals and
 * the first `top` methods by each of the four, a line at a time. It costs
 * what counting instructions costs, and the counts of two threads in one
 * method at the same moment can lose a few.
 */
void hk_dvm_profile(bool on);
void hk_dvm_profile_report(int top, void (*line)(const char *text, void *user), void *user);

typedef struct hk_dvm_stats {
    u32 dex_files, classes_defined;    /* what the DEX files hold */
    u32 classes_linked, classes_initialised;
    u64 objects, object_bytes;         /* allocated; nothing is freed yet */
    u64 instructions;                  /* executed, counted only while a step limit or tracing is on */
    u64 meta_bytes;                    /* what the runtime keeps of DEX files and linked classes (not the files themselves) */
    u32 strings_interned;
} hk_dvm_stats;
void hk_dvm_get_stats(hk_dvm_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* HK_DVM_H */
