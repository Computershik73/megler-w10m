/*
 * Annotations, as reflection reads them on libcore (art/runtime/dex/dex_file_annotations.cc
 * is what this is written against).
 *
 * A DEX file keeps, for a class, an annotations directory: a set for the
 * class itself, one for each annotated field and method, and a list of sets
 * for each method with annotated parameters. An annotation in a set has a
 * visibility (0 for the build only, 1 for reflection at run time, 2 for the
 * system) and is an encoded value: a type and named elements.
 *
 * Two kinds are read here:
 *
 *  - The system's own (dalvik.annotation.*), which is where a DEX file keeps
 *    what a class file had in attributes: which class a nested class is
 *    declared in and under what name (EnclosingClass, InnerClass,
 *    MemberClasses, EnclosingMethod), generic signatures (Signature), declared
 *    exceptions (Throws), an annotation's defaults (AnnotationDefault). These
 *    answer Class.getSimpleName, getDeclaringClass, getGenericSuperclass and
 *    the like, and no object is made of them.
 *  - An app's own with run-time visibility. Each becomes an object the way
 *    Android's runtime makes it: libcore's AnnotationFactory.createAnnotation
 *    with an AnnotationMember for each element, which gives a
 *    java.lang.reflect.Proxy of the annotation's interface.
 *
 * Nothing is kept: every question reads the file again. Reflection on
 * annotations is asked seldom enough for that.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>

#define N(name) static void name(mg_jcall *c)

/* ------------------------------------------------------- reading the file */

/* A set of annotations in a DEX file: `count` offsets of annotation items at `offsets`. */
typedef struct aset {
    mg_dvm_dex *d;
    const u8 *offsets;
    u32 count;
} aset;

static bool set_at(mg_dvm_dex *d, u32 off, aset *out)
{
    out->d = d;
    out->offsets = NULL;
    out->count = 0;
    if (!off || (u64)off + 4 > d->size) return false;
    out->count = mg_dvm_rd32(d->b + off);
    if ((u64)off + 4 + (u64)out->count * 4 > d->size) {
        out->count = 0;
        return false;
    }
    out->offsets = d->b + off + 4;
    return true;
}

/* A class's annotations directory: its four counts read, or NULL when it has none. */
static const u8 *directory(const mg_dvm_class *c, u32 *nfields, u32 *nmethods, u32 *nparams)
{
    const mg_dvm_dex *d = c->dex;
    u32 off;
    if (!d || c->prim || c->component) return NULL;
    off = mg_dvm_rd32(d->b + d->cls_off + 32 * (size_t)c->def + 20);
    if (!off || (u64)off + 16 > d->size) return NULL;
    *nfields = mg_dvm_rd32(d->b + off + 4);
    *nmethods = mg_dvm_rd32(d->b + off + 8);
    *nparams = mg_dvm_rd32(d->b + off + 12);
    if ((u64)off + 16 + ((u64)*nfields + *nmethods + *nparams) * 8 > d->size) return NULL;
    return d->b + off;
}

static bool class_set(mg_dvm_class *c, aset *out)
{
    u32 nf, nm, np;
    const u8 *dir = directory(c, &nf, &nm, &np);
    out->count = 0;
    return dir && set_at(c->dex, mg_dvm_rd32(dir), out);
}

/* The entry for an index among `count` pairs (index, offset) at `list`: its offset, or 0. */
static u32 entry_for(const u8 *list, u32 count, u32 idx)
{
    u32 i;
    for (i = 0; i < count; i++)
        if (mg_dvm_rd32(list + 8 * (size_t)i) == idx) return mg_dvm_rd32(list + 8 * (size_t)i + 4);
    return 0;
}

static bool field_set(const mg_dvm_field *f, aset *out)
{
    u32 nf, nm, np;
    const u8 *dir = directory(f->cls, &nf, &nm, &np);
    out->count = 0;
    return dir && set_at(f->cls->dex, entry_for(dir + 16, nf, f->idx), out);
}

