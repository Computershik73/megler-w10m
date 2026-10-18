/*
 * The interpreter: one loop over a method's code units, a switch on the
 * opcode. (No computed goto: MSVC has none.)
 *
 * A call from bytecode to bytecode does not call this function again. It
 * pushes a frame on the thread's own stack and goes on in the callee; a return
 * pops it and goes on in the caller; an exception pops frames until one has a
 * handler. So how deep Java code recurses is bounded by that stack, and a
 * StackOverflowError is an ordinary check, not a guess about the host's
 * stack. The function is entered again only where C stands between two pieces
 * of bytecode: a native method calling back, a static initialiser, the
 * constructor of an exception the interpreter itself throws.
 *
 * How registers hold values is in hk_dvm_internal.h (hk_dvm_reg).
 */
#include "hk_dvm_internal.h"

#include "../hk_core.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------- frames */

#define FRAME_WORDS ((sizeof(hk_dvm_frame) + sizeof(hk_dvm_reg) - 1) / sizeof(hk_dvm_reg))

/* The stack has no room for a frame of `words` registers: the error, or nothing when even the reserve is used up. */
static HK_NOINLINE hk_dvm_frame *stack_full(hk_dvm_thread *t, hk_dvm_method *m, size_t words)
{
    hk_dvm_reg *limit = t->stack + DVM_STACK_WORDS;
    hk_dvm_frame *f;
    if (t->overflowing && words + FRAME_WORDS <= (size_t)(limit - t->sp)) {
        /* The making of the error itself, in the reserve. */
        f = (hk_dvm_frame *)(void *)t->sp;
        f->prev = t->top;
        f->m = m;
        f->regs = t->sp + FRAME_WORDS;
        f->pc = 0;
        f->entry = false;
        f->sync = NULL;
        f->prof_start = t->prof_total;
        t->sp += FRAME_WORDS + words;
        t->top = f;
        t->depth++;
        return f;
    }
    if (t->overflowing) {
        /* Even the reserve is used up, by the making of the error itself: what is pending by now has to do. */
        if (!t->pending) hk_fatal("dvm: the stack is exhausted and no StackOverflowError could be made");
        return NULL;
    }
    t->overflowing = true;
    {
        /*
         * A stack this deep is a recursion that does not end, and the error's own trace is too long to be read for which
         * one: the innermost frames go to the log, where the cycle shows. The first few times only; a program that
         * catches the error and tries again would otherwise fill the log.
         */
        static volatile i32 said;
        if (hk_atomic_add(&said, 1) <= 3) {
            char text[1800];
            size_t k = 0;
            const hk_dvm_frame *up;
            int n;
            for (up = t->top, n = 0; up && n < 28 && k + 160 < sizeof(text); up = up->prev, n++)
                k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s.%s", n ? " < " : "", up->m->cls->name, up->m->name);
            hk_warn("dvm: the stack is full %d frames deep, calling %s.%s; innermost first: %s", t->depth, m->cls->name, m->name, text);
        }
    }
    hk_dvm_throw("java/lang/StackOverflowError", "%d frames deep, calling %s.%s", t->depth, m->cls->name, m->name);
    t->overflowing = false;
    return NULL;
}

/*
 * The flags trace-calls-<prefix> (hk_java.c reads them; the prefix a class name with dots, "com.example.Auth"): every call
 * of a method of a class whose name starts with one of the prefixes is said in the log, with its thread and depth, and so
 * is its return, with the value it returns, or the exception it ends with. What an app does between two things it logs,
 * when it logs nothing in between: VK mp3 mod's sign-in thread ended with no exception and nothing after it ran. Classes
 * are looked at once each (hk_dvm_class.call_traced). At most 8000 lines a run.
 */
static volatile bool g_call_trace;
static char g_call_prefixes[8][160];
static int g_ncall_prefixes;
static volatile i32 g_call_lines;

void hk_dvm_call_trace_add(const char *prefix)
{
    char *p;
    if (g_ncall_prefixes == (int)HK_COUNT(g_call_prefixes) || !*prefix) return;
    hk_strlcpy(g_call_prefixes[g_ncall_prefixes], prefix, sizeof(g_call_prefixes[0]));
    for (p = g_call_prefixes[g_ncall_prefixes]; *p; p++)
        if (*p == '.') *p = '/';
    hk_info("calls: every call of a method of a class whose name starts with %s is said", g_call_prefixes[g_ncall_prefixes]);
    g_ncall_prefixes++;
    g_call_trace = true;
}

static bool call_traced(hk_dvm_method *m)
{
    hk_dvm_class *c = m->cls;
    if (!c->call_traced) {
        int i;
        u8 v = 2;
        for (i = 0; i < g_ncall_prefixes; i++)
            if (!strncmp(c->name, g_call_prefixes[i], strlen(g_call_prefixes[i]))) v = 1;
        c->call_traced = v;
    }
    return c->call_traced == 1 && hk_atomic_add(&g_call_lines, 1) <= 8000;
}

static void call_trace_enter(hk_dvm_thread *t, hk_dvm_method *m, const char *how)
{
    if (call_traced(m)) hk_info("calls: thread %u depth %d: %s.%s%s%s", (unsigned)t->id, t->depth, m->cls->name, m->name, m->sig, how);
}

static void call_trace_return(hk_dvm_thread *t, hk_dvm_method *m, const hk_jvalue *v)
{
    const char *r;
    if (!call_traced(m)) return;
    r = strchr(m->sig, ')');
    switch (r ? r[1] : 'V') {
    case 'V': hk_info("calls: thread %u depth %d: %s.%s returns", (unsigned)t->id, t->depth, m->cls->name, m->name); break;
    case 'L':
    case '[':
        hk_info("calls: thread %u depth %d: %s.%s returns %s", (unsigned)t->id, t->depth, m->cls->name, m->name, v->l ? hk_dvm_class_of(v->l)->name : "null");
        break;
    case 'J': hk_info("calls: thread %u depth %d: %s.%s returns %lld", (unsigned)t->id, t->depth, m->cls->name, m->name, (long long)v->j); break;
    case 'F': hk_info("calls: thread %u depth %d: %s.%s returns %g", (unsigned)t->id, t->depth, m->cls->name, m->name, (double)v->f); break;
    case 'D': hk_info("calls: thread %u depth %d: %s.%s returns %g", (unsigned)t->id, t->depth, m->cls->name, m->name, v->d); break;
    default: hk_info("calls: thread %u depth %d: %s.%s returns %d", (unsigned)t->id, t->depth, m->cls->name, m->name, (int)v->i); break;
    }
}

