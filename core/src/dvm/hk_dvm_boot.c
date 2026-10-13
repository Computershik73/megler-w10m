/*
 * The bootstrap natives: java.lang as far as bytecode needs it to mean
 * anything, with no libcore.
 *
 * The classes themselves are DEX like any others -- the .smali files of
 * core/src/dvm/boot and what tools/dvm-boot.py generates, assembled into
 * hk_dvm_boot_dex.h -- and
 * this file is what they declare native. It stands in for two things at once:
 * what ART implements inside itself for libcore (Object, Class, the string
 * constructors, Throwable's stack trace), and what libcore writes in Java and
 * is not here (String's and StringBuilder's methods, the number formatting).
 *
 * All of it goes when libcore's own bytecode is the boot class path
 * (docs/JAVA.md): the first kind is then rewritten against libcore's fields,
 * the second kind is simply libcore's. So nothing else in the runtime may
 * depend on anything here beyond hk_dvm_boot_add and hk_dvm_boot_string_init.
 *
 * Known to differ from Java: float and double print with the shortest digits
 * that read back the same, which is not always what Java prints; the case
 * conversions know ASCII only; Integer.valueOf keeps no cache of small values.
 */
#include "hk_dvm_internal.h"

#include <math.h>
#include <stdio.h>

#include "hk_dvm_boot_dex.h"

/* ------------------------------------------------------------------ helpers */

static bool npe(const char *what) { return hk_dvm_throw("java/lang/NullPointerException", "%s", what); }

static hk_jobj *string_from_ascii(const char *s) { return hk_dvm_new_string(s); }

/* A class name as Java prints it: dots for slashes. */
static hk_jobj *dotted(const char *prefix, const char *name)
{
    char *text = hk_sprintf("%s%s", prefix, name), *p;
    hk_jobj *s;
    for (p = text + strlen(prefix); *p; p++)
        if (*p == '/') *p = '.';
    s = hk_dvm_new_string(text);
    free(text);
    return s;
}

/* The text of a string argument, or NULL with NullPointerException pending. */
static const u16 *chars_of(hk_jobj *s, i32 *len)
{
    if (!s || s->kind != HK_J_STRING) {
        npe("a null String");
        return NULL;
    }
    *len = s->u.str.len16;
    return s->u.str.u16;
}

/* Bytes as UTF-8 into UTF-16 (`out` NULL: only counts). Unlike a C string's, these have a length and may hold zeros. */
static i32 utf8_bytes_to_utf16(const u8 *p, i32 n, u16 *out)
{
    i32 i = 0, k = 0;
    while (i < n) {
        u32 c = p[i++];
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1, j;
        if (extra < 0) c = 0xFFFD;
        else {
            if (extra) c &= (u32)(0x3F >> extra);
            for (j = 0; j < extra && i < n && (p[i] & 0xC0) == 0x80; j++) c = (c << 6) | (p[i++] & 0x3F);
            if (j < extra) c = 0xFFFD;
        }
        if (c >= 0x10000 && c <= 0x10FFFF) {
            c -= 0x10000;
            if (out) {
                out[k] = (u16)(0xD800 | (c >> 10));
                out[k + 1] = (u16)(0xDC00 | (c & 0x3FF));
            }
            k += 2;
        } else {
            if (out) out[k] = c > 0xFFFF ? 0xFFFD : (u16)c;
            k++;
        }
    }
    return k;
}

/*
 * A float or double as Java writes it: "1.0", "0.001", "1.0E7", "NaN". The
 * digits are the fewest that read back as the same value.
 */
static void format_real(double v, bool is_float, char *out, size_t n)
{
    char digits[40], sci[48];
    int precision, exponent, ndigits, i;
    size_t k = 0;
    char *e;
    if (v != v) {
        hk_strlcpy(out, "NaN", n);
        return;
    }
    if (v > 1.7976931348623157e308 || v < -1.7976931348623157e308) {
        hk_strlcpy(out, v < 0 ? "-Infinity" : "Infinity", n);
        return;
    }
    if (v == 0) {
        hk_strlcpy(out, hk_db(v) >> 63 ? "-0.0" : "0.0", n);
        return;
    }
    for (precision = 1; precision <= 17; precision++) {
        snprintf(sci, sizeof(sci), "%.*e", precision - 1, v);
        if (is_float ? (float)strtod(sci, NULL) == (float)v : strtod(sci, NULL) == v) break;
    }
    /* sci is [-]d[.ddd]e[+-]xx */
    e = strchr(sci, 'e');
    exponent = e ? atoi(e + 1) : 0;
    ndigits = 0;
    for (i = sci[0] == '-' ? 1 : 0; sci[i] && sci[i] != 'e'; i++)
        if (sci[i] != '.') digits[ndigits++] = sci[i];
    while (ndigits > 1 && digits[ndigits - 1] == '0') ndigits--;
    if (v < 0) out[k++] = '-';
    if (exponent >= -3 && exponent < 7) {
        if (exponent < 0) {
            out[k++] = '0';
            out[k++] = '.';
            for (i = -1; i > exponent; i--) out[k++] = '0';
            for (i = 0; i < ndigits; i++) out[k++] = digits[i];
        } else {
            for (i = 0; i <= exponent; i++) out[k++] = i < ndigits ? digits[i] : '0';
            out[k++] = '.';
            if (ndigits <= exponent + 1) out[k++] = '0';
            for (i = exponent + 1; i < ndigits; i++) out[k++] = digits[i];
        }
        out[k] = 0;
    } else {
        out[k++] = digits[0];
        out[k++] = '.';
        if (ndigits == 1) out[k++] = '0';
        for (i = 1; i < ndigits; i++) out[k++] = digits[i];
        snprintf(out + k, n - k, "E%d", exponent);
    }
}

