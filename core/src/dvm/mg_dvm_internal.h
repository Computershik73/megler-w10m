/* The Dalvik runtime's own structures, shared between its files. */
#ifndef MG_DVM_INTERNAL_H
#define MG_DVM_INTERNAL_H

#include "../host/mg_host.h"
#include "mg_dvm.h"

typedef struct mg_dvm_dex mg_dvm_dex;
typedef struct mg_dvm_class mg_dvm_class;
typedef struct mg_dvm_method mg_dvm_method;
typedef struct mg_dvm_field mg_dvm_field;
typedef struct mg_dvm_thread mg_dvm_thread;
typedef struct mg_dvm_frame mg_dvm_frame;

enum {
    DVM_ACC_PUBLIC = 0x1, DVM_ACC_PRIVATE = 0x2, DVM_ACC_PROTECTED = 0x4, DVM_ACC_STATIC = 0x8, DVM_ACC_FINAL = 0x10,
    DVM_ACC_SYNCHRONIZED = 0x20, DVM_ACC_NATIVE = 0x100, DVM_ACC_INTERFACE = 0x200, DVM_ACC_ABSTRACT = 0x400,
    DVM_ACC_CONSTRUCTOR = 0x10000,
};

/*
 * A register is one machine word.
 *
 * Dalvik's registers are 32 bits wide, and a long or a double takes two of
 * them: that is how the bytecode numbers them (v4 and v5 hold one long, the
 * next free register is v6). A reference here is a pointer, not ART's 32-bit
 * compressed one, so a register has to be as wide as a pointer: 32 bits on the
 * phone, 64 on a PC. What a register holds:
 *
 *   - an int, a float's bits, a boolean, byte, char or short: the low 32 bits,
 *     the rest zero. (Zero, so that if-eq and if-eqz compare whole registers
 *     whether the bytecode means ints or references.)
 *   - a reference: the pointer, NULL for null.
 *   - a long or a double: its low 32 bits in the first register of the pair
 *     and its high 32 bits in the second, on both hosts. On the phone that is
 *     Dalvik's own layout. On a 64-bit host each half sits in a word twice its
 *     size, which wastes room and buys this: the PC's tests take the same
 *     path through every wide instruction as the phone does.
 *
 * Between a call from C and the interpreter, and to native methods, values
 * travel as mg_jvalue, one per parameter.
 */
typedef uintptr_t mg_dvm_reg;

static inline i64 mg_dvm_get_wide(const mg_dvm_reg *r, u32 n) { return (i64)((u64)(u32)r[n] | (u64)(u32)r[n + 1] << 32); }
static inline void mg_dvm_put_wide(mg_dvm_reg *r, u32 n, i64 v)
{
    r[n] = (u32)(u64)v;
    r[n + 1] = (u32)((u64)v >> 32);
}

/*
 * An object: what the JNI layer and native code know as an mg_jobj, then what
 * the interpreter adds, then the contents in the same allocation -- instance
 * fields as mg_jvalue slots (the superclass's first), or an array's elements,
 * or a string's UTF-16. The mg_jobj's pointers (u.arr.data, u.oarr.v,
 * u.str.u16) point at those contents, so everything that reads an mg_jobj
 * finds what it always found. `j.permanent` is set: the JNI layer's reference
 * counts must not free what the interpreter may still reach.
 *
 * A Class object is an object of java.lang.Class whose `j.native` is the
 * mg_dvm_class it stands for.
 */
typedef struct mg_dvm_obj {
    mg_jobj j;
    mg_dvm_class *cls;                 /* never NULL; java.lang.Class for a Class object */
    void *volatile monitor;            /* made at the first monitor-enter */
    u32 size;                          /* of the whole allocation: what a collector will free */
} mg_dvm_obj;

#define MG_DVM_OBJ(o) ((mg_dvm_obj *)(o))
#define MG_DVM_HEADER MG_ALIGN_UP(sizeof(mg_dvm_obj), (size_t)8)
static inline mg_jvalue *mg_dvm_slots(mg_jobj *o) { return (mg_jvalue *)(void *)((u8 *)o + MG_DVM_HEADER); }
static inline mg_dvm_class *mg_dvm_class_of(const mg_jobj *o) { return ((const mg_dvm_obj *)o)->cls; }

struct mg_dvm_dex {
    int scope; /* 0: existing app/boot path; >0: isolated package loader. */
    mg_jobj *loader; /* Defining loader for a DEX added through DexFile.openDexFileNative. */
    const u8 *b;
    size_t size;
    u32 nstr, ntype, nproto, nfield, nmeth, ncls;
    u32 str_off, type_off, proto_off, field_off, meth_off, cls_off;
    /* What each index resolved to, filled as code first uses it. A slot is written once with a value that never changes. */
    mg_dvm_class *volatile *tcache;
    mg_dvm_method *volatile *mcache;
    mg_dvm_field *volatile *fcache;
    mg_jobj *volatile *scache;         /* constant strings, interned */
    const char *volatile *pcache;      /* each prototype as a signature, interned */
    char name[96];
    bool boot;                         /* part of the boot class path */
    bool app;                          /* added after mg_dvm_app_follows: the app's own, whose classes say they come from its class loader */
    bool process_scope_known;          /* mg_dvm_dex_process_scope has its answer in process_scope */
    u8 process_scope;
    /*
     * A DEX file registered by its classes' names alone (mg_dvm_add_dex_lazy): `b` and the caches are NULL until the first
     * of its classes is linked (mg_dvm_dex_ready), when the file is viewed from `map_path` at `map_at` and its checksum
     * checked. `names` holds its classes' descriptors, which the table of definitions points at.
     */
    bool lazy, lazy_failed;
    char *map_path;
    u64 map_at;
    size_t map_size;
    u32 checksum;
    char *names;
    size_t names_size;
    u32 cdef_first, cdef_end;          /* the entries of the table of definitions it added, [first, end) */
    /*
     * A package the file keeps to itself ("android/icu/number/", core-libart's), or NULL: its classes in it are registered
     * under megler/libart/ names, and what the file names in that package resolves to them (mg_dvm_own_desc).
     */
    const char *own;
};