static void call_trace_unwind(hk_dvm_thread *t, hk_dvm_method *m, hk_jobj *e)
{
    if (call_traced(m))
        hk_info("calls: thread %u depth %d: %s.%s ends with %s", (unsigned)t->id, t->depth, m->cls->name, m->name, e ? hk_dvm_class_of(e)->name : "?");
}

/* A frame for `m` with `words` registers on the thread's stack, made the method being run. NULL when the stack is full (thrown). */
static inline hk_dvm_frame *push_frame(hk_dvm_thread *t, hk_dvm_method *m, size_t words)
{
    hk_dvm_frame *f;
    if (words + FRAME_WORDS > (size_t)(t->stack_end - t->sp)) return stack_full(t, m, words);
    f = (hk_dvm_frame *)(void *)t->sp;
    f->prev = t->top;
    f->m = m;
    f->regs = t->sp + FRAME_WORDS;
    f->pc = 0;
    f->entry = false;
    f->sync = NULL;
    f->prof_start = t->prof_total;
    t->sp += FRAME_WORDS + words;
    t->top = f;
    t->depth++;
    return f;
}

static inline void pop_frame(hk_dvm_thread *t)
{
    hk_dvm_frame *f = t->top;
    if (g_dvm.profiling) {
        f->m->prof_incl += t->prof_total - f->prof_start;
        f->m->prof_calls++;
    }
    t->sp = (hk_dvm_reg *)(void *)f;
    t->top = f->prev;
    t->depth--;
}

/* ----------------------------------------------------------- values in and out */

/* Arguments as the registers hold them (the object first, a long in two) into one hk_jvalue per parameter. */
static void marshal(const hk_dvm_method *m, const hk_dvm_reg *argv, hk_jvalue *out)
{
    const char *s = m->shorty + 1;
    u32 k = (m->flags & DVM_ACC_STATIC) ? 0 : 1;
    int i;
    for (i = 0; *s; s++, i++) {
        out[i].j = 0;
        switch (*s) {
        case 'J': case 'D':
            out[i].j = hk_dvm_get_wide(argv, k);
            k += 2;
            break;
        case 'L': out[i].l = (hk_jobj *)argv[k++]; break;
        /* The small types are given whole: .b and .i both read right, whichever a native method takes. */
        case 'Z': out[i].i = (i32)(argv[k++] & 0xFF); break;
        case 'B': out[i].i = (i32)(i8)argv[k++]; break;
        case 'C': out[i].i = (i32)(u16)argv[k++]; break;
        case 'S': out[i].i = (i32)(i16)argv[k++]; break;
        default: out[i].i = (i32)(u32)argv[k++]; break;          /* int, and a float's bits */
        }
    }
}

/* What C wrote into a result is only defined in its own member; the registers want every bit. */
static void normalise(char kind, hk_jvalue *v)
{
    i32 x;
    hk_jobj *o;
    switch (kind) {
    case 'J': case 'D': return;
    case 'L':
        o = v->l;
        v->j = 0;
        v->l = o;
        return;
    case 'Z': x = v->z ? 1 : 0; break;
    case 'B': x = v->b; break;
    case 'C': x = v->c; break;
    case 'S': x = v->s; break;
    case 'V': x = 0; break;
    default: x = v->i; break;
    }
    v->j = 0;
    v->i = x;
}

/*
 * What the meter counts of a native method's time (hk_dvm_thread_costs) without reading the clock on every call: a read
 * costs more than a short native method itself (on this PC two reads were over half of String.charAt's call). Each method's
 * first 64 calls are timed, and so is every 64th after them; a method whose average is under SHORT_NATIVE_NS has its other
 * calls counted at that average, and one whose calls are longer -- drawing, files, the network -- is timed every time.
 */
#define SHORT_NATIVE_NS 4000
static u64 g_short_native_ticks;

static inline bool native_timed(hk_dvm_method *m)
{
    u32 n = m->meter_calls++;
    if (!g_short_native_ticks) {
        u64 per_ms = 1000000000ull / (hk_ticks_ns(1000000000ull) / 1000000ull + 1) + 1;   /* ticks in a millisecond, at least 1 */
        g_short_native_ticks = per_ms * SHORT_NATIVE_NS / 1000000ull + 1;
    }
    return n < 64 || (n & 63) == 0 || m->meter_avg >= g_short_native_ticks;
}

static inline void native_time(hk_dvm_thread *t, hk_dvm_method *m, u64 started)
{
    u64 d = hk_time_ticks() - started;
    t->native_ticks += d;
    /* An average that follows the method's calls (an eighth of each new one), kept in 32 bits. */
    if (d > 0xFFFFFFFFu) d = 0xFFFFFFFFu;
    m->meter_avg = m->meter_calls <= 1 ? (u32)d : (u32)(((u64)m->meter_avg * 7 + d) / 8);
}

