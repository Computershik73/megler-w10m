/*
 * A guest library of native methods, for the Dalvik runtime's tests: the class
 * t.Guest (core/tests/dvm/fixtures/a/Guest.smali) declares them, bytecode and
 * the tests call them, and they call back into bytecode through the JNIEnv.
 *
 * Built like any app's library: softfp, so a double arrives in two integer
 * registers and a float in one. jni.h is not used; the environment is a
 * pointer to a table of functions, and each is called by its place in it.
 */
typedef void *jobject;
typedef void **JNIEnv;                 /* JNIEnv *env; (*env)[n] is the n-th function */
#define F(env, n, type) ((type)(*(env))[n])

static jobject find_class(JNIEnv *env, const char *name) { return F(env, 6, jobject (*)(JNIEnv *, const char *))(env, name); }
static jobject new_string(JNIEnv *env, const char *text) { return F(env, 167, jobject (*)(JNIEnv *, const char *))(env, text); }

/* Found by its name, with nothing registered. */
int Java_t_Guest_add(JNIEnv *env, jobject cls, int a, int b)
{
    (void)env; (void)cls;
    return a + b;
}

/* One argument of each kind, the wide ones at odd places: a + b + c * 10 + d * 10 + the string's length + the array's sum. */
static long long g_mix(JNIEnv *env, jobject cls, int a, long long b, double c, float d, jobject s, jobject arr)
{
    int n = F(env, 171, int (*)(JNIEnv *, jobject))(env, arr);                                 /* GetArrayLength */
    int *e = F(env, 187, int *(*)(JNIEnv *, jobject, void *))(env, arr, 0);                    /* GetIntArrayElements */
    long long r = a + b + (int)(c * 10) + (int)(d * 10) + F(env, 168, int (*)(JNIEnv *, jobject))(env, s);   /* GetStringUTFLength */
    int i;
    (void)cls;
    for (i = 0; i < n; i++) r += e[i];
    F(env, 195, void (*)(JNIEnv *, jobject, int *, int))(env, arr, e, 0);                      /* ReleaseIntArrayElements */
    return r;
}

/* Fourteen longs: thirty words of arguments, most of them on the stack. */
static long long g_many(JNIEnv *env, jobject cls, long long a1, long long a2, long long a3, long long a4, long long a5, long long a6, long long a7,
                        long long a8, long long a9, long long a10, long long a11, long long a12, long long a13, long long a14)
{
    (void)env; (void)cls;
    return a1 + 2 * a2 + 3 * a3 + 4 * a4 + 5 * a5 + 6 * a6 + 7 * a7 + 8 * a8 + 9 * a9 + 10 * a10 + 11 * a11 + 12 * a12 + 13 * a13 + 14 * a14;
}

/* "hello, " and the name, which is read as UTF-8 and as UTF-16: the second must agree with the first about the last character. */
static jobject g_greet(JNIEnv *env, jobject cls, jobject name)
{
    const char *text = F(env, 169, const char *(*)(JNIEnv *, jobject, void *))(env, name, 0);  /* GetStringUTFChars */
    int len16 = F(env, 164, int (*)(JNIEnv *, jobject))(env, name);                            /* GetStringLength */
    unsigned short last = 0;
    char out[80] = "hello, ";
    int k = 7, i;
    (void)cls;
    if (len16 > 0) F(env, 220, void (*)(JNIEnv *, jobject, int, int, unsigned short *))(env, name, len16 - 1, 1, &last);   /* GetStringRegion */
    for (i = 0; text[i] && k < 70; i++) out[k++] = text[i];
    out[k++] = last == (unsigned char)text[i ? i - 1 : 0] ? '!' : '?';
    out[k] = 0;
    F(env, 170, void (*)(JNIEnv *, jobject, const char *))(env, name, text);                   /* ReleaseStringUTFChars */
    return new_string(env, out);
}

static jobject g_squares(JNIEnv *env, jobject cls, int n)
{
    jobject arr = F(env, 179, jobject (*)(JNIEnv *, int))(env, n);                             /* NewIntArray */
    int tmp[16], i;
    (void)cls;
    for (i = 0; i < n && i < 16; i++) tmp[i] = i * i;
    F(env, 211, void (*)(JNIEnv *, jobject, int, int, const int *))(env, arr, 0, n < 16 ? n : 16, tmp);   /* SetIntArrayRegion */
    return arr;
}

static jobject g_words(JNIEnv *env, jobject cls)
{
    static const char *const words[] = { "one", "two", "three" };
    jobject arr = F(env, 172, jobject (*)(JNIEnv *, int, jobject, jobject))(env, 3, find_class(env, "java/lang/String"), 0);   /* NewObjectArray */
    int i;
    (void)cls;
    for (i = 0; i < 3; i++) F(env, 174, void (*)(JNIEnv *, jobject, int, jobject))(env, arr, i, new_string(env, words[i]));   /* SetObjectArrayElement */
    return arr;
}

/*
 * Back into bytecode: Guest.twice(x), a static method; this.scale(x), a
 * virtual one, asked for by Guest's name and run as the object's own class
 * has it; and the object's and the class's fields, read and written.
 */
