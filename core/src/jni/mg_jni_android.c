/*
 * The Android framework classes that native code of any engine asks about:
 * the activity as a Context, files, the device's build, the locale. Each
 * driver adds its own classes on top (drivers/).
 */
#include "mg_jni_android.h"

#include <stdio.h>

#include "../bionic/mg_bionic.h"
#include "../bionic/mg_fd.h"
#include "../ld/mg_ld_internal.h"
#include "../ndk/mg_ndk.h"

static char g_package[160], g_apk_guest[300], g_lib_dir[300];
static int g_width, g_height, g_density;
static mg_jobj *g_activity, *g_assets, *g_app_info, *g_window, *g_display, *g_locale;

static mg_jobj *permanent(mg_jobj *o)
{
    o->permanent = true;
    return o;
}

static mg_jobj *file_object(const char *path)
{
    mg_jobj *f = mg_jni_new_object("java/io/File");
    f->native = mg_strdup(path);
    return f;
}

/* A directory of the app, made on first use, as a java.io.File. */
static mg_jobj *app_dir(const char *guest_path)
{
    char *host = mg_fs_host_path(guest_path);
    if (host) {
        mg_path_mkdirs(host);
        free(host);
    }
    return file_object(guest_path);
}

/* --------------------------------------------------------------- Context */

static void ctx_getPackageName(mg_jcall *c) { c->ret.l = mg_jni_new_string(g_package); }
static void ctx_getPackageCodePath(mg_jcall *c) { c->ret.l = mg_jni_new_string(g_apk_guest); }
static void ctx_getFilesDir(mg_jcall *c)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/files", mg_fs_internal_dir());
    c->ret.l = app_dir(path);
}
static void ctx_getCacheDir(mg_jcall *c)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/cache", mg_fs_internal_dir());
    c->ret.l = app_dir(path);
}
static void ctx_getExternalFilesDir(mg_jcall *c)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/Android/data/%s/files", mg_fs_external_dir(), g_package);
    c->ret.l = app_dir(path);
}
static void ctx_getExternalCacheDir(mg_jcall *c)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/Android/data/%s/cache", mg_fs_external_dir(), g_package);
    c->ret.l = app_dir(path);
}
static void ctx_getObbDir(mg_jcall *c)
{
    char path[400];
    snprintf(path, sizeof(path), "%s/Android/obb/%s", mg_fs_external_dir(), g_package);
    c->ret.l = app_dir(path);
}
static void ctx_getDir(mg_jcall *c)
{
    char path[400];
    const char *name = mg_jni_string(c->args[0].l);
    snprintf(path, sizeof(path), "%s/app_%s", mg_fs_internal_dir(), name ? name : "dir");
    c->ret.l = app_dir(path);
}
static void ctx_getAssets(mg_jcall *c) { c->ret.l = g_assets; }
static void ctx_self(mg_jcall *c) { c->ret.l = c->self; }
static void ctx_getApplicationInfo(mg_jcall *c) { c->ret.l = g_app_info; }
static void ctx_getWindow(mg_jcall *c) { c->ret.l = g_window; }
static void ctx_getSystemService(mg_jcall *c)
{
    const char *name = mg_jni_string(c->args[0].l);
    char cls[96];
    /* An object of a class named after the service: its methods are implemented, or log and return zero. */
    snprintf(cls, sizeof(cls), "megler/service/%s", name ? name : "unknown");
    c->ret.l = permanent(mg_jni_new_object(cls));
}
static void ctx_getClassLoader(mg_jcall *c) { c->ret.l = permanent(mg_jni_new_object("java/lang/ClassLoader")); }
static void ctx_checkPermission(mg_jcall *c) { c->ret.i = 0; }    /* PERMISSION_GRANTED */
static void ctx_getIntent(mg_jcall *c) { c->ret.l = permanent(mg_jni_new_object("android/content/Intent")); }
static void ret_null(mg_jcall *c) { c->ret.l = NULL; }
static void ret_false(mg_jcall *c) { c->ret.z = 0; }
static void ret_void(mg_jcall *c) { (void)c; }
static void ret_zero(mg_jcall *c) { c->ret.j = 0; }

/* ------------------------------------------------------------------ files */

static void object_self(mg_jcall *c) { c->ret.l = mg_jni_ref(c->self); }

static void file_init(mg_jcall *c)
{
    const char *a = mg_jni_string(c->args[0].l);
    if (!strcmp(c->sig, "(Ljava/lang/String;)V")) c->self->native = mg_strdup(a ? a : "");
    else if (!strcmp(c->sig, "(Ljava/lang/String;Ljava/lang/String;)V")) c->self->native = mg_sprintf("%s/%s", a ? a : "", mg_jni_string(c->args[1].l) ? mg_jni_string(c->args[1].l) : "");
    else if (!strcmp(c->sig, "(Ljava/io/File;Ljava/lang/String;)V"))
        c->self->native = mg_sprintf("%s/%s", c->args[0].l && c->args[0].l->native ? (const char *)c->args[0].l->native : "", mg_jni_string(c->args[1].l) ? mg_jni_string(c->args[1].l) : "");
}
static const char *file_path(mg_jcall *c) { return c->self && c->self->native ? (const char *)c->self->native : ""; }
static void file_getPath(mg_jcall *c) { c->ret.l = mg_jni_new_string(file_path(c)); }
/* The path without its last name: getParent() as text, getParentFile() as a File; null when there is no directory in it. */
static char *file_parent(mg_jcall *c)
{
    char *p = mg_strdup(file_path(c)), *slash;
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
    slash = strrchr(p, '/');
    if (!slash) {
        free(p);
        return NULL;
    }
    if (slash == p) slash[1] = 0;
    else *slash = 0;
    return p;
}
static void file_getParent(mg_jcall *c)
{
    char *p = file_parent(c);
    c->ret.l = p ? mg_jni_new_string(p) : NULL;
    free(p);
}
static void file_getParentFile(mg_jcall *c)
{
    char *p = file_parent(c);
    if (p) {
        mg_jobj *f = mg_jni_new_object("java/io/File");
        f->native = p;
        c->ret.l = f;
    }
}
static void file_getName(mg_jcall *c)
{
    const char *p = file_path(c), *slash = strrchr(p, '/');
    c->ret.l = mg_jni_new_string(slash ? slash + 1 : p);
}
static void file_true(mg_jcall *c) { c->ret.z = 1; }
static void file_isFile(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    mg_stat st;
    c->ret.z = host && mg_path_stat(host, &st, NULL) && !st.is_dir;
    free(host);
}