static bool invoke_native(hk_dvm_thread *t, hk_dvm_method *m, hk_jobj *self, const hk_dvm_reg *argv, const hk_jvalue *cparams, hk_jvalue *result)
{
    hk_dvm_frame *f;
    hk_jobj *sync = NULL, *target;
    const hk_jvalue *params = cparams;
    u64 native_started = 0, call_started = 0;
    bool is_static = (m->flags & DVM_ACC_STATIC) != 0, outermost, timed = false;
    result->j = 0;
    if (!m->bound && !hk_dvm_bind_native(m)) return false;
    /* The frame is what a stack trace shows of a native method; the parameters are laid out above it. */
    f = push_frame(t, m, argv ? ((size_t)m->nparams * sizeof(hk_jvalue) + 8) / sizeof(hk_dvm_reg) + 1 : 0);
    if (!f) return false;
    if (argv) {
        hk_jvalue *made = (hk_jvalue *)(void *)HK_ALIGN_UP((hk_addr)f->regs, (hk_addr)8);
        marshal(m, argv, made);
        params = made;
    }
    f->regs = NULL;
    target = is_static ? hk_dvm_mirror(m->cls) : self;
    if (m->flags & DVM_ACC_SYNCHRONIZED) {
        sync = target;
        hk_dvm_monitor_enter(sync);
    }
    if (g_dvm.trace >= 2) hk_info("dvm: %*s-> %s.%s%s (native)", t->depth, "", m->cls->name, m->name, m->sig);
    if (g_call_trace) call_trace_enter(t, m, " (native)");
    if (g_dvm.profiling) native_started = hk_time_monotonic_ns();
    /* The always-on count (hk_dvm_thread_costs): the outermost native method's time; one called back from it is in it. */
    outermost = t->native_depth++ == 0;
    if (outermost) {
        timed = native_timed(m);
        if (timed) call_started = hk_time_ticks();
    }
    t->natives++;
    if (m->native) {
        hk_jcall c;
        memset(&c, 0, sizeof(c));
        c.self = is_static ? NULL : self;
        c.cls = m->cls->jc;
        c.name = m->name;
        c.sig = m->sig;
        c.args = params;
        m->native(&c);
        *result = c.ret;
    } else if (g_dvm.jni.call) {
        /* A library's function. One marked @CriticalNative is called with its arguments alone, as Android calls it. */
        if (!m->critical) m->critical = is_static && hk_dvm_method_annotated(m, "Ldalvik/annotation/optimization/CriticalNative;") ? 1 : -1;
        if (m->critical > 0 && g_dvm.jni.call_bare) *result = g_dvm.jni.call_bare(m->jni, m->sig, params);
        else *result = g_dvm.jni.call(m->jni, target, m->sig, params);
        /* What a library hands back as an object must be one of the runtime's; the JNI layer has others, from before it started. */
        if (m->shorty[0] == 'L' && result->l && !result->l->dvm) {
            hk_note_once("dvm: %s.%s returned an object that is not the runtime's; null is taken instead", m->cls->name, m->name);
            result->l = NULL;
        }
    } else {
        hk_dvm_throw("java/lang/UnsatisfiedLinkError", "%s.%s%s is in a library, and the way to it is gone", m->cls->name, m->name, m->sig);
    }
    --t->native_depth;
    if (timed) native_time(t, m, call_started);
    else if (outermost) t->native_ticks += m->meter_avg;
    if (sync) hk_dvm_monitor_exit(sync);
    if (native_started) m->prof_ns += hk_time_monotonic_ns() - native_started;
    pop_frame(t);
    normalise(m->shorty[0], result);
    return t->pending == NULL;
}

/* ---------------------------------------------------------------- intrinsics */

/*
 * Methods of libcore's that Android's own runtime does without calling them (its intrinsics), done here the same way:
 * an invoke whose target is one of them does its work in place, with no frame and no native call, when the arguments
 * are ordinary. When they are not -- an index out of range, a null, arrays of two kinds -- the intrinsic declines and the
 * method is called as it always was, so every exception is the method's own, from its own frame. Marked on libcore
 * only (hk_dvm_intrinsics_start), where the classes are Android's.
 *
 * They are the ones the benchmark and the phone's profiles showed calls of, a native call each or a Java loop around
 * one: String.charAt (a native call), length and isEmpty (a field the runtime owns, read by a call), equals (a loop of
 * charAt calls), hashCode (the same, once); System.arraycopy; Math.min, max and abs of int and long, and sqrt;
 * Object.getClass; Thread.currentThread.
 */
enum {
    INTR_NONE, INTR_STRING_CHARAT, INTR_STRING_LENGTH, INTR_STRING_ISEMPTY, INTR_STRING_EQUALS, INTR_STRING_HASHCODE, INTR_ARRAYCOPY,
    INTR_MIN_I, INTR_MAX_I, INTR_ABS_I, INTR_MIN_J, INTR_MAX_J, INTR_ABS_J, INTR_SQRT, INTR_GETCLASS, INTR_CURRENT_THREAD,
};

/* String's hash field, the slot String.hashCode keeps its result in. */
static u32 g_string_hash_slot = ~0u;

static void mark_intrinsic(const char *cls, const char *name, const char *sig, bool is_static, u8 id)
{
    hk_dvm_class *c = hk_dvm_find_class(cls);
    hk_dvm_method *m = c ? hk_dvm_find_method(c, name, sig, is_static) : NULL;
    if (!c) hk_dvm_clear_pending();
    if (m) m->intrinsic = id;
    else hk_warn("dvm: %s.%s%s is not there to be done in place", cls, name, sig);
}

void hk_dvm_intrinsics_start(void)
{
    hk_dvm_class *s = g_dvm.c_string;
    hk_dvm_field *hash = s ? hk_dvm_find_field_typed(s, "hash", "I", false) : NULL;
    if (hash && hash->slot < s->nslots) g_string_hash_slot = hash->slot;
    mark_intrinsic("java/lang/String", "charAt", "(I)C", false, INTR_STRING_CHARAT);
    mark_intrinsic("java/lang/String", "length", "()I", false, INTR_STRING_LENGTH);
    mark_intrinsic("java/lang/String", "isEmpty", "()Z", false, INTR_STRING_ISEMPTY);
    mark_intrinsic("java/lang/String", "equals", "(Ljava/lang/Object;)Z", false, INTR_STRING_EQUALS);
    if (g_string_hash_slot != ~0u) mark_intrinsic("java/lang/String", "hashCode", "()I", false, INTR_STRING_HASHCODE);
    mark_intrinsic("java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", true, INTR_ARRAYCOPY);
    mark_intrinsic("java/lang/Math", "min", "(II)I", true, INTR_MIN_I);
    mark_intrinsic("java/lang/Math", "max", "(II)I", true, INTR_MAX_I);
    mark_intrinsic("java/lang/Math", "abs", "(I)I", true, INTR_ABS_I);
    mark_intrinsic("java/lang/Math", "min", "(JJ)J", true, INTR_MIN_J);
    mark_intrinsic("java/lang/Math", "max", "(JJ)J", true, INTR_MAX_J);
    mark_intrinsic("java/lang/Math", "abs", "(J)J", true, INTR_ABS_J);
    mark_intrinsic("java/lang/Math", "sqrt", "(D)D", true, INTR_SQRT);
    mark_intrinsic("java/lang/Object", "getClass", "()Ljava/lang/Class;", false, INTR_GETCLASS);
    mark_intrinsic("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", true, INTR_CURRENT_THREAD);
}

