/*
 * java.lang.reflect on libcore: Field, Method and Constructor objects, what
 * java.lang.Class says of its members, and calls and field accesses made
 * through them. See hk_dvm_art.c; this is its second half, written against
 * the same sources (art/runtime/native/java_lang_Class.cc,
 * java_lang_reflect_*.cc).
 *
 * A Field object's `offset` is the field's place in an object, counted in
 * bytes from the object itself: what sun.misc.Unsafe is given, and all that
 * java.util.concurrent wants of reflection. A Method's or Constructor's
 * `artMethod` is the runtime's own method.
 *
 * Not done: access checks (everything is accessible), annotations (there are
 * none to be found: the natives that look for them find nothing), generic
 * signatures, parameter names.
 */
#include "hk_dvm_internal.h"

#include <stdio.h>

#define N(name) static void name(hk_jcall *c)

static void put_int(hk_jobj *o, const char *name, i32 v)
{
    hk_jvalue x;
    x.j = 0;
    x.i = v;
    hk_dvm_set_field(o, name, x);
}

static void put_obj(hk_jobj *o, const char *name, hk_jobj *v)
{
    hk_jvalue x;
    x.j = 0;
    x.l = v;
    hk_dvm_set_field(o, name, x);
}

/* A class's descriptor: "I", "[J", "Ljava/lang/String;". */
static void descriptor_of(const hk_dvm_class *k, char *out, size_t n)
{
    if (k->prim) snprintf(out, n, "%c", k->prim);
    else if (k->name[0] == '[') snprintf(out, n, "%s", k->name);
    else snprintf(out, n, "L%s;", k->name);
}

/* The next type of a signature's parameters, copied out; NULL at the closing parenthesis. */
static const char *next_param(const char *p, char *out, size_t n)
{
    const char *start = p;
    size_t len;
    if (*p == ')' || !*p) return NULL;
    while (*p == '[') p++;
    if (*p == 'L') p = strchr(p, ';');
    if (!p) return NULL;
    p++;
    len = (size_t)(p - start);
    if (len >= n) return NULL;
    memcpy(out, start, len);
    out[len] = 0;
    return p;
}

/* ------------------------------------------------------------------- boxes */

static hk_jobj *box(char kind, hk_jvalue v)
{
    static const struct { char kind; const char *cls, *sig; } boxes[] = {
        { 'Z', "java/lang/Boolean", "(Z)Ljava/lang/Boolean;" }, { 'B', "java/lang/Byte", "(B)Ljava/lang/Byte;" },
        { 'C', "java/lang/Character", "(C)Ljava/lang/Character;" }, { 'S', "java/lang/Short", "(S)Ljava/lang/Short;" },
        { 'I', "java/lang/Integer", "(I)Ljava/lang/Integer;" }, { 'J', "java/lang/Long", "(J)Ljava/lang/Long;" },
        { 'F', "java/lang/Float", "(F)Ljava/lang/Float;" }, { 'D', "java/lang/Double", "(D)Ljava/lang/Double;" },
    };
    hk_jvalue r;
    size_t i;
    if (kind == 'L' || kind == '[') return v.l;
    for (i = 0; i < HK_COUNT(boxes); i++)
        if (boxes[i].kind == kind) return hk_dvm_call_static(boxes[i].cls, "valueOf", boxes[i].sig, &v, &r) ? r.l : NULL;
    return NULL;                                                 /* void */
}

/* What a box holds, as its own kind: 0 when the object is no box. */
static char unbox(hk_jobj *o, hk_jvalue *out)
{
    static const struct { const char *cls; char kind; } boxes[] = { { "java/lang/Boolean", 'Z' }, { "java/lang/Byte", 'B' }, { "java/lang/Character", 'C' },
        { "java/lang/Short", 'S' }, { "java/lang/Integer", 'I' }, { "java/lang/Long", 'J' }, { "java/lang/Float", 'F' }, { "java/lang/Double", 'D' } };
    const char *name;
    size_t i;
    if (!o) return 0;
    name = hk_dvm_class_of(o)->name;
    for (i = 0; i < HK_COUNT(boxes); i++)
        if (!strcmp(boxes[i].cls, name)) {
            out->j = 0;
            hk_dvm_get_field(o, "value", out);
            return boxes[i].kind;
        }
    return 0;
}

