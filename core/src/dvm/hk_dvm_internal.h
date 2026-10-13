/* The Dalvik runtime's own structures, shared between its files. */
#ifndef HK_DVM_INTERNAL_H
#define HK_DVM_INTERNAL_H

#include "../host/hk_host.h"
#include "hk_dvm.h"

typedef struct hk_dvm_dex hk_dvm_dex;
typedef struct hk_dvm_class hk_dvm_class;
typedef struct hk_dvm_method hk_dvm_method;
typedef struct hk_dvm_field hk_dvm_field;
typedef struct hk_dvm_thread hk_dvm_thread;
typedef struct hk_dvm_frame hk_dvm_frame;

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
 * travel as hk_jvalue, one per parameter.
 */
typedef uintptr_t hk_dvm_reg;

static inline i64 hk_dvm_get_wide(const hk_dvm_reg *r, u32 n) { return (i64)((u64)(u32)r[n] | (u64)(u32)r[n + 1] << 32); }
static inline void hk_dvm_put_wide(hk_dvm_reg *r, u32 n, i64 v)
{
    r[n] = (u32)(u64)v;
    r[n + 1] = (u32)((u64)v >> 32);
}

/*
 * An object: what the JNI layer and native code know as an hk_jobj, then what
 * the interpreter adds, then the contents in the same allocation -- instance
 * fields as hk_jvalue slots (the superclass's first), or an array's elements,
 * or a string's UTF-16. The hk_jobj's pointers (u.arr.data, u.oarr.v,
 * u.str.u16) point at those contents, so everything that reads an hk_jobj
 * finds what it always found. `j.permanent` is set: the JNI layer's reference
 * counts must not free what the interpreter may still reach.
 *
 * A Class object is an object of java.lang.Class whose `j.native` is the
 * hk_dvm_class it stands for.
 */
typedef struct hk_dvm_obj {
    hk_jobj j;
    hk_dvm_class *cls;                 /* never NULL; java.lang.Class for a Class object */
    void *volatile monitor;            /* made at the first monitor-enter */
    u32 size;                          /* of the whole allocation: what a collector will free */
} hk_dvm_obj;

#define HK_DVM_OBJ(o) ((hk_dvm_obj *)(o))
#define HK_DVM_HEADER HK_ALIGN_UP(sizeof(hk_dvm_obj), (size_t)8)
static inline hk_jvalue *hk_dvm_slots(hk_jobj *o) { return (hk_jvalue *)(void *)((u8 *)o + HK_DVM_HEADER); }
static inline hk_dvm_class *hk_dvm_class_of(const hk_jobj *o) { return ((const hk_dvm_obj *)o)->cls; }