/* -------------------------------------------------------- java.lang.Object */

static void Object_getClass(hk_jcall *c) { c->ret.l = hk_dvm_mirror(hk_dvm_class_of(c->self)); }
static void Object_hashCode(hk_jcall *c) { c->ret.i = (i32)hk_dvm_identity_hash(c->self); }

static void Object_toString(hk_jcall *c)
{
    hk_jvalue hash;
    char *text, *p;
    /* The object's own hashCode(), which a class may have replaced. */
    if (!hk_dvm_call_virtual(c->self, "hashCode", "()I", NULL, &hash)) return;
    text = hk_sprintf("%s@%x", hk_dvm_class_of(c->self)->name, (unsigned)hash.i);
    for (p = text; *p; p++)
        if (*p == '/') *p = '.';
    c->ret.l = hk_dvm_new_string(text);
    free(text);
}

static void Object_clone(hk_jcall *c)
{
    hk_jobj *o = c->self, *copy;
    hk_dvm_class *k = hk_dvm_class_of(o);
    if (o->kind == HK_J_PRIM_ARRAY || o->kind == HK_J_OBJ_ARRAY) {
        copy = hk_dvm_alloc_array(k, hk_dvm_array_length(o));
        if (!copy) return;
        if (o->kind == HK_J_OBJ_ARRAY) memcpy(copy->u.oarr.v, o->u.oarr.v, sizeof(hk_jobj *) * o->u.oarr.len);
        else memcpy(copy->u.arr.data, o->u.arr.data, (size_t)o->u.arr.len * o->u.arr.esz);
        c->ret.l = copy;
        return;
    }
    if (o->kind != HK_J_OBJECT || !g_dvm.c_cloneable || !hk_dvm_assignable(k, g_dvm.c_cloneable)) {
        hk_dvm_throw("java/lang/CloneNotSupportedException", "%s", k->name);
        return;
    }
    copy = hk_dvm_alloc_object(k);
    if (!copy) return;
    memcpy(hk_dvm_slots(copy), hk_dvm_slots(o), sizeof(hk_jvalue) * k->nslots);
    c->ret.l = copy;
}

static void not_owner(const char *what) { hk_dvm_throw("java/lang/IllegalMonitorStateException", "object not locked by thread before %s()", what); }
static void Object_notify(hk_jcall *c) { if (!hk_dvm_monitor_notify(c->self, false)) not_owner("notify"); }
static void Object_notifyAll(hk_jcall *c) { if (!hk_dvm_monitor_notify(c->self, true)) not_owner("notifyAll"); }
static void Object_wait(hk_jcall *c) { if (!hk_dvm_monitor_wait(c->self, 0)) not_owner("wait"); }
static void Object_waitJ(hk_jcall *c) { if (!hk_dvm_monitor_wait(c->self, c->args[0].j)) not_owner("wait"); }

/* --------------------------------------------------------- java.lang.Class */

static void Class_getName(hk_jcall *c) { c->ret.l = dotted("", hk_dvm_mirror_class(c->self)->name); }

static void Class_toString(hk_jcall *c)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    c->ret.l = dotted(k->prim ? "" : (k->flags & DVM_ACC_INTERFACE) ? "interface " : "class ", k->name);
}

static void Class_getSimpleName(hk_jcall *c)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    const char *name = k->name, *cut;
    char text[300];
    int dims = 0, i;
    size_t n;
    while (*name == '[') {
        name++;
        dims++;
    }
    if (dims && *name == 'L') name++;
    else if (dims) {
        static const char *const prims[] = { "Zboolean", "Bbyte", "Cchar", "Sshort", "Iint", "Jlong", "Ffloat", "Ddouble" };
        for (i = 0; i < (int)HK_COUNT(prims); i++)
            if (prims[i][0] == *name) name = prims[i] + 1;
    }
    cut = strrchr(name, '/');
    if (cut) name = cut + 1;
    cut = strrchr(name, '$');
    if (cut) name = cut + 1;
    n = hk_strlcpy(text, name, sizeof(text) - 2 * 16);
    if (n >= sizeof(text) - 2 * 16) n = sizeof(text) - 2 * 16 - 1;
    if (n && text[n - 1] == ';') text[--n] = 0;
    for (i = 0; i < dims && i < 16; i++) {
        text[n++] = '[';
        text[n++] = ']';
    }
    text[n] = 0;
    c->ret.l = hk_dvm_new_string(text);
}

static void Class_isInterface(hk_jcall *c) { c->ret.z = (hk_dvm_mirror_class(c->self)->flags & DVM_ACC_INTERFACE) != 0; }
static void Class_isArray(hk_jcall *c) { c->ret.z = hk_dvm_mirror_class(c->self)->name[0] == '['; }
static void Class_isPrimitive(hk_jcall *c) { c->ret.z = hk_dvm_mirror_class(c->self)->prim != 0; }
static void Class_getModifiers(hk_jcall *c) { c->ret.i = (i32)(hk_dvm_mirror_class(c->self)->flags & 0xFFFF); }

static void Class_getSuperclass(hk_jcall *c)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    c->ret.l = k->super && !(k->flags & DVM_ACC_INTERFACE) ? hk_dvm_mirror(k->super) : NULL;
}

static void Class_getComponentType(hk_jcall *c)
{
    hk_dvm_class *k = hk_dvm_mirror_class(c->self);
    c->ret.l = k->component ? hk_dvm_mirror(k->component) : NULL;
}

static void Class_isInstance(hk_jcall *c) { c->ret.z = hk_dvm_is_instance(c->args[0].l, hk_dvm_mirror_class(c->self)); }