/*
 * Fields libcore declares and the runtime owns: they are not in an object's
 * slots but in what the runtime keeps of every object anyway. Object's
 * shadow$_klass_ is the object's class and shadow$_monitor_ a word that holds
 * its identity hash (ART keeps the lock there; the monitors here are
 * elsewhere); String's count is its length with the flag that says its
 * characters are 16 bits wide. A field numbered so has no slot; reading it
 * gives what it stands for, writing it is nothing.
 */
#define DVM_SLOT_KLASS 0xFFFFFFF1u
#define DVM_SLOT_MONITOR 0xFFFFFFF2u
#define DVM_SLOT_COUNT 0xFFFFFFF3u
/* What Object.hashCode() finds in shadow$_monitor_: "this word holds a hash", and 28 bits of it. */
#define DVM_HASH_WORD(o) ((i32)(0x80000000u | (mg_dvm_identity_hash(o) & 0x0FFFFFFFu)))
/* String.count: twice the length, plus one because the characters are not packed into bytes; zero for the empty string, as libcore checks it. */
#define DVM_STRING_COUNT(len) ((len) ? (i32)(((u32)(len) << 1) | 1u) : 0)

struct mg_dvm_field {
    mg_dvm_class *cls;
    const char *name;                  /* interned: equal names are the same pointer */
    const char *type;                  /* descriptor, interned */
    u32 flags;
    u32 slot;                          /* instance: index into the object's slots; static: into the class's statics */
    u32 idx;                           /* field_idx in its dex */
};

struct mg_dvm_method {
    mg_dvm_class *cls;
    const char *name;                  /* interned */
    const char *sig;                   /* "(ILjava/lang/String;)V", interned */
    const char *shorty;                /* return kind first, then each parameter: V Z B S C I J F D L */
    u32 flags;
    u32 idx;                           /* method_idx in its dex */
    u16 regs, ins, outs, ntries;
    u32 ninsns;
    const u16 *insns;                  /* NULL: abstract or native */
    const u8 *code;                    /* the code item */
    i32 vidx;                          /* vtable slot, or -1 */
    u16 nparams;
    mg_jhle_fn native;                 /* a native method's implementation in C, once bound */
    void *jni;                         /* or its function in a library of the app's: registered by it, or found by its JNI name */
    bool bound;                        /* which of the two it is to be has been settled */
    i8 critical;                       /* a library's function that takes its arguments alone (@CriticalNative): 1 yes, -1 no, 0 not looked at */
    /* The profile (mg_dvm_profile): counted only while it is on, and not exactly when two threads run the same method. */
    u64 prof_self;                     /* instructions executed in the method itself */
    u64 prof_incl;                     /* and in everything called from it, summed over its calls (a method that calls itself counts twice) */
    u64 prof_ns;                       /* a native method: the time spent in it, what it called back included */
    u64 prof_bytes;                    /* bytes of objects made while it was the innermost method */
    u32 prof_calls, prof_objects;
    /*
     * The meter's time of a native method (mg_dvm_thread_costs): its calls from bytecode, and what one of them takes on
     * average, in the host's ticks. A method whose calls are short is not timed every time (see invoke_native).
     */
    u32 meter_calls, meter_avg;
    /* One of the library's methods the interpreter does itself when it can (mg_dvm_interp.c, "intrinsics"): its number, or 0. */
    u8 intrinsic;
    /*
     * What the whole body is when it is one of the trivial shapes a call does in place (mg_dvm_interp.c, "trivial
     * methods"): 0 not looked at yet. One word, written once with all of it -- the shape, the instruction that does the
     * work, a constant -- so that another thread sees it whole; the field is written before it.
     */
    volatile u32 shape;
    mg_dvm_field *volatile shape_field;
};

enum { DVM_CS_FAILED = -1, DVM_CS_LINKED = 0, DVM_CS_INITIALIZING = 1, DVM_CS_INITIALIZED = 2 };

#define DVM_ICACHE 16

