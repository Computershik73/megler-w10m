/*
 * JNI for guest native code, over a Java world that is implemented here.
 *
 * Native code reaches Java through a JNIEnv: it looks classes up by name,
 * methods and fields by name and signature, and calls them. No Java runs, so
 * each method the guest calls is either one a driver implements in C (an
 * "HLE" entry, registered with mg_jni_register) or nothing: it logs once and
 * returns zero.
 *
 * Objects handed to the guest (jobject, jclass, jstring, arrays) are mg_jobj
 * pointers; references are counted, and deleting a local or global reference
 * releases one.
 */
#ifndef MG_JNI_H
#define MG_JNI_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_jobj mg_jobj;
typedef struct mg_jclass mg_jclass;

typedef union mg_jvalue {
    u8 z;
    i8 b;
    u16 c;
    i16 s;
    i32 i;
    i64 j;
    float f;
    double d;
    mg_jobj *l;
} mg_jvalue;

enum { MG_J_OBJECT, MG_J_CLASS, MG_J_STRING, MG_J_PRIM_ARRAY, MG_J_OBJ_ARRAY };

typedef struct mg_jfield_value {
    char *name;
    mg_jvalue v;
    bool is_object;
} mg_jfield_value;

struct mg_jobj {
    volatile i32 refs;
    u8 kind;
    bool permanent;                    /* a class, a singleton: never freed */
    bool dvm;                          /* made by the Dalvik runtime (dvm/): its class, monitor and contents follow this struct */
    mg_jclass *cls;
    union {
        struct { char *utf8; u16 *u16; i32 len16; } str;
        struct { void *data; u32 len; u8 esz; char etype; } arr;
        struct { mg_jobj **v; u32 len; } oarr;
        struct { mg_jclass *jc; } klass;
    } u;
    mg_jfield_value *fields;           /* instance fields, by name, made on first use */
    int nfields;
    void *native;                      /* an implementation's own state */
};

/* What an implemented Java method receives. */
typedef struct mg_jcall {
    mg_jobj *self;                     /* NULL for a static method */
    mg_jclass *cls;
    const char *name, *sig;
    const mg_jvalue *args;
    mg_jvalue ret;                     /* written by the method; an object returned is a new reference */
} mg_jcall;

typedef void (*mg_jhle_fn)(mg_jcall *c);

typedef struct mg_jhle {
    const char *cls, *name, *sig;      /* sig NULL: any signature */
    mg_jhle_fn fn;
} mg_jhle;

void mg_jni_init(void);
/* A table of implemented methods, ended by an entry with cls == NULL. Later tables win over earlier ones. */
void mg_jni_register(const mg_jhle *table);
/* Declare a class with its superclass (JNI names, "android/app/Activity"). Safe to repeat. */
mg_jclass *mg_jni_class(const char *name, const char *super);
const char *mg_jni_class_name(const mg_jobj *o);

void *mg_jni_vm(void);                 /* JavaVM * */
void *mg_jni_env(void);                /* JNIEnv * */

mg_jobj *mg_jni_new_object(const char *cls);
mg_jobj *mg_jni_new_string(const char *utf8);
const char *mg_jni_string(mg_jobj *s);             /* NULL for NULL or not a string */
/* Gives a string object its text: what a constructor of java.lang.String does. From UTF-8, or from `len` UTF-16 code units. */
void mg_jni_string_set(mg_jobj *s, const char *utf8);
void mg_jni_string_set_utf16(mg_jobj *s, const u16 *chars, i32 len);
/* The string's UTF-16 code units (kept by the string) and how many. */
const u16 *mg_jni_string_utf16(mg_jobj *s, i32 *len);
mg_jobj *mg_jni_new_prim_array(char etype, u32 len);
/* An array of `len` nulls of the class; the caller fills u.oarr.v, each element a reference the array then owns. */
mg_jobj *mg_jni_new_object_array(const char *element_class, u32 len);
/* A java.nio.ByteBuffer over memory of the caller's, as NewDirectByteBuffer makes for native code. */
mg_jobj *mg_jni_new_direct_buffer(void *address, i64 capacity);
mg_jobj *mg_jni_class_object(const char *name);    /* the jclass */
mg_jobj *mg_jni_ref(mg_jobj *o);
void mg_jni_unref(mg_jobj *o);

void mg_jni_set_static(const char *cls, const char *name, char kind, mg_jvalue v);
/* A field of one object, for native code that reads it with Get<Type>Field. */
void mg_jni_set_field(mg_jobj *o, const char *name, char kind, mg_jvalue v);
/* What the field holds; zero when the object has no such field yet. An object read is not a new reference. */
mg_jvalue mg_jni_get_field(mg_jobj *o, const char *name);
void mg_jni_throw(const char *cls, const char *msg);
/* Whether a Java exception is waiting on the calling thread (thrown by native code, or by mg_jni_throw); clearing says whether one was. */
bool mg_jni_exception_clear(void);

/*
 * A java.lang.reflect.Method for a method of a class: the object that
 * FromReflectedMethod turns back into the same id GetMethodID gives for it.
 * One object per method, kept for good.
 */
mg_jobj *mg_jni_reflect_method(const char *cls, const char *name, const char *sig, bool is_static);
/* The name and the signature of the method a Method object stands for; NULL when it stands for none. */
const char *mg_jni_reflected_name(const mg_jobj *method);
const char *mg_jni_reflected_sig(const mg_jobj *method);

/* The native method a library registered (RegisterNatives) or exports under its JNI name; NULL when there is none. */
void *mg_jni_native(const char *cls, const char *name, const char *sig);
/*
 * Of the classes a DEX file defines, while the Dalvik runtime runs: how many
 * native methods libraries have registered, how many entries of their tables
 * named no native method of the class (said in the log and left out), and how
 * many methods were not registered but found by their JNI names (Java_...).
 */
void mg_jni_native_counts(i32 *registered, i32 *unknown, i32 *by_name);
/*
 * Call a native method of the guest's: `fn` with (env, self-or-class, args...).
 * `sig` is the Java signature; the arguments are marshalled the way Android's
 * 32-bit ARM ABI passes them. The result is in the returned value's member
 * for the signature's return type.
 */
mg_jvalue mg_jni_call_native(void *fn, mg_jobj *self_or_class, const char *sig, const mg_jvalue *args);

#ifdef __cplusplus
}
#endif

#endif