static bool method_set(const mg_dvm_method *m, aset *out)
{
    u32 nf, nm, np;
    const u8 *dir = directory(m->cls, &nf, &nm, &np);
    out->count = 0;
    return dir && set_at(m->cls->dex, entry_for(dir + 16 + 8 * (size_t)nf, nm, m->idx), out);
}

/* The i-th annotation of a set: where its encoded form starts (its type), and its visibility. NULL when the file is damaged. */
static const u8 *item(const aset *s, u32 i, u8 *visibility)
{
    u32 off = mg_dvm_rd32(s->offsets + 4 * (size_t)i);
    if (!off || (u64)off + 2 > s->d->size) return NULL;
    *visibility = s->d->b[off];
    return s->d->b + off + 1;
}

static void skip_value(const u8 **pp);

/* Past an encoded annotation: its type, its count, and each name and value. */
static void skip_annotation(const u8 **pp)
{
    u32 n;
    mg_dvm_uleb(pp);
    for (n = mg_dvm_uleb(pp); n; n--) {
        mg_dvm_uleb(pp);
        skip_value(pp);
    }
}

static void skip_value(const u8 **pp)
{
    u8 head = *(*pp)++;
    u32 type = head & 0x1F, arg = head >> 5, n;
    switch (type) {
    case 0x1C:
        for (n = mg_dvm_uleb(pp); n; n--) skip_value(pp);
        break;
    case 0x1D: skip_annotation(pp); break;
    case 0x1E: case 0x1F: break;
    default: *pp += arg + 1; break;
    }
}

/* The annotation of that type in a set (any visibility when `visibility` is negative), or NULL. */
static const u8 *find_in(const aset *s, const char *descriptor, int visibility)
{
    u32 i;
    for (i = 0; i < s->count; i++) {
        u8 v = 0;
        const u8 *a = item(s, i, &v), *p = a;
        if (!a || (visibility >= 0 && v != visibility)) continue;
        if (!strcmp(mg_dvm_dex_type(s->d, mg_dvm_uleb(&p)), descriptor)) return a;
    }
    return NULL;
}

/* The value of a named element of an annotation: where it starts, or NULL. */
static const u8 *element(mg_dvm_dex *d, const u8 *annotation, const char *name)
{
    const u8 *p = annotation;
    u32 n;
    mg_dvm_uleb(&p);
    for (n = mg_dvm_uleb(&p); n; n--) {
        const char *its = mg_dvm_dex_string(d, mg_dvm_uleb(&p), NULL);
        if (!strcmp(its, name)) return p;
        skip_value(&p);
    }
    return NULL;
}

/* The bytes of a value as a number: `size` of them, least significant first; signed ones carry their sign up. */
static i64 read_int(const u8 *p, u32 size, bool is_signed)
{
    u64 v = 0;
    u32 i;
    for (i = 0; i < size && i < 8; i++) v |= (u64)p[i] << (8 * i);
    if (is_signed && size && size < 8 && (p[size - 1] & 0x80)) v |= ~(u64)0 << (8 * size);
    return (i64)v;
}

/* An index a value holds (a string, a type, a field, a method), when the value is of that type; 0xFFFFFFFF otherwise. */
static u32 index_of(const u8 *value, u32 type)
{
    if (!value || (value[0] & 0x1F) != type) return 0xFFFFFFFFu;
    return (u32)read_int(value + 1, (u32)(value[0] >> 5) + 1, false);
}

/* --------------------------------------------- the system's own annotations */

#define SYSTEM 2