/* A primitive value of kind `from` as one of kind `to`, where Java widens the one to the other without being asked. */
static bool widen(char from, hk_jvalue v, char to, hk_jvalue *out)
{
    static const char order[] = "BSIJFD";                      /* each widens to those after it; char joins at int */
    const char *a = strchr(order, from == 'C' ? 'I' : from), *b = strchr(order, to);
    double d;
    i64 n;
    out->j = 0;
    if (from == to) {
        *out = v;
        return true;
    }
    if (from == 'Z' || to == 'Z' || to == 'C' || !a || !b || a > b || (from == 'C' && to == 'S')) return false;
    n = from == 'B' ? v.b : from == 'S' ? v.s : from == 'C' ? v.c : from == 'I' ? v.i : v.j;
    d = from == 'F' ? (double)v.f : from == 'D' ? v.d : (double)n;
    switch (to) {
    case 'S': out->s = (i16)n; break;
    case 'I': out->i = (i32)n; break;
    case 'J': out->j = n; break;
    case 'F': out->f = (float)d; break;
    default: out->d = d; break;
    }
    return true;
}

/* ------------------------------------------------------------------ fields */

static hk_jobj *make_field(hk_dvm_field *f)
{
    hk_dvm_class *fc = hk_dvm_find_class("java/lang/reflect/Field"), *type = hk_dvm_find_class_desc(f->type);
    hk_jobj *o = fc && type ? hk_dvm_alloc_object(fc) : NULL;
    if (!o) return NULL;
    o->native = f;
    put_int(o, "accessFlags", (i32)(f->flags & 0xFFFF));
    put_obj(o, "declaringClass", hk_dvm_mirror(f->cls));
    put_int(o, "dexFieldIndex", (i32)f->idx);
    put_int(o, "offset", (f->flags & DVM_ACC_STATIC) || f->slot >= f->cls->nslots ? 0 : (i32)(HK_DVM_HEADER + f->slot * sizeof(hk_jvalue)));
    put_obj(o, "type", hk_dvm_mirror(type));
    return o;
}

static hk_dvm_field *field_in(hk_dvm_class *k, const char *name)
{
    const char *interned = hk_dvm_intern(name);
    int i;
    for (i = 0; i < k->nifields; i++)
        if (k->ifields[i].name == interned) return &k->ifields[i];
    for (i = 0; i < k->nsfields; i++)
        if (k->sfields[i].name == interned) return &k->sfields[i];
    return NULL;
}

N(Class_getDeclaredField)
{
    const char *name = hk_dvm_string_utf8(c->args[0].l);
    hk_dvm_field *f = name ? field_in(hk_dvm_mirror_class(c->self), name) : NULL;
    if (!name) hk_dvm_throw("java/lang/NullPointerException", "name == null");
    else if (!f) hk_dvm_throw("java/lang/NoSuchFieldException", "No field %s in class L%s;", name, hk_dvm_mirror_class(c->self)->name);
    else c->ret.l = make_field(f);
}

static hk_jobj *fields_of(hk_dvm_class *k, bool public_only)
{
    hk_jobj *a;
    int n = 0, i, at = 0, group;
    for (i = 0; i < k->nifields; i++) n += !public_only || (k->ifields[i].flags & DVM_ACC_PUBLIC);
    for (i = 0; i < k->nsfields; i++) n += !public_only || (k->sfields[i].flags & DVM_ACC_PUBLIC);
    a = hk_dvm_new_array("[Ljava/lang/reflect/Field;", n);
    for (group = 0; a && group < 2; group++) {
        hk_dvm_field *list = group ? k->sfields : k->ifields;
        int count = group ? k->nsfields : k->nifields;
        for (i = 0; i < count; i++) {
            if (public_only && !(list[i].flags & DVM_ACC_PUBLIC)) continue;
            a->u.oarr.v[at] = make_field(&list[i]);
            if (!a->u.oarr.v[at++]) return NULL;
        }
    }
    return a;
}

N(Class_getDeclaredFields) { c->ret.l = fields_of(hk_dvm_mirror_class(c->self), false); }
N(Class_getDeclaredFieldsUnchecked) { c->ret.l = fields_of(hk_dvm_mirror_class(c->self), c->args[0].z != 0); }
N(Class_getPublicDeclaredFields) { c->ret.l = fields_of(hk_dvm_mirror_class(c->self), true); }

static hk_dvm_field *public_field(hk_dvm_class *k, const char *name)
{
    hk_dvm_field *f;
    int i;
    for (; k; k = k->super) {
        f = field_in(k, name);
        if (f && (f->flags & DVM_ACC_PUBLIC)) return f;
        for (i = 0; i < k->nifaces; i++) {
            f = public_field(k->ifaces[i], name);
            if (f) return f;
        }
    }
    return NULL;
}

