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
 * Its objects are the JNI layer's: every object here begins with an mg_jobj
 * (jni/mg_jni.h), so a pointer to one is a jobject as native code knows it,
 * and a native method written for the interpreter has the shape of a
 * driver's (mg_jhle, mg_jcall). Once the runtime is started the two are one
 * Java world: jni/mg_jni.c sends what native code asks about a class that a
 * DEX file defines here, makes its strings and arrays here, and a native
 * method of such a class that nothing registered below is looked for in the
 * app's libraries. A class no DEX file defines stays what it was, implemented
 * in C or not at all. Until it is started the runtime costs nothing.
 *
 * What makes up java.lang without libcore is in mg_dvm_boot.c: enough of
 * Object, Class, String, StringBuilder, Throwable, the exceptions the
 * interpreter raises, Enum and the boxes for bytecode to mean something. The
 * plan is libcore's own bytecode in its place (docs/JAVA.md).
 *
 * Nothing is collected yet: an object lives as long as the process.
 */
#ifndef MG_DVM_H
#define MG_DVM_H

#include "../mg_base.h"
#include "../jni/mg_jni.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ setup */

/*
 * Starts the runtime. `boot` lists .dex files that make up the boot class
 * path, searched in order before anything added later; with none, the built-in
 * java.lang of mg_dvm_boot.c is the boot class path. False when
 * java/lang/Object cannot be found in it. Call once. It starts the JNI layer
 * (mg_jni_init) if nothing has.
 */
bool mg_dvm_start(const char *const *boot, int nboot);
/*
 * Starts it on Android's own libcore: `root` is a folder laid out like
 * Android's /system, with the boot class path as DEX files in
 * `framework` (core-oj.dex, core-libart.dex and what else of conscrypt,
 * okhttp, bouncycastle and apache-xml is there), libcore's native libraries
 * in `lib`, ICU's data in `usr/icu` and the time zones in `usr/share/zoneinfo`.
 * java.lang is then libcore's, with what Android's runtime does for it done
 * in mg_dvm_art.c. The libraries are loaded where guest code can run
 * (mg_dvm_load_libcore_natives); without them libcore works as far as its
 * bytecode goes. False when the files are not there.
 */
bool mg_dvm_start_android(const char *root);
/*
 * The framework's methods that are signatures alone (tools/list-stubs.py: "<class> <name> <descriptor>" a line, sorted),
 * read from `path` before the framework's classes are linked: each is marked as it is linked, and its first call is said
 * in the log ("api: ... is a stub here"), so that what an app needs of Android 14 that Megler does not do shows. False
 * when the file is not there (nothing is marked).
 */
bool mg_dvm_stubs_load(const char *path);
/*
 * Class loaders. There is one class path and no loading through a loader:
 * every class is found by name wherever it was added. But an app's classes
 * must say they come from the app's loader (Class.getClassLoader: resources
 * and services are found through it), and the system's that they come from
 * none. mg_dvm_app_follows marks the line: DEX files added after it are the
 * app's. mg_dvm_set_app_loader gives their classes the loader object, those
 * linked already and those linked later.
 */
void mg_dvm_app_follows(void);
void mg_dvm_set_app_loader(mg_jobj *loader);
/*
 * Says that a file whose classes are on the class path already is known to
 * the app under this (guest) path: when one of the app's class loaders opens
 * it (dalvik.system.DexFile), nothing is read again. A file the app opens
 * that was not named here is read then, and its classes join the path.
 */
void mg_dvm_dex_source_added(const char *guest_path);

/* Whether libcore's own native libraries are loaded (mg_dvm_start_android loads them where they are and can run). */
bool mg_dvm_libcore_natives(void);
/*
 * Loads one of the app's libraries of native methods by its file name
 * ("libfoo.so"; a path's last part is what is looked for in the APK) through
 * the layer's linker and runs its JNI_OnLoad: what System.loadLibrary comes
 * to for a class of the app's. False, the reason in the log, when it could
 * not be loaded, or on a host that cannot run it.
 */
bool mg_dvm_load_library(const char *name_or_path);
bool mg_dvm_running(void);

/*
 * Adds a DEX file's classes after those already there: where a class is
 * defined twice, the first definition wins, as on Android's class path. The
 * bytes are used where they are, for good, and must be aligned to four bytes
 * (anything from malloc is). False when it is not a DEX file this runtime can
 * read; the log says why.
 */
bool mg_dvm_add_dex(const void *data, size_t size, const char *label);
/* The same for a .dex file read whole. */
bool mg_dvm_add_dex_file(const char *path);
/* Every classesN.dex of an APK or a jar (mg_dvm_apk.c; needs zip/mg_zip.c). Returns how many were added. */
int mg_dvm_add_apk(const char *path);