struct mg_dvm_class {
    int scope; /* Defining loader namespace; arrays inherit their component namespace. */
    const char *name;                  /* "java/lang/String", "[I", "int"; interned */
    u8 call_traced;                    /* under a flag trace-calls-<prefix> (mg_dvm_interp.c): 0 not yet looked at, 1 traced, 2 not */
    mg_dvm_dex *dex;                   /* NULL for array and primitive classes */
    u32 def;                           /* class_def index */
    u32 flags;
    mg_dvm_class *super;
    mg_dvm_class **ifaces;             /* as declared */
    int nifaces;
    mg_dvm_field *sfields, *ifields;
    int nsfields, nifields;
    mg_jvalue *statics;
    u32 nslots;                        /* instance slots, the superclasses' included */
    mg_dvm_method *direct, *virt;      /* direct: static, private, constructors */
    int ndirect, nvirt;
    mg_dvm_method **vtab;              /* the superclass's, overridden and extended */
    int nvtab;
    /*
     * Where interface calls on objects of this class last went, by a hash of
     * the name and signature. One word an entry, so it is read and replaced
     * without a lock; whoever reads one checks its name and signature.
     */
    mg_dvm_method *volatile icache[DVM_ICACHE];
    volatile i32 state;
    u32 init_thread;
    mg_jobj *init_error;               /* DVM_CS_FAILED: what its initialisation threw the first time, the cause of every later NoClassDefFoundError */
    char elem;                         /* arrays: the element's kind, 'L' for objects and arrays */
    char prim;                         /* primitive classes: their letter */
    mg_dvm_class *component;           /* arrays: the element's class */
    mg_jobj *volatile mirror;          /* its java.lang.Class object, made when first asked for */
    mg_jclass *jc;                     /* the JNI layer's class of the same name */
    bool foreign;                      /* a class of the JNI layer's that no DEX file defines: a name under Object, with no members here */
    mg_dvm_class *next;                /* in its bucket of the table of linked classes */
    /* The instance slots that hold references, the superclasses' included: what the collector follows. Made at the first instance. */
    u32 *ref_slots;
    u32 nref_slots;
    volatile i32 ref_slots_ready;
    u8 gc_ref;                         /* a java.lang.ref class: 1 soft, 2 weak, 3 phantom (their referent is not followed); 0 none */
    bool finalizable;                  /* it overrides finalize() with a body: its objects are finalized before they are freed */
};

/*
 * A method being executed. Frames and their registers are carved from the
 * thread's own stack (mg_dvm_thread.stack), not from C's: a call from
 * bytecode to bytecode pushes one and stays in the same C function, so Java
 * recursion costs words of that stack and nothing of the host's.
 */
struct mg_dvm_frame {
    mg_dvm_frame *prev;
    mg_dvm_method *m;
    mg_dvm_reg *regs;                  /* m->regs of them; NULL for a native method */
    u32 pc;                            /* where it called out or threw: what a stack trace shows */
    u64 prof_start;                    /* the thread's count of instructions when the frame was made */
    bool entry;                        /* called from C: returning from it leaves the interpreter */
    mg_jobj *sync;                     /* the monitor a synchronized method holds */
};

struct mg_dvm_thread {
    mg_dvm_reg *stack, *stack_end;     /* frames and registers; grows up */
    size_t stack_committed;            /* words of it backed by memory: reserved whole, committed as it grows (stack_grow) */
    size_t stack_words;                /* words reserved: DVM_STACK_WORDS, or DVM_STACK_ATTACHED_WORDS for a thread native code attached */
    mg_dvm_reg *sp;
    mg_dvm_frame *top;
    mg_jobj *pending;                  /* the exception being thrown */
    mg_jobj *traced;                   /* the last exception the flag trace-exceptions said (find_handler) */
    u32 id;
    int depth;                         /* frames */
    int c_depth;                       /* calls from C into the interpreter, nested */
    mg_addr c_stack_low;               /* the host's stack must not be used below here; 0 when the host could not say */
    mg_addr c_stack_min;               /* the lowest the host's stack pointer has been (mg_dvm_invoke, invoke_native): its deepest use */
    bool overflowing;                  /* StackOverflowError is being made: its own calls may use the reserve */
    u64 steps_left;                    /* under a step limit: instructions this call from C may still execute; all ones once it is used up */
    mg_jvalue scratch;                 /* where a field with no slot of its own is read from and written to */
    mg_jobj *peer;                     /* its java.lang.Thread, with libcore: made when first asked for */
    bool interrupted;
    /*
     * While a library's JNI_OnLoad runs on this thread: the frame that was on
     * top when it was called, and whether the library is the app's. A
     * FindClass made there is made for the class loader the library is being
     * loaded for, as ART has it, and not for whoever called System.loadLibrary.
     */
    mg_dvm_frame *onload_frame;
    bool onload_running, onload_app;
    int onload_scope;
    int owner_scope;                   /* the class scope of the service whose code started this thread (Thread.start), else 0 */
    mg_dvm_thread *next;
    u64 prof_total;                    /* instructions this thread has executed while the profile was on */
    /*
     * Always counted (mg_dvm_thread_costs): instructions the interpreter has executed on this thread, as of its last call
     * of a native method or return to C, and the time it has spent waiting in Object.wait (and so in Thread.sleep).
     */
    volatile u64 executed;
    volatile u64 waited_ns;
    /*
     * And where the rest of its time goes (mg_dvm_thread_costs): native methods (the outermost of nested ones, what they
     * call back included), allocation made by bytecode, and waiting for a monitor another thread holds, from bytecode.
     * Times in the host's ticks (mg_time_ticks), which are read on every native call and allocation and are cheap to read;
     * mg_dvm_thread_costs gives them as nanoseconds.
     */
    int native_depth;
    volatile u64 native_ticks, natives, alloc_ticks, allocs, blocked_ticks;
    /* Opt-in monitor diagnosis. Published only while entering a contended
     * monitor; sampled with this thread suspended and the VM lock held. */
    mg_jobj *volatile waiting_monitor;
    volatile u64 monitor_wait_ns;
    /*
     * What it has made (mg_dvm_get_stats sums every thread's), and the bytes not yet counted toward the next collection:
     * counted by the thread alone, and handed to the collector a few kilobytes at a time (alloc_raw), so that an
     * allocation needs no atomic operation of its own.
     */
    u32 made_objects, gc_unsaid;
    u64 made_bytes;
    /* For the collector: the thread as the host knows it, to stop it; its C stack's limits, [stack_lo, stack_hi). */
    mg_thread *host;
    mg_addr stack_lo, stack_hi;
};