/* System.arraycopy's work when nothing about it can throw: false to have the method called instead. */
static bool arraycopy_in_place(hk_jobj *src, i32 sp, hk_jobj *dst, i32 dp, i32 n)
{
    u32 len_s, len_d, esz;
    if (!src || !dst || sp < 0 || dp < 0 || n < 0 || src->kind != dst->kind) return false;
    if (src->kind == HK_J_PRIM_ARRAY) {
        if (src->u.arr.esz != dst->u.arr.esz || src->u.arr.etype != dst->u.arr.etype) return false;
        esz = src->u.arr.esz;
        len_s = src->u.arr.len;
        len_d = dst->u.arr.len;
        if ((u32)sp > len_s || (u32)n > len_s - (u32)sp || (u32)dp > len_d || (u32)n > len_d - (u32)dp) return false;
        if (n) memmove((u8 *)dst->u.arr.data + (size_t)dp * esz, (const u8 *)src->u.arr.data + (size_t)sp * esz, (size_t)n * esz);
        return true;
    }
    if (src->kind != HK_J_OBJ_ARRAY) return false;
    /* References: only where every element of the one is sure to fit the other (no element is looked at). */
    if (hk_dvm_class_of(src) != hk_dvm_class_of(dst)) {
        hk_dvm_class *from = hk_dvm_class_of(src)->component, *to = hk_dvm_class_of(dst)->component;
        if (!from || !to || (to != g_dvm.c_object && !hk_dvm_assignable(from, to))) return false;
    }
    len_s = src->u.oarr.len;
    len_d = dst->u.oarr.len;
    if ((u32)sp > len_s || (u32)n > len_s - (u32)sp || (u32)dp > len_d || (u32)n > len_d - (u32)dp) return false;
    if (n) memmove(dst->u.oarr.v + dp, src->u.oarr.v + sp, (size_t)n * sizeof(hk_jobj *));
    return true;
}

/* An intrinsic's work on the invoke's arguments as the registers hold them: false to call the method instead. */
static bool intrinsic(hk_dvm_thread *t, u8 id, const hk_dvm_reg *a, hk_jvalue *r)
{
    hk_jobj *s = (hk_jobj *)a[0];
    r->j = 0;
    switch (id) {
    case INTR_STRING_CHARAT:
        if ((u32)a[1] >= (u32)s->u.str.len16) return false;
        r->i = s->u.str.u16[(u32)a[1]];
        return true;
    case INTR_STRING_LENGTH: r->i = s->u.str.len16; return true;
    case INTR_STRING_ISEMPTY: r->i = s->u.str.len16 == 0; return true;
    case INTR_STRING_EQUALS: {
        hk_jobj *o = (hk_jobj *)a[1];
        if (o == s) r->i = 1;
        else if (!o || hk_dvm_class_of(o) != hk_dvm_class_of(s)) r->i = 0;
        else r->i = o->u.str.len16 == s->u.str.len16 && !memcmp(o->u.str.u16, s->u.str.u16, (size_t)s->u.str.len16 * 2);
        return true;
    }
    case INTR_STRING_HASHCODE: {
        hk_jvalue *slot = &hk_dvm_slots(s)[g_string_hash_slot];
        i32 h = slot->i, i;
        if (h == 0 && s->u.str.len16 > 0) {
            u32 x = 0;
            for (i = 0; i < s->u.str.len16; i++) x = x * 31u + s->u.str.u16[i];
            h = (i32)x;
            slot->i = h;
        }
        r->i = h;
        return true;
    }
    case INTR_ARRAYCOPY: return arraycopy_in_place(s, (i32)(u32)a[1], (hk_jobj *)a[2], (i32)(u32)a[3], (i32)(u32)a[4]);
    case INTR_MIN_I: r->i = (i32)(u32)a[0] < (i32)(u32)a[1] ? (i32)(u32)a[0] : (i32)(u32)a[1]; return true;
    case INTR_MAX_I: r->i = (i32)(u32)a[0] > (i32)(u32)a[1] ? (i32)(u32)a[0] : (i32)(u32)a[1]; return true;
    case INTR_ABS_I: r->i = (i32)(u32)a[0] < 0 ? (i32)(0u - (u32)a[0]) : (i32)(u32)a[0]; return true;
    case INTR_MIN_J: {
        i64 x = hk_dvm_get_wide(a, 0), y = hk_dvm_get_wide(a, 2);
        r->j = x < y ? x : y;
        return true;
    }
    case INTR_MAX_J: {
        i64 x = hk_dvm_get_wide(a, 0), y = hk_dvm_get_wide(a, 2);
        r->j = x > y ? x : y;
        return true;
    }
    case INTR_ABS_J: {
        i64 x = hk_dvm_get_wide(a, 0);
        r->j = x < 0 ? (i64)(0 - (u64)x) : x;
        return true;
    }
    case INTR_SQRT: r->j = (i64)hk_db(sqrt(hk_d((u64)hk_dvm_get_wide(a, 0)))); return true;
    case INTR_GETCLASS: {
        hk_jobj *mirror = hk_dvm_class_of(s)->mirror;
        if (!mirror) return false;
        r->l = mirror;
        return true;
    }
    case INTR_CURRENT_THREAD:
        if (!t->peer) return false;
        r->l = t->peer;
        return true;
    default: return false;
    }
}

/* ----------------------------------------------------------- trivial methods */

/*
 * Methods whose whole body is one of a few trivial shapes are done in place by the call, with no frame: an empty body
 * (Object.<init>, which every constructor ends in, and the empty methods listeners leave), a constant returned, a field
 * of the object returned (a getter), a field of the object set from the argument (a setter), a static field returned.
 * What dexopt did on Dalvik and inlining does on ART for the same code. The shape is found at a method's first call. A
 * call that cannot be done in place (the field's class not initialised yet, an object without the field) runs the method
 * as before, so anything that throws throws from the method's own frame. Not while every instruction is watched (the
 * trace, the step limit, the profile): those see each method's own instructions.
 */
enum { SHAPE_UNKNOWN, SHAPE_REGULAR, SHAPE_EMPTY, SHAPE_CONST, SHAPE_GETTER, SHAPE_SETTER, SHAPE_STATIC_GETTER };

/* The return instruction that goes with each iget and sget kind: return, return-wide, return-object. */
static u16 return_for(u32 op)
{
    u32 k = op >= 0x60 ? op - 0x60 : op - 0x52;
    return k == 1 ? 0x10 : k == 2 ? 0x11 : 0x0f;
}