N(Class_getPublicFieldRecursive)
{
    const char *name = hk_dvm_string_utf8(c->args[0].l);
    hk_dvm_field *f = name ? public_field(hk_dvm_mirror_class(c->self), name) : NULL;
    if (f) c->ret.l = make_field(f);
}

static hk_dvm_field *field_from(hk_jobj *o) { return (hk_dvm_field *)o->native; }

N(Field_getNameInternal) { c->ret.l = hk_dvm_intern_string(hk_dvm_new_string(field_from(c->self)->name)); }
N(Field_getArtField) { c->ret.j = (i64)(hk_addr)field_from(c->self); }

/* Where the field's value is, for this object (which a static field ignores); NULL with an exception pending. */
static hk_jvalue *field_place(hk_dvm_field *f, hk_jobj *o)
{
    if (f->flags & DVM_ACC_STATIC) return hk_dvm_ensure_init(f->cls) ? &f->cls->statics[f->slot] : NULL;
    if (!o) {
        hk_dvm_throw("java/lang/NullPointerException", "null receiver");
        return NULL;
    }
    if (!hk_dvm_is_instance(o, f->cls) || f->slot >= hk_dvm_class_of(o)->nslots) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "Expected receiver of type %s, but got %s", f->cls->name, hk_dvm_class_of(o)->name);
        return NULL;
    }
    return &hk_dvm_slots(o)[f->slot];
}

/* Field.get and the typed getters: `want` is the kind asked for, 'L' for get(), which boxes whatever is there. */
static void field_get(hk_jcall *c, char want)
{
    hk_dvm_field *f = field_from(c->self);
    hk_jvalue *place = field_place(f, c->args[0].l), v;
    char kind = f->type[0] == '[' ? 'L' : f->type[0];
    if (!place) return;
    v = *place;
    if (want == 'L') c->ret.l = box(kind, v);
    else if (kind == 'L' || !widen(kind, v, want, &c->ret)) hk_dvm_throw("java/lang/IllegalArgumentException", "Not a primitive field of a type that widens to %c: %s", want, f->name);
}

static void field_set(hk_jcall *c, char given)
{
    hk_dvm_field *f = field_from(c->self);
    hk_jvalue *place = field_place(f, c->args[0].l), v = c->args[1], out;
    char kind = f->type[0] == '[' ? 'L' : f->type[0];
    if (!place) return;
    if (given == 'L' && kind == 'L') {
        hk_dvm_class *type = hk_dvm_find_class_desc(f->type);
        if (!type) return;
        if (v.l && !hk_dvm_is_instance(v.l, type)) {
            hk_dvm_throw("java/lang/IllegalArgumentException", "field %s.%s has type %s, got %s", f->cls->name, f->name, f->type, hk_dvm_class_of(v.l)->name);
            return;
        }
        place->l = v.l;
        return;
    }
    if (given == 'L') given = unbox(v.l, &v);                    /* set(Object, Object) on a primitive field: the box's value */
    if (!given || kind == 'L' || !widen(given, v, kind, &out)) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "field %s.%s has type %s", f->cls->name, f->name, f->type);
        return;
    }
    *place = out;
}

N(Field_get) { field_get(c, 'L'); }
N(Field_getBoolean) { field_get(c, 'Z'); }
N(Field_getByte) { field_get(c, 'B'); }
N(Field_getChar) { field_get(c, 'C'); }
N(Field_getShort) { field_get(c, 'S'); }
N(Field_getInt) { field_get(c, 'I'); }
N(Field_getLong) { field_get(c, 'J'); }
N(Field_getFloat) { field_get(c, 'F'); }
N(Field_getDouble) { field_get(c, 'D'); }
N(Field_set) { field_set(c, 'L'); }
N(Field_setBoolean) { field_set(c, 'Z'); }
N(Field_setByte) { field_set(c, 'B'); }
N(Field_setChar) { field_set(c, 'C'); }
N(Field_setShort) { field_set(c, 'S'); }
N(Field_setInt) { field_set(c, 'I'); }
N(Field_setLong) { field_set(c, 'J'); }
N(Field_setFloat) { field_set(c, 'F'); }
N(Field_setDouble) { field_set(c, 'D'); }

/* ---------------------------------------------------- methods, constructors */