/*
 * How much of a thread's stack only the making of a StackOverflowError may use, in registers; and, while the stack is not
 * committed whole, how far below its committed end frames stop, so that nothing ever reaches past what is committed.
 */
#define DVM_STACK_RESERVE 2048
#define DVM_STACK_WORDS (192u * 1024u)
/*
 * What is committed of it when a thread starts (64 KB); it doubles as frames need more. It was committed whole (768 KB) for
 * every thread: Facebook's 170 threads held 130 MB of it, nearly all never touched, and Windows counts it all against the
 * app's limit.
 */
#define DVM_STACK_FIRST_WORDS (16u * 1024u)
/*
 * What is reserved for a thread the app's native code made and attached (AttachCurrentThread): 128 KB. Such a thread calls
 * into Java to be told something, or to tell, and comes back; the threads Java starts get the whole. Yandex Maps had 199
 * threads when its 2 GB of addresses ran out, 150 MB of them frame stacks reserved whole (package 489).
 */
#define DVM_STACK_ATTACHED_WORDS (32u * 1024u)
/*
 * And for a thread Java starts (Thread.start), unless it asked for a stack larger than 1 MB: 384 KB, some four thousand
 * frames, as many as ART's interpreter fits in an Android thread's 1 MB. The main thread keeps the whole. Yandex Maps had
 * 229 such threads, with Google Play services' in the same process, when its addresses ran out (package 493).
 */
#define DVM_STACK_JAVA_WORDS (96u * 1024u)
/* Calls from C into the interpreter that may be nested (a native method calling back, a static initialiser): each is real C recursion. */
#define DVM_MAX_C_DEPTH 200

/*
 * The JNI layer's half of the join (jni/mg_jni.c, mg_jni_dvm_join). `find` and
 * `call` are mg_jni_native and mg_jni_call_native: a native method nothing was
 * registered for here is looked for with one and called with the other, with
 * the object (or, for a static method, the class) as its second argument.
 * `class_for` gives the JNI layer's class for one linked here, and tells it
 * which class of the runtime's that is.
 */
typedef struct mg_dvm_jni {
    /* `app_class`: the method is of a class of the app's, and the app's libraries are looked in first; otherwise the runtime's are. */
    void *(*find)(const char *cls, const char *name, const char *sig, bool app_class);
    /* The names a library could have exported the method under, as ART lists them when none did: "Java_a_B_c and Java_a_B_c__I". */
    void (*tried)(const char *cls, const char *name, const char *sig, char *out, size_t n);
    mg_jvalue (*call)(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args);
    /* The same under a guard: *faulted when the native code faulted beyond repair, and the call was ended instead of the app. */
    mg_jvalue (*call_guarded)(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args, bool *faulted);
    /* The same for a method that is given its arguments alone, no JNIEnv and no class (@CriticalNative). */
    mg_jvalue (*call_bare)(void *fn, const char *sig, const mg_jvalue *args);
    mg_jclass *(*class_for)(const char *name, const char *super, mg_dvm_class *c);
} mg_dvm_jni;
void mg_jni_dvm_join(mg_dvm_jni *out);
/* jni/mg_jni.c, for the collector: its lock of the references' table, and the JNI layer's objects that are roots. */
void mg_jni_gc_lock(bool take);
void mg_jni_gc_roots(void (*fn)(mg_jobj *o, void *user), void *user);

/* The few classes the runtime itself names. */
typedef struct mg_dvm_globals {
    mg_dvm_class *c_object, *c_class, *c_string, *c_throwable, *c_cloneable, *c_serializable;
    /*
     * With libcore as the boot class path (mg_dvm_art.c): set, and the two things the rest of the runtime does differently
     * then. A Class object has fields bytecode reads, which are filled when it is made; and `new String(...)` is a call of
     * the StringFactory method with the same parameters.
     */
    bool libcore;
    void (*fill_mirror)(mg_jobj *mirror, mg_dvm_class *c);
    bool conscrypt;                    /* conscrypt's native library is in the Android root: its providers can be made */
    void (*before_init)(mg_dvm_class *c);      /* on libcore: called before a class's static initialiser runs */
    bool app_follows;                  /* DEX files added from now on are the app's */
    mg_jobj *app_loader;               /* the class loader the app's classes name as theirs; NULL until the framework has made it */
    mg_dvm_class *c_string_factory;
    char *android_root;                /* the folder laid out like Android's /system that libcore's files are in */
    mg_dvm_field *f_message, *f_cause, *f_backtrace;       /* java.lang.Throwable's */
    mg_dvm_jni jni;
    bool running;
    volatile i32 trace;
    /* Not zero while every instruction must look up from its work: tracing at level 3, or a step limit. */
    volatile i32 slow;
    volatile i32 profiling;
    u64 step_limit;
    u64 steps;
    volatile i32 n_linked, n_initialised, n_interned;
    volatile i32 meta_units;           /* what the tables and classes take, in units of 16 bytes */
    volatile i32 meta_kind_units[8];   /* the same by kind (DVM_META_*) */
    u64 objects, object_bytes;
} mg_dvm_globals;
extern mg_dvm_globals g_dvm;