/*
 * Native methods implemented in C, by class, name and signature: a table
 * ended by an entry with cls == NULL, as mg_jni_register takes. sig NULL
 * matches any signature. Later tables win over earlier ones. A method is bound
 * when its class is linked, or at its first call if nothing was registered
 * for it by then.
 *
 * The function receives its parameters in declaration order, one mg_jvalue
 * each (a long or a double is one), and writes its result to the member of
 * `ret` for its return type. It reports failure by throwing (mg_dvm_throw)
 * and returning. An object it returns needs no reference counting.
 */
void mg_dvm_register(const mg_jhle *table);

/* ---------------------------------------------------------------- calling */

/*
 * Calls a static method by name (class as "java/lang/String", signature as
 * "(I)V"), initialising its class first. `args` holds one value per
 * parameter; `ret` may be NULL. False when it threw: the exception is then
 * pending on the calling thread (mg_dvm_describe_pending, mg_dvm_clear_pending).
 */
bool mg_dvm_call_static(const char *cls, const char *name, const char *sig, const mg_jvalue *args, mg_jvalue *ret);
/* Calls a method on an object as invoke-virtual would: the object's own class decides which. */
bool mg_dvm_call_virtual(mg_jobj *self, const char *name, const char *sig, const mg_jvalue *args, mg_jvalue *ret);
/* A new object, made with the constructor of that signature ("(I)V"); NULL when it threw. */
mg_jobj *mg_dvm_new(const char *cls, const char *ctor_sig, const mg_jvalue *args);

/* ---------------------------------------------------------------- objects */

/* The java.lang.Class object of a class, loading it if need be; NULL, with NoClassDefFoundError pending, when nothing defines it. */
mg_jobj *mg_dvm_class_object(const char *cls);
/* The class of an object as a JNI name ("java/lang/String", "[I"); "null" for NULL. */
const char *mg_dvm_class_name(const mg_jobj *o);
bool mg_dvm_instance_of(mg_jobj *o, const char *cls);

mg_jobj *mg_dvm_new_string(const char *utf8);
mg_jobj *mg_dvm_new_string_utf16(const u16 *chars, i32 len);
/* The string's text: UTF-16 as it is kept, or UTF-8 made on first asking and kept with the string. NULL for NULL or not a string. */
const u16 *mg_dvm_string_utf16(mg_jobj *s, i32 *len);
const char *mg_dvm_string_utf8(mg_jobj *s);
/* An array of `len` zeros or nulls; `array_class` is "[I", "[Ljava/lang/String;", "[[D". Its elements are at u.arr.data / u.oarr.v. */
mg_jobj *mg_dvm_new_array(const char *array_class, i32 len);

/* A static field's value; false (nothing pending) when the class has no such field. The class is initialised first. */
bool mg_dvm_get_static(const char *cls, const char *name, mg_jvalue *out);
bool mg_dvm_set_static(const char *cls, const char *name, mg_jvalue v);
/* An instance field's value, looked for by name in the object's class and above. */
bool mg_dvm_get_field(mg_jobj *o, const char *name, mg_jvalue *out);
bool mg_dvm_set_field(mg_jobj *o, const char *name, mg_jvalue v);

/* ------------------------------------------------------------- exceptions */

/* Makes an exception of that class with the message and leaves it pending on the calling thread. Always returns false. */
bool mg_dvm_throw(const char *cls, const char *fmt, ...) MG_PRINTF(2, 3);
mg_jobj *mg_dvm_pending(void);
void mg_dvm_clear_pending(void);
/*
 * The pending exception as text, for a log: "java.lang.Foo: message", then
 * " | caused by ..." for each cause. NULL when nothing is pending.
 */
const char *mg_dvm_describe_pending(char *buf, size_t n);
/*
 * Where the pending exception was made: one line per frame, innermost first,
 * "  at some.Class.method(File.java:12)" (or "(pc 7)" where the DEX has no
 * line numbers, "(native)" for a native method). Returns the number of frames.
 */
int mg_dvm_pending_stack(char *buf, size_t n);

/* ----------------------------------------------------------- the collector */