static hk_jobj *make_executable(hk_dvm_method *m)
{
    hk_dvm_class *ec = hk_dvm_find_class(m->name[0] == '<' ? "java/lang/reflect/Constructor" : "java/lang/reflect/Method");
    hk_jobj *o = ec ? hk_dvm_alloc_object(ec) : NULL;
    hk_jvalue v;
    if (!o) return NULL;
    o->native = m;
    put_int(o, "accessFlags", (i32)m->flags);
    v.j = (i64)(hk_addr)m;
    hk_dvm_set_field(o, "artMethod", v);
    put_obj(o, "declaringClass", hk_dvm_mirror(m->cls));
    put_obj(o, "declaringClassOfOverriddenMethod", hk_dvm_mirror(m->cls));
    put_int(o, "dexMethodIndex", (i32)m->idx);
    return o;
}

static hk_dvm_method *method_from(hk_jobj *o) { return (hk_dvm_method *)o->native; }

/* Whether a method's parameters are exactly these classes (a Class[]; null is none). */
static bool params_are(const hk_dvm_method *m, hk_jobj *types)
{
    const char *p = m->sig + 1;
    char want[512], have[512];
    u32 n = types ? types->u.oarr.len : 0, i;
    for (i = 0; i < n; i++) {
        hk_jobj *t = types->u.oarr.v[i];
        p = next_param(p, want, sizeof(want));
        if (!p || !t) return false;
        descriptor_of(hk_dvm_mirror_class(t), have, sizeof(have));
        if (strcmp(want, have) != 0) return false;
    }
    return *p == ')';
}

N(Class_getDeclaredMethodInternal)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    const char *name = hk_dvm_string_utf8(c->args[0].l), *interned;
    hk_dvm_method *found = NULL;
    int group, i;
    if (!name) return;
    interned = hk_dvm_intern(name);
    /* Several may share a name and parameters and differ in what they return (a bridge beside the method it bridges to): the one a compiler did not make up is meant. */
    for (group = 0; group < 2; group++) {
        hk_dvm_method *list = group ? k->direct : k->virt;
        int n = group ? k->ndirect : k->nvirt;
        for (i = 0; i < n; i++) {
            hk_dvm_method *m = &list[i];
            if (m->name != interned || m->name[0] == '<' || !params_are(m, c->args[1].l)) continue;
            if (!found || ((found->flags & 0x1040) && !(m->flags & 0x1040))) found = m;
        }
    }
    if (found) c->ret.l = make_executable(found);
}

N(Class_getDeclaredConstructorInternal)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    int i;
    for (i = 0; i < k->ndirect; i++)
        if (!strcmp(k->direct[i].name, "<init>") && params_are(&k->direct[i], c->args[0].l)) {
            c->ret.l = make_executable(&k->direct[i]);
            return;
        }
}

/* Every constructor (`constructors`) or every method that is not one, of the class itself; only the public ones when asked. */
static hk_jobj *executables_of(hk_dvm_class *k, bool constructors, bool public_only)
{
    hk_jobj *a;
    int pass, group, i, n = 0;
    a = NULL;
    for (pass = 0; pass < 2; pass++) {
        int at = 0;
        for (group = 0; group < 2; group++) {
            hk_dvm_method *list = group ? k->virt : k->direct;
            int count = group ? k->nvirt : k->ndirect;
            for (i = 0; i < count; i++) {
                hk_dvm_method *m = &list[i];
                bool ctor = !strcmp(m->name, "<init>");
                if (m->name[0] == '<' && !ctor) continue;       /* the static initialiser is nobody's to see */
                if (ctor != constructors || (public_only && !(m->flags & DVM_ACC_PUBLIC))) continue;
                if (pass) {
                    a->u.oarr.v[at] = make_executable(m);
                    if (!a->u.oarr.v[at]) return NULL;
                }
                at++;
            }
        }
        if (!pass) {
            n = at;
            a = hk_dvm_new_array(constructors ? "[Ljava/lang/reflect/Constructor;" : "[Ljava/lang/reflect/Method;", n);
            if (!a) return NULL;
        }
    }
    return a;
}

N(Class_getDeclaredConstructorsInternal) { c->ret.l = executables_of(hk_dvm_mirror_class(c->self), true, c->args[0].z != 0); }
N(Class_getDeclaredMethodsUnchecked) { c->ret.l = executables_of(hk_dvm_mirror_class(c->self), false, c->args[0].z != 0); }

