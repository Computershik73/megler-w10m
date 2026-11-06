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

/*
 * A method's counts for the profile (mg_dvm_profile): counted only while it is on, and not exactly when two threads run the
 * same method. Apart from the method, made the first time the profile counts it: they were 40 bytes of each of the 128 a
 * method took, for every method of every class linked (Yandex Maps and Google Play services link some 400,000).
 */
typedef struct mg_dvm_method_prof {
    u64 self;                          /* instructions executed in the method itself */
    u64 incl;                          /* and in everything called from it, summed over its calls (a method that calls itself counts twice) */
    u64 ns;                            /* a native method: the time spent in it, what it called back included */
    u64 bytes;                         /* bytes of objects made while it was the innermost method */
    u32 calls, objects;
} mg_dvm_method_prof;

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
    /* One of the library's methods the interpreter does itself when it can (mg_dvm_interp.c, "intrinsics"): its number, or 0. */
    u8 intrinsic;
    /* 1: a framework method that is its signature alone (megler-stubs.txt, mg_dvm_stubs_load) whose first call is still to be said. */
    volatile u8 stub;
    /* A native method's binding and the meter of its calls (mg_dvm_native_of); NULL for one never bound, and for bytecode. */
    struct mg_dvm_native_info *volatile nat;
    mg_dvm_method_prof *volatile prof; /* the profile's counts of it, NULL until it counts it (mg_dvm_prof) */
    /*
     * The JIT (mg_dvm_jit.c, docs/JIT.md): jumps back taken and calls of it made in the interpreter, until it is compiled;
     * whether it was tried (1 compiled, 2 not to be, 3 compiled and its page not executable yet); and its code, NULL until
     * it can run.
     */
    u16 jit_backs;
    volatile u8 jit_state;
    struct mg_dvm_jit_code *volatile jit;
    /*
     * What the whole body is when it is one of the trivial shapes a call does in place (mg_dvm_interp.c, "trivial
     * methods"): 0 not looked at yet. One word, written once with all of it -- the shape, the instruction that does the
     * work, a constant -- so that another thread sees it whole; the field is written before it.
     */
    volatile u32 shape;
    mg_dvm_field *volatile shape_field;
};

/*
 * What only a native method needs, apart from the rest of its method so that every other method (most of them, some 450,000
 * in Yandex Maps' process, at 96 bytes each before) is 20 bytes smaller: made when it is first bound or registered.
 */
typedef struct mg_dvm_native_info {
    mg_jhle_fn native;                 /* its implementation in C, once bound */
    void *jni;                         /* or its function in a library of the app's: registered by it, or found by its JNI name */
    bool bound;                        /* which of the two it is to be has been settled */
    i8 critical;                       /* a library's function that takes its arguments alone (@CriticalNative): 1 yes, -1 no, 0 not looked at */
    /*
     * The meter's time of it (mg_dvm_thread_costs): its calls from bytecode, and what one of them takes on average, in the
     * host's ticks. A method whose calls are short is not timed every time (see invoke_native).
     */
    u32 meter_calls, meter_avg;
} mg_dvm_native_info;
/* mg_dvm_class.c: the method's native part, made the first time it is wanted (published with an interlocked exchange). */
mg_dvm_native_info *mg_dvm_native_of(mg_dvm_method *m);

/*
 * The shapes of a whole body that a call does in place (mg_dvm_interp.c, "trivial methods"; mg_dvm_jit.c, inline_call), and
 * the parts of a method's shape word: the shape, the instruction that does the work, a constant.
 */
enum { SHAPE_UNKNOWN, SHAPE_REGULAR, SHAPE_EMPTY, SHAPE_CONST, SHAPE_GETTER, SHAPE_SETTER, SHAPE_STATIC_GETTER };
#define SHAPE_OF(w) ((w) & 0xFFu)
#define SHAPE_OP(w) (((w) >> 8) & 0xFFu)
#define SHAPE_VALUE(w) ((i32)(i16)((w) >> 16))