#define SHAPE_OF(w) ((w) & 0xFFu)
#define SHAPE_OP(w) (((w) >> 8) & 0xFFu)
#define SHAPE_VALUE(w) ((i32)(i16)((w) >> 16))

static HK_NOINLINE void classify(hk_dvm_method *m)
{
    const u16 *c = m->insns;
    bool is_static = (m->flags & DVM_ACC_STATIC) != 0;
    u32 shape = SHAPE_REGULAR, value = 0;
    u32 op = c ? c[0] & 0xFF : 0;
    hk_dvm_field *fld = NULL;
    if (!c || (m->flags & (DVM_ACC_SYNCHRONIZED | DVM_ACC_NATIVE)) || m->ntries) {
        hk_atomic_xchg((volatile i32 *)&m->shape, SHAPE_REGULAR);
        return;
    }
    if (m->ninsns == 1 && c[0] == 0x000e) {
        shape = SHAPE_EMPTY;
    } else if (m->ninsns == 2 && op == 0x12 && (c[1] & 0xFF) == 0x0f && (u32)(c[1] >> 8) == ((c[0] >> 8) & 0xFu)) {
        /* const/4 vA, #+B; return vA */
        shape = SHAPE_CONST;
        value = (u32)((i32)((c[0] >> 12) ^ 8) - 8) & 0xFFFFu;
    } else if (m->ninsns == 3 && !is_static && m->ins == 1 && op >= 0x52 && op <= 0x58 && (u32)(c[0] >> 12) == (u32)m->regs - 1u
               && (c[2] & 0xFF) == return_for(op) && (u32)(c[2] >> 8) == ((c[0] >> 8) & 0xFu)) {
        /* iget-kind vA, this, field; return-kind vA */
        fld = hk_dvm_resolve_field(m->cls->dex, c[1], false);
        if (fld && fld->slot < fld->cls->nslots) shape = SHAPE_GETTER;
    } else if (m->ninsns == 3 && !is_static && m->ins == 2 && op >= 0x59 && op <= 0x5f && op != 0x5a && (u32)(c[0] >> 12) == (u32)m->regs - 2u
               && ((c[0] >> 8) & 0xFu) == (u32)m->regs - 1u && c[2] == 0x000e) {
        /* iput-kind value, this, field; return-void (not a long or a double, which come in two registers) */
        fld = hk_dvm_resolve_field(m->cls->dex, c[1], false);
        if (fld && fld->slot < fld->cls->nslots) shape = SHAPE_SETTER;
    } else if (m->ninsns == 3 && is_static && m->ins == 0 && op >= 0x60 && op <= 0x66 && (c[2] & 0xFF) == return_for(op) && (u32)(c[2] >> 8) == (u32)(c[0] >> 8)) {
        /* sget-kind vAA, field; return-kind vAA */
        fld = hk_dvm_resolve_field(m->cls->dex, c[1], true);
        if (fld && fld->cls->statics) shape = SHAPE_STATIC_GETTER;
    }
    /* A field that does not resolve is the method's to throw about when it runs. */
    if (!fld && shape == SHAPE_REGULAR) hk_dvm_clear_pending();
    /* (A regular method's word is SHAPE_REGULAR alone, which the call compares with first.) */
    if (shape == SHAPE_REGULAR) op = value = 0;
    /* The field first, then the word that says how to use it, with a barrier between (another thread reads them unlocked). */
    m->shape_field = fld;
    hk_atomic_xchg((volatile i32 *)&m->shape, (i32)(shape | op << 8 | value << 16));
}

/* A field slot's value as the iget or sget of that kind reads it, into a result. */
static inline void read_kind(u32 op, const hk_jvalue *s, hk_jvalue *r)
{
    r->j = 0;
    switch (op >= 0x60 ? op - 0x60 : op - 0x52) {
    case 0: r->i = s->i; break;
    case 1: r->j = s->j; break;
    case 2: r->l = s->l; break;
    case 3: r->i = s->z; break;
    case 4: r->i = (i32)s->b; break;
    case 5: r->i = s->c; break;
    default: r->i = (i32)s->s; break;
    }
}

/* A trivial method's work on the invoke's arguments (`w` its shape word): false to call it instead. */
static bool trivial(const hk_dvm_method *m, u32 w, const hk_dvm_reg *a, hk_jvalue *r)
{
    hk_jobj *o = (hk_jobj *)a[0];
    const hk_dvm_field *fld = m->shape_field;
    hk_jvalue *s;
    switch (SHAPE_OF(w)) {
    case SHAPE_EMPTY: return true;
    case SHAPE_CONST:
        r->j = 0;
        r->i = SHAPE_VALUE(w);
        return true;
    case SHAPE_GETTER:
        /* (The field may not be seen yet on another processor than the one that wrote it: then the method runs.) */
        if (!fld || fld->slot >= hk_dvm_class_of(o)->nslots) return false;
        read_kind(SHAPE_OP(w), &hk_dvm_slots(o)[fld->slot], r);
        return true;
    case SHAPE_SETTER:
        if (!fld || fld->slot >= hk_dvm_class_of(o)->nslots) return false;
        s = &hk_dvm_slots(o)[fld->slot];
        switch (SHAPE_OP(w)) {
        case 0x59: s->i = (i32)(u32)a[1]; break;
        case 0x5b: s->l = (hk_jobj *)a[1]; break;
        case 0x5c: s->z = (u8)a[1]; break;
        case 0x5d: s->b = (i8)a[1]; break;
        case 0x5e: s->c = (u16)a[1]; break;
        default: s->s = (i16)a[1]; break;
        }
        return true;
    case SHAPE_STATIC_GETTER:
        if (!fld || fld->cls->state != DVM_CS_INITIALIZED) return false;
        read_kind(SHAPE_OP(w), &fld->cls->statics[fld->slot], r);
        return true;
    default: return false;
    }
}

/* ---------------------------------------------------------------- exceptions */

/*
 * The flag trace-exceptions (a file of that name in LocalState\flags, read once): every exception thrown is said in the log
 * once, with its message and the method it was thrown in, caught or not -- what an app catches and says nothing about is
 * otherwise not seen at all (VK mp3 mod's sign-in stood still with nothing in the log). At most 2000 lines a run.
 */