static mg_dvm_class *enclosing_class_named(mg_dvm_class *c)
{
    aset s;
    const u8 *a = class_set(c, &s) ? find_in(&s, "Ldalvik/annotation/EnclosingClass;", SYSTEM) : NULL;
    u32 idx = a ? index_of(element(c->dex, a, "value"), 0x18) : 0xFFFFFFFFu;
    mg_dvm_class *k = idx != 0xFFFFFFFFu ? mg_dvm_resolve_type(c->dex, idx) : NULL;
    if (!k) mg_dvm_clear_pending();
    return k;
}

/* The method a local or anonymous class was written in, or NULL. */
static mg_dvm_method *enclosing_method(mg_dvm_class *c)
{
    aset s;
    const u8 *a = class_set(c, &s) ? find_in(&s, "Ldalvik/annotation/EnclosingMethod;", SYSTEM) : NULL;
    u32 idx = a ? index_of(element(c->dex, a, "value"), 0x1A) : 0xFFFFFFFFu;
    mg_dvm_method *m = NULL;
    const u8 *id;
    mg_dvm_class *k;
    const char *name, *sig;
    if (idx == 0xFFFFFFFFu || idx >= c->dex->nmeth) return NULL;
    /* A method named by its index alone, with no instruction to say what kind it is: looked up by name in its class, static or not. */
    id = c->dex->b + c->dex->meth_off + 8 * (size_t)idx;
    k = mg_dvm_resolve_type(c->dex, mg_dvm_rd16(id));
    if (!k) {
        mg_dvm_clear_pending();
        return NULL;
    }
    name = mg_dvm_dex_string(c->dex, mg_dvm_rd32(id + 4), NULL);
    sig = mg_dvm_dex_proto_sig(c->dex, mg_dvm_rd16(id + 2));
    m = mg_dvm_find_method(k, name, sig, false);
    if (!m) m = mg_dvm_find_method(k, name, sig, true);
    return m;
}

/* InnerClass: whether the class has one, and its name (NULL: anonymous) and flags. */
static bool inner_class(mg_dvm_class *c, const char **name, i32 *flags)
{
    aset s;
    const u8 *a = class_set(c, &s) ? find_in(&s, "Ldalvik/annotation/InnerClass;", SYSTEM) : NULL, *v;
    u32 idx;
    if (!a) return false;
    idx = index_of(element(c->dex, a, "name"), 0x17);
    if (name) *name = idx != 0xFFFFFFFFu ? mg_dvm_dex_string(c->dex, idx, NULL) : NULL;
    v = element(c->dex, a, "accessFlags");
    if (flags && v && (v[0] & 0x1F) == 0x04) *flags = (i32)read_int(v + 1, (u32)(v[0] >> 5) + 1, true);
    return true;
}

N(Class_isAnonymousClass)
{
    const char *name = "";
    c->ret.z = inner_class(mg_dvm_mirror_class(c->self), &name, NULL) && !name;
}

N(Class_getInnerClassName)
{
    const char *name = NULL;
    c->ret.l = inner_class(mg_dvm_mirror_class(c->self), &name, NULL) && name ? mg_dvm_new_string(name) : NULL;
}

N(Class_getInnerClassFlags)
{
    i32 flags = c->args[0].i;
    inner_class(mg_dvm_mirror_class(c->self), NULL, &flags);
    c->ret.i = flags;
}

N(Class_getDeclaringClass)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self), *outer;
    const char *name = "";
    /* An anonymous class is declared by nobody, though it is enclosed. */
    if (inner_class(k, &name, NULL) && !name) return;
    outer = enclosing_class_named(k);
    c->ret.l = outer ? mg_dvm_mirror(outer) : NULL;
}

N(Class_getEnclosingClass)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self), *outer = enclosing_class_named(k);
    mg_dvm_method *m = outer ? NULL : enclosing_method(k);
    if (m) outer = m->cls;
    c->ret.l = outer ? mg_dvm_mirror(outer) : NULL;
}

N(Class_getEnclosingMethodNative)
{
    mg_dvm_method *m = enclosing_method(mg_dvm_mirror_class(c->self));
    c->ret.l = m && m->name[0] != '<' ? mg_dvm_reflect_method(m) : NULL;
}

