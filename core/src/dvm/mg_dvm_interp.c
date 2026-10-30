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
 * How registers hold values is in mg_dvm_internal.h (mg_dvm_reg).
 */
#include "mg_dvm_internal.h"
#include "../ld/mg_ld.h"

#include "../mg_core.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------- frames */

#define FRAME_WORDS ((sizeof(mg_dvm_frame) + sizeof(mg_dvm_reg) - 1) / sizeof(mg_dvm_reg))

/* The stack has no room for a frame of `words` registers: the error, or nothing when even the reserve is used up. */
static MG_NOINLINE mg_dvm_frame *stack_full(mg_dvm_thread *t, mg_dvm_method *m, size_t words)
{
    mg_dvm_reg *limit = t->stack + t->stack_committed; /* the whole stack, unless more of it could not be committed */
    mg_dvm_frame *f;
    if (t->overflowing && words + FRAME_WORDS <= (size_t)(limit - t->sp)) {
        /* The making of the error itself, in the reserve. */
        f = (mg_dvm_frame *)(void *)t->sp;
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
        if (!t->pending) mg_fatal("dvm: the stack is exhausted and no StackOverflowError could be made");
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
        if (mg_atomic_add(&said, 1) <= 3) {
            char text[1800];
            size_t k = 0;
            const mg_dvm_frame *up;
            int n;
            for (up = t->top, n = 0; up && n < 28 && k + 160 < sizeof(text); up = up->prev, n++)
                k += (size_t)snprintf(text + k, sizeof(text) - k, "%s%s.%s", n ? " < " : "", up->m->cls->name, up->m->name);
            mg_warn("dvm: the stack is full %d frames deep, calling %s.%s; innermost first: %s", t->depth, m->cls->name, m->name, text);
        }
    }
    mg_dvm_throw("java/lang/StackOverflowError", "%d frames deep, calling %s.%s", t->depth, m->cls->name, m->name);
    t->overflowing = false;
    return NULL;
}

/*
 * The flags trace-calls-<prefix> (mg_java.c reads them; the prefix a class name with dots, "com.example.Auth"): every call
 * of a method of a class whose name starts with one of the prefixes is said in the log, with its thread and depth, and so
 * is its return, with the value it returns, or the exception it ends with. What an app does between two things it logs,
 * when it logs nothing in between: VK mp3 mod's sign-in thread ended with no exception and nothing after it ran. A String
 * among the arguments of a call from the interpreter, or returned, is said too, its first 80 characters (call_trace_text):
 * GmsCore's sign-in ended in "Unknown" for an error its server answered with, which it says nowhere. Classes are looked
 * at once each (mg_dvm_class.call_traced). At most 8000 lines a run.
 */
static volatile bool g_call_trace;
static char g_call_prefixes[8][160];
static int g_ncall_prefixes;
static volatile i32 g_call_lines;

void mg_dvm_call_trace_add(const char *prefix)
{
    char *p;
    if (g_ncall_prefixes == (int)MG_COUNT(g_call_prefixes) || !*prefix) return;
    mg_strlcpy(g_call_prefixes[g_ncall_prefixes], prefix, sizeof(g_call_prefixes[0]));
    for (p = g_call_prefixes[g_ncall_prefixes]; *p; p++)
        if (*p == '.') *p = '/';
    mg_info("calls: every call of a method of a class whose name starts with %s is said", g_call_prefixes[g_ncall_prefixes]);
    g_ncall_prefixes++;
    g_call_trace = true;
}

static bool call_traced(mg_dvm_method *m)
{
    mg_dvm_class *c = m->cls;
    if (!c->call_traced) {
        int i;
        u8 v = 2;
        for (i = 0; i < g_ncall_prefixes; i++)
            if (!strncmp(c->name, g_call_prefixes[i], strlen(g_call_prefixes[i]))) v = 1;
        c->call_traced = v;
    }
    return c->call_traced == 1 && mg_atomic_add(&g_call_lines, 1) <= 8000;
}

/* A String of a traced call, as said: in quotes, its first 80 characters, those not printable ASCII as '?'. */
static void call_trace_text(mg_jobj *s, char *out, size_t size)
{
    i32 n = 0, i;
    const u16 *u = mg_dvm_string_utf16(s, &n);
    size_t k = 0;
    if (!u) { mg_strlcpy(out, "null", size); return; }
    out[k++] = '"';
    for (i = 0; i < n && i < 80 && k + 5 < size; i++) out[k++] = u[i] >= 0x20 && u[i] < 0x7F ? (char)u[i] : '?';
    if (i < n) { out[k++] = '.'; out[k++] = '.'; out[k++] = '.'; }
    out[k++] = '"';
    out[k] = 0;
}

/* The String arguments of a call (argv: its registers, the object first for one not static), said after its name. */
static void call_trace_strings(const mg_dvm_method *m, const mg_dvm_reg *argv, u32 argc, char *out, size_t size)
{
    const char *p = m->sig + 1;
    u32 r = (m->flags & DVM_ACC_STATIC) ? 0 : 1;
    size_t k = 0;
    out[0] = 0;
    while (*p && *p != ')' && r < argc && k + 100 < size) {
        const char *start = p;
        while (*p == '[') p++;
        if (*p == 'L') { while (*p && *p != ';') p++; if (*p) p++; } else if (*p) p++;
        if ((size_t)(p - start) == 18 && !strncmp(start, "Ljava/lang/String;", 18)) {
            out[k++] = ' ';
            call_trace_text((mg_jobj *)argv[r], out + k, size - k);
            k += strlen(out + k);
        }
        r += (*start == 'J' || *start == 'D') ? 2 : 1;
    }
}

/*
 * The first call of a framework method that is its signature alone (mg_dvm_stubs_load), said with who called it: what an
 * app needs of Android 14 that Megler does not do. Gmail's sync and its conversation view stayed empty or loading, with
 * nothing in the log, on calls of such methods (ContentResolver's sync, FragmentManager's saved state). Once a method a
 * run (two threads at once may both say it), at most 800 lines a run.
 */
static volatile i32 g_stub_lines;

void mg_dvm_stub_called(const mg_dvm_method *caller, mg_dvm_method *m)
{
    if (!m->stub) return;
    m->stub = 0;
    if (mg_atomic_add(&g_stub_lines, 1) > 800) return;
    if (caller)
        mg_info("api: %s.%s%s is a stub here (its signature alone), first called from %s.%s", m->cls->name, m->name, m->sig, caller->cls->name, caller->name);
    else
        mg_info("api: %s.%s%s is a stub here (its signature alone), first called from native code or the runtime", m->cls->name, m->name, m->sig);
}

static void call_trace_enter(mg_dvm_thread *t, mg_dvm_method *m, const char *how, const mg_dvm_reg *argv, u32 argc)
{
    char strings[400];
    if (!call_traced(m)) return;
    strings[0] = 0;
    if (argv) call_trace_strings(m, argv, argc, strings, sizeof(strings));
    mg_info("calls: thread %u depth %d: %s.%s%s%s%s", (unsigned)t->id, t->depth, m->cls->name, m->name, m->sig, how, strings);
}