static void file_exists(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    mg_stat st;
    c->ret.z = host && mg_path_stat(host, &st, NULL);
    free(host);
}
static void file_isDirectory(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    mg_stat st;
    c->ret.z = host && mg_path_stat(host, &st, NULL) && st.is_dir;
    free(host);
}
static void file_mkdirs(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    c->ret.z = host && mg_path_mkdirs(host);
    free(host);
}
static void file_length(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    mg_stat st;
    c->ret.j = host && mg_path_stat(host, &st, NULL) ? (i64)st.size : 0;
    free(host);
}
static void file_delete(mg_jcall *c)
{
    char *host = mg_fs_host_path(file_path(c));
    c->ret.z = host && (mg_path_remove(host, NULL) || mg_path_rmdir(host, NULL));
    free(host);
}

/* ----------------------------------------------------------- the device */

static void env_getExternalStorageDirectory(mg_jcall *c) { c->ret.l = app_dir(mg_fs_external_dir()); }
static void env_getExternalStorageState(mg_jcall *c) { c->ret.l = mg_jni_new_string("mounted"); }
static void locale_getDefault(mg_jcall *c) { c->ret.l = g_locale; }
/* The phone's language and region (mg_locale_parts). */
static void locale_getLanguage(mg_jcall *c)
{
    char language[8], region[8];
    mg_locale_parts(language, sizeof(language), region, sizeof(region));
    c->ret.l = mg_jni_new_string(language);
}
static void locale_getCountry(mg_jcall *c)
{
    char language[8], region[8];
    mg_locale_parts(language, sizeof(language), region, sizeof(region));
    c->ret.l = mg_jni_new_string(region);
}
static void locale_toString(mg_jcall *c)
{
    char language[8], region[8], text[20];
    mg_locale_parts(language, sizeof(language), region, sizeof(region));
    snprintf(text, sizeof(text), "%s%s%s", language, region[0] ? "_" : "", region);
    c->ret.l = mg_jni_new_string(text);
}
static void window_getDecorView(mg_jcall *c) { c->ret.l = permanent(mg_jni_new_object("android/view/View")); }
static void wm_getDefaultDisplay(mg_jcall *c) { c->ret.l = g_display; }
static void display_getWidth(mg_jcall *c) { c->ret.i = g_width; }
static void display_getHeight(mg_jcall *c) { c->ret.i = g_height; }
static void display_getRotation(mg_jcall *c) { c->ret.i = 0; }
static void display_getRefreshRate(mg_jcall *c) { c->ret.f = 60.0f; }
void mg_jni_android_fill_metrics(mg_jobj *dm)
{
    /* The guest reads these with GetIntField/GetFloatField afterwards: fields are found by name. */
    int i;
    static const char *const ints[] = { "widthPixels", "heightPixels", "densityDpi" };
    static const char *const floats[] = { "density", "scaledDensity", "xdpi", "ydpi" };
    if (!dm) return;
    for (i = 0; i < 3; i++) {
        dm->fields = (mg_jfield_value *)mg_realloc(dm->fields, sizeof(mg_jfield_value) * (size_t)(dm->nfields + 1));
        memset(&dm->fields[dm->nfields], 0, sizeof(mg_jfield_value));
        dm->fields[dm->nfields].name = mg_strdup(ints[i]);
        dm->fields[dm->nfields++].v.i = i == 0 ? g_width : i == 1 ? g_height : g_density;
    }
    for (i = 0; i < 4; i++) {
        dm->fields = (mg_jfield_value *)mg_realloc(dm->fields, sizeof(mg_jfield_value) * (size_t)(dm->nfields + 1));
        memset(&dm->fields[dm->nfields], 0, sizeof(mg_jfield_value));
        dm->fields[dm->nfields].name = mg_strdup(floats[i]);
        dm->fields[dm->nfields++].v.f = i < 2 ? (float)g_density / 160.0f : (float)g_density;
    }
}
static void display_getMetrics(mg_jcall *c) { mg_jni_android_fill_metrics(c->args[0].l); }
static void system_currentTimeMillis(mg_jcall *c) { c->ret.j = (i64)(mg_time_realtime_ns() / 1000000ull); }
static void system_nanoTime(mg_jcall *c) { c->ret.j = (i64)mg_time_monotonic_ns(); }
static void system_getProperty(mg_jcall *c)
{
    const char *key = mg_jni_string(c->args[0].l);
    c->ret.l = NULL;
    if (key && !strcmp(key, "os.arch")) c->ret.l = mg_jni_new_string(MG_GUEST_MACHINE);
    else if (key && !strcmp(key, "java.io.tmpdir")) c->ret.l = mg_jni_new_string("/data/local/tmp");
    else if (key && !strcmp(key, "line.separator")) c->ret.l = mg_jni_new_string("\n");
}
/* A string's length is counted in UTF-16 code units, as Java counts. */
static void string_length(mg_jcall *c)
{
    i32 n = 0;
    mg_jni_string_utf16(c->self, &n);
    c->ret.i = n;
}