N(Class_getEnclosingConstructorNative)
{
    mg_dvm_method *m = enclosing_method(mg_dvm_mirror_class(c->self));
    c->ret.l = m && !strcmp(m->name, "<init>") ? mg_dvm_reflect_method(m) : NULL;
}

/* An element that is an array of types, as Class[]; an empty array when there is none. */
static mg_jobj *types_of(mg_dvm_dex *d, const u8 *value)
{
    const u8 *p = value;
    u32 n = 0, i;
    mg_jobj *out;
    if (p && (*p & 0x1F) == 0x1C) {
        p++;
        n = mg_dvm_uleb(&p);
    }
    out = mg_dvm_new_array("[Ljava/lang/Class;", (i32)n);
    for (i = 0; out && i < n; i++) {
        u32 idx = index_of(p, 0x18);
        mg_dvm_class *k = idx != 0xFFFFFFFFu ? mg_dvm_resolve_type(d, idx) : NULL;
        if (!k) mg_dvm_clear_pending();
        out->u.oarr.v[i] = k ? mg_dvm_mirror(k) : NULL;
        skip_value(&p);
    }
    return out;
}

N(Class_getDeclaredClasses)
{
    mg_dvm_class *k = mg_dvm_mirror_class(c->self);
    aset s;
    const u8 *a = class_set(k, &s) ? find_in(&s, "Ldalvik/annotation/MemberClasses;", SYSTEM) : NULL;
    c->ret.l = types_of(k->dex, a ? element(k->dex, a, "value") : NULL);
}

/* Signature: the pieces of a generic signature as String[], which libcore joins and parses; null when there is none. */
static mg_jobj *signature_of(const aset *s)
{
    const u8 *a = s->count ? find_in(s, "Ldalvik/annotation/Signature;", SYSTEM) : NULL, *p = a ? element(s->d, a, "value") : NULL;
    u32 n, i;
    mg_jobj *out;
    if (!p || (*p & 0x1F) != 0x1C) return NULL;
    p++;
    n = mg_dvm_uleb(&p);
    out = mg_dvm_new_array("[Ljava/lang/String;", (i32)n);
    for (i = 0; out && i < n; i++) {
        u32 idx = index_of(p, 0x17);
        out->u.oarr.v[i] = idx != 0xFFFFFFFFu ? mg_dvm_new_string(mg_dvm_dex_string(s->d, idx, NULL)) : NULL;
        skip_value(&p);
    }
    return out;
}

N(Class_getSignatureAnnotation)
{
    aset s;
    class_set(mg_dvm_mirror_class(c->self), &s);
    c->ret.l = signature_of(&s);
}

N(Field_getSignatureAnnotation)
{
    aset s;
    field_set((const mg_dvm_field *)c->self->native, &s);
    c->ret.l = signature_of(&s);
}

N(Executable_getSignatureAnnotation)
{
    aset s;
    method_set((const mg_dvm_method *)c->self->native, &s);
    c->ret.l = signature_of(&s);
}

/* Throws: the exceptions a method declares. */
N(Executable_getExceptionTypes)
{
    const mg_dvm_method *m = (const mg_dvm_method *)c->self->native;
    aset s;
    const u8 *a = method_set(m, &s) ? find_in(&s, "Ldalvik/annotation/Throws;", SYSTEM) : NULL;
    c->ret.l = types_of(m->cls->dex, a ? element(m->cls->dex, a, "value") : NULL);
}

/* ---------------------------------------------------- an app's annotations */

static mg_jobj *make_annotation(mg_dvm_dex *d, const u8 **pp);

/* The class a method returns, by its signature; NULL (nothing pending) when it cannot be found. */
static mg_dvm_class *return_class(const mg_dvm_method *m)
{
    const char *r = strchr(m->sig, ')');
    mg_dvm_class *k = r ? mg_dvm_find_class_desc(r + 1) : NULL;
    if (!k) mg_dvm_clear_pending();
    return k;
}