static void call_trace_return(mg_dvm_thread *t, mg_dvm_method *m, const mg_jvalue *v)
{
    const char *r;
    if (!call_traced(m)) return;
    r = strchr(m->sig, ')');
    switch (r ? r[1] : 'V') {
    case 'V': mg_info("calls: thread %u depth %d: %s.%s returns", (unsigned)t->id, t->depth, m->cls->name, m->name); break;
    case 'L':
    case '[':
        if (v->l && v->l->kind == MG_J_STRING) {
            char text[100];
            call_trace_text(v->l, text, sizeof(text));
            mg_info("calls: thread %u depth %d: %s.%s returns %s", (unsigned)t->id, t->depth, m->cls->name, m->name, text);
            break;
        }
        mg_info("calls: thread %u depth %d: %s.%s returns %s", (unsigned)t->id, t->depth, m->cls->name, m->name, v->l ? mg_dvm_class_of(v->l)->name : "null");
        break;
    case 'J': mg_info("calls: thread %u depth %d: %s.%s returns %lld", (unsigned)t->id, t->depth, m->cls->name, m->name, (long long)v->j); break;
    case 'F': mg_info("calls: thread %u depth %d: %s.%s returns %g", (unsigned)t->id, t->depth, m->cls->name, m->name, (double)v->f); break;
    case 'D': mg_info("calls: thread %u depth %d: %s.%s returns %g", (unsigned)t->id, t->depth, m->cls->name, m->name, v->d); break;
    default: mg_info("calls: thread %u depth %d: %s.%s returns %d", (unsigned)t->id, t->depth, m->cls->name, m->name, (int)v->i); break;
    }
}

static void call_trace_unwind(mg_dvm_thread *t, mg_dvm_method *m, mg_jobj *e)
{
    if (call_traced(m))
        mg_info("calls: thread %u depth %d: %s.%s ends with %s", (unsigned)t->id, t->depth, m->cls->name, m->name, e ? mg_dvm_class_of(e)->name : "?");
}

/*
 * More of the thread's stack committed, enough for a frame of `words` registers above sp and the reserve: doubled until it
 * is, up to the whole stack. False when it is all committed already (the stack is full) or the memory is not there.
 */
static MG_NOINLINE bool stack_grow(mg_dvm_thread *t, size_t words)
{
    size_t need = (size_t)(t->sp - t->stack) + words + FRAME_WORDS + DVM_STACK_RESERVE, more = t->stack_committed;
    if (t->stack_committed >= t->stack_words) return false;
    while (more < need && more < t->stack_words) more *= 2;
    if (more > t->stack_words) more = t->stack_words;
    if (!mg_mem_commit(t->stack + t->stack_committed, sizeof(mg_dvm_reg) * (more - t->stack_committed), MG_PROT_RW)) return false;
    t->stack_committed = more;
    t->stack_end = t->stack + more - DVM_STACK_RESERVE;
    return words + FRAME_WORDS <= (size_t)(t->stack_end - t->sp);
}

/*
 * The frames go on in the next segment (mg_dvm_stack_seg), the kept one if a frame of `words` fits in it, else a new one twice
 * the size of this, or as large as the frame needs: false when the segments would hold more than the thread's whole stack
 * between them (and each its reserve, which only the last one's frames reach), or there is no memory.
 */
static MG_NOINLINE bool stack_next(mg_dvm_thread *t, size_t words)
{
    size_t need = words + FRAME_WORDS + DVM_STACK_RESERVE, used = t->stack_below + t->stack_words;
    size_t cap = t->stack_max + (size_t)(t->nsegs + 1) * DVM_STACK_RESERVE, size;
    mg_dvm_stack_seg next;
    if (t->nsegs == DVM_STACK_SEGS || used >= cap) return false;
    if (t->spare.base && t->spare.words >= need) {
        next = t->spare;
    } else {
        size = t->nsegs == DVM_STACK_SEGS - 1 ? cap - used : t->stack_words * 2;
        if (size < need) size = (need + DVM_STACK_FIRST_WORDS - 1) & ~(size_t)(DVM_STACK_FIRST_WORDS - 1);
        if (size > cap - used) size = cap - used;
        if (size < need) return false;
        next.base = (mg_dvm_reg *)mg_mem_reserve(sizeof(mg_dvm_reg) * size);
        if (!next.base) return false;
        next.words = size;
        next.committed = size < DVM_STACK_FIRST_WORDS ? size : DVM_STACK_FIRST_WORDS;
        if (next.committed < need) next.committed = need;
        if (!mg_mem_commit(next.base, sizeof(mg_dvm_reg) * next.committed, MG_PROT_RW)) {
            mg_mem_release(next.base, sizeof(mg_dvm_reg) * size);
            return false;
        }
        if (t->spare.base) mg_mem_release(t->spare.base, sizeof(mg_dvm_reg) * t->spare.words);
    }
    t->spare.base = NULL;
    /* This one below, written whole before it is counted: the collector may stop the thread anywhere, and scans both. */
    t->segs[t->nsegs].base = t->stack;
    t->segs[t->nsegs].words = t->stack_words;
    t->segs[t->nsegs].committed = t->stack_committed;
    t->segs[t->nsegs].sp = t->sp;
    t->stack_below += t->stack_words;
    t->nsegs++;
    t->stack = next.base;
    t->stack_words = next.words;
    t->stack_committed = next.committed;
    t->stack_end = t->stack + t->stack_committed - DVM_STACK_RESERVE;
    t->sp = t->stack;
    if (t->stack_below + t->stack_words > t->stack_peak) t->stack_peak = t->stack_below + t->stack_words;
    return words + FRAME_WORDS <= (size_t)(t->stack_end - t->sp);
}

/* The first frame of a segment has returned: back to the segment below, where the frames now end; the one left is kept. */
static MG_NOINLINE void stack_back(mg_dvm_thread *t)
{
    mg_dvm_stack_seg *below = &t->segs[t->nsegs - 1];
    if (t->spare.base) mg_mem_release(t->spare.base, sizeof(mg_dvm_reg) * t->spare.words);
    t->spare.base = t->stack;
    t->spare.words = t->stack_words;
    t->spare.committed = t->stack_committed;
    t->stack = below->base;
    t->stack_words = below->words;
    t->stack_committed = below->committed;
    t->stack_end = t->stack + t->stack_committed - DVM_STACK_RESERVE;
    t->sp = below->sp;
    t->stack_below -= below->words;
    t->nsegs--;
}