/* The library's methods the interpreter does itself (mg_dvm_interp.c, "intrinsics"), as m->intrinsic numbers them. */
enum {
    INTR_NONE, INTR_STRING_CHARAT, INTR_STRING_LENGTH, INTR_STRING_ISEMPTY, INTR_STRING_EQUALS, INTR_STRING_HASHCODE, INTR_ARRAYCOPY,
    INTR_MIN_I, INTR_MAX_I, INTR_ABS_I, INTR_MIN_J, INTR_MAX_J, INTR_ABS_J, INTR_SQRT, INTR_GETCLASS, INTR_CURRENT_THREAD,
    INTR_REFERENT, INTR_CLASS_ISINSTANCE, INTR_CLASS_ASSIGNABLE, INTR_OBJECT_HASHCODE, INTR_IDENTITY_HASHCODE,
    INTR_PEEK_BYTE, INTR_PEEK_SHORT, INTR_PEEK_INT, INTR_PEEK_LONG, INTR_PEEK_BYTES, INTR_POKE_BYTE, INTR_POKE_SHORT, INTR_POKE_INT,
    INTR_POKE_LONG, INTR_POKE_BYTES, INTR_RAW_BITS32, INTR_RAW_BITS64,
};
/* java.lang.ref.Reference's referent field's slot, for INTR_REFERENT (Reference.getReferent); ~0 when it has none. */
extern u32 g_dvm_referent_slot;

/* mg_dvm.c: a method's profile counts, made the first time they are wanted. */
mg_dvm_method_prof *mg_dvm_prof_make(mg_dvm_method *m);
static inline mg_dvm_method_prof *mg_dvm_prof(mg_dvm_method *m) { mg_dvm_method_prof *p = m->prof; return p ? p : mg_dvm_prof_make(m); }

enum { DVM_CS_FAILED = -1, DVM_CS_LINKED = 0, DVM_CS_INITIALIZING = 1, DVM_CS_INITIALIZED = 2 };

#define DVM_ICACHE 16