/* A C variable that keeps an object across calls (a driver's): the collector keeps what it points to, whatever it is then. */
void mg_dvm_add_root(mg_jobj **slot);
typedef struct mg_dvm_gc_result {
    u64 objects, live_objects, live_bytes;   /* in the heap; reachable, and their bytes */
    u64 conservative, conservative_bytes;    /* reached first through a conservative root: a stack or a frame's register */
    u64 freed_objects, freed_bytes;
    u32 cleared, enqueued, to_finalize;      /* references cleared, of them enqueued; objects found to finalize */
    u32 threads_stopped;
    u32 markers, sweepers;                   /* threads that marked at once, at the most; that swept */
    u64 stop_ns, mark_ns, sweep_ns;
    u64 roots_ns, stacks_ns;                 /* of the mark: the exact roots, and the conservative scan of the stacks */
    u64 classes_ns, strings_ns, jni_ns;      /* of the exact roots: the classes', the interned strings', the JNI layer's */
    u32 classes, strings, jni_roots;         /* how many of each were read */
} mg_dvm_gc_result;
/* A collection that marks only: what is reachable, counted, nothing freed. False when not every thread could be stopped. */
bool mg_dvm_gc_census(mg_dvm_gc_result *out);
/* Whether the last census found the object reachable (for the tests). */
bool mg_dvm_gc_marked(const mg_jobj *o);
/* A collection: what is not reachable is freed, and a line in the log says what it took and found. `out` may be NULL. */
bool mg_dvm_gc(const char *reason, mg_dvm_gc_result *out);
/* Collections as allocation calls for them: off until the runtime's start has turned them on (mg_dvm_start*). */
void mg_dvm_gc_enable(bool on);
/* For the tests: freed memory filled with 0xDB and never used again, so that an object freed too early shows; and a smaller budget. */
void mg_dvm_gc_set_poison(bool on);
void mg_dvm_gc_set_min_budget(u64 bytes);
/* Native memory an object keeps alive (VMRuntime.registerNativeAllocation, a bitmap's pixels): it counts toward the next collection. */
void mg_dvm_gc_native(i64 bytes);
/* Collections made so far and the time they took, for the frame meter. */
void mg_dvm_gc_totals(u64 *collections, u64 *pause_ns);
/* Bytes of the objects in the heap now (live since the last collection, and what was made after it). */
u64 mg_dvm_live_bytes(void);
/* Returns once the objects found to finalize so far have been finalized (Runtime.runFinalization), or after ten seconds. */
void mg_dvm_gc_run_finalization(void);

/* ------------------------------------------------------------ diagnostics */

/*
 * For a frame meter, counted always: the instructions executed by every thread there has been, those that have ended
 * included; and the calling thread's
 * instructions executed, time spent waiting (Object.wait, Thread.sleep), time in native methods (the outermost of nested
 * ones, what they called back included) and how many it called, time allocating objects for bytecode's own instructions
 * (allocations inside native methods are in those) and how many objects it made in all, and time waiting to enter a
 * monitor another thread held, from bytecode. Native calls and allocations are timed now and then and counted at their
 * average (mg_dvm_interp.c, invoke_native; mg_dvm_object.c, alloc_raw). The instruction
 * counts move at each call of a native method and each return to C, so a thread in a long stretch of bytecode is a little
 * behind.
 */
u64 mg_dvm_executed_all(void);
/*
 * The runtime's threads: how many, how many native code attached, the bytes their frame stacks reserve, the bytes their
 * host (C) stacks reserve -- committed whole for Android code (mg_thread_whole_stack), so this is memory, not only
 * address -- and the most any one thread's host stack has been seen to use.
 */
void mg_dvm_thread_census(u32 *threads, u32 *attached, u64 *frame_bytes, u64 *c_reserved, u64 *c_peak);
/* The runtime's frame stacks: the bytes their segments reserve, the bytes of it committed, and the most any thread's frames take now. */
void mg_dvm_frame_census(u64 *reserved, u64 *committed, u64 *deepest);
typedef struct mg_dvm_costs {
    u64 executed, waited_ns, native_ns, natives, alloc_ns, allocs, blocked_ns;
} mg_dvm_costs;
void mg_dvm_thread_costs(mg_dvm_costs *out);

/* 0: quiet. 1: classes linked and initialised, natives bound, exceptions thrown. 2: every call. 3: every instruction. */
void mg_dvm_set_trace(int level);
/*
 * A limit on the instructions one call from C may execute, for running code
 * that is not trusted to end: when it is used up, java.lang.InternalError is
 * thrown where execution stands, and nothing in the code can catch it. While
 * a limit is set, Object.wait() returns at once (Java lets any wait end
 * early), so that code which waits cannot outlast it either. 0: no limit.
 */
void mg_dvm_set_step_limit(u64 instructions);
/* Has every instruction counted (mg_dvm_stats.instructions) without limiting any: for measuring. It slows the interpreter by the counting. */
void mg_dvm_count_instructions(bool on);
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
void mg_dvm_profile(bool on);
void mg_dvm_profile_report(int top, void (*line)(const char *text, void *user), void *user);

typedef struct mg_dvm_stats {
    u32 dex_files, classes_defined;    /* what the DEX files hold */
    u32 classes_linked, classes_initialised;
    u64 objects, object_bytes;         /* allocated; nothing is freed yet */
    u64 instructions;                  /* executed, counted only while a step limit or tracing is on */
    u64 meta_bytes;                    /* what the runtime keeps of DEX files and linked classes (not the files themselves) */
    /* Of it: the classes themselves (and their interfaces' lists), their methods, their fields (and static values), their
       vtables, the DEX files' own tables (each index's cache), and names (interned, and the lazily viewed files' pools). */
    u64 meta_classes, meta_methods, meta_fields, meta_vtables, meta_dex, meta_names;
    u64 meta_pools;                    /* the class names of DEX files registered and not viewed yet, copied */
    u32 strings_interned;
} mg_dvm_stats;
void mg_dvm_get_stats(mg_dvm_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* MG_DVM_H */