/*
 * A value as the object reflection hands out: primitives boxed, strings,
 * classes, enum constants, nested annotations, and arrays of the type the
 * annotation's method returns (`want`; NULL when it is not known). False with
 * an exception pending when something could not be made.
 */
static bool value_object(mg_dvm_dex *d, const u8 **pp, mg_dvm_class *want, mg_jobj **out)
{
    static const char kinds[] = { 'B', 0, 'S', 'C', 'I', 0, 'J' };
    u8 head = *(*pp)++;
    u32 type = head & 0x1F, size = (u32)(head >> 5) + 1, n, i;
    const u8 *p = *pp;
    mg_jvalue v;
    mg_dvm_class *k;
    v.j = 0;
    *out = NULL;
    switch (type) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x06:
        *pp += size;
        v.j = read_int(p, size, type != 0x03);
        if (type != 0x06) v.j = (i64)(u64)(u32)(i32)v.j;             /* the narrow kinds live in the low word of the value */
        *out = mg_dvm_reflect_box(kinds[type], v);
        return *out != NULL;
    case 0x10:
        *pp += size;
        /* A float's bytes are its most significant ones: the missing low ones are zero. */
        v.j = (i64)(u64)((u32)read_int(p, size, false) << (8 * (4 - (size > 4 ? 4 : size))));
        *out = mg_dvm_reflect_box('F', v);
        return *out != NULL;
    case 0x11:
        *pp += size;
        v.j = (i64)((u64)read_int(p, size, false) << (8 * (8 - size)));
        *out = mg_dvm_reflect_box('D', v);
        return *out != NULL;
    case 0x17:
        *pp += size;
        *out = mg_dvm_const_string(d, (u32)read_int(p, size, false));
        return *out != NULL;
    case 0x18:
        *pp += size;
        k = mg_dvm_resolve_type(d, (u32)read_int(p, size, false));
        /* A class that is not there is not an error until the element is asked for; here it is simply absent. */
        if (!k) mg_dvm_clear_pending();
        *out = k ? mg_dvm_mirror(k) : NULL;
        return true;
    case 0x1B: {
        mg_dvm_field *f;
        *pp += size;
        f = mg_dvm_resolve_field(d, (u32)read_int(p, size, false), true);
        if (!f || !mg_dvm_ensure_init(f->cls)) return false;
        *out = f->cls->statics[f->slot].l;
        return true;
    }
    case 0x1C: {
        mg_dvm_class *elem = want && want->component ? want->component : NULL;
        mg_jobj *a;
        n = mg_dvm_uleb(pp);
        a = mg_dvm_new_array(elem ? want->name : "[Ljava/lang/Object;", (i32)n);
        if (!a) return false;
        for (i = 0; i < n; i++) {
            if (elem && elem->prim) {
                /* An array of primitives holds the values themselves. */
                u8 h = *(*pp)++;
                u32 t = h & 0x1F, sz = (u32)(h >> 5) + 1;
                i64 x = t == 0x1F ? (i64)(h >> 5) : t == 0x10 ? (i64)(u64)((u32)read_int(*pp, sz, false) << (8 * (4 - (sz > 4 ? 4 : sz))))
                      : t == 0x11 ? (i64)((u64)read_int(*pp, sz, false) << (8 * (8 - sz))) : read_int(*pp, sz, t != 0x03);
                size_t width = elem->prim == 'J' || elem->prim == 'D' ? 8 : elem->prim == 'I' || elem->prim == 'F' ? 4 : elem->prim == 'S' || elem->prim == 'C' ? 2 : 1;
                if (t != 0x1F) *pp += sz;
                memcpy((u8 *)a->u.arr.data + width * i, &x, width);  /* little-endian hosts only, as everything here */
            } else {
                mg_jobj *e = NULL;
                if (!value_object(d, pp, elem, &e)) return false;
                a->u.oarr.v[i] = e;
            }
        }
        *out = a;
        return true;
    }
    case 0x1D:
        *out = make_annotation(d, pp);
        return *out != NULL || !mg_dvm_pending();
    case 0x1E: return true;
    case 0x1F:
        v.z = (head >> 5) != 0;
        *out = mg_dvm_reflect_box('Z', v);
        return *out != NULL;
    default:
        /* A method, a field, a method type or handle as a value: Java's annotations have none of these. */
        *pp += size;
        return true;
    }
}