struct hk_dvm_dex {
    const u8 *b;
    size_t size;
    u32 nstr, ntype, nproto, nfield, nmeth, ncls;
    u32 str_off, type_off, proto_off, field_off, meth_off, cls_off;
    /* What each index resolved to, filled as code first uses it. A slot is written once with a value that never changes. */
    hk_dvm_class *volatile *tcache;
    hk_dvm_method *volatile *mcache;
    hk_dvm_field *volatile *fcache;
    hk_jobj *volatile *scache;         /* constant strings, interned */
    const char *volatile *pcache;      /* each prototype as a signature, interned */
    char name[96];
    bool boot;                         /* part of the boot class path */
    bool app;                          /* added after hk_dvm_app_follows: the app's own, whose classes say they come from its class loader */
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
#define DVM_HASH_WORD(o) ((i32)(0x80000000u | (hk_dvm_identity_hash(o) & 0x0FFFFFFFu)))
/* String.count: twice the length, plus one because the characters are not packed into bytes; zero for the empty string, as libcore checks it. */
#define DVM_STRING_COUNT(len) ((len) ? (i32)(((u32)(len) << 1) | 1u) : 0)

struct hk_dvm_field {
    hk_dvm_class *cls;
    const char *name;                  /* interned: equal names are the same pointer */
    const char *type;                  /* descriptor, interned */
    u32 flags;
    u32 slot;                          /* instance: index into the object's slots; static: into the class's statics */
    u32 idx;                           /* field_idx in its dex */
};

struct hk_dvm_method {
    hk_dvm_class *cls;
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
    hk_jhle_fn native;                 /* a native method's implementation in C, once bound */
    void *jni;                         /* or its function in a library of the app's: registered by it, or found by its JNI name */
    bool bound;                        /* which of the two it is to be has been settled */
    i8 critical;                       /* a library's function that takes its arguments alone (@CriticalNative): 1 yes, -1 no, 0 not looked at */
    /* The profile (hk_dvm_profile): counted only while it is on, and not exactly when two threads run the same method. */
    u64 prof_self;                     /* instructions executed in the method itself */
    u64 prof_incl;                     /* and in everything called from it, summed over its calls (a method that calls itself counts twice) */
    u64 prof_ns;                       /* a native method: the time spent in it, what it called back included */
    u64 prof_bytes;                    /* bytes of objects made while it was the innermost method */
    u32 prof_calls, prof_objects;
};

enum { DVM_CS_FAILED = -1, DVM_CS_LINKED = 0, DVM_CS_INITIALIZING = 1, DVM_CS_INITIALIZED = 2 };

#define DVM_ICACHE 16

struct hk_dvm_class {
    const char *name;                  /* "java/lang/String", "[I", "int"; interned */
    hk_dvm_dex *dex;                   /* NULL for array and primitive classes */
    u32 def;                           /* class_def index */
    u32 flags;
    hk_dvm_class *super;
    hk_dvm_class **ifaces;             /* as declared */
    int nifaces;
    hk_dvm_field *sfields, *ifields;
    int nsfields, nifields;
    hk_jvalue *statics;
    u32 nslots;                        /* instance slots, the superclasses' included */
    hk_dvm_method *direct, *virt;      /* direct: static, private, constructors */
    int ndirect, nvirt;
    hk_dvm_method **vtab;              /* the superclass's, overridden and extended */
    int nvtab;
    /*
     * Where interface calls on objects of this class last went, by a hash of
     * the name and signature. One word an entry, so it is read and replaced
     * without a lock; whoever reads one checks its name and signature.
     */
    hk_dvm_method *volatile icache[DVM_ICACHE];
    volatile i32 state;
    u32 init_thread;
    char elem;                         /* arrays: the element's kind, 'L' for objects and arrays */
    char prim;                         /* primitive classes: their letter */
    hk_dvm_class *component;           /* arrays: the element's class */
    hk_jobj *volatile mirror;          /* its java.lang.Class object, made when first asked for */
    hk_jclass *jc;                     /* the JNI layer's class of the same name */
    bool foreign;                      /* a class of the JNI layer's that no DEX file defines: a name under Object, with no members here */
    hk_dvm_class *next;                /* in its bucket of the table of linked classes */
};

/*
 * A method being executed. Frames and their registers are carved from the
 * thread's own stack (hk_dvm_thread.stack), not from C's: a call from
 * bytecode to bytecode pushes one and stays in the same C function, so Java
 * recursion costs words of that stack and nothing of the host's.
 */
struct hk_dvm_frame {
    hk_dvm_frame *prev;
    hk_dvm_method *m;
    hk_dvm_reg *regs;                  /* m->regs of them; NULL for a native method */
    u32 pc;                            /* where it called out or threw: what a stack trace shows */
    u64 prof_start;                    /* the thread's count of instructions when the frame was made */
    bool entry;                        /* called from C: returning from it leaves the interpreter */
    hk_jobj *sync;                     /* the monitor a synchronized method holds */
};

struct hk_dvm_thread {
    hk_dvm_reg *stack, *stack_end;     /* frames and registers; grows up */
    hk_dvm_reg *sp;
    hk_dvm_frame *top;
    hk_jobj *pending;                  /* the exception being thrown */
    u32 id;
    int depth;                         /* frames */
    int c_depth;                       /* calls from C into the interpreter, nested */
    hk_addr c_stack_low;               /* the host's stack must not be used below here; 0 when the host could not say */
    bool overflowing;                  /* StackOverflowError is being made: its own calls may use the reserve */
    u64 steps_left;                    /* under a step limit: instructions this call from C may still execute; all ones once it is used up */
    hk_jvalue scratch;                 /* where a field with no slot of its own is read from and written to */
    hk_jobj *peer;                     /* its java.lang.Thread, with libcore: made when first asked for */
    bool interrupted;
    /*
     * While a library's JNI_OnLoad runs on this thread: the frame that was on
     * top when it was called, and whether the library is the app's. A
     * FindClass made there is made for the class loader the library is being
     * loaded for, as ART has it, and not for whoever called System.loadLibrary.
     */
    hk_dvm_frame *onload_frame;
    bool onload_running, onload_app;
    hk_dvm_thread *next;
    u64 prof_total;                    /* instructions this thread has executed while the profile was on */
};

/* How much of a thread's stack only the making of a StackOverflowError may use, in registers. */
#define DVM_STACK_RESERVE 2048
#define DVM_STACK_WORDS (192u * 1024u)
/* Calls from C into the interpreter that may be nested (a native method calling back, a static initialiser): each is real C recursion. */
#define DVM_MAX_C_DEPTH 200

/*
 * The JNI layer's half of the join (jni/hk_jni.c, hk_jni_dvm_join). `find` and
 * `call` are hk_jni_native and hk_jni_call_native: a native method nothing was
 * registered for here is looked for with one and called with the other, with
 * the object (or, for a static method, the class) as its second argument.
 * `class_for` gives the JNI layer's class for one linked here, and tells it
 * which class of the runtime's that is.
 */
typedef struct hk_dvm_jni {
    /* `app_class`: the method is of a class of the app's, and the app's libraries are looked in first; otherwise the runtime's are. */
    void *(*find)(const char *cls, const char *name, const char *sig, bool app_class);
    /* The names a library could have exported the method under, as ART lists them when none did: "Java_a_B_c and Java_a_B_c__I". */
    void (*tried)(const char *cls, const char *name, const char *sig, char *out, size_t n);
    hk_jvalue (*call)(void *fn, hk_jobj *self_or_class, const char *sig, const hk_jvalue *args);
    /* The same for a method that is given its arguments alone, no JNIEnv and no class (@CriticalNative). */
    hk_jvalue (*call_bare)(void *fn, const char *sig, const hk_jvalue *args);
    hk_jclass *(*class_for)(const char *name, const char *super, hk_dvm_class *c);
} hk_dvm_jni;
void hk_jni_dvm_join(hk_dvm_jni *out);

/* The few classes the runtime itself names. */
typedef struct hk_dvm_globals {
    hk_dvm_class *c_object, *c_class, *c_string, *c_throwable, *c_cloneable, *c_serializable;
    /*
     * With libcore as the boot class path (hk_dvm_art.c): set, and the two things the rest of the runtime does differently
     * then. A Class object has fields bytecode reads, which are filled when it is made; and `new String(...)` is a call of
     * the StringFactory method with the same parameters.
     */
    bool libcore;
    void (*fill_mirror)(hk_jobj *mirror, hk_dvm_class *c);
    bool conscrypt;                    /* conscrypt's native library is in the Android root: its providers can be made */
    void (*before_init)(hk_dvm_class *c);      /* on libcore: called before a class's static initialiser runs */
    bool app_follows;                  /* DEX files added from now on are the app's */
    hk_jobj *app_loader;               /* the class loader the app's classes name as theirs; NULL until the framework has made it */
    hk_dvm_class *c_string_factory;
    char *android_root;                /* the folder laid out like Android's /system that libcore's files are in */
    hk_dvm_field *f_message, *f_cause, *f_backtrace;       /* java.lang.Throwable's */
    hk_dvm_jni jni;
    bool running;
    volatile i32 trace;
    /* Not zero while every instruction must look up from its work: tracing at level 3, or a step limit. */
    volatile i32 slow;
    volatile i32 profiling;
    u64 step_limit;
    u64 steps;
    volatile i32 n_linked, n_initialised, n_interned;
    volatile i32 meta_units;           /* what the tables and classes take, in units of 16 bytes */
    u64 objects, object_bytes;
} hk_dvm_globals;
extern hk_dvm_globals g_dvm;

/* One lock for the tables everything shares: classes, interned names and strings, registered natives. The same thread may take it again. */
void hk_dvm_lock(void);
void hk_dvm_unlock(void);

/* ----------------------------------------------------------- hk_dvm_dex.c */

static inline u16 hk_dvm_rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static inline u32 hk_dvm_rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
u32 hk_dvm_uleb(const u8 **pp);
i32 hk_dvm_sleb(const u8 **pp);

/* Reads the header and checks every table lies inside the file. NULL (with a line in the log) when it does not. */
hk_dvm_dex *hk_dvm_dex_open(const u8 *b, size_t size, const char *label);
/* A string's bytes (modified UTF-8, NUL-terminated) and its length in UTF-16 units; "" for an index out of range. */
const char *hk_dvm_dex_string(const hk_dvm_dex *d, u32 idx, u32 *len16);
const char *hk_dvm_dex_type(const hk_dvm_dex *d, u32 idx);      /* a descriptor */
/* A prototype as a signature, "(ILjava/lang/String;)V", interned; and its shorty, the file's own string ("VIL"). */
const char *hk_dvm_dex_proto_sig(hk_dvm_dex *d, u32 proto);
const char *hk_dvm_dex_proto_shorty(const hk_dvm_dex *d, u32 proto);
/* "Ljava/lang/String;" -> "java/lang/String"; arrays and primitives as they are. False when it does not fit. */
bool hk_dvm_descriptor_name(const char *desc, char *out, size_t n);
/* The source line of a pc in a method, from its debug info; -1 without one. And the class's source file, or NULL. */
int hk_dvm_dex_line(const hk_dvm_method *m, u32 pc);
const char *hk_dvm_dex_source(const hk_dvm_class *c);
/* The length in code units of the instruction or payload at `pc`; 0 when the opcode is none, or it runs past the method. */
u32 hk_dvm_insn_units(const u16 *insns, u32 ninsns, u32 pc);
/* Whether a method carries an annotation of that type ("Ldalvik/annotation/optimization/CriticalNative;"), of any visibility. */
bool hk_dvm_method_annotated(const hk_dvm_method *m, const char *descriptor);
const char *hk_dvm_insn_name(u16 unit);

/* --------------------------------------------------------- hk_dvm_class.c */

/* The one copy of a name: equal strings give the same pointer, for good. */
const char *hk_dvm_intern(const char *s);

void hk_dvm_classes_add_dex(hk_dvm_dex *d);
/* A class by its JNI name, loaded and linked if need be. NULL with NoClassDefFoundError (or what linking threw) pending. */
hk_dvm_class *hk_dvm_find_class(const char *name);
hk_dvm_class *hk_dvm_find_class_desc(const char *descriptor);
/* Whether any DEX file defines it (or it is an array or primitive class), without loading it. */
bool hk_dvm_class_exists(const char *name);
/* Runs the static initialiser once, the superclass's first. False when it threw, or had thrown before. */
bool hk_dvm_ensure_init(hk_dvm_class *c);

/* What an index in a DEX file names, as the instruction using it needs. NULL with an exception pending. */
hk_dvm_class *hk_dvm_resolve_type(hk_dvm_dex *d, u32 idx);
enum { DVM_INVOKE_VIRTUAL, DVM_INVOKE_SUPER, DVM_INVOKE_DIRECT, DVM_INVOKE_STATIC, DVM_INVOKE_INTERFACE };
hk_dvm_method *hk_dvm_resolve_method(hk_dvm_dex *d, u32 idx, int kind);
hk_dvm_field *hk_dvm_resolve_field(hk_dvm_dex *d, u32 idx, bool is_static);

/* By name; `name` and `sig` need not be interned. No exception when there is none. */
hk_dvm_method *hk_dvm_find_method(hk_dvm_class *c, const char *name, const char *sig, bool is_static);
/* The method a virtual or interface call of that name runs on an object of class `receiver`. `name` and `sig` interned. */
hk_dvm_method *hk_dvm_find_virtual(hk_dvm_class *receiver, const char *name, const char *sig);
hk_dvm_field *hk_dvm_find_field(hk_dvm_class *c, const char *name, bool is_static);
hk_dvm_field *hk_dvm_find_field_typed(hk_dvm_class *c, const char *name, const char *type, bool is_static);
bool hk_dvm_assignable(hk_dvm_class *sub, hk_dvm_class *sup);
/* The method a call of `m` on `self` runs when it is not a call of exactly `m`: what invoke-virtual and invoke-interface find. */
hk_dvm_method *hk_dvm_virtual_target(hk_dvm_method *m, hk_jobj *self);
/*
 * A class of the JNI layer's that no DEX file defines, for as long as the
 * runtime runs: so that its objects are objects here (they can be locked,
 * stored in arrays, asked for their class) and its name means something to
 * bytecode. It has no methods or fields here; the JNI layer keeps those.
 */
hk_dvm_class *hk_dvm_foreign_class(const char *name, hk_jclass *jc);
/*
 * A class no DEX file defines, made for java.lang.reflect.Proxy: under `super`
 * (Proxy itself), implementing `ifaces`, with a constructor that is the C
 * function `construct` and one final method for each of `like` -- its name,
 * signature and parameters -- all of them the C function `invoke`, which
 * tells which it is being by the idx of the method in its frame (its place in
 * `like`). The class there already under that name is returned as it is.
 */
hk_dvm_class *hk_dvm_make_proxy_class(const char *name, hk_dvm_class *super, hk_dvm_class **ifaces, int nifaces, hk_dvm_method *const *like, int nmethods,
                                      hk_jhle_fn invoke, hk_jhle_fn construct, const char *construct_sig);
/* RegisterNatives: gives a native method the class itself declares its function. False when the class declares no such method. */
bool hk_dvm_native_registered(hk_dvm_class *c, const char *name, const char *sig, void *fn);
static inline bool hk_dvm_is_instance(hk_jobj *o, hk_dvm_class *c) { return o && (hk_dvm_class_of(o) == c || hk_dvm_assignable(hk_dvm_class_of(o), c)); }

/* Binds a native method to what was registered for it or to the app's library; false with UnsatisfiedLinkError pending. */
bool hk_dvm_bind_native(hk_dvm_method *m);

/* For the tests and for reports: every class any DEX file defines, in the order they were added. */
u32 hk_dvm_defined_count(void);
u32 hk_dvm_dex_count(void);
/* Every class that is linked now, for what has to be done to all of them. Called with the runtime's lock not held; `fn` must not link anything. */
void hk_dvm_each_linked(void (*fn)(hk_dvm_class *c, void *user), void *user);
const char *hk_dvm_defined_name(u32 i, hk_dvm_dex **dex);

/* -------------------------------------------------------- hk_dvm_object.c */

hk_dvm_thread *hk_dvm_self(void);
/* Once the boot class path is in place: what has to exist before it is needed (the OutOfMemoryError to throw when nothing more can be made). */
void hk_dvm_objects_start(void);

/* A new object with every field zero; no constructor is run. NULL with an exception pending (an interface, an abstract class). */
hk_jobj *hk_dvm_alloc_object(hk_dvm_class *c);
hk_jobj *hk_dvm_alloc_array(hk_dvm_class *array_class, i32 len);
hk_jobj *hk_dvm_alloc_string(const u16 *chars, i32 len);        /* chars NULL: `len` zeros, to be filled */
/* The one string with these contents (String.intern). */
hk_jobj *hk_dvm_intern_string(hk_jobj *s);
hk_jobj *hk_dvm_const_string(hk_dvm_dex *d, u32 idx);
hk_jobj *hk_dvm_mirror(hk_dvm_class *c);
static inline hk_dvm_class *hk_dvm_mirror_class(hk_jobj *mirror) { return (hk_dvm_class *)mirror->native; }
static inline i32 hk_dvm_array_length(const hk_jobj *a) { return (i32)(a->kind == HK_J_OBJ_ARRAY ? a->u.oarr.len : a->u.arr.len); }
u32 hk_dvm_identity_hash(const hk_jobj *o);

/* System.arraycopy, with every check Java makes. False when it threw. */
bool hk_dvm_arraycopy(hk_jobj *src, i32 sp, hk_jobj *dst, i32 dp, i32 n);
void hk_dvm_monitor_enter(hk_jobj *o);
/* False when the calling thread does not hold it. */
bool hk_dvm_monitor_exit(hk_jobj *o);
bool hk_dvm_monitor_wait(hk_jobj *o, i64 ms);
bool hk_dvm_monitor_notify(hk_jobj *o, bool all);

/* Leaves an existing object pending. */
void hk_dvm_throw_object(hk_jobj *e);
/* The frames of the calling thread as (method, pc) pairs in a long[], for Throwable.fillInStackTrace. */
hk_jobj *hk_dvm_capture_stack(void);
/* A fallback: a native method registered here is used only when neither hk_dvm_register nor a library has one. */
void hk_dvm_register_fallback(const hk_jhle *table);

/* -------------------------------------------------------- hk_dvm_interp.c */

/* Runs a method, bytecode or native, from C. `params` holds one value per parameter. False when it threw. */
bool hk_dvm_invoke(hk_dvm_method *m, hk_jobj *self, const hk_jvalue *params, hk_jvalue *ret);

/* ---------------------------------------------------------- hk_dvm_boot.c */

/* The built-in java.lang: its DEX file and its natives. */
bool hk_dvm_boot_add(void);
/* What `new String(...)` means there: the string a constructor of that signature makes from its arguments. False when it threw. */
bool hk_dvm_boot_string_init(const char *sig, const hk_jvalue *args, hk_jobj **out);

/* ----------------------------------------------------------- hk_dvm_art.c */

/* With libcore's own classes on the boot class path: what Android's runtime implements of them inside itself. */
bool hk_dvm_art_start(void);
/* hk_dvm_art_reflect.c: java.lang.reflect and what of java.lang.Class is about members. */
void hk_dvm_art_reflect_start(void);
/* hk_dvm_art_host.c: the few natives of libcore's own libraries that are done here, for where those libraries cannot run. */
void hk_dvm_art_host_start(void);

/* hk_dvm_art_reflect.c: a value in its box (a reference as it is); the Method or Constructor object of a method. */
hk_jobj *hk_dvm_reflect_box(char kind, hk_jvalue v);
hk_jobj *hk_dvm_reflect_method(hk_dvm_method *m);
hk_jobj *hk_dvm_reflect_field(hk_dvm_field *f);
/* The other way: the runtime's method or field of a Method, Constructor or Field object; NULL for anything else. */
hk_dvm_method *hk_dvm_reflected_method(hk_jobj *o);
hk_dvm_field *hk_dvm_reflected_field(hk_jobj *o);
/*
 * hk_dvm_art.c: a thread that native code made and attaches (JNI's
 * AttachCurrentThread) gets its java.lang.Thread now, with the name and group
 * it asked for, a daemon or not; one that detaches is ended as Java ends a
 * thread -- its group forgets it and whoever waits in join() is told. Both do
 * nothing where libcore is not what runs.
 */
bool hk_dvm_thread_attach(const char *name, hk_jobj *group, bool daemon);
void hk_dvm_thread_detach(void);
/* Whether a class is one of the app's own: from a DEX file added after hk_dvm_app_follows. */
bool hk_dvm_class_is_app(const hk_dvm_class *c);
/* hk_dvm_art_annot.c: annotations as reflection reads them. */
void hk_dvm_art_annot_start(void);
/* hk_dvm_jvm.c: libjavacore.so and libopenjdk.so from <root>/lib, where guest code can run; and libopenjdkjvm.so's functions for them. */
bool hk_dvm_load_libcore_natives(const char *root);

/* hk_dvm_dex.c: memory for what is kept of DEX files and classes, counted (hk_dvm_stats.meta_bytes). */
void *hk_dvm_meta_alloc(size_t size);

/* ---------------------------------------------------------------- hk_dvm.c */

/* What `new String(...)` means, whichever java.lang there is. False when it threw. */
bool hk_dvm_string_init(const char *sig, const hk_jvalue *args, hk_jobj **out);

#endif /* HK_DVM_INTERNAL_H */