static void Class_isAssignableFrom(hk_jcall *c)
{
    if (!c->args[0].l) npe("isAssignableFrom(null)");
    else c->ret.z = hk_dvm_assignable(hk_dvm_mirror_class(c->args[0].l), hk_dvm_mirror_class(c->self));
}

static void Class_forName(hk_jcall *c)
{
    const char *given = hk_dvm_string_utf8(c->args[0].l);
    hk_dvm_class *k;
    char *name, *p;
    if (!given) {
        npe("Class.forName(null)");
        return;
    }
    name = hk_strdup(given);
    for (p = name; *p; p++)
        if (*p == '.') *p = '/';
    /* Nothing defines it: that is ClassNotFoundException here, not the error a missing class is elsewhere. */
    if (!hk_dvm_class_exists(name)) hk_dvm_throw("java/lang/ClassNotFoundException", "%s", given);
    else {
        k = hk_dvm_find_class(name);
        if (k && hk_dvm_ensure_init(k)) c->ret.l = hk_dvm_mirror(k);
    }
    free(name);
}

/* -------------------------------------------------------- java.lang.String */

static void String_length(hk_jcall *c) { c->ret.i = c->self->u.str.len16; }
static void String_isEmpty(hk_jcall *c) { c->ret.z = c->self->u.str.len16 == 0; }

static void String_charAt(hk_jcall *c)
{
    i32 i = c->args[0].i;
    if (i < 0 || i >= c->self->u.str.len16) hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "length=%d; index=%d", (int)c->self->u.str.len16, (int)i);
    else c->ret.c = c->self->u.str.u16[i];
}

static bool same_text(const hk_jobj *a, const hk_jobj *b)
{
    return a->u.str.len16 == b->u.str.len16 && !memcmp(a->u.str.u16, b->u.str.u16, (size_t)a->u.str.len16 * 2);
}

static void String_equals(hk_jcall *c)
{
    hk_jobj *o = c->args[0].l;
    c->ret.z = o == c->self || (o && o->kind == HK_J_STRING && same_text(c->self, o));
}

static void String_hashCode(hk_jcall *c)
{
    u32 h = 0;
    i32 i;
    for (i = 0; i < c->self->u.str.len16; i++) h = 31 * h + c->self->u.str.u16[i];
    c->ret.i = (i32)h;
}

static void String_concat(hk_jcall *c)
{
    const u16 *b;
    i32 nb, na = c->self->u.str.len16;
    hk_jobj *s;
    b = chars_of(c->args[0].l, &nb);
    if (!b) return;
    if (!nb) {
        c->ret.l = c->self;
        return;
    }
    s = hk_dvm_alloc_string(NULL, na + nb);
    if (!s) return;
    memcpy(s->u.str.u16, c->self->u.str.u16, (size_t)na * 2);
    memcpy(s->u.str.u16 + na, b, (size_t)nb * 2);
    c->ret.l = s;
}

static void substring(hk_jcall *c, i32 from, i32 to)
{
    i32 n = c->self->u.str.len16;
    if (from < 0 || to > n || from > to) hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "begin %d, end %d, length %d", (int)from, (int)to, (int)n);
    else c->ret.l = from == 0 && to == n ? c->self : hk_dvm_alloc_string(c->self->u.str.u16 + from, to - from);
}
static void String_substringI(hk_jcall *c) { substring(c, c->args[0].i, c->self->u.str.len16); }
static void String_substringII(hk_jcall *c) { substring(c, c->args[0].i, c->args[1].i); }

static i32 index_of_char(const hk_jobj *s, i32 ch, i32 from)
{
    i32 i;
    if (from < 0) from = 0;
    for (i = from; i < s->u.str.len16; i++)
        if (s->u.str.u16[i] == ch) return i;
    return -1;
}
static void String_indexOfI(hk_jcall *c) { c->ret.i = index_of_char(c->self, c->args[0].i, 0); }
static void String_indexOfII(hk_jcall *c) { c->ret.i = index_of_char(c->self, c->args[0].i, c->args[1].i); }

static void String_lastIndexOfI(hk_jcall *c)
{
    i32 i;
    c->ret.i = -1;
    for (i = c->self->u.str.len16 - 1; i >= 0; i--)
        if (c->self->u.str.u16[i] == c->args[0].i) {
            c->ret.i = i;
            break;
        }
}

static i32 index_of_text(const u16 *a, i32 na, const u16 *b, i32 nb, i32 from)
{
    i32 i;
    if (from < 0) from = 0;
    for (i = from; i + nb <= na; i++)
        if (!memcmp(a + i, b, (size_t)nb * 2)) return i;
    return -1;
}

static void String_indexOfS(hk_jcall *c)
{
    i32 nb;
    const u16 *b = chars_of(c->args[0].l, &nb);
    if (b) c->ret.i = index_of_text(c->self->u.str.u16, c->self->u.str.len16, b, nb, 0);
}

static void String_contains(hk_jcall *c)
{
    hk_jobj *other = c->args[0].l;
    hk_jvalue text;
    i32 nb;
    const u16 *b;
    if (!other) {
        npe("contains(null)");
        return;
    }
    /* Any CharSequence: its toString() is what is looked for. */
    text.l = other;
    if (other->kind != HK_J_STRING && !hk_dvm_call_virtual(other, "toString", "()Ljava/lang/String;", NULL, &text)) return;
    b = chars_of(text.l, &nb);
    if (b) c->ret.z = index_of_text(c->self->u.str.u16, c->self->u.str.len16, b, nb, 0) >= 0;
}