N(Executable_getMethodNameInternal) { c->ret.l = hk_dvm_intern_string(hk_dvm_new_string(method_from(c->self)->name)); }

N(Executable_getMethodReturnTypeInternal)
{
    const char *ret = strchr(method_from(c->self)->sig, ')');
    hk_dvm_class *k = ret ? hk_dvm_find_class_desc(ret + 1) : NULL;
    if (k) c->ret.l = hk_dvm_mirror(k);
}

N(Executable_getParameterCountInternal) { c->ret.i = method_from(c->self)->nparams; }

N(Executable_getParameterTypesInternal)
{
    hk_dvm_method *m = method_from(c->self);
    const char *p = m->sig + 1;
    char desc[512];
    hk_jobj *a;
    int i;
    if (!m->nparams) return;                                     /* null for none: the caller has an empty array of its own */
    a = hk_dvm_new_array("[Ljava/lang/Class;", m->nparams);
    for (i = 0; a && i < m->nparams; i++) {
        hk_dvm_class *k;
        p = next_param(p, desc, sizeof(desc));
        k = p ? hk_dvm_find_class_desc(desc) : NULL;
        if (!k) return;
        a->u.oarr.v[i] = hk_dvm_mirror(k);
    }
    c->ret.l = a;
}

N(Executable_compareMethodParametersInternal)
{
    hk_dvm_method *a = method_from(c->self), *b = method_from(c->args[0].l);
    const char *ea = strchr(a->sig, ')'), *eb = strchr(b->sig, ')');
    size_t la = (size_t)(ea - a->sig), lb = (size_t)(eb - b->sig);
    int order = strncmp(a->sig, b->sig, la < lb ? la : lb);
    c->ret.i = order ? order : (i32)la - (i32)lb;
}

N(Executable_noAnnotations) { c->ret.l = hk_dvm_new_array("[Ljava/lang/annotation/Annotation;", 0); }
N(Executable_noClasses) { c->ret.l = hk_dvm_new_array("[Ljava/lang/Class;", 0); }
N(reflect_null) { c->ret.l = NULL; }
N(reflect_false) { c->ret.z = 0; }

/*
 * A call through reflection: the arguments come as objects, each unboxed to
 * what the parameter is; what the method throws comes back wrapped in an
 * InvocationTargetException, as Java says.
 */
static bool invoke_boxed(hk_dvm_method *target, const hk_dvm_method *declared, hk_jobj *self, hk_jobj *args, hk_jvalue *ret)
{
    hk_jvalue params[64], wrapped;
    const char *p = declared->sig + 1;
    char desc[512];
    u32 n = args ? args->u.oarr.len : 0, i;
    hk_jobj *e;
    if (n != declared->nparams || n > HK_COUNT(params)) return hk_dvm_throw("java/lang/IllegalArgumentException", "Wrong number of arguments; expected %d, got %u", (int)declared->nparams, (unsigned)n);
    for (i = 0; i < n; i++) {
        hk_jobj *a = args->u.oarr.v[i];
        hk_jvalue v;
        char kind;
        p = next_param(p, desc, sizeof(desc));
        if (!p) return hk_dvm_throw("java/lang/InternalError", "a signature that cannot be read: %s", declared->sig);
        params[i].j = 0;
        if (desc[0] == 'L' || desc[0] == '[') {
            hk_dvm_class *type = a ? hk_dvm_find_class_desc(desc) : NULL;
            if (a && !type) return false;
            if (a && !hk_dvm_is_instance(a, type)) return hk_dvm_throw("java/lang/IllegalArgumentException", "method %s.%s argument %u has type %s, got %s", declared->cls->name, declared->name, (unsigned)i + 1, desc, hk_dvm_class_of(a)->name);
            params[i].l = a;
            continue;
        }
        kind = unbox(a, &v);
        if (!kind || !widen(kind, v, desc[0], &params[i])) return hk_dvm_throw("java/lang/IllegalArgumentException", "method %s.%s argument %u has type %s, got %s", declared->cls->name, declared->name, (unsigned)i + 1, desc, a ? hk_dvm_class_of(a)->name : "null");
    }
    if (hk_dvm_invoke(target, self, params, ret)) return true;
    e = hk_dvm_pending();
    hk_dvm_clear_pending();
    wrapped.j = 0;
    wrapped.l = e;
    e = hk_dvm_new("java/lang/reflect/InvocationTargetException", "(Ljava/lang/Throwable;)V", &wrapped);
    if (e) hk_dvm_throw_object(e);
    return false;
}

