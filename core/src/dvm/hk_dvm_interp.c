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

/* ---------------------------------------------------------------- exceptions */

/* The handler in method `m` for exception `e` thrown at `pc`, if it has one. */
static bool find_handler(hk_dvm_thread *t, hk_dvm_method *m, u32 pc, hk_jobj *e, u32 *target)
{
    const u8 *tries, *handlers, *end;
    hk_dvm_class *ec = hk_dvm_class_of(e);
    u32 i;
    bool found = false;
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
static bool slow_step(hk_dvm_thread *t, hk_dvm_frame *f, u32 pc)
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
#define JUMP(off) { pc = (u32)((i32)pc + (i32)(off)); continue; }
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

static bool interpret(hk_dvm_thread *t, hk_dvm_frame *f, hk_jvalue *ret)
{
    hk_dvm_method *m = f->m;
    const u16 *insns = m->insns;
    hk_dvm_reg *regs = f->regs;
    hk_dvm_dex *dex = m->cls->dex;
    u32 pc = 0;
    hk_jvalue result;
    hk_jobj *caught = NULL;
    /* What an invoke instruction decoded, for the code they all share. */
    int inv_kind = 0;
    u32 inv_idx = 0, inv_count = 0;
    const hk_dvm_reg *inv_argv = NULL;
    hk_dvm_reg inv_five[5];
    u16 w;
    /* Instructions executed, given to the thread at each native call and at the way out (hk_dvm_thread_costs). */
    u32 executed = 0;
    result.j = 0;

    for (;;) {
        w = insns[pc];
        executed++;
        if (g_dvm.slow && !slow_step(t, f, pc)) goto exception;
        switch (w & 0xFF) {
        case 0x00:                                               /* nop */
            if (w == 0x0100 || w == 0x0200 || w == 0x0300) {
                /* Data, which no instruction should fall into; stepped over whole rather than run. */
                u32 n = hk_dvm_insn_units(insns, m->ninsns, pc);
                if (!n) THROW("java/lang/VerifyError", "damaged data in %s.%s at %u", m->cls->name, m->name, (unsigned)pc);
                NEXT(n);
            }
            NEXT(1);

        /* moves: a whole register either way, so the plain and the object forms are one */
        case 0x01: case 0x07: regs[A4] = regs[B4]; NEXT(1);
        case 0x02: case 0x08: regs[AA] = regs[FETCH(1)]; NEXT(2);
        case 0x03: case 0x09: regs[FETCH(1)] = regs[FETCH(2)]; NEXT(3);
        case 0x04: WJ(A4, RJ(B4)); NEXT(1);
        case 0x05: WJ(AA, RJ(FETCH(1))); NEXT(2);
        case 0x06: WJ(FETCH(1), RJ(FETCH(2))); NEXT(3);
        case 0x0a: WI(AA, result.i); NEXT(1);
        case 0x0b: WJ(AA, result.j); NEXT(1);
        case 0x0c: WL(AA, result.l); NEXT(1);
        case 0x0d:
            WL(AA, caught);
            caught = NULL;
            NEXT(1);

        case 0x0e:
            result.j = 0;
            goto do_return;
        case 0x0f:
            result.j = 0;
            result.i = RI(AA);
            goto do_return;
        case 0x10:
            result.j = RJ(AA);
            goto do_return;
        case 0x11: {
            hk_jobj *o = RL(AA);
            result.j = 0;
            result.l = o;
            goto do_return;
        }

        case 0x12: WI(A4, (i32)(B4 ^ 8) - 8); NEXT(1);           /* const/4: the nibble sign-extended */
        case 0x13: WI(AA, (i32)(i16)FETCH(1)); NEXT(2);
        case 0x14: WI(AA, FETCH(1) | (u32)FETCH(2) << 16); NEXT(3);
        case 0x15: WI(AA, (u32)FETCH(1) << 16); NEXT(2);
        case 0x16: WJ(AA, (i64)(i16)FETCH(1)); NEXT(2);
        case 0x17: WJ(AA, (i64)(i32)(FETCH(1) | (u32)FETCH(2) << 16)); NEXT(3);
        case 0x18: WJ(AA, (u64)FETCH(1) | (u64)FETCH(2) << 16 | (u64)FETCH(3) << 32 | (u64)FETCH(4) << 48); NEXT(5);
        case 0x19: WJ(AA, (u64)FETCH(1) << 48); NEXT(2);
        case 0x1a: case 0x1b: {                                  /* const-string, const-string/jumbo */
            bool jumbo = (w & 0xFF) == 0x1b;
            u32 idx = jumbo ? (FETCH(1) | (u32)FETCH(2) << 16) : FETCH(1);
            hk_jobj *s = idx < dex->nstr ? dex->scache[idx] : NULL;
            if (!s) {
                f->pc = pc;
                s = hk_dvm_const_string(dex, idx);
                if (!s) goto exception;
            }
            WL(AA, s);
            NEXT(jumbo ? 3 : 2);
        }
        case 0x1c: {                                             /* const-class */
            hk_dvm_class *c;
            hk_jobj *mirror;
            RESOLVE_TYPE(c, FETCH(1));
            f->pc = pc;
            mirror = hk_dvm_mirror(c);
            if (!mirror) goto exception;
            WL(AA, mirror);
            NEXT(2);
        }
        case 0x1d: {                                             /* monitor-enter */
            hk_jobj *o = RL(AA);
            if (!o) THROW("java/lang/NullPointerException", "monitor-enter on null");
            hk_dvm_monitor_enter(o);
            NEXT(1);
        }
        case 0x1e: {                                             /* monitor-exit */
            hk_jobj *o = RL(AA);
            if (!o) THROW("java/lang/NullPointerException", "monitor-exit on null");
            if (!hk_dvm_monitor_exit(o)) THROW("java/lang/IllegalMonitorStateException", "monitor-exit on an object this thread has not locked");
            NEXT(1);
        }
        case 0x1f: {                                             /* check-cast: null is of every class, and its class need not exist */
            hk_jobj *o = RL(AA);
            if (o) {
                hk_dvm_class *c;
                RESOLVE_TYPE(c, FETCH(1));
                if (hk_dvm_class_of(o) != c && !hk_dvm_assignable(hk_dvm_class_of(o), c))
                    THROW("java/lang/ClassCastException", "%s cannot be cast to %s", hk_dvm_class_of(o)->name, c->name);
            }
            NEXT(2);
        }
        case 0x20: {                                             /* instance-of */
            hk_jobj *o = RL(B4);
            if (o) {
                hk_dvm_class *c;
                RESOLVE_TYPE(c, FETCH(1));
                WI(A4, hk_dvm_class_of(o) == c || hk_dvm_assignable(hk_dvm_class_of(o), c));
            } else {
                WI(A4, 0);
            }
            NEXT(2);
        }
        case 0x21: {                                             /* array-length */
            hk_jobj *a = RL(B4);
            if (!a) THROW("java/lang/NullPointerException", "Attempt to get length of null array");
            WI(A4, hk_dvm_array_length(a));
            NEXT(1);
        }
        case 0x22: {                                             /* new-instance */
            hk_dvm_class *c;
            hk_jobj *o;
            RESOLVE_TYPE(c, FETCH(1));
            f->pc = pc;
            if (c->state != DVM_CS_INITIALIZED && !hk_dvm_ensure_init(c)) goto exception;
            o = hk_dvm_alloc_object(c);
            if (!o) goto exception;
            WL(AA, o);
            NEXT(2);
        }
        case 0x23: {                                             /* new-array */
            hk_dvm_class *c;
            hk_jobj *a;
            RESOLVE_TYPE(c, FETCH(1));
            if (c->name[0] != '[') THROW("java/lang/VerifyError", "new-array of %s", c->name);
            f->pc = pc;
            a = hk_dvm_alloc_array(c, RI(B4));
            if (!a) goto exception;
            WL(A4, a);
            NEXT(2);
        }
        case 0x24: case 0x25: {                                  /* filled-new-array(/range) */
            bool range = (w & 0xFF) == 0x25;
            u32 count = range ? AA : B4, i;
            u16 list = FETCH(2);
            hk_dvm_class *c;
            hk_jobj *a;
            RESOLVE_TYPE(c, FETCH(1));
            /* Arrays of int and of references only, which is all Android's own runtime fills this way. */
            if (c->name[0] != '[' || (c->elem != 'I' && c->elem != 'L')) THROW("java/lang/InternalError", "filled-new-array of %s", c->name);
            f->pc = pc;
            a = hk_dvm_alloc_array(c, (i32)count);
            if (!a) goto exception;
            for (i = 0; i < count; i++) {
                u32 r = range ? (u32)list + i : (i < 4 ? ((u32)list >> (4 * i)) & 0xF : A4);
                if (c->elem == 'L') a->u.oarr.v[i] = RL(r);
                else ((i32 *)a->u.arr.data)[i] = RI(r);
            }
            result.j = 0;
            result.l = a;
            NEXT(3);
        }
        case 0x26: {                                             /* fill-array-data */
            hk_jobj *a = RL(AA);
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0300);
            u32 width, n;
            if (!a) THROW("java/lang/NullPointerException", "fill-array-data on a null array");
            if (!p) THROW("java/lang/VerifyError", "fill-array-data without its data in %s.%s", m->cls->name, m->name);
            width = p[1];
            n = p[2] | (u32)p[3] << 16;
            if (a->kind != HK_J_PRIM_ARRAY || a->u.arr.esz != width)
                THROW("java/lang/InternalError", "fill-array-data of width %u on a %s", (unsigned)width, hk_dvm_class_of(a)->name);
            if (n > a->u.arr.len)
                THROW("java/lang/ArrayIndexOutOfBoundsException", "fill-array-data: %u elements into an array of %u", (unsigned)n, (unsigned)a->u.arr.len);
            memcpy(a->u.arr.data, p + 4, (size_t)width * n);
            NEXT(3);
        }
        case 0x27: {                                             /* throw */
            hk_jobj *e = RL(AA);
            f->pc = pc;
            if (!e) hk_dvm_throw("java/lang/NullPointerException", "throw with null exception");
            else t->pending = e;
            /* With the trace on, every exception bytecode throws is a line, with where: one that is caught and dropped is otherwise never seen. */
            if (e && g_dvm.trace >= 1) hk_info("dvm: throw %s in %s.%s", hk_dvm_class_of(e)->name, f->m->cls->name, f->m->name);
            goto exception;
        }
        case 0x28: JUMP((i8)(w >> 8));
        case 0x29: JUMP((i16)FETCH(1));
        case 0x2a: JUMP((i32)(FETCH(1) | (u32)FETCH(2) << 16));
        case 0x2b: {                                             /* packed-switch: consecutive keys from the first */
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0100);
            i64 i;
            if (!p) THROW("java/lang/VerifyError", "packed-switch without its table in %s.%s", m->cls->name, m->name);
            i = (i64)RI(AA) - (i32)(p[2] | (u32)p[3] << 16);
            if (i >= 0 && i < p[1]) {
                const u16 *target = p + 4 + 2 * i;
                JUMP((i32)(target[0] | (u32)target[1] << 16));
            }
            NEXT(3);
        }
        case 0x2c: {                                             /* sparse-switch: sorted keys, then their targets */
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0200);
            i32 v = RI(AA), lo = 0, hi;
            if (!p) THROW("java/lang/VerifyError", "sparse-switch without its table in %s.%s", m->cls->name, m->name);
            hi = (i32)p[1] - 1;
            while (lo <= hi) {
                i32 mid = (lo + hi) / 2;
                i32 key = (i32)(p[2 + 2 * mid] | (u32)p[3 + 2 * mid] << 16);
                if (key < v) lo = mid + 1;
                else if (key > v) hi = mid - 1;
                else {
                    const u16 *target = p + 2 + 2 * (i32)p[1] + 2 * mid;
                    lo = (i32)(target[0] | (u32)target[1] << 16);
                    hi = -2;
                    break;
                }
            }
            if (hi == -2) JUMP(lo);
            NEXT(3);
        }
        case 0x2d: case 0x2e: {                                  /* cmpl-float, cmpg-float: what a NaN compares as is the difference */
            float a = RF(FETCH(1) & 0xFF), b = RF(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : a == b ? 0 : ((w & 0xFF) == 0x2d ? -1 : 1));
            NEXT(2);
        }
        case 0x2f: case 0x30: {
            double a = RD(FETCH(1) & 0xFF), b = RD(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : a == b ? 0 : ((w & 0xFF) == 0x2f ? -1 : 1));
            NEXT(2);
        }
        case 0x31: {
            i64 a = RJ(FETCH(1) & 0xFF), b = RJ(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : 0);
            NEXT(2);
        }
        /* Ints are kept zero-extended and references whole, so equality is the whole register either way. */
        IF2(0x32, regs[A4] == regs[B4])
        IF2(0x33, regs[A4] != regs[B4])
        IF2(0x34, RI(A4) < RI(B4))
        IF2(0x35, RI(A4) >= RI(B4))
        IF2(0x36, RI(A4) > RI(B4))
        IF2(0x37, RI(A4) <= RI(B4))
        IF2(0x38, regs[AA] == 0)
        IF2(0x39, regs[AA] != 0)
        IF2(0x3a, RI(AA) < 0)
        IF2(0x3b, RI(AA) >= 0)
        IF2(0x3c, RI(AA) > 0)
        IF2(0x3d, RI(AA) <= 0)

        /* aget, aput */
        case 0x44: { hk_jobj *a; i32 i; ARRAY(a, i, 4); WI(AA, ((i32 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x45: { hk_jobj *a; i32 i; ARRAY(a, i, 8); WJ(AA, ((i64 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x46: { hk_jobj *a; i32 i; ARRAY(a, i, 0); WL(AA, a->u.oarr.v[i]); NEXT(2); }
        case 0x47: { hk_jobj *a; i32 i; ARRAY(a, i, 1); WI(AA, ((u8 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x48: { hk_jobj *a; i32 i; ARRAY(a, i, 1); WI(AA, (i32)((i8 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x49: { hk_jobj *a; i32 i; ARRAY(a, i, 2); WI(AA, ((u16 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x4a: { hk_jobj *a; i32 i; ARRAY(a, i, 2); WI(AA, (i32)((i16 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x4b: { hk_jobj *a; i32 i; ARRAY(a, i, 4); ((i32 *)a->u.arr.data)[i] = RI(AA); NEXT(2); }
        case 0x4c: { hk_jobj *a; i32 i; ARRAY(a, i, 8); ((i64 *)a->u.arr.data)[i] = RJ(AA); NEXT(2); }
        case 0x4d: {
            hk_jobj *a, *v = RL(AA);
            i32 i;
            ARRAY(a, i, 0);
            if (v && !hk_dvm_is_instance(v, hk_dvm_class_of(a)->component))
                THROW("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", hk_dvm_class_of(v)->name, hk_dvm_class_of(a)->name);
            a->u.oarr.v[i] = v;
            NEXT(2);
        }
        case 0x4e: { hk_jobj *a; i32 i; ARRAY(a, i, 1); ((u8 *)a->u.arr.data)[i] = (u8)regs[AA]; NEXT(2); }
        case 0x4f: { hk_jobj *a; i32 i; ARRAY(a, i, 1); ((i8 *)a->u.arr.data)[i] = (i8)regs[AA]; NEXT(2); }
        case 0x50: { hk_jobj *a; i32 i; ARRAY(a, i, 2); ((u16 *)a->u.arr.data)[i] = (u16)regs[AA]; NEXT(2); }
        case 0x51: { hk_jobj *a; i32 i; ARRAY(a, i, 2); ((i16 *)a->u.arr.data)[i] = (i16)regs[AA]; NEXT(2); }

        /* iget, iput */
        case 0x52: { hk_jvalue *s; IFIELD(s); WI(A4, s->i); NEXT(2); }
        case 0x53: { hk_jvalue *s; IFIELD(s); WJ(A4, s->j); NEXT(2); }
        case 0x54: { hk_jvalue *s; IFIELD(s); WL(A4, s->l); NEXT(2); }
        case 0x55: { hk_jvalue *s; IFIELD(s); WI(A4, s->z); NEXT(2); }
        case 0x56: { hk_jvalue *s; IFIELD(s); WI(A4, (i32)s->b); NEXT(2); }
        case 0x57: { hk_jvalue *s; IFIELD(s); WI(A4, s->c); NEXT(2); }
        case 0x58: { hk_jvalue *s; IFIELD(s); WI(A4, (i32)s->s); NEXT(2); }
        case 0x59: { hk_jvalue *s; IFIELD(s); s->i = RI(A4); NEXT(2); }
        case 0x5a: { hk_jvalue *s; IFIELD(s); s->j = RJ(A4); NEXT(2); }
        case 0x5b: { hk_jvalue *s; IFIELD(s); s->l = RL(A4); NEXT(2); }
        case 0x5c: { hk_jvalue *s; IFIELD(s); s->z = (u8)regs[A4]; NEXT(2); }
        case 0x5d: { hk_jvalue *s; IFIELD(s); s->b = (i8)regs[A4]; NEXT(2); }
        case 0x5e: { hk_jvalue *s; IFIELD(s); s->c = (u16)regs[A4]; NEXT(2); }
        case 0x5f: { hk_jvalue *s; IFIELD(s); s->s = (i16)regs[A4]; NEXT(2); }

        /* sget, sput */
        case 0x60: { hk_jvalue *s; SFIELD(s); WI(AA, s->i); NEXT(2); }
        case 0x61: { hk_jvalue *s; SFIELD(s); WJ(AA, s->j); NEXT(2); }
        case 0x62: { hk_jvalue *s; SFIELD(s); WL(AA, s->l); NEXT(2); }
        case 0x63: { hk_jvalue *s; SFIELD(s); WI(AA, s->z); NEXT(2); }
        case 0x64: { hk_jvalue *s; SFIELD(s); WI(AA, (i32)s->b); NEXT(2); }
        case 0x65: { hk_jvalue *s; SFIELD(s); WI(AA, s->c); NEXT(2); }
        case 0x66: { hk_jvalue *s; SFIELD(s); WI(AA, (i32)s->s); NEXT(2); }
        case 0x67: { hk_jvalue *s; SFIELD(s); s->i = RI(AA); NEXT(2); }
        case 0x68: { hk_jvalue *s; SFIELD(s); s->j = RJ(AA); NEXT(2); }
        case 0x69: { hk_jvalue *s; SFIELD(s); s->l = RL(AA); NEXT(2); }
        case 0x6a: { hk_jvalue *s; SFIELD(s); s->z = (u8)regs[AA]; NEXT(2); }
        case 0x6b: { hk_jvalue *s; SFIELD(s); s->b = (i8)regs[AA]; NEXT(2); }
        case 0x6c: { hk_jvalue *s; SFIELD(s); s->c = (u16)regs[AA]; NEXT(2); }
        case 0x6d: { hk_jvalue *s; SFIELD(s); s->s = (i16)regs[AA]; NEXT(2); }

        case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72: {  /* invoke-kind {vC, vD, vE, vF, vG} */
            u16 list = FETCH(2);
            inv_kind = (w & 0xFF) - 0x6e;
            inv_idx = FETCH(1);
            inv_count = B4 > 5 ? 5 : B4;
            /* Only the registers the count names: the nibbles beyond it may name registers the method does not have. */
            switch (inv_count) {
            case 5: inv_five[4] = regs[A4]; /* fall through */
            case 4: inv_five[3] = regs[list >> 12]; /* fall through */
            case 3: inv_five[2] = regs[(list >> 8) & 0xF]; /* fall through */
            case 2: inv_five[1] = regs[(list >> 4) & 0xF]; /* fall through */
            case 1: inv_five[0] = regs[list & 0xF]; /* fall through */
            default: break;
            }
            inv_argv = inv_five;
            goto do_invoke;
        }
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:    /* invoke-kind/range {vCCCC .. vNNNN} */
            inv_kind = (w & 0xFF) - 0x74;
            inv_idx = FETCH(1);
            inv_count = AA;
            inv_argv = regs + FETCH(2);
            goto do_invoke;

        UNARY(0x7b, WI, 0u - (u32)RI(B4))
        UNARY(0x7c, WI, ~RI(B4))
        UNARY(0x7d, WJ, (u64)0 - (u64)RJ(B4))
        UNARY(0x7e, WJ, ~RJ(B4))
        UNARY(0x7f, WF, -RF(B4))
        UNARY(0x80, WD, -RD(B4))
        UNARY(0x81, WJ, (i64)RI(B4))
        UNARY(0x82, WF, (float)RI(B4))
        UNARY(0x83, WD, (double)RI(B4))
        UNARY(0x84, WI, (u32)(u64)RJ(B4))
        UNARY(0x85, WF, (float)RJ(B4))
        UNARY(0x86, WD, (double)RJ(B4))
        UNARY(0x87, WI, float_to_int(RF(B4)))
        UNARY(0x88, WJ, double_to_long((double)RF(B4)))
        UNARY(0x89, WD, (double)RF(B4))
        UNARY(0x8a, WI, double_to_int(RD(B4)))
        UNARY(0x8b, WJ, double_to_long(RD(B4)))
        UNARY(0x8c, WF, (float)RD(B4))
        UNARY(0x8d, WI, (i32)(i8)RI(B4))
        UNARY(0x8e, WI, (u16)RI(B4))
        UNARY(0x8f, WI, (i32)(i16)RI(B4))

        BIN(0x90, i32, RI, WI, (u32)a + (u32)b)
        BIN(0x91, i32, RI, WI, (u32)a - (u32)b)
        BIN(0x92, i32, RI, WI, (u32)a * (u32)b)
        DIVIDE(0x93, i32, RI, WI, div_int(a, b))
        DIVIDE(0x94, i32, RI, WI, rem_int(a, b))
        BIN(0x95, i32, RI, WI, a & b)
        BIN(0x96, i32, RI, WI, a | b)
        BIN(0x97, i32, RI, WI, a ^ b)
        BIN(0x98, i32, RI, WI, (u32)a << (b & 31))
        BIN(0x99, i32, RI, WI, shr_int(a, (u32)b & 31))
        BIN(0x9a, i32, RI, WI, (u32)a >> (b & 31))
        BIN(0x9b, i64, RJ, WJ, (u64)a + (u64)b)
        BIN(0x9c, i64, RJ, WJ, (u64)a - (u64)b)
        BIN(0x9d, i64, RJ, WJ, (u64)a * (u64)b)
        DIVIDE(0x9e, i64, RJ, WJ, div_long(a, b))
        DIVIDE(0x9f, i64, RJ, WJ, rem_long(a, b))
        BIN(0xa0, i64, RJ, WJ, a & b)
        BIN(0xa1, i64, RJ, WJ, a | b)
        BIN(0xa2, i64, RJ, WJ, a ^ b)
        SHIFT_LONG(0xa3, (u64)a << n)
        SHIFT_LONG(0xa4, shr_long(a, n))
        SHIFT_LONG(0xa5, (u64)a >> n)
        BIN(0xa6, float, RF, WF, a + b)
        BIN(0xa7, float, RF, WF, a - b)
        BIN(0xa8, float, RF, WF, a * b)
        BIN(0xa9, float, RF, WF, a / b)
        BIN(0xaa, float, RF, WF, fmodf(a, b))
        BIN(0xab, double, RD, WD, a + b)
        BIN(0xac, double, RD, WD, a - b)
        BIN(0xad, double, RD, WD, a * b)
        BIN(0xae, double, RD, WD, a / b)
        BIN(0xaf, double, RD, WD, fmod(a, b))

        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7:     /* int with a 16-bit literal */
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:     /* int with an 8-bit literal */
        case 0xe0: case 0xe1: case 0xe2: {
            u32 op = w & 0xFF, dst, src;
            i32 lit, a, v = 0;
            if (op <= 0xd7) {
                dst = A4;
                src = B4;
                lit = (i16)FETCH(1);
                op -= 0xd0;
            } else {
                dst = AA;
                src = FETCH(1) & 0xFF;
                lit = (i8)(FETCH(1) >> 8);
                op -= 0xd8;
            }
            a = RI(src);
            switch (op) {
            case 0: v = (i32)((u32)a + (u32)lit); break;
            case 1: v = (i32)((u32)lit - (u32)a); break;         /* rsub: the literal minus the register */
            case 2: v = (i32)((u32)a * (u32)lit); break;
            case 3: case 4:
                if (!lit) THROW("java/lang/ArithmeticException", "divide by zero");
                v = op == 3 ? div_int(a, lit) : rem_int(a, lit);
                break;
            case 5: v = a & lit; break;
            case 6: v = a | lit; break;
            case 7: v = a ^ lit; break;
            case 8: v = (i32)((u32)a << (lit & 31)); break;
            case 9: v = shr_int(a, (u32)lit & 31); break;
            default: v = (i32)((u32)a >> (lit & 31)); break;
            }
            WI(dst, v);
            NEXT(2);
        }

        case 0xfa: case 0xfb:                                    /* invoke-polymorphic(/range) */
            hk_note_once("dvm: invoke-polymorphic is not implemented (met in %s.%s)", m->cls->name, m->name);
            THROW("java/lang/UnsupportedOperationException", "invoke-polymorphic (a MethodHandle or VarHandle call) is not implemented, in %s.%s", m->cls->name, m->name);
        case 0xfc: case 0xfd:                                    /* invoke-custom(/range) */
            hk_note_once("dvm: invoke-custom is not implemented (met in %s.%s)", m->cls->name, m->name);
            THROW("java/lang/UnsupportedOperationException", "invoke-custom (a call site bound at run time) is not implemented, in %s.%s", m->cls->name, m->name);
        case 0xfe: case 0xff:                                    /* const-method-handle, const-method-type */
            THROW("java/lang/UnsupportedOperationException", "%s is not implemented, in %s.%s", hk_dvm_insn_name(w), m->cls->name, m->name);
        default:
            THROW("java/lang/VerifyError", "unused opcode %#x in %s.%s at %u", (unsigned)(w & 0xFF), m->cls->name, m->name, (unsigned)pc);
        }

    do_invoke: {
        hk_dvm_method *ref, *target;
        hk_jobj *self = NULL;
        hk_dvm_frame *nf;
        u32 base, n;
        f->pc = pc;
        ref = inv_idx < dex->nmeth ? dex->mcache[inv_idx] : NULL;
        if (!ref || ((ref->flags & DVM_ACC_STATIC) != 0) != (inv_kind == DVM_INVOKE_STATIC)) {
            ref = hk_dvm_resolve_method(dex, inv_idx, inv_kind);
            if (!ref) goto exception;
        }
        target = ref;
        if (inv_kind != DVM_INVOKE_STATIC) {
            self = inv_count ? (hk_jobj *)inv_argv[0] : NULL;
            if (!self) {
                hk_dvm_throw("java/lang/NullPointerException", "Attempt to invoke %s method '%s.%s%s' on a null object reference",
                             inv_kind == DVM_INVOKE_INTERFACE ? "interface" : inv_kind == DVM_INVOKE_DIRECT ? "direct" : "virtual", ref->cls->name, ref->name,
                             ref->sig);
                goto exception;
            }
        }
        switch (inv_kind) {
        case DVM_INVOKE_VIRTUAL: case DVM_INVOKE_INTERFACE: {
            hk_dvm_class *rc = hk_dvm_class_of(self);
            if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < 0) break;      /* a private or final method named as if virtual: itself */
            if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < rc->nvtab) {
                target = rc->vtab[ref->vidx];
            } else {
                /* Through an interface, or a method an abstract class only inherits from one: by name on the object's class, remembered there. */
                u32 slot = (u32)(((hk_addr)ref->name >> 3) ^ ((hk_addr)ref->sig >> 5)) % DVM_ICACHE;
                target = rc->icache[slot];
                if (!target || target->name != ref->name || target->sig != ref->sig) {
                    target = hk_dvm_find_virtual(rc, ref->name, ref->sig);
                    if (!target) {
                        hk_dvm_throw("java/lang/IncompatibleClassChangeError", "%s does not implement %s.%s%s", rc->name, ref->cls->name, ref->name, ref->sig);
                        goto exception;
                    }
                    rc->icache[slot] = target;
                }
            }
            break;
        }
        case DVM_INVOKE_SUPER: {
            hk_dvm_class *sc = m->cls->super;
            if (ref->cls->flags & DVM_ACC_INTERFACE) break;      /* Interface.super.method(): that interface's own */
            if (sc && ref->vidx >= 0 && ref->vidx < sc->nvtab) target = sc->vtab[ref->vidx];
            else target = sc ? hk_dvm_find_virtual(sc, ref->name, ref->sig) : NULL;
            if (!target) {
                hk_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s above %s", ref->cls->name, ref->name, ref->sig, m->cls->name);
                goto exception;
            }
            break;
        }
        case DVM_INVOKE_STATIC:
            if (target->cls->state != DVM_CS_INITIALIZED && !hk_dvm_ensure_init(target->cls)) goto exception;
            break;
        default:                                                 /* direct */
            if (self->kind == HK_J_STRING && ref->cls == g_dvm.c_string && ref->name[0] == '<') {
                /*
                 * new String(...): a string's length is fixed when it is made, so the object new-instance made cannot become
                 * the result. The constructor's result replaces it in every register that holds it, as it does in ART.
                 */
                hk_jvalue args[8];
                hk_jobj *s;
                u32 i;
                if (ref->nparams > HK_COUNT(args)) {
                    hk_dvm_throw("java/lang/InternalError", "String%s", ref->sig);
                    goto exception;
                }
                marshal(ref, inv_argv, args);
                if (!hk_dvm_string_init(ref->sig, args, &s)) goto exception;
                for (i = 0; i < m->regs; i++)
                    if (regs[i] == (hk_dvm_reg)self) regs[i] = (hk_dvm_reg)s;
                result.j = 0;
                NEXT(3);
            }
            break;
        }
        if (!target->insns) {
            if (!(target->flags & DVM_ACC_NATIVE)) {
                hk_dvm_throw("java/lang/AbstractMethodError", "%s.%s%s", target->cls->name, target->name, target->sig);
                goto exception;
            }
            t->executed += executed;
            executed = 0;
            if (!invoke_native(t, target, self, inv_argv, NULL, &result)) goto exception;
            NEXT(3);
        }
        if (g_dvm.trace >= 2) hk_info("dvm: %*s-> %s.%s%s", t->depth, "", target->cls->name, target->name, target->sig);
        nf = push_frame(t, target, target->regs);
        if (!nf) goto exception;
        /*
         * The arguments are the callee's last registers, in the order and the widths the caller's hold them; the others
         * start at zero. (Loops, not memset and memcpy: a call has a few registers, and the C library's calls cost more.)
         */
        base = (u32)target->regs - target->ins;
        n = inv_count < target->ins ? inv_count : target->ins;
        {
            hk_dvm_reg *to = nf->regs;
            u32 k;
            for (k = 0; k < base; k++) to[k] = 0;
            for (k = 0; k < n; k++) to[base + k] = inv_argv[k];
            for (k = base + n; k < target->regs; k++) to[k] = 0;
        }
        if (target->flags & DVM_ACC_SYNCHRONIZED) {
            nf->sync = (target->flags & DVM_ACC_STATIC) ? hk_dvm_mirror(target->cls) : self;
            if (nf->sync) hk_dvm_monitor_enter(nf->sync);
        }
        f = nf;
        LOAD_FRAME();
        pc = 0;
        continue;
    }

    do_return:
        if (f->sync) hk_dvm_monitor_exit(f->sync);
        if (f->entry) {
            *ret = result;
            pop_frame(t);
            t->executed += executed;
            return true;
        }
        pop_frame(t);
        f = t->top;
        LOAD_FRAME();
        pc = f->pc + 3;                                          /* past the invoke, which is three code units in every form */
        continue;

    exception: {
        hk_jobj *e = t->pending;
        u32 target_pc = 0;
        if (!e) {
            hk_dvm_throw("java/lang/InternalError", "an exception with nothing pending, in %s.%s", m->cls->name, m->name);
            e = t->pending;
        }
        for (;;) {
            if (find_handler(t, m, pc, e, &target_pc)) {
                t->pending = NULL;
                caught = e;
                pc = target_pc;
                break;
            }
            if (f->sync) hk_dvm_monitor_exit(f->sync);
            if (f->entry) {
                pop_frame(t);
                t->executed += executed;
                return false;
            }
            pop_frame(t);
            f = t->top;
            LOAD_FRAME();
            pc = f->pc;
        }
        continue;
    }
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
        ok = interpret(t, f, ret);
    }
    t->c_depth--;
    return ok;
}