/* One lock for the tables everything shares: classes, interned names and strings, registered natives. The same thread may take it again. */
void mg_dvm_lock(void);
void mg_dvm_unlock(void);

/* ----------------------------------------------------------- mg_dvm_dex.c */

static inline u16 mg_dvm_rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 mg_dvm_rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u32 mg_dvm_uleb(const u8 **pp);
i32 mg_dvm_sleb(const u8 **pp);

/* Reads the header and checks every table lies inside the file. NULL (with a line in the log) when it does not. */
mg_dvm_dex *mg_dvm_dex_open(const u8 *b, size_t size, const char *label);
/* A string's bytes (modified UTF-8, NUL-terminated) and its length in UTF-16 units; "" for an index out of range. */
const char *mg_dvm_dex_string(const mg_dvm_dex *d, u32 idx, u32 *len16);
const char *mg_dvm_dex_type(const mg_dvm_dex *d, u32 idx);      /* a descriptor */
/* A prototype as a signature, "(ILjava/lang/String;)V", interned; and its shorty, the file's own string ("VIL"). */
const char *mg_dvm_dex_proto_sig(mg_dvm_dex *d, u32 proto);
const char *mg_dvm_dex_proto_shorty(const mg_dvm_dex *d, u32 proto);
/* "Ljava/lang/String;" -> "java/lang/String"; arrays and primitives as they are. False when it does not fit. */
bool mg_dvm_descriptor_name(const char *desc, char *out, size_t n);
/* The source line of a pc in a method, from its debug info; -1 without one. And the class's source file, or NULL. */
int mg_dvm_dex_line(const mg_dvm_method *m, u32 pc);
const char *mg_dvm_dex_source(const mg_dvm_class *c);
/* The length in code units of the instruction or payload at `pc`; 0 when the opcode is none, or it runs past the method. */
u32 mg_dvm_insn_units(const u16 *insns, u32 ninsns, u32 pc);
/* Whether a method carries an annotation of that type ("Ldalvik/annotation/optimization/CriticalNative;"), of any visibility. */
bool mg_dvm_method_annotated(const mg_dvm_method *m, const char *descriptor);
const char *mg_dvm_insn_name(u16 unit);

/* --------------------------------------------------------- mg_dvm_class.c */

/* The one copy of a name: equal strings give the same pointer, for good. */
const char *mg_dvm_intern(const char *s);

void mg_dvm_classes_add_dex(mg_dvm_dex *d);
/* A class by its JNI name, loaded and linked if need be. NULL with NoClassDefFoundError (or what linking threw) pending. */
mg_dvm_class *mg_dvm_find_class(const char *name);
mg_dvm_class *mg_dvm_find_class_scope(const char *name, int scope);
mg_dvm_class *mg_dvm_find_desc_scope(const char *desc, int scope);
bool mg_dvm_class_exists_scope(const char *name, int scope);
bool mg_dvm_add_dex_scope(const void *data, size_t size, const char *label, int scope);
int mg_dvm_add_apk_scope(const char *path, int scope);
int mg_dvm_loader_scope(mg_jobj *loader);
/* The service scope a dex's classes are of (mg_dvm_art.c): its own, else that of the first loader in its loader's parents that
   is a service's; 0 for the app's and the platform's. */
int mg_dvm_dex_process_scope(mg_dvm_dex *d);
/* A child scope's parent scope (0 the app's, -1 the boot path's alone) and whether its own files come first (mg_dvm_art.c). */
bool mg_dvm_scope_child(int scope, int *parent, bool *own_first);
/* The service scope a scope is under: itself for a service's, a child's parent's root, 0 for the app's. */
int mg_dvm_scope_root(int scope);
mg_jobj *mg_dvm_scope_loader(int scope);
void mg_dvm_loader_roots(void (*fn)(mg_jobj *, void *), void *user);
mg_dvm_class *mg_dvm_find_class_desc(const char *descriptor);
/* Whether any DEX file defines it (or it is an array or primitive class), without loading it. */
bool mg_dvm_class_exists(const char *name);
/* Runs the static initialiser once, the superclass's first. False when it threw, or had thrown before. */
bool mg_dvm_ensure_init(mg_dvm_class *c);