static void trace_exception(hk_dvm_thread *t, hk_dvm_method *m, u32 pc, hk_jobj *e)
{
    static int wanted = -1;
    static volatile i32 said;
    const char *text = NULL;
    if (wanted < 0) wanted = hk_flag("trace-exceptions") ? 1 : 0;
    if (!wanted || e == t->traced) return;
    t->traced = e;
    if (hk_atomic_add(&said, 1) > 2000) return;
    if (g_dvm.f_message && hk_dvm_is_instance(e, g_dvm.c_throwable)) {
        hk_jobj *msg = hk_dvm_slots(e)[g_dvm.f_message->slot].l;
        if (msg) text = hk_dvm_string_utf8(msg);
    }
    hk_info("dvm: thrown on thread %u: %s%s%s, in %s.%s at %u", (unsigned)t->id, hk_dvm_class_name(e), text ? ": " : "", text ? text : "", m->cls->name,
            m->name, (unsigned)pc);
}

/* The handler in method `m` for exception `e` thrown at `pc`, if it has one. */
static bool find_handler(hk_dvm_thread *t, hk_dvm_method *m, u32 pc, hk_jobj *e, u32 *target)
{
    const u8 *tries, *handlers, *end;
    hk_dvm_class *ec = hk_dvm_class_of(e);
    u32 i;
    bool found = false;
    trace_exception(t, m, pc, e);
    /* Nothing catches the error a used-up step limit throws: code that catches everything and goes round again would never end. */
    if (!m->ntries || t->steps_left == ~(u64)0) return false;
    tries = (const u8 *)(m->insns + m->ninsns);
    if (m->ninsns & 1) tries += 2;                               /* padding to four bytes */
    handlers = tries + 8 * (size_t)m->ntries;
    end = m->cls->dex->b + m->cls->dex->size;
    if (handlers >= end) return false;
    for (i = 0; i < m->ntries && !found; i++) {
        const u8 *ti = tries + 8 * i, *h;
        u32 start = hk_dvm_rd32(ti), count = hk_dvm_rd16(ti + 4), hoff = hk_dvm_rd16(ti + 6);
        i32 size;
        int n, k;
        if (pc < start || pc >= start + count) continue;
        h = handlers + hoff;
        if (h >= end) return false;
        size = hk_dvm_sleb(&h);
        n = size < 0 ? -size : size;
        for (k = 0; k < n && h < end; k++) {
            u32 type = hk_dvm_uleb(&h), addr = hk_dvm_uleb(&h);
            hk_dvm_class *tc;
            /* A catch of a class that cannot be found catches nothing; looking for it must not disturb what is being thrown. */
            t->pending = NULL;
            tc = hk_dvm_resolve_type(m->cls->dex, type);
            t->pending = NULL;
            if (tc && (tc == ec || hk_dvm_assignable(ec, tc))) {
                *target = addr;
                found = true;
                break;
            }
        }
        if (!found && size <= 0 && h < end) {
            *target = hk_dvm_uleb(&h);
            found = true;
        }
        break;                                                   /* the first range that covers pc decides: ranges do not overlap */
    }
    if (found && *target >= m->ninsns) found = false;
    t->pending = e;
    return found;
}

/* ------------------------------------------------------- the slow instruction */

/* Tracing at level 3 and the step limit: what every instruction does first while g_dvm.slow is set. False when it threw. */
static HK_NOINLINE bool slow_step(hk_dvm_thread *t, hk_dvm_frame *f, u32 pc)
{
    g_dvm.steps++;
    if (g_dvm.profiling) {
        f->m->prof_self++;
        t->prof_total++;
    }
    if (g_dvm.trace >= 3) hk_info("dvm: %*s%s.%s %u: %s", t->depth, "", f->m->cls->name, f->m->name, (unsigned)pc, hk_dvm_insn_name(f->m->insns[pc]));
    if (g_dvm.step_limit && t->steps_left != ~(u64)0) {
        if (t->steps_left == 0) {
            /* All ones marks the limit as hit: the error's own constructor is not counted, and nothing may catch it. */
            t->steps_left = ~(u64)0;
            f->pc = pc;
            return hk_dvm_throw("java/lang/InternalError", "the step limit is used up in %s.%s", f->m->cls->name, f->m->name);
        }
        t->steps_left--;
    }
    return true;
}

/* -------------------------------------------------------------- the main loop */

static i32 float_to_int(float v)
{
    if (v != v) return 0;
    if (v >= 2147483648.0f) return INT32_MAX;
    if (v <= -2147483648.0f) return INT32_MIN;
    return (i32)v;
}
static i32 double_to_int(double v)
{
    if (v != v) return 0;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return (i32)v;
}
static i64 double_to_long(double v)
{
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v <= -9223372036854775808.0) return INT64_MIN;
    return (i64)v;
}

static inline i32 div_int(i32 a, i32 b) { return a == INT32_MIN && b == -1 ? a : a / b; }
static inline i32 rem_int(i32 a, i32 b) { return a == INT32_MIN && b == -1 ? 0 : a % b; }
static inline i64 div_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? a : a / b; }
static inline i64 rem_long(i64 a, i64 b) { return a == INT64_MIN && b == -1 ? 0 : a % b; }
/* Shifts of signed values, spelled so that no compiler may think them undefined. */
static inline i32 shr_int(i32 a, u32 n) { return a < 0 ? (i32)~(~(u32)a >> n) : (i32)((u32)a >> n); }
static inline i64 shr_long(i64 a, u32 n) { return a < 0 ? (i64)~(~(u64)a >> n) : (i64)((u64)a >> n); }

/* An instance field's slot in an object, or NULL when the object is of no class that has the field (bytecode nothing verified). */
static HK_NOINLINE hk_jvalue *slotless_field(hk_dvm_thread *t, hk_jobj *o, const hk_dvm_field *fld)
{
    /* One of the fields the runtime owns: what it stands for, in a place of the thread's own, where a write is lost. */
    t->scratch.j = 0;
    if (fld->slot == DVM_SLOT_KLASS) t->scratch.l = hk_dvm_mirror(hk_dvm_class_of(o));
    else if (fld->slot == DVM_SLOT_MONITOR) t->scratch.i = DVM_HASH_WORD(o);
    else if (fld->slot == DVM_SLOT_COUNT && o->kind == HK_J_STRING) t->scratch.i = DVM_STRING_COUNT(o->u.str.len16);
    else return NULL;
    return &t->scratch;
}