/* Whether a charset argument (a name, or a Charset object that says nothing here) means UTF-16, and which end first. */
static int charset_utf16(mg_jobj *charset, bool *big)
{
    const char *name = mg_jni_string(charset);
    *big = false;
    if (!name || !(strstr(name, "16") || strstr(name, "UCS-2") || strstr(name, "ucs-2"))) return 0;
    *big = strstr(name, "BE") || strstr(name, "be") || !(strstr(name, "LE") || strstr(name, "le"));
    return 1;
}

/* getBytes(), getBytes(String charset), getBytes(Charset): UTF-8 unless UTF-16 is asked for by name. */
static void string_getBytes(mg_jcall *c)
{
    const char *s = mg_jni_string(c->self);
    bool big = false;
    mg_jobj *a;
    if (c->sig[1] == 'L' && charset_utf16(c->args[0].l, &big)) {
        i32 n = 0, i;
        const u16 *w = mg_jni_string_utf16(c->self, &n);
        a = mg_jni_new_prim_array('B', (u32)n * 2);
        for (i = 0; i < n; i++) {
            ((u8 *)a->u.arr.data)[i * 2 + (big ? 1 : 0)] = (u8)w[i];
            ((u8 *)a->u.arr.data)[i * 2 + (big ? 0 : 1)] = (u8)(w[i] >> 8);
        }
    } else {
        u32 n = s ? (u32)strlen(s) : 0;
        a = mg_jni_new_prim_array('B', n);
        if (n) memcpy(a->u.arr.data, s, n);
    }
    c->ret.l = a;
}

/*
 * new String(...): from bytes (with an offset and a length, with a charset,
 * with both or neither), from chars, from another String. Native code makes
 * its Java strings this way when it does not trust NewStringUTF with its
 * text (Unity does, for every PlayerPrefs key and value); a constructor that
 * did nothing left them all empty, and a game's saved language came back as
 * "" from a file whose every key was "".
 */