N(Method_invoke)
{
    hk_dvm_method *m = method_from(c->self), *target = m;
    hk_jobj *self = c->args[0].l;
    hk_jvalue r;
    if (m->flags & DVM_ACC_STATIC) {
        if (!hk_dvm_ensure_init(m->cls)) return;
        self = NULL;
    } else if (!self) {
        hk_dvm_throw("java/lang/NullPointerException", "null receiver");
        return;
    } else if (!hk_dvm_is_instance(self, m->cls)) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "Expected receiver of type %s, but got %s", m->cls->name, hk_dvm_class_of(self)->name);
        return;
    } else {
        target = hk_dvm_virtual_target(m, self);
    }
    if (invoke_boxed(target, m, self, c->args[1].l, &r)) c->ret.l = box(m->shorty[0], r);
}

N(Constructor_newInstance0)
{
    hk_dvm_method *m = method_from(c->self);
    hk_dvm_class *k = m->cls;
    hk_jobj *o;
    if (k->flags & (DVM_ACC_ABSTRACT | DVM_ACC_INTERFACE)) {
        hk_dvm_throw("java/lang/InstantiationException", "Can't instantiate abstract class %s", k->name);
        return;
    }
    if (!hk_dvm_ensure_init(k)) return;
    if (k == g_dvm.c_string) {
        hk_dvm_throw("java/lang/UnsupportedOperationException", "a String through its Constructor");
        return;
    }
    o = hk_dvm_alloc_object(k);
    if (o && invoke_boxed(m, m, o, c->args[0].l, NULL)) c->ret.l = o;
}

/* For hk_dvm_art_annot.c, which hands out the same objects. */
hk_jobj *hk_dvm_reflect_box(char kind, hk_jvalue v) { return box(kind, v); }
hk_jobj *hk_dvm_reflect_method(hk_dvm_method *m) { return make_executable(m); }
hk_jobj *hk_dvm_reflect_field(hk_dvm_field *f) { return make_field(f); }

hk_dvm_method *hk_dvm_reflected_method(hk_jobj *o)
{
    return o && o->dvm && hk_dvm_instance_of(o, "java/lang/reflect/Executable") ? method_from(o) : NULL;
}

hk_dvm_field *hk_dvm_reflected_field(hk_jobj *o)
{
    return o && o->dvm && hk_dvm_instance_of(o, "java/lang/reflect/Field") ? field_from(o) : NULL;
}

/* ------------------------------------------------------------------ proxies */

/*
 * java.lang.reflect.Proxy.generateProxy(name, interfaces, loader, methods,
 * exceptions): a class that implements the interfaces by handing every call
 * to an InvocationHandler. libcore has worked out which methods (the
 * interfaces' and three of Object's) and passes them as Method objects; the
 * class gets one native method for each, and when one is called its
 * arguments are boxed and what Proxy.invoke does is done here:
 * handler.invoke(proxy, method, args). The Method objects are kept beside the
 * class, since the method called must be handed to the handler as the very
 * object libcore made.
 */
#define MAX_PROXIES 256
static struct {
    hk_dvm_class *cls;
    hk_jobj *methods;
} g_proxies[MAX_PROXIES];
static volatile i32 g_nproxies;

static void proxy_construct(hk_jcall *c)
{
    if (!c->args[0].l) {
        hk_dvm_throw("java/lang/NullPointerException", "a proxy without an InvocationHandler");
        return;
    }
    hk_dvm_set_field(c->self, "h", c->args[0]);
}