/* The object for an encoded annotation, made by libcore's own factory. NULL, with nothing pending, when its class is not there. */
static mg_jobj *make_annotation(mg_dvm_dex *d, const u8 **pp)
{
    mg_dvm_class *ac = mg_dvm_resolve_type(d, mg_dvm_uleb(pp));
    u32 n = mg_dvm_uleb(pp), i, made = 0;
    mg_jobj *members;
    mg_jvalue args[4], r;
    if (!ac) {
        mg_dvm_clear_pending();
        for (i = 0; i < n; i++) {
            mg_dvm_uleb(pp);
            skip_value(pp);
        }
        return NULL;
    }
    members = mg_dvm_new_array("[Llibcore/reflect/AnnotationMember;", (i32)n);
    if (!members) return NULL;
    for (i = 0; i < n; i++) {
        const char *name = mg_dvm_dex_string(d, mg_dvm_uleb(pp), NULL);
        mg_dvm_method *m = NULL;
        mg_dvm_class *want;
        mg_jobj *value = NULL;
        int k;
        for (k = 0; k < ac->nvirt && !m; k++)
            if (!strcmp(ac->virt[k].name, name)) m = &ac->virt[k];
        if (!m) {
            /* The annotation's class has no such element any more: the value is left out, as a newer class's default would stand. */
            skip_value(pp);
            continue;
        }
        want = return_class(m);
        if (!value_object(d, pp, want, &value)) return NULL;
        memset(args, 0, sizeof(args));
        args[0].l = mg_dvm_new_string(name);
        args[1].l = value;
        args[2].l = want ? mg_dvm_mirror(want) : NULL;
        args[3].l = mg_dvm_reflect_method(m);
        members->u.oarr.v[made] = mg_dvm_new("libcore/reflect/AnnotationMember", "(Ljava/lang/String;Ljava/lang/Object;Ljava/lang/Class;Ljava/lang/reflect/Method;)V", args);
        if (!members->u.oarr.v[made++]) return NULL;
    }
    if (made < n) {
        /* The factory wants an array with no gaps. */
        mg_jobj *exact = mg_dvm_new_array("[Llibcore/reflect/AnnotationMember;", (i32)made);
        if (!exact) return NULL;
        memcpy(exact->u.oarr.v, members->u.oarr.v, sizeof(mg_jobj *) * made);
        members = exact;
    }
    memset(args, 0, sizeof(args));
    args[0].l = mg_dvm_mirror(ac);
    args[1].l = members;
    r.j = 0;
    if (!mg_dvm_call_static("libcore/reflect/AnnotationFactory", "createAnnotation", "(Ljava/lang/Class;[Llibcore/reflect/AnnotationMember;)Ljava/lang/annotation/Annotation;", args, &r))
        return NULL;
    return r.l;
}

#define RUNTIME 1

/* Every annotation of a set that reflection may see, as Annotation[]. */
static mg_jobj *all_of(const aset *s)
{
    mg_jobj *found[64], *out;
    u32 i, n = 0;
    for (i = 0; i < s->count && n < MG_COUNT(found); i++) {
        u8 v = 0;
        const u8 *a = item(s, i, &v);
        if (!a || v != RUNTIME) continue;
        found[n] = make_annotation(s->d, &a);
        if (found[n]) n++;
        else if (mg_dvm_pending()) return NULL;
    }
    out = mg_dvm_new_array("[Ljava/lang/annotation/Annotation;", (i32)n);
    if (out && n) memcpy(out->u.oarr.v, found, sizeof(mg_jobj *) * n);
    return out;
}