/* What an index in a DEX file names, as the instruction using it needs. NULL with an exception pending. */
mg_dvm_class *mg_dvm_resolve_type(mg_dvm_dex *d, u32 idx);
/* A descriptor as the file `d` means it: one in the package it keeps to itself, as that class is registered (in `buf`). */
const char *mg_dvm_own_desc(const mg_dvm_dex *d, const char *desc, char *buf, size_t size);
enum { DVM_INVOKE_VIRTUAL, DVM_INVOKE_SUPER, DVM_INVOKE_DIRECT, DVM_INVOKE_STATIC, DVM_INVOKE_INTERFACE };
mg_dvm_method *mg_dvm_resolve_method(mg_dvm_dex *d, u32 idx, int kind);
mg_dvm_field *mg_dvm_resolve_field(mg_dvm_dex *d, u32 idx, bool is_static);

/* By name; `name` and `sig` need not be interned. No exception when there is none. */
mg_dvm_method *mg_dvm_find_method(mg_dvm_class *c, const char *name, const char *sig, bool is_static);
/* The method a virtual or interface call of that name runs on an object of class `receiver`. `name` and `sig` interned. */
mg_dvm_method *mg_dvm_find_virtual(mg_dvm_class *receiver, const char *name, const char *sig);
mg_dvm_field *mg_dvm_find_field(mg_dvm_class *c, const char *name, bool is_static);
mg_dvm_field *mg_dvm_find_field_typed(mg_dvm_class *c, const char *name, const char *type, bool is_static);
bool mg_dvm_assignable(mg_dvm_class *sub, mg_dvm_class *sup);
/* The method a call of `m` on `self` runs when it is not a call of exactly `m`: what invoke-virtual and invoke-interface find. */
mg_dvm_method *mg_dvm_virtual_target(mg_dvm_method *m, mg_jobj *self);
/*
 * A class of the JNI layer's that no DEX file defines, for as long as the
 * runtime runs: so that its objects are objects here (they can be locked,
 * stored in arrays, asked for their class) and its name means something to
 * bytecode. It has no methods or fields here; the JNI layer keeps those.
 */
mg_dvm_class *mg_dvm_foreign_class(const char *name, mg_jclass *jc);
/*
 * A class no DEX file defines, made for java.lang.reflect.Proxy: under `super`
 * (Proxy itself), implementing `ifaces`, with a constructor that is the C
 * function `construct` and one final method for each of `like` -- its name,
 * signature and parameters -- all of them the C function `invoke`, which
 * tells which it is being by the idx of the method in its frame (its place in
 * `like`). The class there already under that name is returned as it is.
 */
mg_dvm_class *mg_dvm_make_proxy_class(const char *name, mg_dvm_class *super, mg_dvm_class **ifaces, int nifaces, mg_dvm_method *const *like, int nmethods,
                                      mg_jhle_fn invoke, mg_jhle_fn construct, const char *construct_sig);
/* RegisterNatives: gives a native method the class itself declares its function. False when the class declares no such method. */
bool mg_dvm_native_registered(mg_dvm_class *c, const char *name, const char *sig, void *fn);
static inline bool mg_dvm_is_instance(mg_jobj *o, mg_dvm_class *c) { return o && (mg_dvm_class_of(o) == c || mg_dvm_assignable(mg_dvm_class_of(o), c)); }

/* Binds a native method to what was registered for it or to the app's library; false with UnsatisfiedLinkError pending. */
bool mg_dvm_bind_native(mg_dvm_method *m);

/* For the tests and for reports: every class any DEX file defines, in the order they were added. */
u32 mg_dvm_defined_count(void);
u32 mg_dvm_dex_count(void);
/* Every class that is linked now, for what has to be done to all of them. Called with the runtime's lock not held; `fn` must not link anything. */
void mg_dvm_each_linked(void (*fn)(mg_dvm_class *c, void *user), void *user);
/* The same for the collector: with the runtime's lock held and the other threads stopped, so nothing is allocated. */
void mg_dvm_each_linked_locked(void (*fn)(mg_dvm_class *c, void *user), void *user);
mg_dvm_dex *mg_dvm_dex_at(u32 i);
const char *mg_dvm_defined_name(u32 i, mg_dvm_dex **dex);

/* -------------------------------------------------------- mg_dvm_object.c */

/*
 * mg_dvm_dex.c: a DEX file whose bytes are at `b` for now (a view the caller unmaps once its classes are registered), opened
 * for its tables and classes' names alone: no checksum, no caches. mg_dvm_dex_ready views it again from `map_path` at
 * `map_at` when the first of its classes is linked, checks it, and makes its caches. With the runtime's lock held.
 */
mg_dvm_dex *mg_dvm_dex_open_lazy(const u8 *b, size_t size, const char *label, const char *map_path, u64 map_at);
bool mg_dvm_dex_ready(mg_dvm_dex *d);
/* Whether a DEX file of these bytes of that file is the runtime's already (registered, viewed or not). */
bool mg_dvm_dex_known(const char *map_path, u64 map_at, size_t size);
/* mg_dvm.c: such a file added to the class path, of the app or of a scope (`scope` > 0). */
bool mg_dvm_add_dex_lazy(const void *data, size_t size, const char *label, int scope, const char *map_path, u64 map_at);
/* mg_dvm_apk.c: bytes of DEX files viewed now, for the memory line. */
void mg_dvm_dex_viewed(u64 bytes);