static void proxy_invoke(hk_jcall *c)
{
    const hk_dvm_method *m = hk_dvm_self()->top->m;
    hk_jobj *methods = NULL, *args = NULL;
    hk_jvalue a[3], r, h, plain;
    char kind, held;
    i32 i, n = g_nproxies;
    for (i = 0; i < n && !methods; i++)
        if (g_proxies[i].cls == m->cls) methods = g_proxies[i].methods;
    h.j = 0;
    if (!methods || m->idx >= (u32)methods->u.oarr.len || !hk_dvm_get_field(c->self, "h", &h) || !h.l) {
        hk_dvm_throw("java/lang/IllegalStateException", "a proxy's method %s without its handler", m->name);
        return;
    }
    if (m->nparams) {
        /* (A method that takes nothing is handed null for its arguments, as Java's own proxies hand it.) */
        args = hk_dvm_new_array("[Ljava/lang/Object;", m->nparams);
        if (!args) return;
        for (i = 0; i < m->nparams; i++) {
            args->u.oarr.v[i] = box(m->shorty[1 + i], c->args[i]);
            if (hk_dvm_pending()) return;
        }
    }
    memset(a, 0, sizeof(a));
    a[0].l = c->self;
    a[1].l = methods->u.oarr.v[m->idx];
    a[2].l = args;
    r.j = 0;
    if (!hk_dvm_call_virtual(h.l, "invoke", "(Ljava/lang/Object;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;", a, &r)) return;
    kind = m->shorty[0];
    if (kind == 'V') return;
    if (kind == 'L') {
        c->ret.l = r.l;
        return;
    }
    /* A primitive is wanted: what the handler returned must be a box that holds one, or one that widens to it. */
    plain.j = 0;
    held = unbox(r.l, &plain);
    if (!r.l) hk_dvm_throw("java/lang/NullPointerException", "a proxy's handler returned null for %s, which returns a primitive", m->name);
    else if (!held || !widen(held, plain, kind, &c->ret)) hk_dvm_throw("java/lang/ClassCastException", "a proxy's handler returned a %s for %s", hk_dvm_class_of(r.l)->name, m->name);
}

N(Proxy_generateProxy)
{
    hk_jobj *name = c->args[0].l, *ifaces = c->args[1].l, *methods = c->args[3].l;
    hk_dvm_class *super = hk_dvm_find_class("java/lang/reflect/Proxy"), *list[64], *made;
    hk_dvm_method *like[512];
    char internal[300];
    const char *dotted = hk_dvm_string_utf8(name);
    i32 i, n, nm, slot;
    size_t k;
    if (!super || !dotted || !ifaces || !methods) {
        if (!hk_dvm_pending()) hk_dvm_throw("java/lang/NullPointerException", "generateProxy");
        return;
    }
    n = ifaces->u.oarr.len;
    nm = methods->u.oarr.len;
    if (n > (i32)HK_COUNT(list) || nm > (i32)HK_COUNT(like) || strlen(dotted) >= sizeof(internal)) {
        hk_dvm_throw("java/lang/IllegalArgumentException", "a proxy of %d interfaces and %d methods", (int)n, (int)nm);
        return;
    }
    for (k = 0; dotted[k]; k++) internal[k] = dotted[k] == '.' ? '/' : dotted[k];
    internal[k] = 0;
    for (i = 0; i < n; i++) {
        list[i] = ifaces->u.oarr.v[i] ? hk_dvm_mirror_class(ifaces->u.oarr.v[i]) : NULL;
        if (!list[i]) {
            hk_dvm_throw("java/lang/NullPointerException", "a proxy of a null interface");
            return;
        }
    }
    for (i = 0; i < nm; i++) {
        like[i] = methods->u.oarr.v[i] ? method_from(methods->u.oarr.v[i]) : NULL;
        if (!like[i]) {
            hk_dvm_throw("java/lang/NullPointerException", "a proxy of a null method");
            return;
        }
    }
    made = hk_dvm_make_proxy_class(internal, super, list, n, like, nm, proxy_invoke, proxy_construct, "(Ljava/lang/reflect/InvocationHandler;)V");
    slot = hk_atomic_add(&g_nproxies, 1) - 1;
    if (!made || slot >= MAX_PROXIES) {
        hk_atomic_add(&g_nproxies, -1);
        hk_dvm_throw("java/lang/OutOfMemoryError", "too many proxy classes (%d)", MAX_PROXIES);
        return;
    }
    g_proxies[slot].methods = methods;
    g_proxies[slot].cls = made;
    c->ret.l = hk_dvm_mirror(made);
}