struct mg_dvm_class {
    int scope; /* Defining loader namespace; arrays inherit their component namespace. */
    const char *name;                  /* "java/lang/String", "[I", "int"; interned */
    u8 call_traced;                    /* the flags trace-calls-<prefix> and trace-calls-within-<prefix> (mg_dvm_interp.c): 0 not yet looked at; else 1, | 2 its calls said, | 4 those inside them */
    mg_dvm_dex *dex;                   /* NULL for array and primitive classes */
    u32 def;                           /* class_def index */
    u32 flags;
    mg_dvm_class *super;
    mg_dvm_class **ifaces;             /* as declared */
    int nifaces;
    mg_dvm_field *sfields, *ifields;
    int nsfields, nifields;
    /* Its static fields' values by slot: those of reference type first, nsrefs of them (what the collector reads). */
    mg_jvalue *statics;
    int nsrefs;
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

/*
 * A frame stack is in segments, each reserved when the frames first need it: a thread's first is DVM_STACK_FIRST_WORDS,
 * each next one twice the one before, until together they hold the thread's whole (stack_max). A frame that does not fit in
 * the rest of a segment goes at the start of the next (stack_next); when the first frame of a segment returns, the thread is
 * back in the one below (stack_back), and the one it left is kept for the next time the frames come that deep. Each was
 * reserved whole: Yandex Maps' 202 threads held 76 MB of addresses in frame stacks of 384 KB, where most never used 64 KB,
 * with 15 MB of its 2 GB left in one piece (package 506).
 */
#define DVM_STACK_SEGS 12
typedef struct mg_dvm_stack_seg {
    mg_dvm_reg *base;
    size_t words, committed;
    mg_dvm_reg *sp;                    /* a segment below the current one: where its frames end */
} mg_dvm_stack_seg;

struct mg_dvm_thread {
    /*
     * The method the thread runs now, as it writes it itself at each call, return and native call: always a method, or
     * NULL outside Java. What the profile of the main thread samples (mg_java.c, "hot").
     */
    mg_dvm_method *volatile running;
    /* The method that made the thread's latest native call, written before `running` is: whose wait a wait is (the same). */
    mg_dvm_method *volatile running_from;
    mg_dvm_reg *stack, *stack_end;     /* the current segment of frames and registers; grows up */
    size_t stack_committed;            /* words of it backed by memory: reserved whole, committed as it grows (stack_grow) */
    size_t stack_words;                /* words of the current segment reserved */
    size_t stack_max;                  /* words the segments hold together at most: DVM_STACK_WORDS, DVM_STACK_ATTACHED_WORDS... */
    size_t stack_below;                /* words of the segments below the current one */
    size_t stack_peak;                 /* the most words its segments have held together */
    int nsegs;                         /* segments below the current one, in segs */
    mg_dvm_stack_seg segs[DVM_STACK_SEGS];
    mg_dvm_stack_seg spare;            /* the segment above the current one, kept when its frames returned; base NULL for none */
    mg_dvm_reg *sp;
    mg_dvm_frame *top;
    mg_jobj *pending;                  /* the exception being thrown */
    mg_jobj *traced;                   /* the last exception the flag trace-exceptions said (find_handler) */
    u32 id;
    int depth;                         /* frames */
    int call_within[8];                /* the depths of the frames of methods under trace-calls-within-<prefix> that run, the nearest last (mg_dvm_interp.c) */
    u32 ncall_within;
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
    /* The monitor it waits to enter, published while it enters a contended one; read with this thread suspended and the VM
     * lock held (trace-monitors, and a report of the main thread standing in a wait). */
    mg_jobj *volatile waiting_monitor;
    volatile u64 monitor_wait_ns;
    /* The object whose wait() it is in (mg_dvm_monitor_wait), for a report of the main thread standing in one (mg_dvm_stuck_say). */
    mg_jobj *volatile waiting_on;
    /*
     * What it has made (mg_dvm_get_stats sums every thread's), and the bytes not yet counted toward the next collection:
     * counted by the thread alone, and handed to the collector a few kilobytes at a time (alloc_raw), so that an
     * allocation needs no atomic operation of its own.
     */
    u32 made_objects, gc_unsaid;
    u64 made_bytes;
    u64 made_at_gc;                    /* made_bytes at the last collection: what the thread made since, for its line */
    u32 made_sample;                   /* bytes made since its last sample of where objects are made (mg_dvm_made_say) */
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
 * A thread's first segment of it (64 KB), reserved and committed when the thread starts; a segment commits more as frames
 * need it, doubling. The stack was committed whole (768 KB) for every thread: Facebook's 170 threads held 130 MB of it,
 * nearly all never touched, and Windows counts it all against the app's limit.
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
/* The same for the buckets [from, to) of the class table, of mg_dvm_class_buckets (the collector's threads share them so). */
void mg_dvm_each_linked_part(u32 from, u32 to, void (*fn)(mg_dvm_class *c, void *user), void *user);
u32 mg_dvm_class_buckets(void);
mg_dvm_dex *mg_dvm_dex_at(u32 i);
const char *mg_dvm_defined_name(u32 i, mg_dvm_dex **dex);

/* -------------------------------------------------------- mg_dvm_object.c */

/*
 * mg_dvm_dex.c: a DEX file whose bytes are at `b` for now (a view the caller unmaps once its classes are registered), opened
 * for its tables and classes' names alone: no checksum, no caches. mg_dvm_dex_ready views it again from `map_path` at
 * `map_at` when the first of its classes is linked, checks it, and makes its caches. With the runtime's lock held.
 */
mg_dvm_dex *mg_dvm_dex_open_lazy(const u8 *b, size_t size, const char *label, const char *map_path, u64 map_at);
/*
 * The descriptors of a DEX file's classes in the order of its definitions, end to end, each ended by a zero: the pool a file
 * registered by its classes' names keeps (`names`), made in one pass over its tables; DVM_META_POOL memory of `*pool_size`
 * bytes. NULL when the bytes are not a DEX file.
 */
char *mg_dvm_dex_names(const u8 *b, size_t size, size_t *pool_size);
bool mg_dvm_dex_ready(mg_dvm_dex *d);
/* Whether a DEX file of these bytes of that file is the runtime's already (registered, viewed or not). */
bool mg_dvm_dex_known(const char *map_path, u64 map_at, size_t size);
/*
 * Another registered DEX file of the same bytes of the same file as lazy `d`, whose classes' names are at hand: in its pool
 * (`names`) or in its view, which stays. NULL when there is none. With the runtime's lock held.
 */
mg_dvm_dex *mg_dvm_dex_twin(const mg_dvm_dex *d);
/*
 * mg_dvm.c: such a file added to the class path, of the app or of a scope (`scope` > 0). `names`, when not NULL, is its pool
 * (mg_dvm_dex_names), which the file takes when it is added; else the names are read from the file.
 */
bool mg_dvm_add_dex_lazy(const void *data, size_t size, const char *label, int scope, const char *map_path, u64 map_at, char *names, size_t names_size);
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
/* Whether [p, p + size) is within the committed part of one of the thread's segments of frames (mg_dvm_stack_seg). */
bool mg_dvm_in_frames(const mg_dvm_thread *t, const void *p, size_t size);
void mg_dvm_monitor_trace_start(void);
void mg_dvm_stacks_trace_start(void);
/* The main thread standing in a wait for `seconds`: its stack said, with what it waits on, and those of the threads that wait
   to enter a monitor (mg_dvm_object.c); true when said. False when the wait is its looper's, for its next message: then
   `idle_on` is the queue it waits on. From the main thread's sampler (mg_java.c, hot_main), never from the main thread. */
bool mg_dvm_stuck_say(mg_dvm_thread *main_thread, double seconds, const void **idle_on);
/* A thread as a report names it: its java.lang.Thread's name in quotes (ASCII; another character a ?), or "thread N". */
void mg_dvm_thread_label(const mg_dvm_thread *t, char *out, size_t n);
void mg_dvm_call_trace_add(const char *prefix);
void mg_dvm_call_trace_within_add(const char *prefix);
/* mg_dvm_class.c: whether a framework method is one of the signatures alone of megler-stubs.txt (mg_dvm_stubs_load). */
bool mg_dvm_stub_listed(const char *cls, const char *name, const char *sig);
/* mg_dvm_interp.c: the first call of a method marked a stub, said in the log with its caller (NULL: called from C). */
void mg_dvm_stub_called(const mg_dvm_method *caller, mg_dvm_method *m);

/*
 * mg_dvm_jit.c (docs/JIT.md): the method compiled to Thumb-2 once its jumps back pass the threshold, and run from `pc`, a
 * branch target, on the frame's registers until it hands the frame back: true with *next the pc the interpreter goes on
 * at; false when there is no code to run there (nothing done). Where there is no JIT (x64), always false.
 */
typedef struct mg_dvm_jit_code mg_dvm_jit_code;
/*
 * What compiled code is run with (r5 holds it): the frame's registers, the flag that says instructions are watched, the pc
 * it hands the frame back at, the thread and the frame for the calls it makes (mg_dvm_jit_invoke), and the result of the
 * last call, which move-result reads. Its layout is the code's (mg_dvm_jit.c, JIT_CTX_*).
 */
typedef struct mg_dvm_jit_ctx {
    mg_dvm_reg *regs;
    const volatile i32 *slow;
    u32 pc;
    mg_dvm_thread *t;
    mg_dvm_frame *f;
    u32 unused;
    mg_jvalue result;
} mg_dvm_jit_ctx;
struct mg_dvm_jit_code {
    u8 *pages;                         /* the code, read and run: on pages of its own, or among other methods' */
    size_t size;
    /* 0: handed back at ctx->pc; 1: an exception is pending, thrown at ctx->pc; 2: returned, its value in ctx->result */
    int (*run)(mg_dvm_jit_ctx *ctx, const void *entry);
    /*
     * The method's start, entered as `run`'s entry, for the calls compiled code makes the quick way (mg_dvm_jit_fast): set
     * when the code has an entry there and a call of the method needs nothing of the general way's (mg_dvm_jit_invoke) --
     * no handlers, not synchronized, no intrinsic. NULL: calls of it go the general way.
     */
    const void *fast;
    /*
     * Where the code is entered, in the order of their pcs: the heads of the loops compiled whole and the method's start,
     * each with its halfword in the code +1. (A table by pc took four bytes for every instruction of the method, for the
     * one to three entries most have.)
     */
    u32 nentries;
    struct mg_dvm_jit_entry {
        u32 pc, at;
    } entries[];
};
/* The code's entry at `pc`, as its halfword +1; 0 for none. */
static inline u32 mg_dvm_jit_entry(const mg_dvm_jit_code *c, u32 pc)
{
    u32 lo = 0, hi = c->nentries;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (c->entries[mid].pc < pc) lo = mid + 1;
        else if (c->entries[mid].pc > pc) hi = mid;
        else return c->entries[mid].at;
    }
    return 0;
}
extern u32 g_dvm_jit_threshold;      /* jumps back before a method is compiled; 0 off (the flag no-jit, or no JIT on this host) */
extern u32 g_dvm_jit_in_place;       /* calls of trivial methods compiled to be done in the code (inline_call), for the tests */
void mg_dvm_jit_start(bool on, u32 threshold, bool trace);
/* What the JIT has made so far, for the log: methods compiled (and of them waiting for their page), refused, its code and the pages it is on. */
typedef struct mg_dvm_jit_stats {
    u32 methods, waiting, refused;
    u64 code_bytes, page_bytes;
    u64 calls_fast, calls_declined, calls_general; /* calls compiled code made: g_dvm_calls_* */
} mg_dvm_jit_stats;
/* Calls compiled code made (mg_dvm_interp.c): the quick way, those the quick way left to the general way, and the general
   way's own; counted without a lock, near enough for the log. */
extern volatile u64 g_dvm_calls_fast, g_dvm_calls_declined, g_dvm_calls_general;
void mg_dvm_jit_census(mg_dvm_jit_stats *out);
/* mg_dvm_interp.c: an invoke done for compiled code; 0 done, 1 thrown, 2 the interpreter's to do (nothing done). */
int mg_dvm_jit_invoke(mg_dvm_thread *t, mg_dvm_frame *f, u32 pc, mg_jvalue *res);
/*
 * What a call site of compiled code tells mg_dvm_jit_fast of its call (mg_dvm_jit.c, fast_invoke): the kind of call in bits
 * 28-29 (DVM_FAST_*); then, for a range, its first register in bits 0-15 and its count in 16-23, with DVM_FAST_RANGE; else up
 * to five registers' numbers a nibble each in bits 0-19, the first first, and their count in bits 20-23.
 */
#define DVM_FAST_RANGE 0x80000000u
enum { DVM_FAST_STATIC, DVM_FAST_DIRECT, DVM_FAST_VIRTUAL, DVM_FAST_INTERFACE };
/*
 * mg_dvm_interp.c: a call compiled code makes the quick way -- into a method with compiled code at its start (its code's
 * `fast`), a frame pushed and the code run with no more than that -- `m` the method itself (static, direct, final) or the
 * one the invoke names (virtual, interface: the object's class says which). 0 done, the value in ctx->result; 1 thrown
 * (pending); 3 not done, nothing having happened: the call goes the general way (mg_dvm_jit_invoke).
 */
int mg_dvm_jit_fast(mg_dvm_jit_ctx *ctx, mg_dvm_method *m, u32 desc, u32 pc);
/* mg_dvm_interp.c: const-class, the monitors, check-cast, instance-of, new-instance, new-array, aput-object for compiled code; the same answers. */
int mg_dvm_jit_op(mg_dvm_thread *t, mg_dvm_frame *f, u32 pc);
int mg_dvm_jit_enter(mg_dvm_thread *t, mg_dvm_frame *f, mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next, mg_jvalue *res);
/*
 * At a jump back, or at a method's start: 0 nothing run; 1 run, the interpreter goes on at *next; 2 run, and an exception
 * is pending, thrown at *next; 3 run to a return, the method's value in *res.
 */
static inline int mg_dvm_jit_run(mg_dvm_thread *t, mg_dvm_frame *f, mg_dvm_jit_code *c, mg_dvm_reg *regs, u32 at, u32 *next, mg_jvalue *res)
{
    mg_dvm_jit_ctx ctx;
    int r;
    ctx.regs = regs;
    ctx.slow = &g_dvm.slow;
    ctx.pc = 0;
    ctx.t = t;
    ctx.f = f;
    r = c->run(&ctx, c->pages + (size_t)(at - 1) * 2 + 1);
    *next = ctx.pc;
    if (r == 2) {
        *res = ctx.result;
        return 3;
    }
    return r ? 2 : 1;
}
static inline int mg_dvm_jit_back(mg_dvm_thread *t, mg_dvm_frame *f, mg_dvm_method *m, mg_dvm_reg *regs, u32 pc, u32 *next, mg_jvalue *res)
{
    mg_dvm_jit_code *c = m->jit;
    if (c) {
        u32 at = mg_dvm_jit_entry(c, pc);
        return at ? mg_dvm_jit_run(t, f, c, regs, at, next, res) : 0;
    }
    if (!g_dvm_jit_threshold || (m->jit_state && m->jit_state != 3)) return 0;
    if (++m->jit_backs < g_dvm_jit_threshold) return 0;
    return mg_dvm_jit_enter(t, f, m, regs, pc, next, res);
}
/* For the collector, with the runtime's lock held: every thread there is, and every interned string. */
void mg_dvm_threads_each(void (*fn)(mg_dvm_thread *t, void *user), void *user);
void mg_dvm_interned_each(void (*fn)(mg_jobj *s, void *user), void *user);
/* The same for the slots [from, to) of the interned strings' table, of mg_dvm_interned_slots. */
void mg_dvm_interned_part(u32 from, u32 to, void (*fn)(mg_jobj *s, void *user), void *user);
u32 mg_dvm_interned_slots(void);
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
/* The same with an interlocked compare-and-swap, for threads that mark at once (mg_dvm_gc.c): one of them sets it. */
bool mg_dvm_heap_set_mark_shared(void *o);
bool mg_dvm_heap_is_marked(const void *o);
void mg_dvm_heap_clear_marks(void);
void mg_dvm_heap_each_marked(void (*fn)(void *o));
/*
 * Marking alongside the app (mg_dvm_gc.c): objects made while `on` are made marked; whether the arenas' pages are watched
 * for writes; the watch started again (before the first stop, the app running); and, the world stopped, `fn` for every
 * marked object on a page written since then -- every marked one where not watched -- and every marked large object: how
 * many pages that was.
 */
void mg_dvm_heap_alloc_black(bool on);
/* Whether this thread holds the runtime's lock (mg_dvm_lock). */
bool mg_dvm_lock_mine(void);
/*
 * mg_dvm_gc.c: an object given fields by name (the JNI layer's, mg_jni.c), which the collector follows apart from its slots
 * when it marks alongside the app; and one whose fields are freed.
 */
void mg_dvm_gc_has_fields(mg_jobj *o);
void mg_dvm_gc_fields_gone(mg_jobj *o);
bool mg_dvm_heap_watched(void);
void mg_dvm_heap_watch_restart(void);
u32 mg_dvm_heap_each_written(void (*fn)(void *o));
/* ...and the sweep, shared among the collector's threads (mg_dvm_gc_share); how many there were. */
u32 mg_dvm_heap_sweep(void (*dying)(void *o), bool poison, u64 *freed_objects, u64 *freed_bytes);
/*
 * mg_dvm_gc.c: `fn` run at once by the collecting thread (worker 0) and each of the collector's helper threads (1 on),
 * until every one has returned; with its lock held, while the helpers have nothing else to do. How many ran it.
 */
#define MG_DVM_GC_WORKERS 4
int mg_dvm_gc_share(void (*fn)(void *arg, int worker), void *arg);
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
