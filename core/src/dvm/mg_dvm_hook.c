/*
 * Methods an app hooks: Xposed's XposedBridge, done by the runtime itself.
 *
 * Aliucord (Discord patched by Aliucord Manager) carries Aliuhook (github.com/Aliucord/hook): Xposed's Java API --
 * XposedBridge.hookMethod, XC_MethodHook's callbacks before and after the method, invokeOriginalMethod -- over LSPlant,
 * which hooks a method on ART by rewriting ART's own structures (libart.so's symbols, the method's ArtMethod) and a class it
 * makes at run time. There is no ART here: the library's JNI_OnLoad, which starts LSPlant, returned JNI_ERR ("failed to
 * open /system/lib/libart.so"), XposedBridge's static initialiser, which loads it, threw, and the app ended with it. So
 * the runtime hooks methods itself, as the first Xposed did on Dalvik, and XposedBridge's natives are its own: registered
 * for the class by name, which wins over any library's function (mg_dvm_bind_native), the library being loaded without its
 * JNI_OnLoad (mg_dvm_jvm.c).
 *
 * The natives, static methods of de.robv.android.xposed.XposedBridge as Aliuhook declares them, and what its Java expects:
 *
 *   Method hook0(Object context, Member original, Method callback)
 *       From then on every call of `original` (a Method or a Constructor) is a call of context.callback(Object[] args),
 *       `callback` being that method (HookInfo's: public Object callback(Object[])): the call's arguments boxed, the
 *       object first for an instance method and nothing in its place for a static one. What it returns is the call's
 *       value -- unboxed for a primitive, nothing for void -- and what it throws the call throws. hook0 hands back the
 *       backup: a Method whose invoke runs the original body on the object it is given, past the hook -- a Method even for
 *       a constructor, whose body it runs on the object (Constructor.newInstance would make another). Null when the method
 *       cannot be hooked (abstract, hooked already): XposedBridge then throws "Failed to hook method".
 *   boolean unhook0(Member)                       the hook taken off; false when there was none
 *   boolean isHooked0(Member)
 *   boolean deoptimize0(Member)                   about ART's compiled code, which may have the method's callers inline it:
 *                                                 no hooked method stays done in place here (below), so true and nothing
 *   boolean makeClassInheritable0(Class)          "final" taken off the class, its constructors made protected
 *   Object allocateInstance0(Class)               JNI's AllocObject: an object, its constructor not run
 *   boolean invokeConstructor0(Object, Constructor, Object[])
 *                                                 the constructor run on an object there already, its arguments unboxed as
 *                                                 reflection unboxes them; what it throws, thrown
 *   boolean disableProfileSaver(), disableHiddenApiRestrictions()   nothing of the kind here: true
 *
 * How a call reaches the hook. A hooked method keeps its bytecode and is marked (mg_dvm_method.hooked), and every way into
 * a method looks at the mark before it does anything else -- the interpreter's invoke, compiled code's calls
 * (mg_dvm_jit_invoke; the quick way, mg_dvm_jit_fast, declines a hooked method), the calls from C (mg_dvm_invoke:
 * reflection, JNI, the runtime's own) -- and makes mg_dvm_hook_call. A static method's class is initialised first, as for
 * any call of it. The body is left as it was, so a frame already running it runs on. The backup is a copy of the method --
 * a struct of its own, private so that nothing dispatches a call of it again, in no class's tables, never hooked -- with
 * the same body. In compiled code a call done in place (a trivial body or an intrinsic: inline_call, mg_dvm_jit.c) looks at
 * the mark once an app has hooked anything, and the code compiled before that is forgotten then (mg_dvm_jit_hooks_on).
 *
 * What a hook keeps -- its context and the backup's Method -- is a root of the collector (mg_dvm_hook_roots), which reads
 * the table without the lock: each entry is published whole, and none is ever taken out.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hook {
    mg_dvm_method *m;                  /* the method hooked */
    mg_dvm_method *backup;             /* its copy, the original: made at its first hook and kept for good, as calls of it may run */
    mg_jobj *volatile context;         /* whose callback the calls are; NULL while the method is not hooked */
    mg_jobj *volatile backup_method;   /* the backup's Method, as hook0 handed it out */
    mg_dvm_method *callback;           /* Object callback(Object[]), of the context's class */
    mg_dvm_class *returns;             /* the class of what the method returns, where that is an object: the callback's value is checked against it */
} hook;

