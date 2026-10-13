/*
 * JNI for guest native code, over a Java world that is implemented here.
 *
 * Native code reaches Java through a JNIEnv: it looks classes up by name,
 * methods and fields by name and signature, and calls them. No Java runs, so
 * each method the guest calls is either one a driver implements in C (an
 * "HLE" entry, registered with hk_jni_register) or nothing: it logs once and
 * returns zero.
 *
 * Objects handed to the guest (jobject, jclass, jstring, arrays) are hk_jobj
 * pointers; references are counted, and deleting a local or global reference
 * releases one.
 */
#ifndef HK_JNI_H
#define HK_JNI_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_jobj hk_jobj;
typedef struct hk_jclass hk_jclass;

typedef union hk_jvalue {
    u8 z;
    i8 b;
    u16 c;
    i16 s;
    i32 i;
    i64 j;
    float f;
    double d;
    hk_jobj *l;
} hk_jvalue;

enum { HK_J_OBJECT, HK_J_CLASS, HK_J_STRING, HK_J_PRIM_ARRAY, HK_J_OBJ_ARRAY };

typedef struct hk_jfield_value {
    char *name;
    hk_jvalue v;
    bool is_object;
} hk_jfield_value;

struct hk_jobj {
    volatile i32 refs;
    u8 kind;
    bool permanent;                    /* a class, a singleton: never freed */
    bool dvm;                          /* made by the Dalvik runtime (dvm/): its class, monitor and contents follow this struct */
    hk_jclass *cls;
    union {
        struct { char *utf8; u16 *u16; i32 len16; } str;
        struct { void *data; u32 len; u8 esz; char etype; } arr;
        struct { hk_jobj **v; u32 len; } oarr;
        struct { hk_jclass *jc; } klass;
    } u;
    hk_jfield_value *fields;           /* instance fields, by name, made on first use */
    int nfields;
    void *native;                      /* an implementation's own state */
};

/* What an implemented Java method receives. */
typedef struct hk_jcall {
    hk_jobj *self;                     /* NULL for a static method */
    hk_jclass *cls;
    const char *name, *sig;
    const hk_jvalue *args;
    hk_jvalue ret;                     /* written by the method; an object returned is a new reference */
} hk_jcall;

typedef void (*hk_jhle_fn)(hk_jcall *c);

typedef struct hk_jhle {
    const char *cls, *name, *sig;      /* sig NULL: any signature */
    hk_jhle_fn fn;
} hk_jhle;

void hk_jni_init(void);
/* A table of implemented methods, ended by an entry with cls == NULL. Later tables win over earlier ones. */
void hk_jni_register(const hk_jhle *table);
/* Declare a class with its superclass (JNI names, "android/app/Activity"). Safe to repeat. */
hk_jclass *hk_jni_class(const char *name, const char *super);
const char *hk_jni_class_name(const hk_jobj *o);

void *hk_jni_vm(void);                 /* JavaVM * */
void *hk_jni_env(void);                /* JNIEnv * */

hk_jobj *hk_jni_new_object(const char *cls);
hk_jobj *hk_jni_new_string(const char *utf8);
const char *hk_jni_string(hk_jobj *s);             /* NULL for NULL or not a string */
/* Gives a string object its text: what a constructor of java.lang.String does. From UTF-8, or from `len` UTF-16 code units. */
void hk_jni_string_set(hk_jobj *s, const char *utf8);
void hk_jni_string_set_utf16(hk_jobj *s, const u16 *chars, i32 len);
/* The string's UTF-16 code units (kept by the string) and how many. */
const u16 *hk_jni_string_utf16(hk_jobj *s, i32 *len);
hk_jobj *hk_jni_new_prim_array(char etype, u32 len);
/* An array of `len` nulls of the class; the caller fills u.oarr.v, each element a reference the array then owns. */
hk_jobj *hk_jni_new_object_array(const char *element_class, u32 len);
/* A java.nio.ByteBuffer over memory of the caller's, as NewDirectByteBuffer makes for native code. */
hk_jobj *hk_jni_new_direct_buffer(void *address, i64 capacity);
hk_jobj *hk_jni_class_object(const char *name);    /* the jclass */
hk_jobj *hk_jni_ref(hk_jobj *o);
void hk_jni_unref(hk_jobj *o);

void hk_jni_set_static(const char *cls, const char *name, char kind, hk_jvalue v);
/* A field of one object, for native code that reads it with Get<Type>Field. */
void hk_jni_set_field(hk_jobj *o, const char *name, char kind, hk_jvalue v);
/* What the field holds; zero when the object has no such field yet. An object read is not a new reference. */
hk_jvalue hk_jni_get_field(hk_jobj *o, const char *name);
void hk_jni_throw(const char *cls, const char *msg);
/* Whether a Java exception is waiting on the calling thread (thrown by native code, or by hk_jni_throw); clearing says whether one was. */
bool hk_jni_exception_clear(void);

/*
 * A java.lang.reflect.Method for a method of a class: the object that
 * FromReflectedMethod turns back into the same id GetMethodID gives for it.
 * One object per method, kept for good.
 */
hk_jobj *hk_jni_reflect_method(const char *cls, const char *name, const char *sig, bool is_static);
/* The name and the signature of the method a Method object stands for; NULL when it stands for none. */
const char *hk_jni_reflected_name(const hk_jobj *method);
const char *hk_jni_reflected_sig(const hk_jobj *method);

/* The native method a library registered (RegisterNatives) or exports under its JNI name; NULL when there is none. */
void *hk_jni_native(const char *cls, const char *name, const char *sig);
/*
 * Of the classes a DEX file defines, while the Dalvik runtime runs: how many
 * native methods libraries have registered, how many entries of their tables
 * named no native method of the class (said in the log and left out), and how
 * many methods were not registered but found by their JNI names (Java_...).
 */
void hk_jni_native_counts(i32 *registered, i32 *unknown, i32 *by_name);
/*
 * Call a native method of the guest's: `fn` with (env, self-or-class, args...).
 * `sig` is the Java signature; the arguments are marshalled the way Android's
 * 32-bit ARM ABI passes them. The result is in the returned value's member
 * for the signature's return type.
 */
hk_jvalue hk_jni_call_native(void *fn, hk_jobj *self_or_class, const char *sig, const hk_jvalue *args);

#ifdef __cplusplus
}
#endif

#endif