static inline hk_jvalue *field_slot(hk_dvm_thread *t, hk_jobj *o, const hk_dvm_field *fld)
{
    if (fld->slot >= hk_dvm_class_of(o)->nslots) return slotless_field(t, o, fld);
    return &hk_dvm_slots(o)[fld->slot];
}

/* The data of a switch or fill-array-data, checked to lie inside the method; NULL when it does not. */
static const u16 *payload_at(const hk_dvm_method *m, u32 pc, i32 off, u16 ident)
{
    i64 at = (i64)pc + off;
    if (at < 0 || at + 4 > (i64)m->ninsns || m->insns[at] != ident) return NULL;
    if (!hk_dvm_insn_units(m->insns, m->ninsns, (u32)at)) return NULL;
    return m->insns + at;
}

#define FETCH(n) (insns[pc + (n)])
#define A4 (((u32)w >> 8) & 0xF)
#define B4 ((u32)w >> 12)
#define AA ((u32)w >> 8)
#define RI(n) ((i32)(u32)regs[n])
#define RL(n) ((hk_jobj *)regs[n])
#define RJ(n) hk_dvm_get_wide(regs, (n))
#define RF(n) hk_f((u32)regs[n])
#define RD(n) hk_d((u64)hk_dvm_get_wide(regs, (n)))
#define WI(n, v) (regs[n] = (hk_dvm_reg)(u32)(v))
#define WL(n, o) (regs[n] = (hk_dvm_reg)(o))
#define WJ(n, v) hk_dvm_put_wide(regs, (n), (i64)(v))
#define WF(n, v) (regs[n] = (hk_dvm_reg)hk_fb(v))
#define WD(n, v) hk_dvm_put_wide(regs, (n), (i64)hk_db(v))
#define NEXT(n) { pc += (n); continue; }
/* A jump; one back (or to itself) is where the other loop may take over (SAFEPOINT, defined for each loop). */
#define JUMP(off)                                                                    \
    {                                                                                \
        u32 to_ = (u32)((i32)pc + (i32)(off));                                       \
        if ((i32)(off) <= 0) SAFEPOINT(to_);                                         \
        pc = to_;                                                                    \
        continue;                                                                    \
    }
/* Whatever may throw or call out records where it is first: a stack trace reads it, and so does the search for a handler. */
#define THROW(...) { f->pc = pc; hk_dvm_throw(__VA_ARGS__); goto exception; }
#define LOAD_FRAME() (m = f->m, insns = m->insns, regs = f->regs, dex = m->cls->dex)
#define RESOLVE_TYPE(var, idx)                                    \
    {                                                             \
        u32 i_ = (idx);                                           \
        var = i_ < dex->ntype ? dex->tcache[i_] : NULL;           \
        if (!var) {                                               \
            f->pc = pc;                                           \
            var = hk_dvm_resolve_type(dex, i_);                   \
            if (!var) goto exception;                             \
        }                                                         \
    }