/* Every method ever hooked, by its address, open addressing: an entry stays, with its backup, when its method is unhooked. */
#define HOOK_SLOTS 8192u
#define HOOK_MOST (HOOK_SLOTS / 2)
static hook *volatile *volatile g_slots;
static u32 g_nhooks;
static volatile i32 g_said;
static mg_lock g_hook_lock = MG_LOCK_INIT;

static u32 slot_of(const mg_dvm_method *m) { return (u32)(((u64)(mg_addr)m >> 2) * 2654435761u) & (HOOK_SLOTS - 1); }

/* The method's entry, with g_hook_lock held; NULL when it was never hooked. */
static hook *entry_of(const mg_dvm_method *m)
{
    hook *volatile *slots = g_slots;
    u32 i = slot_of(m), n;
    if (!slots) return NULL;
    for (n = 0; n < HOOK_SLOTS; n++, i = (i + 1) & (HOOK_SLOTS - 1)) {
        hook *h = slots[i];
        if (!h || h->m == m) return h;
    }
    return NULL;
}

/* A new entry for the method, with g_hook_lock held; NULL when the table has no room. */
static hook *entry_made(mg_dvm_method *m, mg_dvm_method *backup)
{
    u32 i;
    hook *h;
    if (!g_slots) {
        hook *volatile *slots = (hook *volatile *)calloc(HOOK_SLOTS, sizeof(hook *));
        if (!slots) return NULL;
        g_slots = slots;
    }
    if (g_nhooks >= HOOK_MOST || !(h = (hook *)calloc(1, sizeof(*h)))) return NULL;
    h->m = m;
    h->backup = backup;
    for (i = slot_of(m); g_slots[i]; i = (i + 1) & (HOOK_SLOTS - 1)) {}
    /* (Whole before the collector can read it: an interlocked exchange is a barrier.) */
    mg_atomic_cas_ptr((void *volatile *)&g_slots[i], NULL, h);
    g_nhooks++;
    return h;
}

void mg_dvm_hook_roots(void (*fn)(mg_jobj *o, void *user), void *user)
{
    hook *volatile *slots = g_slots;
    u32 i;
    if (!slots) return;
    for (i = 0; i < HOOK_SLOTS; i++) {
        hook *h = slots[i];
        mg_jobj *o;
        if (!h) continue;
        if ((o = h->context) != NULL) fn(o, user);
        if ((o = h->backup_method) != NULL) fn(o, user);
    }
}

/*
 * The copy of a method that runs its original body: a struct of its own, as the method is, private (a call of it is never
 * dispatched again: mg_dvm_virtual_target), in no class's tables, not hooked, not compiled yet, no intrinsic (its body is
 * the method's own), and a native method's binding shared with the method (a RegisterNatives made after the hook reaches both).
 */
static mg_dvm_method *backup_of(mg_dvm_method *m)
{
    mg_dvm_method *b = (mg_dvm_method *)mg_dvm_meta_alloc_kind(sizeof(*b), DVM_META_METHOD);
    memcpy(b, m, sizeof(*b));
    if (!(m->flags & DVM_ACC_STATIC)) b->flags = (m->flags & ~(u32)(DVM_ACC_PUBLIC | DVM_ACC_PROTECTED)) | DVM_ACC_PRIVATE;
    b->vidx = -1;
    b->intrinsic = 0;
    b->stub = 0;
    b->hooked = 0;
    b->prof = NULL;
    b->jit_backs = 0;
    b->jit_state = 0;
    b->jit = NULL;
    b->nat = (m->flags & DVM_ACC_NATIVE) ? mg_dvm_native_of(m) : NULL;
    return b;
}

/* ------------------------------------------------------------------ the call */

/* A call's i-th argument as one value: from the registers an invoke has them in (`argv`, the next at *k), or from C's (`params`). */
static mg_jvalue argument(char kind, const mg_dvm_reg *argv, u32 *k, const mg_jvalue *params, u32 i)
{
    mg_jvalue v;
    v.j = 0;
    if (!argv) {
        if (params) v = params[i];
        return v;
    }
    switch (kind) {
    case 'J': case 'D':
        v.j = mg_dvm_get_wide(argv, *k);
        *k += 2;
        return v;
    case 'L': v.l = (mg_jobj *)argv[*k]; break;
    /* (The small types given whole, as marshal gives them: .z and .i both read right.) */
    case 'Z': v.i = (i32)(argv[*k] & 0xFF); break;
    case 'B': v.i = (i32)(i8)argv[*k]; break;
    case 'C': v.i = (i32)(u16)argv[*k]; break;
    case 'S': v.i = (i32)(i16)argv[*k]; break;
    default: v.i = (i32)(u32)argv[*k]; break;                   /* int, and a float's bits */
    }
    (*k)++;
    return v;
}