/* The annotation of the class `type` (a Class object) in a set, as an object; `present` instead when only that is asked. */
static mg_jobj *one_of(const aset *s, mg_jobj *type, bool *present)
{
    char desc[400];
    const u8 *a;
    if (present) *present = false;
    if (!type) {
        mg_dvm_throw("java/lang/NullPointerException", "annotationClass == null");
        return NULL;
    }
    snprintf(desc, sizeof(desc), "L%s;", mg_dvm_mirror_class(type)->name);
    a = s->count ? find_in(s, desc, RUNTIME) : NULL;
    if (present) *present = a != NULL;
    return a && !present ? make_annotation(s->d, &a) : NULL;
}

N(Class_getDeclaredAnnotations)
{
    aset s;
    class_set(mg_dvm_mirror_class(c->self), &s);
    c->ret.l = all_of(&s);
}

N(Class_getDeclaredAnnotation)
{
    aset s;
    class_set(mg_dvm_mirror_class(c->self), &s);
    c->ret.l = one_of(&s, c->args[0].l, NULL);
}

N(Class_isDeclaredAnnotationPresent)
{
    aset s;
    bool present = false;
    class_set(mg_dvm_mirror_class(c->self), &s);
    one_of(&s, c->args[0].l, &present);
    c->ret.z = present;
}

N(Field_getDeclaredAnnotations)
{
    aset s;
    field_set((const mg_dvm_field *)c->self->native, &s);
    c->ret.l = all_of(&s);
}

N(Field_getAnnotationNative)
{
    aset s;
    field_set((const mg_dvm_field *)c->self->native, &s);
    c->ret.l = one_of(&s, c->args[0].l, NULL);
}

N(Field_isAnnotationPresentNative)
{
    aset s;
    bool present = false;
    field_set((const mg_dvm_field *)c->self->native, &s);
    one_of(&s, c->args[0].l, &present);
    c->ret.z = present;
}

N(Executable_getDeclaredAnnotationsNative)
{
    aset s;
    method_set((const mg_dvm_method *)c->self->native, &s);
    c->ret.l = all_of(&s);
}

N(Executable_getAnnotationNative)
{
    aset s;
    method_set((const mg_dvm_method *)c->self->native, &s);
    c->ret.l = one_of(&s, c->args[0].l, NULL);
}

N(Executable_isAnnotationPresentNative)
{
    aset s;
    bool present = false;
    method_set((const mg_dvm_method *)c->self->native, &s);
    one_of(&s, c->args[0].l, &present);
    c->ret.z = present;
}

/* The annotations of each parameter, as Annotation[][]; null when no parameter has any, which libcore turns into empty arrays. */
N(Executable_getParameterAnnotationsNative)
{
    const mg_dvm_method *m = (const mg_dvm_method *)c->self->native;
    mg_dvm_dex *d = m->cls->dex;
    u32 nf, nm, np, off, count, i;
    const u8 *dir = directory(m->cls, &nf, &nm, &np);
    mg_jobj *out;
    off = dir ? entry_for(dir + 16 + 8 * ((size_t)nf + nm), np, m->idx) : 0;
    if (!off || (u64)off + 4 > d->size) return;
    count = mg_dvm_rd32(d->b + off);
    if ((u64)off + 4 + (u64)count * 4 > d->size) return;
    out = mg_dvm_new_array("[[Ljava/lang/annotation/Annotation;", (i32)count);
    for (i = 0; out && i < count; i++) {
        aset s;
        set_at(d, mg_dvm_rd32(d->b + off + 4 + 4 * (size_t)i), &s);
        out->u.oarr.v[i] = all_of(&s);
        if (!out->u.oarr.v[i]) return;
    }
    c->ret.l = out;
}