static void String_startsWith(hk_jcall *c)
{
    i32 nb;
    const u16 *b = chars_of(c->args[0].l, &nb);
    if (b) c->ret.z = nb <= c->self->u.str.len16 && !memcmp(c->self->u.str.u16, b, (size_t)nb * 2);
}

static void String_endsWith(hk_jcall *c)
{
    i32 nb, na = c->self->u.str.len16;
    const u16 *b = chars_of(c->args[0].l, &nb);
    if (b) c->ret.z = nb <= na && !memcmp(c->self->u.str.u16 + (na - nb), b, (size_t)nb * 2);
}

static void String_compareTo(hk_jcall *c)
{
    i32 nb, na = c->self->u.str.len16, i;
    const u16 *a = c->self->u.str.u16, *b = chars_of(c->args[0].l, &nb);
    if (!b) return;
    for (i = 0; i < na && i < nb; i++)
        if (a[i] != b[i]) {
            c->ret.i = (i32)a[i] - (i32)b[i];
            return;
        }
    c->ret.i = na - nb;
}

static u16 ascii_lower(u16 ch) { return ch >= 'A' && ch <= 'Z' ? (u16)(ch + 32) : ch; }
static u16 ascii_upper(u16 ch) { return ch >= 'a' && ch <= 'z' ? (u16)(ch - 32) : ch; }

static void String_equalsIgnoreCase(hk_jcall *c)
{
    hk_jobj *o = c->args[0].l;
    i32 i;
    if (!o || o->kind != HK_J_STRING || o->u.str.len16 != c->self->u.str.len16) return;
    for (i = 0; i < o->u.str.len16; i++)
        if (ascii_lower(o->u.str.u16[i]) != ascii_lower(c->self->u.str.u16[i])) return;
    c->ret.z = 1;
}

static void change_case(hk_jcall *c, u16 (*fn)(u16))
{
    hk_jobj *s = hk_dvm_alloc_string(c->self->u.str.u16, c->self->u.str.len16);
    i32 i;
    if (!s) return;
    for (i = 0; i < s->u.str.len16; i++) s->u.str.u16[i] = fn(s->u.str.u16[i]);
    c->ret.l = s;
}
static void String_toLowerCase(hk_jcall *c) { change_case(c, ascii_lower); }
static void String_toUpperCase(hk_jcall *c) { change_case(c, ascii_upper); }

static void String_trim(hk_jcall *c)
{
    i32 a = 0, b = c->self->u.str.len16;
    while (a < b && c->self->u.str.u16[a] <= ' ') a++;
    while (b > a && c->self->u.str.u16[b - 1] <= ' ') b--;
    substring(c, a, b);
}

static void String_replaceCC(hk_jcall *c)
{
    hk_jobj *s = hk_dvm_alloc_string(c->self->u.str.u16, c->self->u.str.len16);
    i32 i;
    if (!s) return;
    for (i = 0; i < s->u.str.len16; i++)
        if (s->u.str.u16[i] == c->args[0].c) s->u.str.u16[i] = c->args[1].c;
    c->ret.l = s;
}

static void String_toCharArray(hk_jcall *c)
{
    hk_jobj *a = hk_dvm_new_array("[C", c->self->u.str.len16);
    if (a) memcpy(a->u.arr.data, c->self->u.str.u16, (size_t)c->self->u.str.len16 * 2);
    c->ret.l = a;
}

static void String_getBytes(hk_jcall *c)
{
    const char *utf8 = hk_dvm_string_utf8(c->self);
    const u16 *u = c->self->u.str.u16;
    i32 n = c->self->u.str.len16, i, k = 0;
    hk_jobj *a;
    u8 *out;
    /* UTF-8 proper: a zero character is one zero byte here, where the kept C string writes two. */
    size_t len = strlen(utf8);
    for (i = 0; i < n; i++)
        if (!u[i]) len--;
    a = hk_dvm_new_array("[B", (i32)len);
    if (!a) return;
    out = (u8 *)a->u.arr.data;
    for (i = 0; utf8[i]; i++) {
        if ((u8)utf8[i] == 0xC0 && (u8)utf8[i + 1] == 0x80) {
            out[k++] = 0;
            i++;
        } else out[k++] = (u8)utf8[i];
    }
    c->ret.l = a;
}

static void String_intern(hk_jcall *c) { c->ret.l = hk_dvm_intern_string(c->self); }

static void String_valueOfI(hk_jcall *c)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)c->args[0].i);
    c->ret.l = string_from_ascii(text);
}
static void String_valueOfJ(hk_jcall *c)
{
    char text[24];
    snprintf(text, sizeof(text), "%lld", (long long)c->args[0].j);
    c->ret.l = string_from_ascii(text);
}
static void String_valueOfC(hk_jcall *c) { c->ret.l = hk_dvm_alloc_string(&c->args[0].c, 1); }
static void String_valueOfZ(hk_jcall *c) { c->ret.l = string_from_ascii(c->args[0].z ? "true" : "false"); }
static void String_valueOfF(hk_jcall *c)
{
    char text[40];
    format_real((double)c->args[0].f, true, text, sizeof(text));
    c->ret.l = string_from_ascii(text);
}
static void String_valueOfD(hk_jcall *c)
{
    char text[40];
    format_real(c->args[0].d, false, text, sizeof(text));
    c->ret.l = string_from_ascii(text);
}
static void String_valueOfChars(hk_jcall *c)
{
    hk_jobj *a = c->args[0].l;
    if (!a) npe("String.valueOf((char[]) null)");
    else c->ret.l = hk_dvm_alloc_string((const u16 *)a->u.arr.data, (i32)a->u.arr.len);
}