static void string_init(mg_jcall *c)
{
    const char *sig = c->sig;
    mg_jobj *a = sig[1] != ')' ? c->args[0].l : NULL;
    if (!a) return;
    if (!strncmp(sig, "(Ljava/lang/String;", 19) || !strncmp(sig, "(Ljava/lang/StringBuilder;", 26) || !strncmp(sig, "(Ljava/lang/StringBuffer;", 25)) {
        mg_jni_string_set(c->self, mg_jni_string(a));
        return;
    }
    if (a->kind != MG_J_PRIM_ARRAY) return;
    {
        bool ranged = sig[3] == 'I' && sig[4] == 'I', big = false;
        u32 off = ranged ? (u32)c->args[1].i : 0, len = ranged ? (u32)c->args[2].i : a->u.arr.len;
        int next = ranged ? 3 : 1;
        bool has_charset = sig[ranged ? 5 : 3] == 'L';
        if (off > a->u.arr.len) off = a->u.arr.len;
        if (len > a->u.arr.len - off) len = a->u.arr.len - off;
        if (sig[2] == 'C') {
            mg_jni_string_set_utf16(c->self, (const u16 *)a->u.arr.data + off, (i32)len);
        } else if (sig[2] == 'B') {
            const u8 *bytes = (const u8 *)a->u.arr.data + off;
            if (has_charset && charset_utf16(c->args[next].l, &big)) {
                u32 n = len / 2, i;
                u16 *w = (u16 *)mg_alloc(sizeof(u16) * (n + 1));
                for (i = 0; i < n; i++) w[i] = big ? (u16)((bytes[i * 2] << 8) | bytes[i * 2 + 1]) : (u16)(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
                /* (A byte-order mark at the front is not text.) */
                if (n && w[0] == 0xFEFF) mg_jni_string_set_utf16(c->self, w + 1, (i32)n - 1);
                else mg_jni_string_set_utf16(c->self, w, (i32)n);
                free(w);
            } else {
                char *text = (char *)mg_alloc(len + 1);
                memcpy(text, bytes, len);
                mg_jni_string_set(c->self, text);
                free(text);
            }
        }
    }
}

static void string_equals(mg_jcall *c)
{
    const char *a = mg_jni_string(c->self), *b = mg_jni_string(c->args[0].l);
    c->ret.z = a && b && !strcmp(a, b);
}
static void string_is_empty(mg_jcall *c)
{
    const char *a = mg_jni_string(c->self);
    c->ret.z = !a || !a[0];
}
static void object_toString(mg_jcall *c)
{
    if (c->self && c->self->kind == MG_J_STRING) c->ret.l = mg_jni_ref(c->self);
    else c->ret.l = mg_jni_new_string(mg_jni_class_name(c->self));
}
static void object_getClass(mg_jcall *c) { c->ret.l = mg_jni_class_object(mg_jni_class_name(c->self)); }
static void class_getName(mg_jcall *c)
{
    char *name = mg_strdup(mg_jni_class_name(c->self)), *p;
    for (p = name; *p; p++)
        if (*p == '/') *p = '.';
    c->ret.l = mg_jni_new_string(name);
    free(name);
}
static void loader_loadClass(mg_jcall *c)
{
    const char *name = mg_jni_string(c->args[0].l);
    char *jni = mg_strdup(name ? name : "java/lang/Object"), *p;
    for (p = jni; *p; p++)
        if (*p == '.') *p = '/';
    c->ret.l = mg_jni_class_object(jni);
    free(jni);
}

/* ClassLoader.findLibrary("il2cpp"): where Android unpacked libil2cpp.so, for the caller to dlopen; null when the app has no such library. */
static void loader_findLibrary(mg_jcall *c)
{
    const char *name = mg_jni_string(c->args[0].l);
    char file[200], path[520];
    c->ret.l = NULL;
    if (!name) return;
    snprintf(file, sizeof(file), "lib%s.so", name);
    if (!mg_ld_has_lib(file)) return;
    snprintf(path, sizeof(path), "%s/%s", g_lib_dir, file);
    c->ret.l = mg_jni_new_string(path);
}

#define CONTEXT_METHODS(cls)                                                                                       \
    { cls, "getPackageName", NULL, ctx_getPackageName }, { cls, "getPackageCodePath", NULL, ctx_getPackageCodePath }, \
    { cls, "getPackageResourcePath", NULL, ctx_getPackageCodePath }, { cls, "getFilesDir", NULL, ctx_getFilesDir },   \
    { cls, "getCacheDir", NULL, ctx_getCacheDir }, { cls, "getExternalFilesDir", NULL, ctx_getExternalFilesDir },     \
    { cls, "getExternalCacheDir", NULL, ctx_getExternalCacheDir }, { cls, "getObbDir", NULL, ctx_getObbDir },         \
    { cls, "getDir", NULL, ctx_getDir }, { cls, "getAssets", NULL, ctx_getAssets },                                   \
    { cls, "getApplicationContext", NULL, ctx_self }, { cls, "getApplication", NULL, ctx_self },                      \
    { cls, "getBaseContext", NULL, ctx_self }, { cls, "getApplicationInfo", NULL, ctx_getApplicationInfo },           \
    { cls, "getSystemService", NULL, ctx_getSystemService }, { cls, "getClassLoader", NULL, ctx_getClassLoader },     \
    { cls, "checkSelfPermission", NULL, ctx_checkPermission }, { cls, "checkCallingOrSelfPermission", NULL, ctx_checkPermission }

static const mg_jhle g_android[] = {
    CONTEXT_METHODS("android/content/Context"),
    { "android/app/Activity", "getWindow", NULL, ctx_getWindow },
    { "android/app/Activity", "getIntent", NULL, ctx_getIntent },
    { "android/app/Activity", "getWindowManager", NULL, ctx_getSystemService },
    { "android/app/Activity", "runOnUiThread", NULL, ret_void },
    { "android/app/Activity", "setRequestedOrientation", NULL, ret_void },
    { "android/app/Activity", "getRequestedOrientation", NULL, ret_zero },
    { "android/content/Intent", "getStringExtra", NULL, ret_null },
    { "android/content/Intent", "getExtras", NULL, ret_null },
    { "android/content/Intent", "getData", NULL, ret_null },
    { "android/content/Intent", "getDataString", NULL, ret_null },
    { "android/content/Intent", "getAction", NULL, ret_null },
    { "android/content/Intent", "hasExtra", NULL, ret_false },
    { "android/content/Intent", "getBooleanExtra", NULL, ret_false },
    { "android/content/Intent", "getIntExtra", NULL, ret_zero },
    { "java/io/File", "<init>", NULL, file_init },
    { "java/io/File", "getAbsolutePath", NULL, file_getPath },
    { "java/io/File", "getPath", NULL, file_getPath },
    { "java/io/File", "getCanonicalPath", NULL, file_getPath },
    { "java/io/File", "toString", NULL, file_getPath },
    { "java/io/File", "exists", NULL, file_exists },
    { "java/io/File", "isDirectory", NULL, file_isDirectory },
    { "java/io/File", "mkdirs", NULL, file_mkdirs },
    { "java/io/File", "mkdir", NULL, file_mkdirs },
    { "java/io/File", "length", NULL, file_length },
    { "java/io/File", "delete", NULL, file_delete },
    { "java/io/File", "getParent", NULL, file_getParent },
    { "java/io/File", "getParentFile", NULL, file_getParentFile },
    { "java/io/File", "getAbsoluteFile", NULL, object_self },
    { "java/io/File", "getCanonicalFile", NULL, object_self },
    { "java/io/File", "getName", NULL, file_getName },
    { "java/io/File", "isFile", NULL, file_isFile },
    { "java/io/File", "canRead", NULL, file_true },
    { "java/io/File", "canWrite", NULL, file_true },
    { "android/os/Environment", "getExternalStorageDirectory", NULL, env_getExternalStorageDirectory },
    { "android/os/Environment", "getExternalStorageState", NULL, env_getExternalStorageState },
    { "java/util/Locale", "getDefault", NULL, locale_getDefault },
    { "java/util/Locale", "getLanguage", NULL, locale_getLanguage },
    { "java/util/Locale", "getCountry", NULL, locale_getCountry },
    { "java/util/Locale", "toString", NULL, locale_toString },
    { "android/view/Window", "getDecorView", NULL, window_getDecorView },
    { "android/view/Window", "addFlags", NULL, ret_void },
    { "android/view/Window", "clearFlags", NULL, ret_void },
    { "android/view/Window", "setFlags", NULL, ret_void },
    { "android/view/View", "setSystemUiVisibility", NULL, ret_void },
    { "android/view/View", "getSystemUiVisibility", NULL, ret_zero },
    { "megler/service/window", "getDefaultDisplay", NULL, wm_getDefaultDisplay },
    { "android/view/Display", "getWidth", NULL, display_getWidth },
    { "android/view/Display", "getHeight", NULL, display_getHeight },
    { "android/view/Display", "getRotation", NULL, display_getRotation },
    { "android/view/Display", "getRefreshRate", NULL, display_getRefreshRate },
    { "android/view/Display", "getMetrics", NULL, display_getMetrics },
    { "android/view/Display", "getRealMetrics", NULL, display_getMetrics },
    { "megler/service/input_method", "showSoftInput", NULL, ret_false },
    { "megler/service/input_method", "hideSoftInputFromWindow", NULL, ret_false },
    { "megler/service/vibrator", "vibrate", NULL, ret_void },
    { "megler/service/vibrator", "hasVibrator", NULL, ret_false },
    { "megler/service/connectivity", "getActiveNetworkInfo", NULL, ret_null },
    { "java/lang/System", "currentTimeMillis", NULL, system_currentTimeMillis },
    { "java/lang/System", "nanoTime", NULL, system_nanoTime },
    { "java/lang/System", "getProperty", NULL, system_getProperty },
    { "java/lang/System", "loadLibrary", NULL, ret_void },
    { "java/lang/String", "length", NULL, string_length },
    { "java/lang/String", "getBytes", NULL, string_getBytes },
    { "java/lang/String", "<init>", NULL, string_init },
    { "java/lang/String", "equals", NULL, string_equals },
    { "java/lang/String", "isEmpty", NULL, string_is_empty },
    { "java/lang/Object", "toString", NULL, object_toString },
    { "java/lang/Object", "getClass", NULL, object_getClass },
    { "java/lang/Object", "hashCode", NULL, ret_zero },
    { "java/lang/Class", "getName", NULL, class_getName },
    { "java/lang/Class", "getClassLoader", NULL, ctx_getClassLoader },
    { "java/lang/ClassLoader", "loadClass", NULL, loader_loadClass },
    { "java/lang/ClassLoader", "findClass", NULL, loader_loadClass },
    { "java/lang/ClassLoader", "findLibrary", NULL, loader_findLibrary },
    { "java/lang/Thread", "currentThread", NULL, ctx_getClassLoader },
    { NULL, NULL, NULL, NULL },
};

/*
 * Android's constants: the static final fields native code reads by name
 * (GetStaticObjectField, GetStaticIntField) rather than carrying their values
 * itself. Their values are part of Android's API and never change. One that
 * is missing here shows in the log the first time it is read (mg_jni.c).
 */
static void android_constants(void)
{
    static const struct { const char *cls, *name, *text; } strings[] = {
        { "android/media/AudioManager", "PROPERTY_OUTPUT_SAMPLE_RATE", "android.media.property.OUTPUT_SAMPLE_RATE" },
        { "android/media/AudioManager", "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", "android.media.property.OUTPUT_FRAMES_PER_BUFFER" },
        { "android/media/AudioManager", "PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED", "android.media.property.SUPPORT_AUDIO_SOURCE_UNPROCESSED" },
        { "android/content/pm/PackageManager", "FEATURE_AUDIO_LOW_LATENCY", "android.hardware.audio.low_latency" },
        { "android/content/pm/PackageManager", "FEATURE_AUDIO_PRO", "android.hardware.audio.pro" },
        { "android/content/pm/PackageManager", "FEATURE_AUDIO_OUTPUT", "android.hardware.audio.output" },
        { "android/content/pm/PackageManager", "FEATURE_TOUCHSCREEN", "android.hardware.touchscreen" },
        { "android/content/pm/PackageManager", "FEATURE_TOUCHSCREEN_MULTITOUCH", "android.hardware.touchscreen.multitouch" },
        { "android/content/pm/PackageManager", "FEATURE_GAMEPAD", "android.hardware.gamepad" },
        { "android/content/pm/PackageManager", "FEATURE_LEANBACK", "android.software.leanback" },
        { "android/content/pm/PackageManager", "FEATURE_MICROPHONE", "android.hardware.microphone" },
        { "android/content/pm/PackageManager", "FEATURE_VR_MODE_HIGH_PERFORMANCE", "android.hardware.vr.high_performance" },
        { "android/Manifest$permission", "RECORD_AUDIO", "android.permission.RECORD_AUDIO" },
        { "android/Manifest$permission", "INTERNET", "android.permission.INTERNET" },
        { "android/Manifest$permission", "WRITE_EXTERNAL_STORAGE", "android.permission.WRITE_EXTERNAL_STORAGE" },
        { "android/Manifest$permission", "READ_EXTERNAL_STORAGE", "android.permission.READ_EXTERNAL_STORAGE" },
        { "android/Manifest$permission", "MODIFY_AUDIO_SETTINGS", "android.permission.MODIFY_AUDIO_SETTINGS" },
        { "android/Manifest$permission", "BLUETOOTH", "android.permission.BLUETOOTH" },
        { "android/os/Environment", "MEDIA_MOUNTED", "mounted" },
        { "android/content/Intent", "ACTION_MAIN", "android.intent.action.MAIN" },
        { "android/content/Intent", "ACTION_VIEW", "android.intent.action.VIEW" },
        { "android/content/Intent", "ACTION_SEND", "android.intent.action.SEND" },
        { "android/content/Intent", "CATEGORY_LAUNCHER", "android.intent.category.LAUNCHER" },
        { "android/content/Intent", "CATEGORY_LEANBACK_LAUNCHER", "android.intent.category.LEANBACK_LAUNCHER" },
        { "android/os/Build", "ID", "PQ3A.190801.002" },
        { "android/os/Build", "DISPLAY", "PQ3A.190801.002" },
        { "android/os/Build", "TYPE", "user" },
        { "android/os/Build", "TAGS", "release-keys" },
        { "android/os/Build", "BOARD", "msm8996" },
        { "android/os/Build", "HOST", "megler" },
        { "android/os/Build", "USER", "megler" },
        { "android/os/Build", "SERIAL", "unknown" },
        { "android/os/Build", "CPU_ABI2", MG_APK_ABI2 },
        { "android/os/Build", "FINGERPRINT", "megler/megler/megler:9/PQ3A.190801.002/5670241:user/release-keys" },
        { "android/os/Build$VERSION", "INCREMENTAL", "5670241" },
        { "android/os/Build$VERSION", "CODENAME", "REL" },
        { "android/media/MediaFormat", "KEY_MIME", "mime" },
        { "android/media/MediaFormat", "KEY_WIDTH", "width" },
        { "android/media/MediaFormat", "KEY_HEIGHT", "height" },
        { "android/media/MediaFormat", "KEY_DURATION", "durationUs" },
        { "android/media/MediaFormat", "KEY_FRAME_RATE", "frame-rate" },
        { "android/media/MediaFormat", "KEY_SAMPLE_RATE", "sample-rate" },
        { "android/media/MediaFormat", "KEY_CHANNEL_COUNT", "channel-count" },
        { "android/media/MediaFormat", "KEY_MAX_INPUT_SIZE", "max-input-size" },
        { "android/media/MediaFormat", "KEY_COLOR_FORMAT", "color-format" },
        { "android/media/MediaFormat", "KEY_ROTATION", "rotation-degrees" },
        { "android/media/MediaFormat", "KEY_BIT_RATE", "bitrate" },
        { "android/media/MediaFormat", "KEY_LANGUAGE", "language" },
        { "android/media/MediaFormat", "KEY_PCM_ENCODING", "pcm-encoding" },
        { "android/media/MediaFormat", "KEY_STRIDE", "stride" },
        { "android/media/MediaFormat", "KEY_SLICE_HEIGHT", "slice-height" },
        { "android/media/MediaFormat", "KEY_IS_ADTS", "is-adts" },
        { "android/media/MediaFormat", "KEY_AAC_PROFILE", "aac-profile" },
        { "android/media/MediaFormat", "KEY_CHANNEL_MASK", "channel-mask" },
        { "android/media/MediaFormat", "KEY_OPERATING_RATE", "operating-rate" },
        { "android/media/MediaFormat", "KEY_PRIORITY", "priority" },
        { "android/media/MediaFormat", "MIMETYPE_VIDEO_AVC", "video/avc" },
        { "android/media/MediaFormat", "MIMETYPE_VIDEO_HEVC", "video/hevc" },
        { "android/media/MediaFormat", "MIMETYPE_VIDEO_VP8", "video/x-vnd.on2.vp8" },
        { "android/media/MediaFormat", "MIMETYPE_VIDEO_VP9", "video/x-vnd.on2.vp9" },
        { "android/media/MediaFormat", "MIMETYPE_AUDIO_AAC", "audio/mp4a-latm" },
        { "android/media/MediaFormat", "MIMETYPE_AUDIO_VORBIS", "audio/vorbis" },
        { "android/media/MediaFormat", "MIMETYPE_AUDIO_MPEG", "audio/mpeg" },
    };
    static const struct { const char *cls, *name; i32 value; } ints[] = {
        { "android/media/AudioManager", "STREAM_VOICE_CALL", 0 },
        { "android/media/AudioManager", "STREAM_SYSTEM", 1 },
        { "android/media/AudioManager", "STREAM_RING", 2 },
        { "android/media/AudioManager", "STREAM_MUSIC", 3 },
        { "android/media/AudioManager", "GET_DEVICES_INPUTS", 1 },
        { "android/media/AudioManager", "GET_DEVICES_OUTPUTS", 2 },
        { "android/media/AudioManager", "AUDIOFOCUS_GAIN", 1 },
        { "android/media/AudioManager", "AUDIOFOCUS_REQUEST_GRANTED", 1 },
        { "android/media/AudioManager", "MODE_NORMAL", 0 },
        { "android/media/AudioDeviceInfo", "TYPE_BUILTIN_SPEAKER", 2 },
        { "android/media/AudioDeviceInfo", "TYPE_WIRED_HEADSET", 3 },
        { "android/media/AudioDeviceInfo", "TYPE_WIRED_HEADPHONES", 4 },
        { "android/media/AudioDeviceInfo", "TYPE_BLUETOOTH_SCO", 7 },
        { "android/media/AudioDeviceInfo", "TYPE_BLUETOOTH_A2DP", 8 },
        { "android/content/pm/PackageManager", "PERMISSION_GRANTED", 0 },
        { "android/content/pm/PackageManager", "PERMISSION_DENIED", -1 },
        { "android/content/pm/PackageManager", "GET_META_DATA", 128 },
        { "android/content/Context", "MODE_PRIVATE", 0 },
        { "android/media/MediaCodec", "BUFFER_FLAG_KEY_FRAME", 1 },
        { "android/media/MediaCodec", "BUFFER_FLAG_SYNC_FRAME", 1 },
        { "android/media/MediaCodec", "BUFFER_FLAG_CODEC_CONFIG", 2 },
        { "android/media/MediaCodec", "BUFFER_FLAG_END_OF_STREAM", 4 },
        { "android/media/MediaCodec", "INFO_TRY_AGAIN_LATER", -1 },
        { "android/media/MediaCodec", "INFO_OUTPUT_FORMAT_CHANGED", -2 },
        { "android/media/MediaCodec", "INFO_OUTPUT_BUFFERS_CHANGED", -3 },
        { "android/media/MediaCodec", "CONFIGURE_FLAG_ENCODE", 1 },
        { "android/media/MediaCodec", "VIDEO_SCALING_MODE_SCALE_TO_FIT", 1 },
        { "android/media/MediaExtractor", "SEEK_TO_PREVIOUS_SYNC", 0 },
        { "android/media/MediaExtractor", "SEEK_TO_NEXT_SYNC", 1 },
        { "android/media/MediaExtractor", "SEEK_TO_CLOSEST_SYNC", 2 },
        { "android/media/MediaExtractor", "SAMPLE_FLAG_SYNC", 1 },
        { "android/media/MediaExtractor", "SAMPLE_FLAG_ENCRYPTED", 2 },
        { "android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420Planar", 19 },
        { "android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420SemiPlanar", 21 },
        { "android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatYUV420Flexible", 0x7F420888 },
        { "android/media/MediaCodecInfo$CodecCapabilities", "COLOR_FormatSurface", 0x7F000789 },
        { "android/media/MediaCodecList", "REGULAR_CODECS", 0 },
        { "android/media/MediaCodecList", "ALL_CODECS", 1 },
        { "android/media/AudioFormat", "ENCODING_PCM_16BIT", 2 },
        { "android/media/AudioFormat", "ENCODING_PCM_8BIT", 3 },
        { "android/media/AudioFormat", "ENCODING_PCM_FLOAT", 4 },
        { "android/media/AudioFormat", "CHANNEL_OUT_MONO", 4 },
        { "android/media/AudioFormat", "CHANNEL_OUT_STEREO", 12 },
        { "android/media/AudioTrack", "MODE_STREAM", 1 },
        { "android/media/AudioTrack", "MODE_STATIC", 0 },
        { "android/media/AudioTrack", "STATE_INITIALIZED", 1 },
        { "android/media/AudioTrack", "PLAYSTATE_PLAYING", 3 },
        { "android/view/Surface", "ROTATION_0", 0 },
        { "android/view/Surface", "ROTATION_90", 1 },
        { "android/view/Surface", "ROTATION_180", 2 },
        { "android/view/Surface", "ROTATION_270", 3 },
        { "android/content/pm/PackageManager", "GET_ACTIVITIES", 1 },
        { "android/content/pm/PackageManager", "GET_INTENT_FILTERS", 32 },
        { "android/view/WindowManager$LayoutParams", "FLAG_KEEP_SCREEN_ON", 128 },
        { "android/view/WindowManager$LayoutParams", "FLAG_FULLSCREEN", 1024 },
        { "android/view/WindowManager$LayoutParams", "FLAG_FORCE_NOT_FULLSCREEN", 2048 },
        { "android/view/WindowManager$LayoutParams", "FLAG_LAYOUT_NO_LIMITS", 512 },
        { "android/view/WindowManager$LayoutParams", "FLAG_SECURE", 8192 },
        { "android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT", 0 },
        { "android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES", 1 },
        { "android/view/WindowManager$LayoutParams", "LAYOUT_IN_DISPLAY_CUTOUT_MODE_NEVER", 2 },
        { "android/view/View", "SYSTEM_UI_FLAG_VISIBLE", 0 },
        { "android/view/View", "SYSTEM_UI_FLAG_LOW_PROFILE", 1 },
        { "android/view/View", "SYSTEM_UI_FLAG_HIDE_NAVIGATION", 2 },
        { "android/view/View", "SYSTEM_UI_FLAG_FULLSCREEN", 4 },
        { "android/view/View", "SYSTEM_UI_FLAG_LAYOUT_STABLE", 256 },
        { "android/view/View", "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION", 512 },
        { "android/view/View", "SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN", 1024 },
        { "android/view/View", "SYSTEM_UI_FLAG_IMMERSIVE", 2048 },
        { "android/view/View", "SYSTEM_UI_FLAG_IMMERSIVE_STICKY", 4096 },
        { "android/view/View", "VISIBLE", 0 },
        { "android/view/View", "INVISIBLE", 4 },
        { "android/view/View", "GONE", 8 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_UNSPECIFIED", -1 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_LANDSCAPE", 0 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_PORTRAIT", 1 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER", 2 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_BEHIND", 3 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR", 4 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_NOSENSOR", 5 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR_LANDSCAPE", 6 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_SENSOR_PORTRAIT", 7 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_REVERSE_LANDSCAPE", 8 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_REVERSE_PORTRAIT", 9 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_FULL_SENSOR", 10 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER_LANDSCAPE", 11 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_USER_PORTRAIT", 12 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_FULL_USER", 13 },
        { "android/content/pm/ActivityInfo", "SCREEN_ORIENTATION_LOCKED", 14 },
        { "android/content/res/Configuration", "ORIENTATION_PORTRAIT", 1 },
        { "android/content/res/Configuration", "ORIENTATION_LANDSCAPE", 2 },
        { "android/content/res/Configuration", "KEYBOARD_NOKEYS", 1 },
        { "android/content/res/Configuration", "KEYBOARD_QWERTY", 2 },
        { "android/content/res/Configuration", "HARDKEYBOARDHIDDEN_YES", 2 },
        { "android/content/res/Configuration", "UI_MODE_TYPE_MASK", 15 },
        { "android/content/res/Configuration", "UI_MODE_TYPE_TELEVISION", 4 },
        { "android/media/MediaRouter", "ROUTE_TYPE_LIVE_AUDIO", 1 },
        { "android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", 2 },
        { "android/media/MediaRouter", "ROUTE_TYPE_USER", 8388608 },
        { "android/view/InputDevice", "SOURCE_KEYBOARD", 0x101 },
        { "android/view/InputDevice", "SOURCE_DPAD", 0x201 },
        { "android/view/InputDevice", "SOURCE_GAMEPAD", 0x401 },
        { "android/view/InputDevice", "SOURCE_TOUCHSCREEN", 0x1002 },
        { "android/view/InputDevice", "SOURCE_MOUSE", 0x2002 },
        { "android/view/InputDevice", "SOURCE_JOYSTICK", 0x1000010 },
        { "android/view/InputDevice", "SOURCE_CLASS_JOYSTICK", 16 },
        { "android/view/InputDevice", "SOURCE_CLASS_BUTTON", 1 },
        { "android/view/InputDevice", "SOURCE_CLASS_POINTER", 2 },
    };
    mg_jvalue v;
    size_t i;
    for (i = 0; i < MG_COUNT(strings); i++) {
        v.j = 0;
        v.l = permanent(mg_jni_new_string(strings[i].text));
        mg_jni_set_static(strings[i].cls, strings[i].name, 'L', v);
    }
    for (i = 0; i < MG_COUNT(ints); i++) {
        v.j = 0;
        v.i = ints[i].value;
        mg_jni_set_static(ints[i].cls, ints[i].name, 'I', v);
    }
}

mg_jobj *mg_jni_android_setup(const char *activity_class, const char *package, const char *apk_host_path, int width, int height, int density_dpi)
{
    mg_jvalue v;
    mg_jni_init();
    mg_strlcpy(g_package, package, sizeof(g_package));
    snprintf(g_apk_guest, sizeof(g_apk_guest), "/data/app/%s-1/base.apk", package);
    snprintf(g_lib_dir, sizeof(g_lib_dir), "/data/app/%s-1/lib/arm", package);
    g_width = width;
    g_height = height;
    g_density = density_dpi > 0 ? density_dpi : 320;
    /* Code that opens its own APK as a file finds it where Android would have put it. */
    mg_fs_mount(g_apk_guest, apk_host_path);
    mg_configuration_set(width, height, g_density);

    mg_jni_class("android/content/Context", "java/lang/Object");
    mg_jni_class("android/content/ContextWrapper", "android/content/Context");
    mg_jni_class("android/view/ContextThemeWrapper", "android/content/ContextWrapper");
    mg_jni_class("android/app/Activity", "android/view/ContextThemeWrapper");
    mg_jni_class("android/app/NativeActivity", "android/app/Activity");
    mg_jni_class("android/app/Application", "android/content/ContextWrapper");
    if (activity_class && strcmp(activity_class, "android/app/NativeActivity") && strcmp(activity_class, "android/app/Activity"))
        mg_jni_class(activity_class, "android/app/NativeActivity");
    mg_jni_register(g_android);

    g_assets = permanent(mg_jni_new_object("android/content/res/AssetManager"));
    g_window = permanent(mg_jni_new_object("android/view/Window"));
    g_display = permanent(mg_jni_new_object("android/view/Display"));
    g_locale = permanent(mg_jni_new_object("java/util/Locale"));
    g_app_info = permanent(mg_jni_new_object("android/content/pm/ApplicationInfo"));
    g_activity = permanent(mg_jni_new_object(activity_class ? activity_class : "android/app/NativeActivity"));

    /* Fields native code reads directly. */
    v.j = 0; v.l = permanent(mg_jni_new_string(g_apk_guest)); mg_jni_set_field(g_app_info, "sourceDir", 'L', v);
    mg_jni_set_field(g_app_info, "publicSourceDir", 'L', v);
    v.l = permanent(mg_jni_new_string(g_lib_dir)); mg_jni_set_field(g_app_info, "nativeLibraryDir", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_fs_internal_dir())); mg_jni_set_field(g_app_info, "dataDir", 'L', v);
    v.j = 0; v.i = 28; mg_jni_set_field(g_app_info, "targetSdkVersion", 'I', v);
    v.j = 0; v.i = 28; mg_jni_set_static("android/os/Build$VERSION", "SDK_INT", 'I', v);
    v.j = 0; v.l = permanent(mg_jni_new_string("9")); mg_jni_set_static("android/os/Build$VERSION", "RELEASE", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.product.model"))); mg_jni_set_static("android/os/Build", "MODEL", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.product.manufacturer"))); mg_jni_set_static("android/os/Build", "MANUFACTURER", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.product.brand"))); mg_jni_set_static("android/os/Build", "BRAND", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.product.device"))); mg_jni_set_static("android/os/Build", "DEVICE", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.product.name"))); mg_jni_set_static("android/os/Build", "PRODUCT", 'L', v);
    v.l = permanent(mg_jni_new_string(mg_sysprop("ro.hardware"))); mg_jni_set_static("android/os/Build", "HARDWARE", 'L', v);
    v.l = permanent(mg_jni_new_string(MG_APK_ABI)); mg_jni_set_static("android/os/Build", "CPU_ABI", 'L', v);
    android_constants();
    return g_activity;
}

mg_jobj *mg_jni_android_activity(void) { return g_activity; }
mg_jobj *mg_jni_android_display(void) { return g_display; }
mg_jobj *mg_jni_android_app_info(void) { return g_app_info; }
const char *mg_jni_android_lib_dir(void) { return g_lib_dir; }