/* Method.getDefaultValue: what an annotation's element is when it is not given, from AnnotationDefault on the annotation's class. */
N(Method_getDefaultValue)
{
    const mg_dvm_method *m = (const mg_dvm_method *)c->self->native;
    aset s;
    const u8 *a = class_set(m->cls, &s) ? find_in(&s, "Ldalvik/annotation/AnnotationDefault;", SYSTEM) : NULL, *v = a ? element(m->cls->dex, a, "value") : NULL;
    mg_jobj *value = NULL;
    if (!v || (*v & 0x1F) != 0x1D) return;
    v = element(m->cls->dex, v + 1, m->name);
    if (v && value_object(m->cls->dex, &v, return_class(m), &value)) c->ret.l = value;
}

static const mg_jhle k_annot[] = {
    { "java/lang/Class", "isAnonymousClass", NULL, Class_isAnonymousClass },
    { "java/lang/Class", "getInnerClassName", NULL, Class_getInnerClassName },
    { "java/lang/Class", "getInnerClassFlags", NULL, Class_getInnerClassFlags },
    { "java/lang/Class", "getDeclaringClass", NULL, Class_getDeclaringClass },
    { "java/lang/Class", "getEnclosingClass", NULL, Class_getEnclosingClass },
    { "java/lang/Class", "getEnclosingMethodNative", NULL, Class_getEnclosingMethodNative },
    { "java/lang/Class", "getEnclosingConstructorNative", NULL, Class_getEnclosingConstructorNative },
    { "java/lang/Class", "getDeclaredClasses", NULL, Class_getDeclaredClasses },
    { "java/lang/Class", "getSignatureAnnotation", NULL, Class_getSignatureAnnotation },
    { "java/lang/Class", "getDeclaredAnnotations", NULL, Class_getDeclaredAnnotations },
    { "java/lang/Class", "getDeclaredAnnotation", NULL, Class_getDeclaredAnnotation },
    { "java/lang/Class", "isDeclaredAnnotationPresent", NULL, Class_isDeclaredAnnotationPresent },
    { "java/lang/reflect/Field", "getSignatureAnnotation", NULL, Field_getSignatureAnnotation },
    { "java/lang/reflect/Field", "getDeclaredAnnotations", NULL, Field_getDeclaredAnnotations },
    { "java/lang/reflect/Field", "getAnnotationNative", NULL, Field_getAnnotationNative },
    { "java/lang/reflect/Field", "isAnnotationPresentNative", NULL, Field_isAnnotationPresentNative },
    { "java/lang/reflect/Executable", "getSignatureAnnotation", NULL, Executable_getSignatureAnnotation },
    { "java/lang/reflect/Executable", "getDeclaredAnnotationsNative", NULL, Executable_getDeclaredAnnotationsNative },
    { "java/lang/reflect/Executable", "getAnnotationNative", NULL, Executable_getAnnotationNative },
    { "java/lang/reflect/Executable", "isAnnotationPresentNative", NULL, Executable_isAnnotationPresentNative },
    { "java/lang/reflect/Executable", "getParameterAnnotationsNative", NULL, Executable_getParameterAnnotationsNative },
    { "java/lang/reflect/Method", "getExceptionTypes", NULL, Executable_getExceptionTypes },
    { "java/lang/reflect/Constructor", "getExceptionTypes", NULL, Executable_getExceptionTypes },
    { "java/lang/reflect/Method", "getDefaultValue", NULL, Method_getDefaultValue },
    { NULL, NULL, NULL, NULL },
};

/* After the tables of mg_dvm_art.c and mg_dvm_art_reflect.c, which answered these with "none": the table registered last is asked first. */
void mg_dvm_art_annot_start(void) { mg_dvm_register(k_annot); }