bool hk_dvm_boot_string_init(const char *sig, const hk_jvalue *args, hk_jobj **out)
{
    hk_jobj *a = args ? args[0].l : NULL, *s;
    i32 from = 0, count, n;
    *out = NULL;
    if (!strcmp(sig, "()V")) {
        *out = hk_dvm_alloc_string(NULL, 0);
        return *out != NULL;
    }
    if (!a) return npe("new String(null)");
    if (!strcmp(sig, "(Ljava/lang/String;)V")) {
        *out = hk_dvm_alloc_string(a->u.str.u16, a->u.str.len16);
        return *out != NULL;
    }
    if (!strcmp(sig, "(Ljava/lang/StringBuilder;)V")) {
        hk_jvalue text;
        if (!hk_dvm_call_virtual(a, "toString", "()Ljava/lang/String;", NULL, &text)) return false;
        *out = text.l;
        return true;
    }
    if (sig[1] != '[' || (sig[2] != 'C' && sig[2] != 'B')) return hk_dvm_throw("java/lang/InternalError", "the String constructor %s is not implemented", sig);
    count = n = (i32)a->u.arr.len;
    if (sig[3] == 'I' && sig[4] == 'I') {
        from = args[1].i;
        count = args[2].i;
        if (from < 0 || count < 0 || from > n - count)
            return hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "offset %d, count %d, length %d", (int)from, (int)count, (int)n);
    }
    if (sig[2] == 'C') {
        *out = hk_dvm_alloc_string((const u16 *)a->u.arr.data + from, count);
        return *out != NULL;
    }
    /* Bytes: UTF-8, whatever charset is named -- the default on Android, and the only one decoded here. */
    s = hk_dvm_alloc_string(NULL, utf8_bytes_to_utf16((const u8 *)a->u.arr.data + from, count, NULL));
    if (!s) return false;
    utf8_bytes_to_utf16((const u8 *)a->u.arr.data + from, count, s->u.str.u16);
    *out = s;
    return true;
}

/* ------------------------------------------------- java.lang.StringBuilder */

static hk_dvm_field *g_sb_value, *g_sb_count;

/* Appends to a StringBuilder's char[], which grows by doubling. False when a new array could not be made. */
static bool sb_append(hk_jobj *sb, const u16 *chars, i32 n)
{
    hk_jvalue *slots = hk_dvm_slots(sb);
    hk_jobj *value = slots[g_sb_value->slot].l;
    i32 count = slots[g_sb_count->slot].i, cap = value ? (i32)value->u.arr.len : 0;
    if (count + n > cap) {
        hk_jobj *grown = hk_dvm_new_array("[C", count + n > cap * 2 + 2 ? count + n : cap * 2 + 2);
        if (!grown) return false;
        if (count) memcpy(grown->u.arr.data, value->u.arr.data, (size_t)count * 2);
        slots[g_sb_value->slot].l = value = grown;
    }
    if (n) memcpy((u16 *)value->u.arr.data + count, chars, (size_t)n * 2);
    slots[g_sb_count->slot].i = count + n;
    return true;
}

static void sb_append_ascii(hk_jcall *c, const char *text)
{
    u16 wide[48];
    i32 n = 0;
    while (text[n] && n < (i32)HK_COUNT(wide)) {
        wide[n] = (u8)text[n];
        n++;
    }
    if (sb_append(c->self, wide, n)) c->ret.l = c->self;
}

static void SB_appendS(hk_jcall *c)
{
    hk_jobj *s = c->args[0].l;
    if (!s) sb_append_ascii(c, "null");
    else if (sb_append(c->self, s->u.str.u16, s->u.str.len16)) c->ret.l = c->self;
}
static void SB_appendC(hk_jcall *c) { if (sb_append(c->self, &c->args[0].c, 1)) c->ret.l = c->self; }
static void SB_appendZ(hk_jcall *c) { sb_append_ascii(c, c->args[0].z ? "true" : "false"); }
static void SB_appendI(hk_jcall *c)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", (int)c->args[0].i);
    sb_append_ascii(c, text);
}
static void SB_appendJ(hk_jcall *c)
{
    char text[24];
    snprintf(text, sizeof(text), "%lld", (long long)c->args[0].j);
    sb_append_ascii(c, text);
}
static void SB_appendF(hk_jcall *c)
{
    char text[40];
    format_real((double)c->args[0].f, true, text, sizeof(text));
    sb_append_ascii(c, text);
}
static void SB_appendD(hk_jcall *c)
{
    char text[40];
    format_real(c->args[0].d, false, text, sizeof(text));
    sb_append_ascii(c, text);
}
static void SB_appendChars(hk_jcall *c)
{
    hk_jobj *a = c->args[0].l;
    if (!a) npe("append((char[]) null)");
    else if (sb_append(c->self, (const u16 *)a->u.arr.data, (i32)a->u.arr.len)) c->ret.l = c->self;
}

static void SB_toString(hk_jcall *c)
{
    hk_jvalue *slots = hk_dvm_slots(c->self);
    hk_jobj *value = slots[g_sb_value->slot].l;
    c->ret.l = hk_dvm_alloc_string(value ? (const u16 *)value->u.arr.data : NULL, slots[g_sb_count->slot].i);
}

static void SB_charAt(hk_jcall *c)
{
    hk_jvalue *slots = hk_dvm_slots(c->self);
    i32 i = c->args[0].i, count = slots[g_sb_count->slot].i;
    if (i < 0 || i >= count) hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "length=%d; index=%d", (int)count, (int)i);
    else c->ret.c = ((const u16 *)slots[g_sb_value->slot].l->u.arr.data)[i];
}