mg_dvm_thread *mg_dvm_self(void);
/* The same for a thread of the app's own that attaches: made, if it is not yet, with the smaller frame stack. */
mg_dvm_thread *mg_dvm_self_attached(void);
/* The same for a thread Java starts, with a frame stack of `words` (DVM_STACK_JAVA_WORDS, or more for a stack asked for). */
mg_dvm_thread *mg_dvm_self_sized(size_t words);
/* The runtime's threads now: how many, how many of them attached by native code, and the frame stacks' reserved bytes. */
/* The thread's Java frames for a fault's report (mg_core.c's fault detail): made nothing, locked nothing. */
void mg_dvm_fault_frames(char *out, size_t cap);
void mg_dvm_monitor_trace_start(void);
void mg_dvm_stacks_trace_start(void);
void mg_dvm_call_trace_add(const char *prefix);
/* For the collector, with the runtime's lock held: every thread there is, and every interned string. */
void mg_dvm_threads_each(void (*fn)(mg_dvm_thread *t, void *user), void *user);
void mg_dvm_interned_each(void (*fn)(mg_jobj *s, void *user), void *user);
/* The objects the C side of the runtime keeps: the preallocated OutOfMemoryError, the app's class loader. */
void mg_dvm_object_roots(void (*fn)(mg_jobj *o, void *user), void *user);
/* mg_dvm_art_reflect.c: the Method[] kept beside each proxy class. */
void mg_dvm_proxy_roots(void (*fn)(mg_jobj *o, void *user), void *user);
/* Once the boot class path is in place: what has to exist before it is needed (the OutOfMemoryError to throw when nothing more can be made). */
void mg_dvm_objects_start(void);

/* A new object with every field zero; no constructor is run. NULL with an exception pending (an interface, an abstract class). */
mg_jobj *mg_dvm_alloc_object(mg_dvm_class *c);
mg_jobj *mg_dvm_alloc_array(mg_dvm_class *array_class, i32 len);
mg_jobj *mg_dvm_alloc_string(const u16 *chars, i32 len);        /* chars NULL: `len` zeros, to be filled */
/* The one string with these contents (String.intern). */
mg_jobj *mg_dvm_intern_string(mg_jobj *s);
mg_jobj *mg_dvm_const_string(mg_dvm_dex *d, u32 idx);
mg_jobj *mg_dvm_mirror(mg_dvm_class *c);
static inline mg_dvm_class *mg_dvm_mirror_class(mg_jobj *mirror) { return (mg_dvm_class *)mirror->native; }
static inline i32 mg_dvm_array_length(const mg_jobj *a) { return (i32)(a->kind == MG_J_OBJ_ARRAY ? a->u.oarr.len : a->u.arr.len); }
u32 mg_dvm_identity_hash(const mg_jobj *o);

/* System.arraycopy, with every check Java makes. False when it threw. */
bool mg_dvm_arraycopy(mg_jobj *src, i32 sp, mg_jobj *dst, i32 dp, i32 n);
void mg_dvm_monitor_enter(mg_jobj *o);
/* False when the calling thread does not hold it. */
bool mg_dvm_monitor_exit(mg_jobj *o);
bool mg_dvm_monitor_wait(mg_jobj *o, i64 ms);
bool mg_dvm_monitor_notify(mg_jobj *o, bool all);

/* Leaves an existing object pending. */
void mg_dvm_throw_object(mg_jobj *e);
/* An exception of the class itself, with its message (NULL: none), constructed as bytecode constructs one. */
bool mg_dvm_throw_class(mg_dvm_class *c, const char *msg);
/* The frames of the calling thread as (method, pc) pairs in a long[], for Throwable.fillInStackTrace. */
mg_jobj *mg_dvm_capture_stack(void);
/* A fallback: a native method registered here is used only when neither mg_dvm_register nor a library has one. */
void mg_dvm_register_fallback(const mg_jhle *table);

/* -------------------------------------------------------- mg_dvm_interp.c */

/* Runs a method, bytecode or native, from C. `params` holds one value per parameter. False when it threw. */
bool mg_dvm_invoke(mg_dvm_method *m, mg_jobj *self, const mg_jvalue *params, mg_jvalue *ret);
/* On libcore, once its classes are there: marks the methods the interpreter does itself when it can (intrinsics). */
void mg_dvm_intrinsics_start(void);

/* ------------------------------------------------------------ mg_dvm_gc.c */

/* The class's reference slots (mg_dvm_class.ref_slots), made when it is first needed: before the first instance is made. */
void mg_dvm_gc_class_ready(mg_dvm_class *c);
/* What a thread's end hands the collector: the thread is no longer to be stopped or scanned. */
void mg_dvm_gc_thread_gone(mg_dvm_thread *t);
/* A new object of a class that is finalizable or a reference: the collector keeps account of it (alloc_raw). */
void mg_dvm_gc_born(mg_jobj *o);

/* ---------------------------------------------------------- mg_dvm_heap.c */