/* An array and an index from an aget/aput, checked: null, bounds, and that the array is of the kind the instruction reads (esz 0: references). */
#define ARRAY(a, i, size)                                                                                                        \
    {                                                                                                                            \
        u16 bc_ = FETCH(1);                                                                                                      \
        a = RL(bc_ & 0xFF);                                                                                                      \
        i = RI(bc_ >> 8);                                                                                                        \
        if (!a) THROW("java/lang/NullPointerException", "Attempt to %s a null array", (w & 0xFF) >= 0x4b ? "write to" : "read from"); \
        if ((size) ? (a->kind != HK_J_PRIM_ARRAY || a->u.arr.esz != (size)) : a->kind != HK_J_OBJ_ARRAY)                          \
            THROW("java/lang/InternalError", "%s on a %s", hk_dvm_insn_name(w), hk_dvm_class_of(a)->name);                        \
        if ((u32)i >= (u32)hk_dvm_array_length(a))                                                                               \
            THROW("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)hk_dvm_array_length(a), (int)i);        \
    }
#define IFIELD(dst_)                                                                                                            \
    {                                                                                                                            \
        u32 i_ = FETCH(1);                                                                                                       \
        hk_jobj *o_ = RL(B4);                                                                                                    \
        hk_dvm_field *fld_ = i_ < dex->nfield ? dex->fcache[i_] : NULL;                                                          \
        if (!fld_ || (fld_->flags & DVM_ACC_STATIC)) {                                                                           \
            f->pc = pc;                                                                                                          \
            fld_ = hk_dvm_resolve_field(dex, i_, false);                                                                         \
            if (!fld_) goto exception;                                                                                           \
        }                                                                                                                        \
        if (!o_) THROW("java/lang/NullPointerException", "Attempt to %s field '%s.%s' on a null object reference",                \
                       (w & 0xFF) >= 0x59 ? "write to" : "read from", fld_->cls->name, fld_->name);                               \
        dst_ = field_slot(t, o_, fld_);                                                                                             \
        if (!dst_) THROW("java/lang/IncompatibleClassChangeError", "a %s has no field %s.%s", hk_dvm_class_of(o_)->name, fld_->cls->name, fld_->name); \
    }
#define SFIELD(dst_)                                                                   \
    {                                                                                   \
        u32 i_ = FETCH(1);                                                              \
        hk_dvm_field *fld_ = i_ < dex->nfield ? dex->fcache[i_] : NULL;                 \
        if (!fld_ || !(fld_->flags & DVM_ACC_STATIC)) {                                 \
            f->pc = pc;                                                                 \
            fld_ = hk_dvm_resolve_field(dex, i_, true);                                 \
            if (!fld_) goto exception;                                                  \
        }                                                                               \
        if (fld_->cls->state != DVM_CS_INITIALIZED) {                                   \
            f->pc = pc;                                                                 \
            if (!hk_dvm_ensure_init(fld_->cls)) goto exception;                         \
        }                                                                               \
        dst_ = &fld_->cls->statics[fld_->slot];                                         \
    }
/* A binary operation in its three-register form and, 0x20 further on, its two-register form. */
#define BIN(op, type, RD_, WR_, expr)                                                   \
    case op: {                                                                          \
        type a = RD_(FETCH(1) & 0xFF), b = RD_(FETCH(1) >> 8);                          \
        WR_(AA, expr);                                                                  \
        NEXT(2);                                                                        \
    }                                                                                   \
    case (op) + 0x20: {                                                                 \
        type a = RD_(A4), b = RD_(B4);                                                  \
        WR_(A4, expr);                                                                  \
        NEXT(1);                                                                        \
    }
/* Division and remainder: zero is checked first. */
#define DIVIDE(op, type, RD_, WR_, expr)                                                \
    case op: {                                                                          \
        type a = RD_(FETCH(1) & 0xFF), b = RD_(FETCH(1) >> 8);                          \
        if (b == 0) THROW("java/lang/ArithmeticException", "divide by zero");           \
        WR_(AA, expr);                                                                  \
        NEXT(2);                                                                        \
    }                                                                                   \
    case (op) + 0x20: {                                                                 \
        type a = RD_(A4), b = RD_(B4);                                                  \
        if (b == 0) THROW("java/lang/ArithmeticException", "divide by zero");           \
        WR_(A4, expr);                                                                  \
        NEXT(1);                                                                        \
    }
/* A long shifted by an int. */
#define SHIFT_LONG(op, expr)                                                            \
    case op: {                                                                          \
        i64 a = RJ(FETCH(1) & 0xFF);                                                    \
        u32 n = (u32)RI(FETCH(1) >> 8) & 63;                                            \
        WJ(AA, expr);                                                                   \
        NEXT(2);                                                                        \
    }                                                                                   \
    case (op) + 0x20: {                                                                 \
        i64 a = RJ(A4);                                                                 \
        u32 n = (u32)RI(B4) & 63;                                                       \
        WJ(A4, expr);                                                                   \
        NEXT(1);                                                                        \
    }
#define IF2(op, cond)                                                                   \
    case op:                                                                            \
        if (cond) JUMP((i16)FETCH(1));                                                  \
        NEXT(2);
#define UNARY(op, WR_, expr)                                                            \
    case op:                                                                            \
        WR_(A4, expr);                                                                  \
        NEXT(1);

enum { DVM_THREW, DVM_RETURNED, DVM_SWITCH };

/* The plain loop and the watched one (hk_dvm_interp_loop.h). Neither static: the benchmark's build places the plain one by name. */
#define DVM_LOOP hk_dvm_interpret
#define DVM_WATCHED 0
#define SAFEPOINT(at)                                                                \
    if (g_dvm.slow) {                                                                \
        f->pc = (at);                                                                \
        goto switch_loop;                                                            \
    }
#include "hk_dvm_interp_loop.h"
#undef DVM_LOOP
#undef DVM_WATCHED
#undef SAFEPOINT
#define DVM_LOOP hk_dvm_interpret_watched
#define DVM_WATCHED 1
#define SAFEPOINT(at)                                                                \
    if (!g_dvm.slow) {                                                               \
        f->pc = (at);                                                                \
        goto switch_loop;                                                            \
    }
#include "hk_dvm_interp_loop.h"
#undef DVM_LOOP
#undef DVM_WATCHED
#undef SAFEPOINT

/* Runs from frame `f` until the frame entered from C returns or throws, in whichever loop is wanted, crossing over when that changes. */
static bool interpret(hk_dvm_thread *t, hk_dvm_frame *f, hk_jvalue *ret)
{
    for (;;) {
        int r = g_dvm.slow ? hk_dvm_interpret_watched(t, f, ret) : hk_dvm_interpret(t, f, ret);
        if (r != DVM_SWITCH) return r == DVM_RETURNED;
        f = t->top;
    }
}

bool hk_dvm_invoke(hk_dvm_method *m, hk_jobj *self, const hk_jvalue *params, hk_jvalue *ret)
{
    hk_dvm_thread *t = hk_dvm_self();
    hk_dvm_frame *f;
    hk_jvalue local;
    bool ok = false;
    if (!ret) ret = &local;
    ret->j = 0;
    /*
     * This is real recursion on the host's stack, once for each time C stands between two pieces of bytecode. It is bounded by
     * count, and by what the host said is the low end of this thread's stack (a thread made with a small one).
     */
    if (t->overflowing ? t->c_depth >= DVM_MAX_C_DEPTH + 8 : (t->c_depth >= DVM_MAX_C_DEPTH || (hk_addr)&local < t->c_stack_low)) {
        /* While the error is being made, its own constructor gets a little further than anything else may go. */
        if (t->overflowing) return false;
        t->overflowing = true;
        hk_dvm_throw("java/lang/StackOverflowError", "%d calls from native code deep, calling %s.%s", t->c_depth, m->cls->name, m->name);
        t->overflowing = false;
        return false;
    }
    if (t->c_depth++ == 0) t->steps_left = g_dvm.step_limit;
    if (!m->insns) {
        if (m->flags & DVM_ACC_NATIVE) ok = invoke_native(t, m, self, NULL, params, ret);
        else hk_dvm_throw("java/lang/AbstractMethodError", "%s.%s%s", m->cls->name, m->name, m->sig);
    } else if ((f = push_frame(t, m, m->regs)) != NULL) {
        const char *s = m->shorty + 1;
        u32 r = (u32)m->regs - m->ins;
        int i;
        f->entry = true;
        memset(f->regs, 0, sizeof(hk_dvm_reg) * m->regs);
        if (!(m->flags & DVM_ACC_STATIC)) f->regs[r++] = (hk_dvm_reg)self;
        for (i = 0; *s; s++, i++) {
            switch (*s) {
            case 'J': case 'D':
                hk_dvm_put_wide(f->regs, r, params[i].j);
                r += 2;
                break;
            case 'L': f->regs[r++] = (hk_dvm_reg)params[i].l; break;
            case 'Z': f->regs[r++] = params[i].z ? 1 : 0; break;
            case 'B': f->regs[r++] = (u32)(i32)params[i].b; break;
            case 'C': f->regs[r++] = params[i].c; break;
            case 'S': f->regs[r++] = (u32)(i32)params[i].s; break;
            default: f->regs[r++] = (u32)params[i].i; break;
            }
        }
        if (m->flags & DVM_ACC_SYNCHRONIZED) {
            f->sync = (m->flags & DVM_ACC_STATIC) ? hk_dvm_mirror(m->cls) : self;
            if (f->sync) hk_dvm_monitor_enter(f->sync);
        }
        if (g_dvm.trace >= 2) hk_info("dvm: %*s-> %s.%s%s (from C)", t->depth, "", m->cls->name, m->name, m->sig);
        if (g_call_trace) call_trace_enter(t, m, " (from C)");
        ok = interpret(t, f, ret);
    }
    t->c_depth--;
    return ok;
}