static const hk_jhle k_reflect[] = {
    { "java/lang/reflect/Proxy", "generateProxy", NULL, Proxy_generateProxy },
    { "java/lang/Class", "getDeclaredField", NULL, Class_getDeclaredField },
    { "java/lang/Class", "getDeclaredFields", NULL, Class_getDeclaredFields },
    { "java/lang/Class", "getDeclaredFieldsUnchecked", NULL, Class_getDeclaredFieldsUnchecked },
    { "java/lang/Class", "getPublicDeclaredFields", NULL, Class_getPublicDeclaredFields },
    { "java/lang/Class", "getPublicFieldRecursive", NULL, Class_getPublicFieldRecursive },
    { "java/lang/Class", "getDeclaredMethodInternal", NULL, Class_getDeclaredMethodInternal },
    { "java/lang/Class", "getDeclaredConstructorInternal", NULL, Class_getDeclaredConstructorInternal },
    { "java/lang/Class", "getDeclaredConstructorsInternal", NULL, Class_getDeclaredConstructorsInternal },
    { "java/lang/Class", "getDeclaredMethodsUnchecked", NULL, Class_getDeclaredMethodsUnchecked },
    { "java/lang/Class", "getDeclaredAnnotations", NULL, Executable_noAnnotations },
    { "java/lang/Class", "getDeclaredClasses", NULL, Executable_noClasses },

    { "java/lang/reflect/Field", "getNameInternal", NULL, Field_getNameInternal },
    { "java/lang/reflect/Field", "getArtField", NULL, Field_getArtField },
    { "java/lang/reflect/Field", "get", NULL, Field_get },
    { "java/lang/reflect/Field", "getBoolean", NULL, Field_getBoolean },
    { "java/lang/reflect/Field", "getByte", NULL, Field_getByte },
    { "java/lang/reflect/Field", "getChar", NULL, Field_getChar },
    { "java/lang/reflect/Field", "getShort", NULL, Field_getShort },
    { "java/lang/reflect/Field", "getInt", NULL, Field_getInt },
    { "java/lang/reflect/Field", "getLong", NULL, Field_getLong },
    { "java/lang/reflect/Field", "getFloat", NULL, Field_getFloat },
    { "java/lang/reflect/Field", "getDouble", NULL, Field_getDouble },
    { "java/lang/reflect/Field", "set", NULL, Field_set },
    { "java/lang/reflect/Field", "setBoolean", NULL, Field_setBoolean },
    { "java/lang/reflect/Field", "setByte", NULL, Field_setByte },
    { "java/lang/reflect/Field", "setChar", NULL, Field_setChar },
    { "java/lang/reflect/Field", "setShort", NULL, Field_setShort },
    { "java/lang/reflect/Field", "setInt", NULL, Field_setInt },
    { "java/lang/reflect/Field", "setLong", NULL, Field_setLong },
    { "java/lang/reflect/Field", "setFloat", NULL, Field_setFloat },
    { "java/lang/reflect/Field", "setDouble", NULL, Field_setDouble },
    { "java/lang/reflect/Field", "getDeclaredAnnotations", NULL, Executable_noAnnotations },
    { "java/lang/reflect/Field", "getAnnotationNative", NULL, reflect_null },
    { "java/lang/reflect/Field", "getSignatureAnnotation", NULL, reflect_null },
    { "java/lang/reflect/Field", "isAnnotationPresentNative", NULL, reflect_false },

    { "java/lang/reflect/Executable", "getMethodNameInternal", NULL, Executable_getMethodNameInternal },
    { "java/lang/reflect/Executable", "getMethodReturnTypeInternal", NULL, Executable_getMethodReturnTypeInternal },
    { "java/lang/reflect/Executable", "getParameterCountInternal", NULL, Executable_getParameterCountInternal },
    { "java/lang/reflect/Executable", "getParameterTypesInternal", NULL, Executable_getParameterTypesInternal },
    { "java/lang/reflect/Executable", "compareMethodParametersInternal", NULL, Executable_compareMethodParametersInternal },
    { "java/lang/reflect/Executable", "getDeclaredAnnotationsNative", NULL, Executable_noAnnotations },
    { "java/lang/reflect/Executable", "getAnnotationNative", NULL, reflect_null },
    { "java/lang/reflect/Executable", "getParameterAnnotationsNative", NULL, reflect_null },
    { "java/lang/reflect/Executable", "getParameters0", NULL, reflect_null },
    { "java/lang/reflect/Executable", "getSignatureAnnotation", NULL, reflect_null },
    { "java/lang/reflect/Executable", "isAnnotationPresentNative", NULL, reflect_false },
    { "java/lang/reflect/Method", "invoke", NULL, Method_invoke },
    { "java/lang/reflect/Method", "getExceptionTypes", NULL, Executable_noClasses },
    { "java/lang/reflect/Method", "getDefaultValue", NULL, reflect_null },
    { "java/lang/reflect/Constructor", "newInstance0", NULL, Constructor_newInstance0 },
    { "java/lang/reflect/Constructor", "getExceptionTypes", NULL, Executable_noClasses },
    { "java/lang/reflect/Parameter", "getAnnotationNative", NULL, reflect_null },
    { NULL, NULL, NULL, NULL },
};

void hk_dvm_art_reflect_start(void) { hk_dvm_register(k_reflect); }