/* A call that found the method hooked, made after the hook was taken off: the method itself, as it is now. */
static bool unhooked(mg_dvm_method *m, mg_jobj *self, const mg_dvm_reg *argv, const mg_jvalue *params, mg_jvalue *result)
{
    mg_jvalue own[16], *p = own;
    const char *s;
    u32 k = (m->flags & DVM_ACC_STATIC) ? 0 : 1, i;
    bool ok;
    if (argv) {
        if (m->nparams > MG_COUNT(own)) p = (mg_jvalue *)mg_alloc(sizeof(*p) * m->nparams);
        for (s = m->shorty + 1, i = 0; *s; s++, i++) p[i] = argument(*s, argv, &k, NULL, i);
        params = p;
    }
    ok = mg_dvm_invoke(m, self, params, result);
    if (p != own) free(p);
    return ok;
}

/* What the callback returned, as the method's value: unboxed for a primitive (a box that widens to it, as reflection has it), checked for an object. */
static bool returned(const mg_dvm_method *m, mg_dvm_class *returns, mg_jobj *o, mg_jvalue *result)
{
    char kind = m->shorty[0];
    mg_jvalue v;
    result->j = 0;
    if (kind == 'V') return true;
    if (kind == 'L') {
        if (o && returns && !mg_dvm_is_instance(o, returns))
            return mg_dvm_throw("java/lang/ClassCastException", "%s cannot be cast to %s (returned by the hook of %s.%s)", mg_dvm_class_of(o)->name, returns->name,
                                m->cls->name, m->name);
        result->l = o;
        return true;
    }
    if (!o) return mg_dvm_throw("java/lang/NullPointerException", "the hook of %s.%s%s returned null, and the method returns a primitive", m->cls->name, m->name, m->sig);
    if (!mg_dvm_reflect_unbox(o, kind, &v))
        return mg_dvm_throw("java/lang/ClassCastException", "%s cannot be unboxed to what %s.%s%s returns (returned by its hook)", mg_dvm_class_of(o)->name, m->cls->name,
                            m->name, m->sig);
    /* As the interpreter keeps a value: the small types widened to an int, nothing above a 32-bit value. */
    switch (kind) {
    case 'Z': result->i = v.z ? 1 : 0; break;
    case 'B': result->i = v.b; break;
    case 'C': result->i = v.c; break;
    case 'S': result->i = v.s; break;
    case 'J': case 'D': result->j = v.j; break;
    default: result->i = v.i; break;                            /* int, and a float's bits */
    }
    return true;
}

bool mg_dvm_hook_call(mg_dvm_thread *t, mg_dvm_method *m, mg_jobj *self, const mg_dvm_reg *argv, const mg_jvalue *params, mg_jvalue *result)
{
    const bool is_static = (m->flags & DVM_ACC_STATIC) != 0;
    const char *s;
    mg_jobj *context = NULL, *args;
    mg_dvm_method *callback = NULL;
    mg_dvm_class *returns = NULL;
    mg_jvalue v, r;
    hook *h;
    u32 at = 0, k = is_static ? 0 : 1, i;
    (void)t;
    result->j = 0;
    mg_lock_acquire(&g_hook_lock);
    h = entry_of(m);
    if (h && m->hooked) {
        context = h->context;
        callback = h->callback;
        returns = h->returns;
    }
    mg_lock_release(&g_hook_lock);
    if (!context || !callback) return unhooked(m, self, argv, params, result);
    /* The arguments boxed into an Object[], the object first: args[0] is `this` for an instance method, as LSPlant has it. */
    args = mg_dvm_new_array("[Ljava/lang/Object;", (i32)m->nparams + (is_static ? 0 : 1));
    if (!args) return false;
    if (!is_static) args->u.oarr.v[at++] = self;
    for (s = m->shorty + 1, i = 0; *s; s++, i++) {
        mg_jobj *boxed;
        v = argument(*s, argv, &k, params, i);
        if (*s == 'L') boxed = v.l;
        else if (!(boxed = mg_dvm_reflect_box(*s, v))) return false;
        args->u.oarr.v[at++] = boxed;
    }
    /* The callback, as the context's class has it; what it throws is the call's. */
    v.j = 0;
    v.l = args;
    r.j = 0;
    if (!mg_dvm_invoke(mg_dvm_virtual_target(callback, context), context, &v, &r)) return false;
    return returned(m, returns, r.l, result);
}