static void SB_setLength(hk_jcall *c)
{
    hk_jvalue *slots = hk_dvm_slots(c->self);
    i32 want = c->args[0].i, count = slots[g_sb_count->slot].i;
    static const u16 zeros[32];
    if (want < 0) {
        hk_dvm_throw("java/lang/StringIndexOutOfBoundsException", "length %d", (int)want);
        return;
    }
    while (count < want) {
        i32 step = want - count < (i32)HK_COUNT(zeros) ? want - count : (i32)HK_COUNT(zeros);
        if (!sb_append(c->self, zeros, step)) return;
        count += step;
    }
    slots[g_sb_count->slot].i = want;
}

/* ----------------------------------------------------- java.lang.Throwable */

static void Throwable_fillInStackTrace(hk_jcall *c)
{
    if (g_dvm.f_backtrace) hk_dvm_slots(c->self)[g_dvm.f_backtrace->slot].l = hk_dvm_capture_stack();
    c->ret.l = c->self;
}

static void Throwable_toString(hk_jcall *c)
{
    hk_jvalue msg;
    const char *text;
    char *line, *p;
    /* The message as the exception itself gives it, which a class may have replaced. */
    if (!hk_dvm_call_virtual(c->self, "getLocalizedMessage", "()Ljava/lang/String;", NULL, &msg)) return;
    text = hk_dvm_string_utf8(msg.l);
    line = text ? hk_sprintf("%s: %s", hk_dvm_class_of(c->self)->name, text) : hk_strdup(hk_dvm_class_of(c->self)->name);
    for (p = line; *p && *p != ':'; p++)
        if (*p == '/') *p = '.';
    c->ret.l = hk_dvm_new_string(line);
    free(line);
}

static void Throwable_printStackTrace(hk_jcall *c)
{
    hk_jobj *before = hk_dvm_pending();
    char line[400], *frames = (char *)hk_alloc(8192), *p, *nl;
    /* The describing functions work on what is pending: make this one pending for as long as it takes. */
    hk_dvm_throw_object(c->self);
    hk_warn("%s", hk_dvm_describe_pending(line, sizeof(line)) ? line : "?");
    hk_dvm_pending_stack(frames, 8192);
    for (p = frames; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        hk_warn("%s", p);
    }
    free(frames);
    hk_dvm_throw_object(before);
}

/* ---------------------------------------------------------- java.lang.Enum */

static void Enum_valueOf(hk_jcall *c)
{
    hk_jobj *mirror = c->args[0].l, *wanted = c->args[1].l, *values;
    hk_dvm_class *k;
    hk_dvm_method *m;
    hk_dvm_field *name_field;
    hk_jvalue r;
    char *sig;
    u32 i;
    if (!mirror || !wanted) {
        npe("Enum.valueOf(null)");
        return;
    }
    k = hk_dvm_mirror_class(mirror);
    /* Every enum has a static values() the compiler wrote; the constant is looked for among what it returns. */
    sig = hk_sprintf("()[L%s;", k->name);
    m = hk_dvm_find_method(k, "values", sig, true);
    free(sig);
    name_field = hk_dvm_find_field(k, "name", false);
    if (!m || !name_field || !hk_dvm_ensure_init(k)) {
        if (!hk_dvm_pending()) hk_dvm_throw("java/lang/IllegalArgumentException", "%s is not an enum type", k->name);
        return;
    }
    if (!hk_dvm_invoke(m, NULL, NULL, &r)) return;
    values = r.l;
    for (i = 0; values && i < values->u.oarr.len; i++) {
        hk_jobj *e = values->u.oarr.v[i], *n = e ? hk_dvm_slots(e)[name_field->slot].l : NULL;
        if (n && same_text(n, wanted)) {
            c->ret.l = e;
            return;
        }
    }
    hk_dvm_throw("java/lang/IllegalArgumentException", "No enum constant %s.%s", k->name, hk_dvm_string_utf8(wanted));
}

/* ------------------------------------------- numbers, java.lang.Math, System */

static void Integer_parseInt(hk_jcall *c)
{
    i32 n, i = 0;
    const u16 *s = chars_of(c->args[0].l, &n);
    i64 v = 0;
    bool neg;
    if (!s) return;
    neg = n > 0 && s[0] == '-';
    if (neg || (n > 0 && s[0] == '+')) i = 1;
    if (i >= n) goto bad;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') goto bad;
        v = v * 10 + (s[i] - '0');
        if (v > (i64)INT32_MAX + 1) goto bad;
    }
    if (neg) v = -v;
    if (v > INT32_MAX) goto bad;
    c->ret.i = (i32)v;
    return;
bad:
    hk_dvm_throw("java/lang/NumberFormatException", "For input string: \"%s\"", hk_dvm_string_utf8(c->args[0].l));
}

static void Integer_toHexString(hk_jcall *c)
{
    char text[16];
    snprintf(text, sizeof(text), "%x", (unsigned)c->args[0].i);
    c->ret.l = string_from_ascii(text);
}

static void Float_floatToRawIntBits(hk_jcall *c) { c->ret.i = (i32)hk_fb(c->args[0].f); }
static void Float_intBitsToFloat(hk_jcall *c) { c->ret.f = hk_f((u32)c->args[0].i); }
static void Double_doubleToRawLongBits(hk_jcall *c) { c->ret.j = (i64)hk_db(c->args[0].d); }
static void Double_longBitsToDouble(hk_jcall *c) { c->ret.d = hk_d((u64)c->args[0].j); }