/* A frame for `m` with `words` registers on the thread's stack, made the method being run. NULL when the stack is full (thrown). */
static inline mg_dvm_frame *push_frame(mg_dvm_thread *t, mg_dvm_method *m, size_t words)
{
    mg_dvm_frame *f;
    if (words + FRAME_WORDS > (size_t)(t->stack_end - t->sp) && !stack_grow(t, words) && !stack_next(t, words)) return stack_full(t, m, words);
    f = (mg_dvm_frame *)(void *)t->sp;
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

static inline void pop_frame(mg_dvm_thread *t)
{
    mg_dvm_frame *f = t->top;
    if (g_dvm.profiling) {
        mg_dvm_method_prof *p = mg_dvm_prof(f->m);
        p->incl += t->prof_total - f->prof_start;
        p->calls++;
    }
    t->sp = (mg_dvm_reg *)(void *)f;
    t->top = f->prev;
    t->depth--;
    if (t->sp == t->stack && t->nsegs) stack_back(t);
}

/* ----------------------------------------------------------- values in and out */

/* Arguments as the registers hold them (the object first, a long in two) into one mg_jvalue per parameter. */
static void marshal(const mg_dvm_method *m, const mg_dvm_reg *argv, mg_jvalue *out)
{
    const char *s = m->shorty + 1;
    u32 k = (m->flags & DVM_ACC_STATIC) ? 0 : 1;
    int i;
    for (i = 0; *s; s++, i++) {
        out[i].j = 0;
        switch (*s) {
        case 'J': case 'D':
            out[i].j = mg_dvm_get_wide(argv, k);
            k += 2;
            break;
        case 'L': out[i].l = (mg_jobj *)argv[k++]; break;
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
static void normalise(char kind, mg_jvalue *v)
{
    i32 x;
    mg_jobj *o;
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
 * What the meter counts of a native method's time (mg_dvm_thread_costs) without reading the clock on every call: a read
 * costs more than a short native method itself (on this PC two reads were over half of String.charAt's call). Each method's
 * first 64 calls are timed, and so is every 64th after them; a method whose average is under SHORT_NATIVE_NS has its other
 * calls counted at that average, and one whose calls are longer -- drawing, files, the network -- is timed every time.
 */
#define SHORT_NATIVE_NS 4000
static u64 g_short_native_ticks;

static inline bool native_timed(mg_dvm_native_info *m)
{
    u32 n = m->meter_calls++;
    if (!g_short_native_ticks) {
        u64 per_ms = 1000000000ull / (mg_ticks_ns(1000000000ull) / 1000000ull + 1) + 1;   /* ticks in a millisecond, at least 1 */
        g_short_native_ticks = per_ms * SHORT_NATIVE_NS / 1000000ull + 1;
    }
    return n < 64 || (n & 63) == 0 || m->meter_avg >= g_short_native_ticks;
}

static inline void native_time(mg_dvm_thread *t, mg_dvm_native_info *m, u64 started)
{
    u64 d = mg_time_ticks() - started;
    t->native_ticks += d;
    /* An average that follows the method's calls (an eighth of each new one), kept in 32 bits. */
    if (d > 0xFFFFFFFFu) d = 0xFFFFFFFFu;
    m->meter_avg = m->meter_calls <= 1 ? (u32)d : (u32)(((u64)m->meter_avg * 7 + d) / 8);
}

static bool invoke_native(mg_dvm_thread *t, mg_dvm_method *m, mg_jobj *self, const mg_dvm_reg *argv, const mg_jvalue *cparams, mg_jvalue *result)
{
    mg_dvm_frame *f;
    mg_dvm_native_info *nat = m->nat;
    mg_jobj *sync = NULL, *target;
    const mg_jvalue *params = cparams;
    u64 native_started = 0, call_started = 0;
    bool is_static = (m->flags & DVM_ACC_STATIC) != 0, outermost, timed = false;
    int library_scope = mg_ld_set_scope(m->cls->scope);
    if ((mg_addr)&f < t->c_stack_min) t->c_stack_min = (mg_addr)&f;
    result->j = 0;
    if (!(nat && nat->bound)) {
        if (!mg_dvm_bind_native(m)) { mg_ld_set_scope(library_scope); return false; }
        nat = m->nat;
    }
    /* The frame is what a stack trace shows of a native method; the parameters are laid out above it. */
    f = push_frame(t, m, argv ? ((size_t)m->nparams * sizeof(mg_jvalue) + 8) / sizeof(mg_dvm_reg) + 1 : 0);
    if (!f) { mg_ld_set_scope(library_scope); return false; }
    if (argv) {
        mg_jvalue *made = (mg_jvalue *)(void *)MG_ALIGN_UP((mg_addr)f->regs, (mg_addr)8);
        marshal(m, argv, made);
        params = made;
    }
    f->regs = NULL;
    target = is_static ? mg_dvm_mirror(m->cls) : self;
    if (m->flags & DVM_ACC_SYNCHRONIZED) {
        sync = target;
        mg_dvm_monitor_enter(sync);
    }
    if (g_dvm.trace >= 2) mg_info("dvm: %*s-> %s.%s%s (native)", t->depth, "", m->cls->name, m->name, m->sig);
    if (g_call_trace) call_trace_enter(t, m, " (native)", NULL, 0);
    if (g_dvm.profiling) native_started = mg_time_monotonic_ns();
    /* The always-on count (mg_dvm_thread_costs): the outermost native method's time; one called back from it is in it. */
    outermost = t->native_depth++ == 0;
    if (outermost) {
        timed = native_timed(nat);
        if (timed) call_started = mg_time_ticks();
    }
    t->natives++;
    if (nat->native) {
        mg_jcall c;
        memset(&c, 0, sizeof(c));
        c.self = is_static ? NULL : self;
        c.cls = m->cls->jc;
        c.name = m->name;
        c.sig = m->sig;
        c.args = params;
        nat->native(&c);
        *result = c.ret;
    } else if (g_dvm.jni.call) {
        /* A library's function. One marked @CriticalNative is called with its arguments alone, as Android calls it. */
        if (!nat->critical) nat->critical = is_static && mg_dvm_method_annotated(m, "Ldalvik/annotation/optimization/CriticalNative;") ? 1 : -1;
        if (nat->critical > 0 && g_dvm.jni.call_bare) {
            *result = g_dvm.jni.call_bare(nat->jni, m->sig, params);
        } else {
            /*
             * Code Android runs in a process of its own, not the app's: a service's (its class, or the loader its dex came
             * from: Google Play services' DroidGuard), or the platform's on a thread a service's code started. A fault in
             * its native code beyond repair ends that process on Android, and the app is told; here it ends this call with
             * an exception, and the app runs on (mg_jni_call_native_guarded).
             */
            int scope = m->cls->scope > 0 ? mg_dvm_scope_root(m->cls->scope) : mg_dvm_dex_process_scope(m->cls->dex);
            if (!scope && !mg_dvm_class_is_app(m->cls)) scope = t->owner_scope;
            if (scope > 0 && g_dvm.jni.call_guarded) {
                bool faulted = false;
                *result = g_dvm.jni.call_guarded(nat->jni, target, m->sig, params, &faulted);
                if (faulted) {
                    mg_warn("dvm: %s.%s%s faulted in its native code; the call ends with an exception, as the process of the code would end on Android", m->cls->name, m->name, m->sig);
                    mg_dvm_throw("java/lang/RuntimeException", "%s.%s%s faulted in its native code: the process it runs in on Android would have ended",
                                 m->cls->name, m->name, m->sig);
                }
            } else {
                *result = g_dvm.jni.call(nat->jni, target, m->sig, params);
            }
        }
        /* What a library hands back as an object must be one of the runtime's; the JNI layer has others, from before it started. */
        if (m->shorty[0] == 'L' && result->l && !result->l->dvm) {
            mg_note_once("dvm: %s.%s returned an object that is not the runtime's; null is taken instead", m->cls->name, m->name);
            result->l = NULL;
        }
    } else {
        mg_dvm_throw("java/lang/UnsatisfiedLinkError", "%s.%s%s is in a library, and the way to it is gone", m->cls->name, m->name, m->sig);
    }
    --t->native_depth;
    if (timed) native_time(t, nat, call_started);
    else if (outermost) t->native_ticks += nat->meter_avg;
    if (sync) mg_dvm_monitor_exit(sync);
    if (native_started) mg_dvm_prof(m)->ns += mg_time_monotonic_ns() - native_started;
    pop_frame(t);
    normalise(m->shorty[0], result);
    mg_ld_set_scope(library_scope);
    return t->pending == NULL;
}

/* ---------------------------------------------------------------- intrinsics */

/*
 * Methods of libcore's that Android's own runtime does without calling them (its intrinsics), done here the same way:
 * an invoke whose target is one of them does its work in place, with no frame and no native call, when the arguments
 * are ordinary. When they are not -- an index out of range, a null, arrays of two kinds -- the intrinsic declines and the
 * method is called as it always was, so every exception is the method's own, from its own frame. Marked on libcore
 * only (mg_dvm_intrinsics_start), where the classes are Android's.
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
    mg_dvm_class *c = mg_dvm_find_class(cls);
    mg_dvm_method *m = c ? mg_dvm_find_method(c, name, sig, is_static) : NULL;
    if (!c) mg_dvm_clear_pending();
    if (m) m->intrinsic = id;
    else mg_warn("dvm: %s.%s%s is not there to be done in place", cls, name, sig);
}

void mg_dvm_intrinsics_start(void)
{
    mg_dvm_class *s = g_dvm.c_string;
    mg_dvm_field *hash = s ? mg_dvm_find_field_typed(s, "hash", "I", false) : NULL;
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
static bool arraycopy_in_place(mg_jobj *src, i32 sp, mg_jobj *dst, i32 dp, i32 n)
{
    u32 len_s, len_d, esz;
    if (!src || !dst || sp < 0 || dp < 0 || n < 0 || src->kind != dst->kind) return false;
    if (src->kind == MG_J_PRIM_ARRAY) {
        if (src->u.arr.esz != dst->u.arr.esz || src->u.arr.etype != dst->u.arr.etype) return false;
        esz = src->u.arr.esz;
        len_s = src->u.arr.len;
        len_d = dst->u.arr.len;
        if ((u32)sp > len_s || (u32)n > len_s - (u32)sp || (u32)dp > len_d || (u32)n > len_d - (u32)dp) return false;
        if (n) memmove((u8 *)dst->u.arr.data + (size_t)dp * esz, (const u8 *)src->u.arr.data + (size_t)sp * esz, (size_t)n * esz);
        return true;
    }
    if (src->kind != MG_J_OBJ_ARRAY) return false;
    /* References: only where every element of the one is sure to fit the other (no element is looked at). */
    if (mg_dvm_class_of(src) != mg_dvm_class_of(dst)) {
        mg_dvm_class *from = mg_dvm_class_of(src)->component, *to = mg_dvm_class_of(dst)->component;
        if (!from || !to || (to != g_dvm.c_object && !mg_dvm_assignable(from, to))) return false;
    }
    len_s = src->u.oarr.len;
    len_d = dst->u.oarr.len;
    if ((u32)sp > len_s || (u32)n > len_s - (u32)sp || (u32)dp > len_d || (u32)n > len_d - (u32)dp) return false;
    if (n) memmove(dst->u.oarr.v + dp, src->u.oarr.v + sp, (size_t)n * sizeof(mg_jobj *));
    return true;
}

/* An intrinsic's work on the invoke's arguments as the registers hold them: false to call the method instead. */
static bool intrinsic(mg_dvm_thread *t, u8 id, const mg_dvm_reg *a, mg_jvalue *r)
{
    mg_jobj *s = (mg_jobj *)a[0];
    r->j = 0;
    switch (id) {
    case INTR_STRING_CHARAT:
        if ((u32)a[1] >= (u32)s->u.str.len16) return false;
        r->i = s->u.str.u16[(u32)a[1]];
        return true;
    case INTR_STRING_LENGTH: r->i = s->u.str.len16; return true;
    case INTR_STRING_ISEMPTY: r->i = s->u.str.len16 == 0; return true;
    case INTR_STRING_EQUALS: {
        mg_jobj *o = (mg_jobj *)a[1];
        if (o == s) r->i = 1;
        else if (!o || mg_dvm_class_of(o) != mg_dvm_class_of(s)) r->i = 0;
        else r->i = o->u.str.len16 == s->u.str.len16 && !memcmp(o->u.str.u16, s->u.str.u16, (size_t)s->u.str.len16 * 2);
        return true;
    }
    case INTR_STRING_HASHCODE: {
        mg_jvalue *slot = &mg_dvm_slots(s)[g_string_hash_slot];
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
    case INTR_ARRAYCOPY: return arraycopy_in_place(s, (i32)(u32)a[1], (mg_jobj *)a[2], (i32)(u32)a[3], (i32)(u32)a[4]);
    case INTR_MIN_I: r->i = (i32)(u32)a[0] < (i32)(u32)a[1] ? (i32)(u32)a[0] : (i32)(u32)a[1]; return true;
    case INTR_MAX_I: r->i = (i32)(u32)a[0] > (i32)(u32)a[1] ? (i32)(u32)a[0] : (i32)(u32)a[1]; return true;
    case INTR_ABS_I: r->i = (i32)(u32)a[0] < 0 ? (i32)(0u - (u32)a[0]) : (i32)(u32)a[0]; return true;
    case INTR_MIN_J: {
        i64 x = mg_dvm_get_wide(a, 0), y = mg_dvm_get_wide(a, 2);
        r->j = x < y ? x : y;
        return true;
    }
    case INTR_MAX_J: {
        i64 x = mg_dvm_get_wide(a, 0), y = mg_dvm_get_wide(a, 2);
        r->j = x > y ? x : y;
        return true;
    }
    case INTR_ABS_J: {
        i64 x = mg_dvm_get_wide(a, 0);
        r->j = x < 0 ? (i64)(0 - (u64)x) : x;
        return true;
    }
    case INTR_SQRT: r->j = (i64)mg_db(sqrt(mg_d((u64)mg_dvm_get_wide(a, 0)))); return true;
    case INTR_GETCLASS: {
        mg_jobj *mirror = mg_dvm_class_of(s)->mirror;
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
/* The return instruction that goes with each iget and sget kind: return, return-wide, return-object. */
static u16 return_for(u32 op)
{
    u32 k = op >= 0x60 ? op - 0x60 : op - 0x52;
    return k == 1 ? 0x10 : k == 2 ? 0x11 : 0x0f;
}

static MG_NOINLINE void classify(mg_dvm_method *m)
{
    const u16 *c = m->insns;
    bool is_static = (m->flags & DVM_ACC_STATIC) != 0;
    u32 shape = SHAPE_REGULAR, value = 0;
    u32 op = c ? c[0] & 0xFF : 0;
    mg_dvm_field *fld = NULL;
    if (!c || (m->flags & (DVM_ACC_SYNCHRONIZED | DVM_ACC_NATIVE)) || m->ntries) {
        mg_atomic_xchg((volatile i32 *)&m->shape, SHAPE_REGULAR);
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
        fld = mg_dvm_resolve_field(m->cls->dex, c[1], false);
        if (fld && fld->slot < fld->cls->nslots) shape = SHAPE_GETTER;
    } else if (m->ninsns == 3 && !is_static && m->ins == 2 && op >= 0x59 && op <= 0x5f && op != 0x5a && (u32)(c[0] >> 12) == (u32)m->regs - 2u
               && ((c[0] >> 8) & 0xFu) == (u32)m->regs - 1u && c[2] == 0x000e) {
        /* iput-kind value, this, field; return-void (not a long or a double, which come in two registers) */
        fld = mg_dvm_resolve_field(m->cls->dex, c[1], false);
        if (fld && fld->slot < fld->cls->nslots) shape = SHAPE_SETTER;
    } else if (m->ninsns == 3 && is_static && m->ins == 0 && op >= 0x60 && op <= 0x66 && (c[2] & 0xFF) == return_for(op) && (u32)(c[2] >> 8) == (u32)(c[0] >> 8)) {
        /* sget-kind vAA, field; return-kind vAA */
        fld = mg_dvm_resolve_field(m->cls->dex, c[1], true);
        if (fld && fld->cls->statics) shape = SHAPE_STATIC_GETTER;
    }
    /* A field that does not resolve is the method's to throw about when it runs. */
    if (!fld && shape == SHAPE_REGULAR) mg_dvm_clear_pending();
    /* (A regular method's word is SHAPE_REGULAR alone, which the call compares with first.) */
    if (shape == SHAPE_REGULAR) op = value = 0;
    /* The field first, then the word that says how to use it, with a barrier between (another thread reads them unlocked). */
    m->shape_field = fld;
    mg_atomic_xchg((volatile i32 *)&m->shape, (i32)(shape | op << 8 | value << 16));
}

/* A field slot's value as the iget or sget of that kind reads it, into a result. */
static inline void read_kind(u32 op, const mg_jvalue *s, mg_jvalue *r)
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
static bool trivial(const mg_dvm_method *m, u32 w, const mg_dvm_reg *a, mg_jvalue *r)
{
    mg_jobj *o = (mg_jobj *)a[0];
    const mg_dvm_field *fld = m->shape_field;
    mg_jvalue *s;
    switch (SHAPE_OF(w)) {
    case SHAPE_EMPTY: return true;
    case SHAPE_CONST:
        r->j = 0;
        r->i = SHAPE_VALUE(w);
        return true;
    case SHAPE_GETTER:
        /* (The field may not be seen yet on another processor than the one that wrote it: then the method runs.) */
        if (!fld || fld->slot >= mg_dvm_class_of(o)->nslots) return false;
        read_kind(SHAPE_OP(w), &mg_dvm_slots(o)[fld->slot], r);
        return true;
    case SHAPE_SETTER:
        if (!fld || fld->slot >= mg_dvm_class_of(o)->nslots) return false;
        s = &mg_dvm_slots(o)[fld->slot];
        switch (SHAPE_OP(w)) {
        case 0x59: s->i = (i32)(u32)a[1]; break;
        case 0x5b: s->l = (mg_jobj *)a[1]; break;
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
 * Each place an exception of a class is thrown at is said three times, then counted: the exceptions an app throws and catches
 * every time from one place used up the 2000 lines in Yandex Maps' first half minute, before what was being looked for
 * (package 507). 0 when the table has no room left near the place: said as before.
 */
typedef struct thrown_site {
    const mg_dvm_method *m;
    const mg_dvm_class *c;
    u32 pc, n;
} thrown_site;
static thrown_site g_thrown_sites[2048];
static mg_lock g_thrown_sites_lock = MG_LOCK_INIT;

static u32 thrown_here(const mg_dvm_method *m, u32 pc, const mg_dvm_class *c)
{
    const u32 mask = (u32)MG_COUNT(g_thrown_sites) - 1;
    u32 h = ((u32)((mg_addr)m >> 3) * 2654435761u ^ pc * 40503u ^ (u32)((mg_addr)c >> 4)) & mask, i, n = 0;
    mg_lock_acquire(&g_thrown_sites_lock);
    for (i = 0; i < 16; i++) {
        thrown_site *s = &g_thrown_sites[(h + i) & mask];
        if (!s->m) {
            s->m = m;
            s->c = c;
            s->pc = pc;
            n = s->n = 1;
            break;
        }
        if (s->m == m && s->pc == pc && s->c == c) {
            n = ++s->n;
            break;
        }
    }
    mg_lock_release(&g_thrown_sites_lock);
    return n;
}

/*
 * The flag trace-exceptions (a file of that name in LocalState\flags, read once): every exception thrown is said in the log
 * once, with its message and the method it was thrown in, caught or not -- what an app catches and says nothing about is
 * otherwise not seen at all (VK mp3 mod's sign-in stood still with nothing in the log). At most 2000 lines a run, and three
 * of each place (thrown_here).
 */
static void trace_exception(mg_dvm_thread *t, mg_dvm_method *m, u32 pc, mg_jobj *e)
{
    static int wanted = -1;
    static volatile i32 said;
    const char *text = NULL;
    u32 here;
    if (wanted < 0) wanted = mg_flag("trace-exceptions") ? 1 : 0;
    if (!wanted || e == t->traced) return;
    t->traced = e;
    here = thrown_here(m, pc, mg_dvm_class_of(e));
    if (here > 3) return;
    if (mg_atomic_add(&said, 1) > 2000) return;
    if (g_dvm.f_message && mg_dvm_is_instance(e, g_dvm.c_throwable)) {
        mg_jobj *msg = mg_dvm_slots(e)[g_dvm.f_message->slot].l;
        if (msg) text = mg_dvm_string_utf8(msg);
    }
    mg_info("dvm: thrown on thread %u: %s%s%s, in %s.%s at %u%s", (unsigned)t->id, mg_dvm_class_name(e), text ? ": " : "", text ? text : "", m->cls->name,
            m->name, (unsigned)pc, here == 3 ? " (the third time there: the next are not said)" : "");
}

/* The handler in method `m` for exception `e` thrown at `pc`, if it has one. */
static bool find_handler(mg_dvm_thread *t, mg_dvm_method *m, u32 pc, mg_jobj *e, u32 *target)
{
    const u8 *tries, *handlers, *end;
    mg_dvm_class *ec = mg_dvm_class_of(e);
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
        u32 start = mg_dvm_rd32(ti), count = mg_dvm_rd16(ti + 4), hoff = mg_dvm_rd16(ti + 6);
        i32 size;
        int n, k;
        if (pc < start || pc >= start + count) continue;
        h = handlers + hoff;
        if (h >= end) return false;
        size = mg_dvm_sleb(&h);
        n = size < 0 ? -size : size;
        for (k = 0; k < n && h < end; k++) {
            u32 type = mg_dvm_uleb(&h), addr = mg_dvm_uleb(&h);
            mg_dvm_class *tc;
            /* A catch of a class that cannot be found catches nothing; looking for it must not disturb what is being thrown. */
            t->pending = NULL;
            tc = mg_dvm_resolve_type(m->cls->dex, type);
            t->pending = NULL;
            if (tc && (tc == ec || mg_dvm_assignable(ec, tc))) {
                *target = addr;
                found = true;
                break;
            }
        }
        if (!found && size <= 0 && h < end) {
            *target = mg_dvm_uleb(&h);
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
static MG_NOINLINE bool slow_step(mg_dvm_thread *t, mg_dvm_frame *f, u32 pc)
{
    g_dvm.steps++;
    if (g_dvm.profiling) {
        mg_dvm_prof(f->m)->self++;
        t->prof_total++;
    }
    if (g_dvm.trace >= 3) mg_info("dvm: %*s%s.%s %u: %s", t->depth, "", f->m->cls->name, f->m->name, (unsigned)pc, mg_dvm_insn_name(f->m->insns[pc]));
    if (g_dvm.step_limit && t->steps_left != ~(u64)0) {
        if (t->steps_left == 0) {
            /* All ones marks the limit as hit: the error's own constructor is not counted, and nothing may catch it. */
            t->steps_left = ~(u64)0;
            f->pc = pc;
            return mg_dvm_throw("java/lang/InternalError", "the step limit is used up in %s.%s", f->m->cls->name, f->m->name);
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
static MG_NOINLINE mg_jvalue *slotless_field(mg_dvm_thread *t, mg_jobj *o, const mg_dvm_field *fld)
{
    /* One of the fields the runtime owns: what it stands for, in a place of the thread's own, where a write is lost. */
    t->scratch.j = 0;
    if (fld->slot == DVM_SLOT_KLASS) t->scratch.l = mg_dvm_mirror(mg_dvm_class_of(o));
    else if (fld->slot == DVM_SLOT_MONITOR) t->scratch.i = DVM_HASH_WORD(o);
    else if (fld->slot == DVM_SLOT_COUNT && o->kind == MG_J_STRING) t->scratch.i = DVM_STRING_COUNT(o->u.str.len16);
    else return NULL;
    return &t->scratch;
}

static inline mg_jvalue *field_slot(mg_dvm_thread *t, mg_jobj *o, const mg_dvm_field *fld)
{
    if (fld->slot >= mg_dvm_class_of(o)->nslots) return slotless_field(t, o, fld);
    return &mg_dvm_slots(o)[fld->slot];
}

/* The data of a switch or fill-array-data, checked to lie inside the method; NULL when it does not. */
static const u16 *payload_at(const mg_dvm_method *m, u32 pc, i32 off, u16 ident)
{
    i64 at = (i64)pc + off;
    if (at < 0 || at + 4 > (i64)m->ninsns || m->insns[at] != ident) return NULL;
    if (!mg_dvm_insn_units(m->insns, m->ninsns, (u32)at)) return NULL;
    return m->insns + at;
}

#define FETCH(n) (insns[pc + (n)])
#define A4 (((u32)w >> 8) & 0xF)
#define B4 ((u32)w >> 12)
#define AA ((u32)w >> 8)
#define RI(n) ((i32)(u32)regs[n])
#define RL(n) ((mg_jobj *)regs[n])
#define RJ(n) mg_dvm_get_wide(regs, (n))
#define RF(n) mg_f((u32)regs[n])
#define RD(n) mg_d((u64)mg_dvm_get_wide(regs, (n)))
#define WI(n, v) (regs[n] = (mg_dvm_reg)(u32)(v))
#define WL(n, o) (regs[n] = (mg_dvm_reg)(o))
#define WJ(n, v) mg_dvm_put_wide(regs, (n), (i64)(v))
#define WF(n, v) (regs[n] = (mg_dvm_reg)mg_fb(v))
#define WD(n, v) mg_dvm_put_wide(regs, (n), (i64)mg_db(v))
#define NEXT(n) { pc += (n); continue; }
/* A jump; one back (or to itself) is where the other loop may take over (SAFEPOINT, defined for each loop). */
#define JUMP(off)                                                                    \
    {                                                                                \
        u32 to_ = (u32)((i32)pc + (i32)(off));                                       \
        if ((i32)(off) <= 0) {                                                       \
            SAFEPOINT(to_);                                                          \
            JIT_BACK(to_);                                                           \
        }                                                                            \
        pc = to_;                                                                    \
        continue;                                                                    \
    }
/* Whatever may throw or call out records where it is first: a stack trace reads it, and so does the search for a handler. */
#define THROW(...) { f->pc = pc; mg_dvm_throw(__VA_ARGS__); goto exception; }
#define LOAD_FRAME() (m = f->m, insns = m->insns, regs = f->regs, dex = m->cls->dex)
#define RESOLVE_TYPE(var, idx)                                    \
    {                                                             \
        u32 i_ = (idx);                                           \
        var = i_ < dex->ntype ? dex->tcache[i_] : NULL;           \
        if (!var) {                                               \
            f->pc = pc;                                           \
            var = mg_dvm_resolve_type(dex, i_);                   \
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
        if ((size) ? (a->kind != MG_J_PRIM_ARRAY || a->u.arr.esz != (size)) : a->kind != MG_J_OBJ_ARRAY)                          \
            THROW("java/lang/InternalError", "%s on a %s", mg_dvm_insn_name(w), mg_dvm_class_of(a)->name);                        \
        if ((u32)i >= (u32)mg_dvm_array_length(a))                                                                               \
            THROW("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)mg_dvm_array_length(a), (int)i);        \
    }
#define IFIELD(dst_)                                                                                                            \
    {                                                                                                                            \
        u32 i_ = FETCH(1);                                                                                                       \
        mg_jobj *o_ = RL(B4);                                                                                                    \
        mg_dvm_field *fld_ = i_ < dex->nfield ? dex->fcache[i_] : NULL;                                                          \
        if (!fld_ || (fld_->flags & DVM_ACC_STATIC)) {                                                                           \
            f->pc = pc;                                                                                                          \
            fld_ = mg_dvm_resolve_field(dex, i_, false);                                                                         \
            if (!fld_) goto exception;                                                                                           \
        }                                                                                                                        \
        if (!o_) THROW("java/lang/NullPointerException", "Attempt to %s field '%s.%s' on a null object reference",                \
                       (w & 0xFF) >= 0x59 ? "write to" : "read from", fld_->cls->name, fld_->name);                               \
        dst_ = field_slot(t, o_, fld_);                                                                                             \
        if (!dst_) THROW("java/lang/IncompatibleClassChangeError", "a %s has no field %s.%s", mg_dvm_class_of(o_)->name, fld_->cls->name, fld_->name); \
    }
#define SFIELD(dst_)                                                                   \
    {                                                                                   \
        u32 i_ = FETCH(1);                                                              \
        mg_dvm_field *fld_ = i_ < dex->nfield ? dex->fcache[i_] : NULL;                 \
        if (!fld_ || !(fld_->flags & DVM_ACC_STATIC)) {                                 \
            f->pc = pc;                                                                 \
            fld_ = mg_dvm_resolve_field(dex, i_, true);                                 \
            if (!fld_) goto exception;                                                  \
        }                                                                               \
        if (fld_->cls->state != DVM_CS_INITIALIZED) {                                   \
            f->pc = pc;                                                                 \
            if (!mg_dvm_ensure_init(fld_->cls)) goto exception;                         \
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

/* The plain loop and the watched one (mg_dvm_interp_loop.h). Neither static: the benchmark's build places the plain one by name. */
#define DVM_LOOP mg_dvm_interpret
#define DVM_WATCHED 0
#define SAFEPOINT(at)                                                                \
    if (g_dvm.slow) {                                                                \
        f->pc = (at);                                                                \
        goto switch_loop;                                                            \
    }
/*
 * A jump back, or a call's start: the method's compiled code run from there when it has some there (mg_dvm_jit.c), until
 * it hands the frame back at the pc the loop goes on at, or returns. (Only in the plain loop: nothing compiled runs while
 * instructions are watched.)
 */
#define JIT_BACK(at)                                                                 \
    {                                                                                \
        u32 next_;                                                                   \
        int ran_ = mg_dvm_jit_back(t, f, m, regs, (at), &next_, &result);            \
        if (ran_) {                                                                  \
            if (ran_ == 3) goto do_return;                                           \
            pc = next_;                                                              \
            if (ran_ == 2) {                                                         \
                f->pc = pc;                                                          \
                goto exception;                                                      \
            }                                                                        \
            continue;                                                                \
        }                                                                            \
    }
#include "mg_dvm_interp_loop.h"
#undef DVM_LOOP
#undef DVM_WATCHED
#undef SAFEPOINT
#undef JIT_BACK
#define JIT_BACK(at)
#define DVM_LOOP mg_dvm_interpret_watched
#define DVM_WATCHED 1
#define SAFEPOINT(at)                                                                \
    if (!g_dvm.slow) {                                                               \
        f->pc = (at);                                                                \
        goto switch_loop;                                                            \
    }
#include "mg_dvm_interp_loop.h"
#undef DVM_LOOP
#undef DVM_WATCHED
#undef SAFEPOINT

/* Runs from frame `f` until the frame entered from C returns or throws, in whichever loop is wanted, crossing over when that changes. */
static bool interpret(mg_dvm_thread *t, mg_dvm_frame *f, mg_jvalue *ret)
{
    for (;;) {
        int r = g_dvm.slow ? mg_dvm_interpret_watched(t, f, ret) : mg_dvm_interpret(t, f, ret);
        if (r != DVM_SWITCH) return r == DVM_RETURNED;
        f = t->top;
    }
}

/*
 * The invoke at `pc` of frame f, done for compiled code (mg_dvm_jit.c, docs/JIT.md "calls"): the method found as do_invoke
 * finds it, then done as do_invoke does it -- an intrinsic or a trivial body in place, a native method called, bytecode run
 * to its return in a loop of its own on the host's stack (where do_invoke would have gone on in the same loop) -- with the
 * result in *res, for the move-result after it. 0 done; 1 thrown (pending, f->pc the invoke's); 2 the interpreter's to do,
 * nothing having happened yet: instructions watched, the host's stack too low for another loop (deep recursion through
 * compiled code goes on interpreted, without one), new String, a kind compiled code does not ask for.
 */
int mg_dvm_jit_invoke(mg_dvm_thread *t, mg_dvm_frame *f, u32 pc, mg_jvalue *res)
{
    mg_dvm_method *m = f->m, *ref, *target;
    mg_dvm_dex *dex = m->cls->dex;
    const u16 *insns = m->insns;
    mg_dvm_reg *regs = f->regs, five[5];
    const mg_dvm_reg *argv;
    u16 w = insns[pc];
    u32 op = w & 0xFF, idx = insns[pc + 1], count;
    int kind;
    mg_jobj *self = NULL;
    mg_dvm_frame *nf;
    bool ok;
    res->j = 0;
    if (g_dvm.slow || t->c_depth >= DVM_MAX_C_DEPTH - 8 || (t->c_stack_low && (mg_addr)&five < t->c_stack_low + 32u * 1024u)) return 2;
    if (op >= 0x6e && op <= 0x72) {
        u16 list = insns[pc + 2];
        u32 b = (u32)w >> 12;
        kind = (int)(op - 0x6e);
        count = b > 5 ? 5 : b;
        switch (count) {
        case 5: five[4] = regs[((u32)w >> 8) & 0xF]; /* fall through */
        case 4: five[3] = regs[list >> 12]; /* fall through */
        case 3: five[2] = regs[(list >> 8) & 0xF]; /* fall through */
        case 2: five[1] = regs[(list >> 4) & 0xF]; /* fall through */
        case 1: five[0] = regs[list & 0xF]; /* fall through */
        default: break;
        }
        argv = five;
    } else if (op >= 0x74 && op <= 0x78) {
        kind = (int)(op - 0x74);
        count = (u32)w >> 8;
        argv = regs + insns[pc + 2];
    } else {
        return 2;
    }
    f->pc = pc;
    ref = idx < dex->nmeth ? dex->mcache[idx] : NULL;
    if (!ref || ((ref->flags & DVM_ACC_STATIC) != 0) != (kind == DVM_INVOKE_STATIC)) {
        ref = mg_dvm_resolve_method(dex, idx, kind);
        if (!ref) return 1;
    }
    target = ref;
    if (kind != DVM_INVOKE_STATIC) {
        self = count ? (mg_jobj *)argv[0] : NULL;
        if (!self) {
            mg_dvm_throw("java/lang/NullPointerException", "Attempt to invoke %s method '%s.%s%s' on a null object reference",
                         kind == DVM_INVOKE_INTERFACE ? "interface" : kind == DVM_INVOKE_DIRECT ? "direct" : "virtual", ref->cls->name, ref->name, ref->sig);
            return 1;
        }
    }
    switch (kind) {
    case DVM_INVOKE_VIRTUAL: case DVM_INVOKE_INTERFACE: {
        mg_dvm_class *rc = mg_dvm_class_of(self);
        if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < 0) break;
        if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < rc->nvtab) {
            target = rc->vtab[ref->vidx];
        } else {
            u32 slot = (u32)(((mg_addr)ref->name >> 3) ^ ((mg_addr)ref->sig >> 5)) % DVM_ICACHE;
            target = rc->icache[slot];
            if (!target || target->name != ref->name || target->sig != ref->sig) {
                target = mg_dvm_find_virtual(rc, ref->name, ref->sig);
                if (!target) {
                    mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s does not implement %s.%s%s", rc->name, ref->cls->name, ref->name, ref->sig);
                    return 1;
                }
                rc->icache[slot] = target;
            }
        }
        break;
    }
    case DVM_INVOKE_SUPER: {
        mg_dvm_class *sc = m->cls->super;
        if (ref->cls->flags & DVM_ACC_INTERFACE) break;
        if (sc && ref->vidx >= 0 && ref->vidx < sc->nvtab) target = sc->vtab[ref->vidx];
        else target = sc ? mg_dvm_find_virtual(sc, ref->name, ref->sig) : NULL;
        if (!target) {
            mg_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s above %s", ref->cls->name, ref->name, ref->sig, m->cls->name);
            return 1;
        }
        break;
    }
    case DVM_INVOKE_STATIC:
        if (target->cls->state != DVM_CS_INITIALIZED && !mg_dvm_ensure_init(target->cls)) return 1;
        break;
    default:                                                     /* direct */
        if (self->kind == MG_J_STRING && ref->cls == g_dvm.c_string && ref->name[0] == '<') return 2;
        break;
    }
    if (target->stub) mg_dvm_stub_called(m, target);
    if (target->intrinsic && intrinsic(t, target->intrinsic, argv, res)) return 0;
    if (target->shape != SHAPE_REGULAR && target->insns) {
        u32 shape;
        if (target->shape == SHAPE_UNKNOWN) classify(target);
        shape = target->shape;
        if (SHAPE_OF(shape) != SHAPE_REGULAR && trivial(target, shape, argv, res)) return 0;
    }
    if (!target->insns) {
        if (!(target->flags & DVM_ACC_NATIVE)) {
            mg_dvm_throw("java/lang/AbstractMethodError", "%s.%s%s", target->cls->name, target->name, target->sig);
            return 1;
        }
        return invoke_native(t, target, self, argv, NULL, res) ? 0 : 1;
    }
    t->c_depth++;
    nf = push_frame(t, target, target->regs);
    if (!nf) {
        t->c_depth--;
        return 1;
    }
    nf->entry = true;
    {
        u32 base = (u32)target->regs - target->ins, n = count < target->ins ? count : target->ins, k;
        mg_dvm_reg *to = nf->regs;
        for (k = 0; k < base; k++) to[k] = 0;
        for (k = 0; k < n; k++) to[base + k] = argv[k];
        for (k = base + n; k < target->regs; k++) to[k] = 0;
    }
    if (target->flags & DVM_ACC_SYNCHRONIZED) {
        nf->sync = (target->flags & DVM_ACC_STATIC) ? mg_dvm_mirror(target->cls) : self;
        if (nf->sync) mg_dvm_monitor_enter(nf->sync);
    }
    if (g_call_trace) call_trace_enter(t, target, " (compiled)", argv, count);
    /*
     * Its own compiled code from its start, when it has some there (its calls counted toward compiling it): a return there
     * is the call's end, with no loop of the interpreter's. Not for a method with handlers, whose exceptions the
     * interpreter's loop decides about; one without simply passes them to its caller, as the loop would.
     */
    if (!target->ntries) {
        u32 next;
        int ran = mg_dvm_jit_back(t, nf, target, nf->regs, 0, &next, res);
        if (ran == 2 || ran == 3) {
            if (g_call_trace) {
                if (ran == 3) call_trace_return(t, target, res);
                else call_trace_unwind(t, target, t->pending);
            }
            if (nf->sync) mg_dvm_monitor_exit(nf->sync);
            pop_frame(t);
            t->c_depth--;
            return ran == 3 ? 0 : 1;
        }
        if (ran == 1) nf->pc = next;
    }
    ok = interpret(t, nf, res);
    t->c_depth--;
    return ok ? 0 : 1;
}

/* A type an instruction names, resolved as RESOLVE_TYPE does; NULL with an exception pending. */
static mg_dvm_class *jit_type(mg_dvm_dex *dex, u32 idx)
{
    mg_dvm_class *c = idx < dex->ntype ? dex->tcache[idx] : NULL;
    return c ? c : mg_dvm_resolve_type(dex, idx);
}

/*
 * For compiled code (mg_dvm_jit.c): one of the instructions that make objects, look at their classes or lock them --
 * const-class, monitor-enter and -exit, check-cast, instance-of, new-instance, new-array, aput-object -- done at `pc` of
 * frame f as the interpreter's loop does it, on the frame's registers. 0 done; 1 thrown (pending, f->pc this one's); 2
 * another instruction, the interpreter's.
 */
int mg_dvm_jit_op(mg_dvm_thread *t, mg_dvm_frame *f, u32 pc)
{
    mg_dvm_method *m = f->m;
    mg_dvm_dex *dex = m->cls->dex;
    const u16 *insns = m->insns;
    mg_dvm_reg *regs = f->regs;
    u16 w = insns[pc];
    u32 a4 = ((u32)w >> 8) & 0xF, b4 = (u32)w >> 12, aa = (u32)w >> 8;
    mg_dvm_class *c;
    mg_jobj *o;
    (void)t;
    f->pc = pc;
    switch (w & 0xFF) {
    case 0x1c:                                                   /* const-class */
        if (!(c = jit_type(dex, insns[pc + 1])) || !(o = mg_dvm_mirror(c))) return 1;
        regs[aa] = (mg_dvm_reg)o;
        return 0;
    case 0x1d:                                                   /* monitor-enter */
        if (!(o = (mg_jobj *)regs[aa])) {
            mg_dvm_throw("java/lang/NullPointerException", "monitor-enter on null");
            return 1;
        }
        mg_dvm_monitor_enter(o);
        return 0;
    case 0x1e:                                                   /* monitor-exit */
        if (!(o = (mg_jobj *)regs[aa])) {
            mg_dvm_throw("java/lang/NullPointerException", "monitor-exit on null");
            return 1;
        }
        if (!mg_dvm_monitor_exit(o)) {
            mg_dvm_throw("java/lang/IllegalMonitorStateException", "monitor-exit on an object this thread has not locked");
            return 1;
        }
        return 0;
    case 0x1f:                                                   /* check-cast */
        if (!(o = (mg_jobj *)regs[aa])) return 0;
        if (!(c = jit_type(dex, insns[pc + 1]))) return 1;
        if (mg_dvm_class_of(o) != c && !mg_dvm_assignable(mg_dvm_class_of(o), c)) {
            mg_dvm_throw("java/lang/ClassCastException", "%s cannot be cast to %s", mg_dvm_class_of(o)->name, c->name);
            return 1;
        }
        return 0;
    case 0x20:                                                   /* instance-of */
        if (!(o = (mg_jobj *)regs[b4])) {
            regs[a4] = 0;
            return 0;
        }
        if (!(c = jit_type(dex, insns[pc + 1]))) return 1;
        regs[a4] = (mg_dvm_reg)(u32)(mg_dvm_class_of(o) == c || mg_dvm_assignable(mg_dvm_class_of(o), c));
        return 0;
    case 0x22:                                                   /* new-instance */
        if (!(c = jit_type(dex, insns[pc + 1]))) return 1;
        if (c->state != DVM_CS_INITIALIZED && !mg_dvm_ensure_init(c)) return 1;
        if (!(o = mg_dvm_alloc_object(c))) return 1;
        regs[aa] = (mg_dvm_reg)o;
        return 0;
    case 0x23:                                                   /* new-array */
        if (!(c = jit_type(dex, insns[pc + 1]))) return 1;
        if (c->name[0] != '[') {
            mg_dvm_throw("java/lang/VerifyError", "new-array of %s", c->name);
            return 1;
        }
        if (!(o = mg_dvm_alloc_array(c, (i32)(u32)regs[b4]))) return 1;
        regs[a4] = (mg_dvm_reg)o;
        return 0;
    case 0x4d: {                                                 /* aput-object */
        u16 bc = insns[pc + 1];
        mg_jobj *a = (mg_jobj *)regs[bc & 0xFF], *v = (mg_jobj *)regs[aa];
        i32 i = (i32)(u32)regs[bc >> 8];
        if (!a) {
            mg_dvm_throw("java/lang/NullPointerException", "Attempt to write to a null array");
            return 1;
        }
        if (a->kind != MG_J_OBJ_ARRAY) {
            mg_dvm_throw("java/lang/InternalError", "%s on a %s", mg_dvm_insn_name(w), mg_dvm_class_of(a)->name);
            return 1;
        }
        if ((u32)i >= (u32)mg_dvm_array_length(a)) {
            mg_dvm_throw("java/lang/ArrayIndexOutOfBoundsException", "length=%d; index=%d", (int)mg_dvm_array_length(a), (int)i);
            return 1;
        }
        if (v && !mg_dvm_is_instance(v, mg_dvm_class_of(a)->component)) {
            mg_dvm_throw("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", mg_dvm_class_of(v)->name, mg_dvm_class_of(a)->name);
            return 1;
        }
        a->u.oarr.v[i] = v;
        return 0;
    }
    default:
        return 2;
    }
}

bool mg_dvm_invoke(mg_dvm_method *m, mg_jobj *self, const mg_jvalue *params, mg_jvalue *ret)
{
    mg_dvm_thread *t = mg_dvm_self();
    mg_dvm_frame *f;
    mg_jvalue local;
    bool ok = false;
    if ((mg_addr)&local < t->c_stack_min) t->c_stack_min = (mg_addr)&local;
    if (!ret) ret = &local;
    ret->j = 0;
    /*
     * This is real recursion on the host's stack, once for each time C stands between two pieces of bytecode. It is bounded by
     * count, and by what the host said is the low end of this thread's stack (a thread made with a small one).
     */
    if (t->overflowing ? t->c_depth >= DVM_MAX_C_DEPTH + 8 : (t->c_depth >= DVM_MAX_C_DEPTH || (mg_addr)&local < t->c_stack_low)) {
        /* While the error is being made, its own constructor gets a little further than anything else may go. */
        if (t->overflowing) return false;
        t->overflowing = true;
        mg_dvm_throw("java/lang/StackOverflowError", "%d calls from native code deep, calling %s.%s", t->c_depth, m->cls->name, m->name);
        t->overflowing = false;
        return false;
    }
    if (t->c_depth++ == 0) t->steps_left = g_dvm.step_limit;
    if (!m->insns) {
        if (m->flags & DVM_ACC_NATIVE) ok = invoke_native(t, m, self, NULL, params, ret);
        else mg_dvm_throw("java/lang/AbstractMethodError", "%s.%s%s", m->cls->name, m->name, m->sig);
    } else if ((f = push_frame(t, m, m->regs)) != NULL) {
        const char *s = m->shorty + 1;
        u32 r = (u32)m->regs - m->ins;
        int i;
        f->entry = true;
        memset(f->regs, 0, sizeof(mg_dvm_reg) * m->regs);
        if (!(m->flags & DVM_ACC_STATIC)) f->regs[r++] = (mg_dvm_reg)self;
        for (i = 0; *s; s++, i++) {
            switch (*s) {
            case 'J': case 'D':
                mg_dvm_put_wide(f->regs, r, params[i].j);
                r += 2;
                break;
            case 'L': f->regs[r++] = (mg_dvm_reg)params[i].l; break;
            case 'Z': f->regs[r++] = params[i].z ? 1 : 0; break;
            case 'B': f->regs[r++] = (u32)(i32)params[i].b; break;
            case 'C': f->regs[r++] = params[i].c; break;
            case 'S': f->regs[r++] = (u32)(i32)params[i].s; break;
            default: f->regs[r++] = (u32)params[i].i; break;
            }
        }
        if (m->flags & DVM_ACC_SYNCHRONIZED) {
            f->sync = (m->flags & DVM_ACC_STATIC) ? mg_dvm_mirror(m->cls) : self;
            if (f->sync) mg_dvm_monitor_enter(f->sync);
        }
        if (g_dvm.trace >= 2) mg_info("dvm: %*s-> %s.%s%s (from C)", t->depth, "", m->cls->name, m->name, m->sig);
        if (g_call_trace) call_trace_enter(t, m, " (from C)", NULL, 0);
        if (m->stub) mg_dvm_stub_called(NULL, m);
        ok = interpret(t, f, ret);
    }
    t->c_depth--;
    return ok;
}