/* ---------------------------------------------------------------- the natives */

#define N(name) static void name(mg_jcall *c)

static void refuse(const mg_dvm_method *m, const char *why)
{
    if (m) mg_warn("dvm: XposedBridge.hook0: %s.%s%s is not hooked: %s", m->cls->name, m->name, m->sig, why);
    else mg_warn("dvm: XposedBridge.hook0: nothing hooked: %s", why);
}

N(bridge_hook0)
{
    mg_jobj *context = c->args[0].l, *made;
    mg_dvm_method *m = mg_dvm_reflected_method(c->args[1].l), *cb = mg_dvm_reflected_method(c->args[2].l), *backup = NULL;
    mg_dvm_class *returns = NULL;
    const char *why = NULL, *ret;
    hook *h;
    i32 n;
    c->ret.l = NULL;
    if (!m || !cb || !context) why = "the method, the callback or the context is missing";
    else if (!m->insns && !(m->flags & DVM_ACC_NATIVE)) why = "it is abstract";
    else if (m->cls == g_dvm.c_string && m->name[0] == '<') why = "a String's constructor is never called as such (new String is made whole)";
    else if ((cb->flags & DVM_ACC_STATIC) || strcmp(cb->sig, "([Ljava/lang/Object;)Ljava/lang/Object;") != 0 || !mg_dvm_is_instance(context, cb->cls))
        why = "the callback is not Object callback(Object[]) of the context";
    else if (m->hooked) why = "it is hooked already";
    if (why) {
        refuse(m, why);
        return;
    }
    /* What the method returns, where it is an object: what the callback's value is checked against (unknown: not checked). */
    ret = strchr(m->sig, ')');
    if (ret && (ret[1] == 'L' || ret[1] == '[')) {
        returns = mg_dvm_find_desc_scope(ret + 1, m->cls->scope);
        if (!returns) mg_dvm_clear_pending();
    }
    /* Compiled code looks at hooks from now on (the first hook). */
    mg_dvm_jit_hooks_on();
    mg_lock_acquire(&g_hook_lock);
    h = entry_of(m);
    if (h) backup = h->backup;
    mg_lock_release(&g_hook_lock);
    if (!backup) backup = backup_of(m);
    /* (Made before the lock is taken: it allocates, which may collect.) */
    made = mg_dvm_reflect_backup(backup);
    if (!made) return;
    mg_lock_acquire(&g_hook_lock);
    h = entry_of(m);
    if (!h) h = entry_made(m, backup);
    if (h && !m->hooked) {
        h->context = context;
        h->backup_method = made;
        h->callback = cb;
        h->returns = returns;
        /* (Set last: a call that finds it takes the lock for the rest.) */
        m->hooked = 1;
        c->ret.l = made;
    }
    mg_lock_release(&g_hook_lock);
    if (!c->ret.l) {
        refuse(m, h ? "it is hooked already" : "too many methods are hooked");
        return;
    }
    n = mg_atomic_add(&g_said, 1);
    if (n == 1)
        mg_info("dvm: the app hooks methods through Xposed's XposedBridge: the runtime hooks them itself (mg_dvm_hook.c), each call going to the app's "
                "callback, and its backup running the original");
    if (n <= 64) mg_info("dvm: XposedBridge hooked %s.%s%s", m->cls->name, m->name, m->sig);
    else if (n == 65) mg_info("dvm: XposedBridge hooks more methods; they are not said one by one");
}

N(bridge_unhook0)
{
    mg_dvm_method *m = mg_dvm_reflected_method(c->args[0].l);
    hook *h;
    if (!m) return;
    mg_lock_acquire(&g_hook_lock);
    h = entry_of(m);
    if (h && m->hooked) {
        m->hooked = 0;
        h->context = NULL;
        h->backup_method = NULL;
        h->callback = NULL;
        c->ret.z = 1;
    }
    mg_lock_release(&g_hook_lock);
}

N(bridge_isHooked0)
{
    mg_dvm_method *m = mg_dvm_reflected_method(c->args[0].l);
    c->ret.z = m && m->hooked;
}

/* Nothing compiled here keeps a hooked method done in place (mg_dvm_jit.c, inline_call): there is nothing to undo. */
N(bridge_deoptimize0) { c->ret.z = mg_dvm_reflected_method(c->args[0].l) != NULL; }