static void Math_absI(hk_jcall *c) { c->ret.i = c->args[0].i < 0 ? (i32)(0u - (u32)c->args[0].i) : c->args[0].i; }
static void Math_absJ(hk_jcall *c) { c->ret.j = c->args[0].j < 0 ? (i64)((u64)0 - (u64)c->args[0].j) : c->args[0].j; }
static void Math_absF(hk_jcall *c) { c->ret.f = (float)fabs((double)c->args[0].f); }
static void Math_absD(hk_jcall *c) { c->ret.d = fabs(c->args[0].d); }
static void Math_maxI(hk_jcall *c) { c->ret.i = c->args[0].i > c->args[1].i ? c->args[0].i : c->args[1].i; }
static void Math_minI(hk_jcall *c) { c->ret.i = c->args[0].i < c->args[1].i ? c->args[0].i : c->args[1].i; }
static void Math_maxJ(hk_jcall *c) { c->ret.j = c->args[0].j > c->args[1].j ? c->args[0].j : c->args[1].j; }
static void Math_minJ(hk_jcall *c) { c->ret.j = c->args[0].j < c->args[1].j ? c->args[0].j : c->args[1].j; }
/* As Java orders them: a NaN wins, and of the two zeros the negative is the smaller. */
static double real_max(double a, double b) { return a != a || b != b ? a + b : a > b ? a : b > a ? b : (hk_db(a) >> 63) ? b : a; }
static double real_min(double a, double b) { return a != a || b != b ? a + b : a < b ? a : b < a ? b : (hk_db(a) >> 63) ? a : b; }
static void Math_maxF(hk_jcall *c) { c->ret.f = (float)real_max((double)c->args[0].f, (double)c->args[1].f); }
static void Math_minF(hk_jcall *c) { c->ret.f = (float)real_min((double)c->args[0].f, (double)c->args[1].f); }
static void Math_maxD(hk_jcall *c) { c->ret.d = real_max(c->args[0].d, c->args[1].d); }
static void Math_minD(hk_jcall *c) { c->ret.d = real_min(c->args[0].d, c->args[1].d); }
static void Math_sqrt(hk_jcall *c) { c->ret.d = sqrt(c->args[0].d); }
static void Math_floor(hk_jcall *c) { c->ret.d = floor(c->args[0].d); }
static void Math_ceil(hk_jcall *c) { c->ret.d = ceil(c->args[0].d); }
static void Math_pow(hk_jcall *c) { c->ret.d = pow(c->args[0].d, c->args[1].d); }

static void System_arraycopy(hk_jcall *c) { hk_dvm_arraycopy(c->args[0].l, c->args[1].i, c->args[2].l, c->args[3].i, c->args[4].i); }
static void System_currentTimeMillis(hk_jcall *c) { c->ret.j = (i64)(hk_time_realtime_ns() / 1000000ull); }
static void System_nanoTime(hk_jcall *c) { c->ret.j = (i64)hk_time_monotonic_ns(); }
static void System_identityHashCode(hk_jcall *c) { c->ret.i = c->args[0].l ? (i32)hk_dvm_identity_hash(c->args[0].l) : 0; }

/* ------------------------------------------------------------------ the table */