/* Where objects live (mg_dvm_heap.c says how). Memory zeroed, or NULL when the host has none. */
void *mg_dvm_heap_alloc(size_t size);
/* The start of the object an address points into, or NULL: what the collector's conservative scan asks. With the heap's lock held. */
void *mg_dvm_heap_find(mg_addr p);
void mg_dvm_heap_lock(void);
void mg_dvm_heap_unlock(void);
typedef struct mg_dvm_heap_info {
    u64 small_bytes, large_bytes, reserved;   /* in cells in use; in large objects; the host's memory the blocks take (committed) */
    u32 large_objects, objects;
} mg_dvm_heap_info;
void mg_dvm_heap_stats(mg_dvm_heap_info *out);
/* The large objects' bytes by class, the most first, when they take more than 32 MB (mg_dvm_heap.c); with the heap's lock held. */
bool mg_dvm_heap_large_report(char *out, size_t cap, int top);
/* The bytes of the apps' DEX files (mg_dvm_apk.c): read into the process's memory, and views of the APKs that store them. */
void mg_dvm_dex_memory(u64 *read, u64 *viewed);
/* The collector's, with the heap's lock held: objects in it; an object's mark set (true when it was not set before), read, all cleared. */
u32 mg_dvm_heap_objects(void);
bool mg_dvm_heap_set_mark(void *o);
bool mg_dvm_heap_is_marked(const void *o);
void mg_dvm_heap_clear_marks(void);
void mg_dvm_heap_each_marked(void (*fn)(void *o));
void mg_dvm_heap_sweep(void (*dying)(void *o), bool poison, u64 *freed_objects, u64 *freed_bytes);
/* mg_dvm_object.c: what an object the collector frees owns besides itself (its monitor, a string's UTF-8, JNI fields), freed. */
void mg_dvm_object_dying(void *o);
/* An allocation of `bytes` is counted toward the next collection: true when one is due now (the caller then runs mg_dvm_gc). */
bool mg_dvm_gc_counted(size_t bytes);

/* ---------------------------------------------------------- mg_dvm_boot.c */

/* The built-in java.lang: its DEX file and its natives. */
bool mg_dvm_boot_add(void);
/* What `new String(...)` means there: the string a constructor of that signature makes from its arguments. False when it threw. */
bool mg_dvm_boot_string_init(const char *sig, const mg_jvalue *args, mg_jobj **out);

/* ----------------------------------------------------------- mg_dvm_art.c */

/* With libcore's own classes on the boot class path: what Android's runtime implements of them inside itself. */
bool mg_dvm_art_start(void);
/* mg_dvm_art_reflect.c: java.lang.reflect and what of java.lang.Class is about members. */
void mg_dvm_art_reflect_start(void);
/* mg_dvm_art_host.c: the few natives of libcore's own libraries that are done here, for where those libraries cannot run. */
void mg_dvm_art_host_start(void);

/* mg_dvm_art_reflect.c: a value in its box (a reference as it is); the Method or Constructor object of a method. */
mg_jobj *mg_dvm_reflect_box(char kind, mg_jvalue v);
mg_jobj *mg_dvm_reflect_method(mg_dvm_method *m);
mg_jobj *mg_dvm_reflect_field(mg_dvm_field *f);
/* The other way: the runtime's method or field of a Method, Constructor or Field object; NULL for anything else. */
mg_dvm_method *mg_dvm_reflected_method(mg_jobj *o);
mg_dvm_field *mg_dvm_reflected_field(mg_jobj *o);
/*
 * mg_dvm_art.c: a thread that native code made and attaches (JNI's
 * AttachCurrentThread) gets its java.lang.Thread now, with the name and group
 * it asked for, a daemon or not; one that detaches is ended as Java ends a
 * thread -- its group forgets it and whoever waits in join() is told. Both do
 * nothing where libcore is not what runs.
 */
bool mg_dvm_thread_attach(const char *name, mg_jobj *group, bool daemon);
void mg_dvm_thread_detach(void);
/* Whether a class is one of the app's own: from a DEX file added after mg_dvm_app_follows. */
bool mg_dvm_class_is_app(const mg_dvm_class *c);
/* mg_dvm_art_annot.c: annotations as reflection reads them. */
void mg_dvm_art_annot_start(void);
/* mg_dvm_jvm.c: libjavacore.so and libopenjdk.so from <root>/lib, where guest code can run; and libopenjdkjvm.so's functions for them. */
bool mg_dvm_load_libcore_natives(const char *root);

/* What a piece of the runtime's own memory is for, in mg_dvm_stats. */
enum { DVM_META_OTHER, DVM_META_CLASS, DVM_META_METHOD, DVM_META_FIELD, DVM_META_VTABLE, DVM_META_DEX, DVM_META_NAME, DVM_META_POOL };
/* mg_dvm_dex.c: memory kept for good, of a kind (DVM_META_*), counted (mg_dvm_stats.meta_bytes). What is freed again is not this. */
void *mg_dvm_meta_alloc_kind(size_t size, int kind);
/* The same given back (a DEX file's pool of class names once the file is viewed): not for the small names, which are kept. */
void mg_dvm_meta_free_kind(void *p, size_t size, int kind);
/* mg_dvm_class.c: a lazily registered DEX file viewed now: its classes' entries point at the file's own names again, and its pool goes. */
void mg_dvm_classes_viewed(mg_dvm_dex *d);
/* mg_dvm_dex.c: memory for what is kept of DEX files and classes, counted (mg_dvm_stats.meta_bytes). */
void *mg_dvm_meta_alloc(size_t size);

/* ---------------------------------------------------------------- mg_dvm.c */

/* What `new String(...)` means, whichever java.lang there is. False when it threw. */
bool mg_dvm_string_init(const char *sig, const mg_jvalue *args, mg_jobj **out);

#endif /* MG_DVM_INTERNAL_H */