static int g_callBack(JNIEnv *env, jobject self, int x)
{
    jobject guest = find_class(env, "t/Guest");
    void *twice = F(env, 113, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "twice", "(I)I");   /* GetStaticMethodID */
    void *scale = F(env, 33, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "scale", "(I)I");    /* GetMethodID */
    void *count = F(env, 94, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "count", "I");       /* GetFieldID */
    void *total = F(env, 144, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "total", "I");      /* GetStaticFieldID */
    int t = F(env, 129, int (*)(JNIEnv *, jobject, void *, ...))(env, guest, twice, x);                                 /* CallStaticIntMethod */
    int s = F(env, 49, int (*)(JNIEnv *, jobject, void *, ...))(env, self, scale, x);                                   /* CallIntMethod */
    int c = F(env, 100, int (*)(JNIEnv *, jobject, void *))(env, self, count);                                          /* GetIntField */
    F(env, 109, void (*)(JNIEnv *, jobject, void *, int))(env, self, count, c + 1);                                     /* SetIntField */
    F(env, 159, void (*)(JNIEnv *, jobject, void *, int))(env, guest, total,
                                                         F(env, 150, int (*)(JNIEnv *, jobject, void *))(env, guest, total) + x);   /* Set/GetStaticIntField */
    return t + s;
}

static void g_thrower(JNIEnv *env, jobject cls)
{
    (void)cls;
    F(env, 14, int (*)(JNIEnv *, jobject, const char *))(env, find_class(env, "java/lang/IllegalStateException"), "from guest");   /* ThrowNew */
}

/* Calls bytecode that divides by zero, and looks at what came back: 1 when it is an ArithmeticException and it could be cleared. */
static int g_catcher(JNIEnv *env, jobject cls)
{
    jobject guest = find_class(env, "t/Guest"), e;
    void *boom = F(env, 113, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "boom", "()I");
    (void)cls;
    F(env, 129, int (*)(JNIEnv *, jobject, void *, ...))(env, guest, boom);
    if (!F(env, 228, unsigned char (*)(JNIEnv *))(env)) return -1;                              /* ExceptionCheck */
    e = F(env, 15, jobject (*)(JNIEnv *))(env);                                                 /* ExceptionOccurred */
    F(env, 17, void (*)(JNIEnv *))(env);                                                        /* ExceptionClear */
    if (F(env, 228, unsigned char (*)(JNIEnv *))(env)) return -2;
    return F(env, 32, unsigned char (*)(JNIEnv *, jobject, jobject))(env, e, find_class(env, "java/lang/ArithmeticException")) ? 1 : 0;   /* IsInstanceOf */
}

static jobject g_make(JNIEnv *env, jobject cls, int v)
{
    jobject guest = find_class(env, "t/Guest");
    void *ctor = F(env, 33, void *(*)(JNIEnv *, jobject, const char *, const char *))(env, guest, "<init>", "(I)V");
    (void)cls;
    return F(env, 28, jobject (*)(JNIEnv *, jobject, void *, ...))(env, guest, ctor, v);        /* NewObject */
}

static double g_fmul(JNIEnv *env, jobject cls, double a, float b)
{
    (void)env; (void)cls;
    return a * b;
}

static int g_locked(JNIEnv *env, jobject self)
{
    int enter = F(env, 217, int (*)(JNIEnv *, jobject))(env, self);                             /* MonitorEnter */
    int leave = F(env, 218, int (*)(JNIEnv *, jobject))(env, self);                             /* MonitorExit */
    return enter * 10 + leave;
}

int JNI_OnLoad(void ***vm, void *reserved)
{
    static const struct { const char *name, *sig; void *fn; } methods[] = {
        { "mix", "(IJDFLjava/lang/String;[I)J", (void *)g_mix },
        { "many", "(JJJJJJJJJJJJJJ)J", (void *)g_many },
        { "greet", "(Ljava/lang/String;)Ljava/lang/String;", (void *)g_greet },
        { "squares", "(I)[I", (void *)g_squares },
        { "words", "()[Ljava/lang/String;", (void *)g_words },
        { "callBack", "(I)I", (void *)g_callBack },
        { "thrower", "()V", (void *)g_thrower },
        { "catcher", "()I", (void *)g_catcher },
        { "make", "(I)Lt/Guest;", (void *)g_make },
        { "fmul", "!(DF)D", (void *)g_fmul },                    /* the mark older Androids took before a signature */
        { "locked", "()I", (void *)g_locked },
        { "notThere", "()V", (void *)g_thrower },                /* a method the class does not declare: to be reported, and the rest taken */
    };
    JNIEnv *env = 0;
    (void)reserved;
    F(vm, 6, int (*)(void ***, JNIEnv **, int))(vm, &env, 0x10006);                             /* GetEnv */
    if (!env) return -1;
    F(env, 215, int (*)(JNIEnv *, jobject, const void *, int))(env, find_class(env, "t/Guest"), methods, (int)(sizeof(methods) / sizeof(methods[0])));
    return 0x10006;
}