static const hk_jhle k_boot_natives[] = {
    { "java/lang/Object", "getClass", "()Ljava/lang/Class;", Object_getClass },
    { "java/lang/Object", "hashCode", "()I", Object_hashCode },
    { "java/lang/Object", "toString", "()Ljava/lang/String;", Object_toString },
    { "java/lang/Object", "clone", "()Ljava/lang/Object;", Object_clone },
    { "java/lang/Object", "notify", "()V", Object_notify },
    { "java/lang/Object", "notifyAll", "()V", Object_notifyAll },
    { "java/lang/Object", "wait", "()V", Object_wait },
    { "java/lang/Object", "wait", "(J)V", Object_waitJ },

    { "java/lang/Class", "getName", "()Ljava/lang/String;", Class_getName },
    { "java/lang/Class", "toString", "()Ljava/lang/String;", Class_toString },
    { "java/lang/Class", "getSimpleName", "()Ljava/lang/String;", Class_getSimpleName },
    { "java/lang/Class", "isInterface", "()Z", Class_isInterface },
    { "java/lang/Class", "isArray", "()Z", Class_isArray },
    { "java/lang/Class", "isPrimitive", "()Z", Class_isPrimitive },
    { "java/lang/Class", "getModifiers", "()I", Class_getModifiers },
    { "java/lang/Class", "getSuperclass", "()Ljava/lang/Class;", Class_getSuperclass },
    { "java/lang/Class", "getComponentType", "()Ljava/lang/Class;", Class_getComponentType },
    { "java/lang/Class", "isInstance", "(Ljava/lang/Object;)Z", Class_isInstance },
    { "java/lang/Class", "isAssignableFrom", "(Ljava/lang/Class;)Z", Class_isAssignableFrom },
    { "java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", Class_forName },

    { "java/lang/String", "length", "()I", String_length },
    { "java/lang/String", "isEmpty", "()Z", String_isEmpty },
    { "java/lang/String", "charAt", "(I)C", String_charAt },
    { "java/lang/String", "equals", "(Ljava/lang/Object;)Z", String_equals },
    { "java/lang/String", "hashCode", "()I", String_hashCode },
    { "java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;", String_concat },
    { "java/lang/String", "substring", "(I)Ljava/lang/String;", String_substringI },
    { "java/lang/String", "substring", "(II)Ljava/lang/String;", String_substringII },
    { "java/lang/String", "indexOf", "(I)I", String_indexOfI },
    { "java/lang/String", "indexOf", "(II)I", String_indexOfII },
    { "java/lang/String", "indexOf", "(Ljava/lang/String;)I", String_indexOfS },
    { "java/lang/String", "lastIndexOf", "(I)I", String_lastIndexOfI },
    { "java/lang/String", "contains", "(Ljava/lang/CharSequence;)Z", String_contains },
    { "java/lang/String", "startsWith", "(Ljava/lang/String;)Z", String_startsWith },
    { "java/lang/String", "endsWith", "(Ljava/lang/String;)Z", String_endsWith },
    { "java/lang/String", "compareTo", "(Ljava/lang/String;)I", String_compareTo },
    { "java/lang/String", "equalsIgnoreCase", "(Ljava/lang/String;)Z", String_equalsIgnoreCase },
    { "java/lang/String", "toLowerCase", "()Ljava/lang/String;", String_toLowerCase },
    { "java/lang/String", "toUpperCase", "()Ljava/lang/String;", String_toUpperCase },
    { "java/lang/String", "trim", "()Ljava/lang/String;", String_trim },
    { "java/lang/String", "replace", "(CC)Ljava/lang/String;", String_replaceCC },
    { "java/lang/String", "toCharArray", "()[C", String_toCharArray },
    { "java/lang/String", "getBytes", "()[B", String_getBytes },
    { "java/lang/String", "intern", "()Ljava/lang/String;", String_intern },
    { "java/lang/String", "valueOf", "(I)Ljava/lang/String;", String_valueOfI },
    { "java/lang/String", "valueOf", "(J)Ljava/lang/String;", String_valueOfJ },
    { "java/lang/String", "valueOf", "(C)Ljava/lang/String;", String_valueOfC },
    { "java/lang/String", "valueOf", "(Z)Ljava/lang/String;", String_valueOfZ },
    { "java/lang/String", "valueOf", "(F)Ljava/lang/String;", String_valueOfF },
    { "java/lang/String", "valueOf", "(D)Ljava/lang/String;", String_valueOfD },
    { "java/lang/String", "valueOf", "([C)Ljava/lang/String;", String_valueOfChars },

    { "java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", SB_appendS },
    { "java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", SB_appendC },
    { "java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", SB_appendZ },
    { "java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", SB_appendI },
    { "java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", SB_appendJ },
    { "java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", SB_appendF },
    { "java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", SB_appendD },
    { "java/lang/StringBuilder", "append", "([C)Ljava/lang/StringBuilder;", SB_appendChars },
    { "java/lang/StringBuilder", "toString", "()Ljava/lang/String;", SB_toString },
    { "java/lang/StringBuilder", "charAt", "(I)C", SB_charAt },
    { "java/lang/StringBuilder", "setLength", "(I)V", SB_setLength },

    { "java/lang/Throwable", "fillInStackTrace", "()Ljava/lang/Throwable;", Throwable_fillInStackTrace },
    { "java/lang/Throwable", "toString", "()Ljava/lang/String;", Throwable_toString },
    { "java/lang/Throwable", "printStackTrace", "()V", Throwable_printStackTrace },

    { "java/lang/Enum", "valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", Enum_valueOf },

    { "java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", Integer_parseInt },
    { "java/lang/Integer", "toHexString", "(I)Ljava/lang/String;", Integer_toHexString },
    { "java/lang/Float", "floatToRawIntBits", "(F)I", Float_floatToRawIntBits },
    { "java/lang/Float", "intBitsToFloat", "(I)F", Float_intBitsToFloat },
    { "java/lang/Double", "doubleToRawLongBits", "(D)J", Double_doubleToRawLongBits },
    { "java/lang/Double", "longBitsToDouble", "(J)D", Double_longBitsToDouble },

    { "java/lang/Math", "abs", "(I)I", Math_absI },
    { "java/lang/Math", "abs", "(J)J", Math_absJ },
    { "java/lang/Math", "abs", "(F)F", Math_absF },
    { "java/lang/Math", "abs", "(D)D", Math_absD },
    { "java/lang/Math", "max", "(II)I", Math_maxI },
    { "java/lang/Math", "min", "(II)I", Math_minI },
    { "java/lang/Math", "max", "(JJ)J", Math_maxJ },
    { "java/lang/Math", "min", "(JJ)J", Math_minJ },
    { "java/lang/Math", "max", "(FF)F", Math_maxF },
    { "java/lang/Math", "min", "(FF)F", Math_minF },
    { "java/lang/Math", "max", "(DD)D", Math_maxD },
    { "java/lang/Math", "min", "(DD)D", Math_minD },
    { "java/lang/Math", "sqrt", "(D)D", Math_sqrt },
    { "java/lang/Math", "floor", "(D)D", Math_floor },
    { "java/lang/Math", "ceil", "(D)D", Math_ceil },
    { "java/lang/Math", "pow", "(DD)D", Math_pow },

    { "java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", System_arraycopy },
    { "java/lang/System", "currentTimeMillis", "()J", System_currentTimeMillis },
    { "java/lang/System", "nanoTime", "()J", System_nanoTime },
    { "java/lang/System", "identityHashCode", "(Ljava/lang/Object;)I", System_identityHashCode },
    { NULL, NULL, NULL, NULL },
};

bool hk_dvm_boot_add(void)
{
    hk_dvm_class *sb;
    hk_dvm_register(k_boot_natives);
    if (!hk_dvm_add_dex(k_dvm_boot_dex, k_dvm_boot_dex_size, "boot.dex")) return false;
    /* The natives above reach into StringBuilder's two fields; found once. */
    sb = hk_dvm_find_class("java/lang/StringBuilder");
    g_sb_value = sb ? hk_dvm_find_field(sb, "value", false) : NULL;
    g_sb_count = sb ? hk_dvm_find_field(sb, "count", false) : NULL;
    if (!g_sb_value || !g_sb_count) {
        hk_err("dvm: the built-in java.lang is damaged (no StringBuilder)");
        hk_dvm_clear_pending();
        return false;
    }
    return true;
}