N(bridge_makeClassInheritable0)
{
    mg_jobj *mirror = c->args[0].l;
    mg_dvm_class *k = mirror ? mg_dvm_mirror_class(mirror) : NULL;
    mg_jvalue v;
    u32 forgotten;
    int i;
    if (!k || k->prim || k->name[0] == '[' || k->foreign || (k->flags & DVM_ACC_INTERFACE)) return;
    k->flags &= ~(u32)DVM_ACC_FINAL;
    for (i = 0; i < k->ndirect; i++) {
        mg_dvm_method *m = &k->direct[i];
        if (strcmp(m->name, "<init>") != 0) continue;
        if (!(m->flags & DVM_ACC_PUBLIC)) m->flags = (m->flags & ~(u32)DVM_ACC_PRIVATE) | DVM_ACC_PROTECTED;
        m->flags &= ~(u32)DVM_ACC_FINAL;
    }
    /* What Class.getModifiers reads. */
    v.j = 0;
    if (mg_dvm_get_field(mirror, "accessFlags", &v)) {
        v.i &= ~DVM_ACC_FINAL;
        mg_dvm_set_field(mirror, "accessFlags", v);
    }
    /* A call compiled as of a method nothing could override (of a final class) may have an override now. */
    forgotten = mg_dvm_jit_forget_all();
    mg_info("dvm: XposedBridge made %s inheritable (%u methods' compiled code forgotten)", k->name, (unsigned)forgotten);
    c->ret.z = 1;
}

N(bridge_allocateInstance0)
{
    mg_dvm_class *k = c->args[0].l ? mg_dvm_mirror_class(c->args[0].l) : NULL;
    if (!k) {
        mg_dvm_throw("java/lang/NullPointerException", "allocateInstance of no class");
        return;
    }
    if (mg_dvm_ensure_init(k)) c->ret.l = mg_dvm_alloc_object(k);
}

N(bridge_invokeConstructor0)
{
    mg_jobj *self = c->args[0].l;
    mg_dvm_method *m = mg_dvm_reflected_method(c->args[1].l);
    mg_jvalue params[64];
    if (!self || !m) {
        mg_dvm_throw("java/lang/NullPointerException", "invokeConstructor with no %s", self ? "constructor" : "object");
        return;
    }
    if (strcmp(m->name, "<init>") != 0 || (m->flags & DVM_ACC_STATIC)) {
        mg_dvm_throw("java/lang/IllegalArgumentException", "%s.%s%s is not a constructor", m->cls->name, m->name, m->sig);
        return;
    }
    if (!mg_dvm_is_instance(self, m->cls)) {
        mg_dvm_throw("java/lang/IllegalArgumentException", "Expected receiver of type %s, but got %s", m->cls->name, mg_dvm_class_of(self)->name);
        return;
    }
    if (!mg_dvm_reflect_args(m, c->args[2].l, params, MG_COUNT(params))) return;
    /* (A hooked constructor's hook sees it, as a JNI call of it does on ART.) */
    c->ret.z = mg_dvm_invoke(m, self, params, NULL);
}

N(bridge_true) { c->ret.z = 1; }

#define XB "de/robv/android/xposed/XposedBridge"
static const mg_jhle k_bridge[] = {
    { XB, "hook0", "(Ljava/lang/Object;Ljava/lang/reflect/Member;Ljava/lang/reflect/Method;)Ljava/lang/reflect/Method;", bridge_hook0 },
    { XB, "unhook0", "(Ljava/lang/reflect/Member;)Z", bridge_unhook0 },
    { XB, "isHooked0", "(Ljava/lang/reflect/Member;)Z", bridge_isHooked0 },
    { XB, "deoptimize0", "(Ljava/lang/reflect/Member;)Z", bridge_deoptimize0 },
    { XB, "makeClassInheritable0", "(Ljava/lang/Class;)Z", bridge_makeClassInheritable0 },
    { XB, "allocateInstance0", "(Ljava/lang/Class;)Ljava/lang/Object;", bridge_allocateInstance0 },
    { XB, "invokeConstructor0", "(Ljava/lang/Object;Ljava/lang/reflect/Constructor;[Ljava/lang/Object;)Z", bridge_invokeConstructor0 },
    { XB, "disableProfileSaver", "()Z", bridge_true },
    { XB, "disableHiddenApiRestrictions", "()Z", bridge_true },
    { NULL, NULL, NULL, NULL },
};

void mg_dvm_hook_start(void) { mg_dvm_register(k_bridge); }
