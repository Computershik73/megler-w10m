/* The symbol registry, errno, guest threads' state and system properties. */
#include "hk_bionic.h"

#include "../hk_core.h"

#include "../ld/hk_ld_internal.h"

/* --------------------------------------------------------------- registry */

#define REG_SLOTS 8192                  /* a power of two, several times the number of symbols */
static hk_sym g_reg[REG_SLOTS];
static u32 g_reg_count;
static bool g_inited;

static u32 name_hash(const char *s)
{
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

void hk_bionic_register(const hk_sym *table)
{
    for (; table->name; table++) {
        u32 i = name_hash(table->name) & (REG_SLOTS - 1);
        while (g_reg[i].name && strcmp(g_reg[i].name, table->name)) i = (i + 1) & (REG_SLOTS - 1);
        if (!g_reg[i].name) {
            if (++g_reg_count > REG_SLOTS * 3 / 4) hk_fatal("bionic: symbol registry is full");
            g_reg[i].name = table->name;
        }
        /* A later table overrides an earlier one: a driver may replace a system function. */
        g_reg[i].addr = table->addr;
    }
}

void *hk_bionic_find(const char *name)
{
    u32 i = name_hash(name) & (REG_SLOTS - 1);
    while (g_reg[i].name) {
        if (!strcmp(g_reg[i].name, name)) return g_reg[i].addr;
        i = (i + 1) & (REG_SLOTS - 1);
    }
    return NULL;
}

bool hk_bionic_is_system_lib(const char *soname)
{
    static const char *const libs[] = {
        "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
        "libOpenSLES.so", "libz.so", "libjnigraphics.so", "libstdc++.so", "libOpenMAXAL.so", "libmediandk.so", "libaaudio.so",
        "libnativewindow.so", "libcamera2ndk.so", "libvulkan.so", "libamidi.so", "libbinder_ndk.so", "libsync.so", "libneuralnetworks.so",
        /* The WebView's platform support, whose natives drivers/hk_java_webview.c answers: the WebView loads it by name. */
        "libwebviewchromium_plat_support.so",
        /* Part of Android's runtime, which libcore's libopenjdk.so imports from: answered by dvm/hk_dvm_jvm.c. */
        "libopenjdkjvm.so",
    };
    size_t i;
    for (i = 0; i < HK_COUNT(libs); i++)
        if (!strcmp(soname, libs[i])) return true;
    return false;
}

/* ------------------------------------------------------------------ errno */

int *hk_errno_ptr(void) { return &hk_gthread_self()->gerrno; }
void hk_set_errno(int guest_errno) { hk_gthread_self()->gerrno = guest_errno; }

int hk_errno_of(int host_err)
{
    switch (host_err) {
    case HK_E_OK: return 0;
    case HK_E_NOENT: return GE_NOENT;
    case HK_E_ACCESS: return GE_ACCES;
    case HK_E_EXIST: return GE_EXIST;
    case HK_E_NOTDIR: return GE_NOTDIR;
    case HK_E_ISDIR: return GE_ISDIR;
    case HK_E_NOSPC: return GE_NOSPC;
    case HK_E_INVAL: return GE_INVAL;
    case HK_E_NOTEMPTY: return GE_NOTEMPTY;
    case HK_E_BUSY: return GE_BUSY;
    case HK_E_XDEV: return GE_XDEV;
    default: return GE_IO;
    }
}

/* ----------------------------------------------------------- guest threads */

static hk_lock g_thread_lock = HK_LOCK_INIT;
static u8 g_tid_used[4096];             /* a bit per thread id; ids are reused, as the kernel's are */
static hk_gthread *g_tid_thread[32761];  /* the live thread of each id in use */
static i32 g_tid_next = 1;

i32 hk_gthread_take_tid(hk_gthread *thread)
{
    i32 n;
    hk_lock_acquire(&g_thread_lock);
    for (n = 0; n < 32760; n++) {
        i32 t = g_tid_next;
        g_tid_next = g_tid_next >= 32760 ? 1 : g_tid_next + 1;
        if (!(g_tid_used[t >> 3] & (1 << (t & 7)))) {
            g_tid_used[t >> 3] |= (u8)(1 << (t & 7));
            g_tid_thread[t] = thread;
            hk_lock_release(&g_thread_lock);
            return t;
        }
    }
    hk_lock_release(&g_thread_lock);
    hk_fatal("bionic: out of thread ids");
}

void hk_gthread_release_tid(i32 tid)
{
    hk_lock_acquire(&g_thread_lock);
    g_tid_used[tid >> 3] &= (u8)~(1 << (tid & 7));
    g_tid_thread[tid] = NULL;
    hk_lock_release(&g_thread_lock);
}

hk_thread *hk_gthread_host_of_tid(i32 tid)
{
    /* (A host thread's record lives as long as the process; the guest thread's goes at its join, hence the lock.) */
    hk_thread *h = NULL;
    hk_lock_acquire(&g_thread_lock);
    if (tid > 0 && tid < (i32)HK_COUNT(g_tid_thread) && g_tid_thread[tid]) h = g_tid_thread[tid]->sig_thread;
    hk_lock_release(&g_thread_lock);
    return h;
}

/*
 * The flag trace-cpu (hk_run.c starts it with the app): every five seconds, how much of the processor the app had -- the
 * process, in the phone's cores -- and the eight of its threads that had the most, each by its id and name as a share of
 * one core. Whether an app that is slow is held by one thread (near a whole core, while the process has a quarter of a
 * four-core phone) or by all of them. A thread is counted here once it has run guest code (a Java thread that never
 * called native code is in the process's figure only). At most 2000 lines a run.
 */
#define CPU_TOP 8

typedef struct cpu_use {
    i32 tid;
    u64 ns;
    char name[32];
} cpu_use;

/* Each live guest thread's processor time now, by id; `before` and `host` keep the last round's, `top` the most since. */
static int cpu_round(u64 *before, hk_thread **host, cpu_use *top, int *threads)
{
    int i, k, n = 0;
    *threads = 0;
    hk_lock_acquire(&g_thread_lock);
    for (i = 1; i < (int)HK_COUNT(g_tid_thread); i++) {
        hk_gthread *g = g_tid_thread[i];
        hk_thread *h = g ? g->sig_thread : NULL;
        u64 ns, d;
        if (!h) continue;
        ++*threads;
        ns = hk_thread_cpu_ns(h);
        /* (An id used again by another thread: that thread's time from its start, which is within the round.) */
        d = host[i] == h && ns >= before[i] ? ns - before[i] : ns;
        before[i] = ns;
        host[i] = h;
        if (!top) continue;
        for (k = n; k > 0 && top[k - 1].ns < d; k--)
            if (k < CPU_TOP) top[k] = top[k - 1];
        if (k < CPU_TOP) {
            top[k].tid = i;
            top[k].ns = d;
            hk_strlcpy(top[k].name, g->name, sizeof(top[k].name));
            if (n < CPU_TOP) n++;
        }
    }
    hk_lock_release(&g_thread_lock);
    return n;
}

static void cpu_observer(void *unused)
{
    u64 *before = (u64 *)hk_alloc(sizeof(u64) * HK_COUNT(g_tid_thread));
    hk_thread **host = (hk_thread **)hk_alloc(sizeof(hk_thread *) * HK_COUNT(g_tid_thread));
    u64 last = hk_time_monotonic_ns(), last_process = hk_process_cpu_ns();
    int lines = 0, cores = hk_cpu_count(), threads;
    (void)unused;
    cpu_round(before, host, NULL, &threads);
    while (lines++ < 2000) {
        cpu_use top[CPU_TOP];
        char text[700];
        size_t at;
        u64 now, process, span;
        int n, i;
        hk_sleep_ns(5000000000ull);
        n = cpu_round(before, host, top, &threads);
        now = hk_time_monotonic_ns();
        process = hk_process_cpu_ns();
        span = now - last > 0 ? now - last : 1;
        at = (size_t)snprintf(text, sizeof(text), "cpu: %.1f s: the process had %.2f of the phone's %d cores (%.0f %%); of its %d threads that ran guest code, the most:",
                              (double)span / 1e9, (double)(process - last_process) / (double)span, cores,
                              100.0 * (double)(process - last_process) / (double)span / (cores > 0 ? cores : 1), threads);
        /* (A thread with less than a hundredth of a core is not named.) */
        for (i = 0; i < n && at + 64 < sizeof(text) && (i == 0 || top[i].ns * 100 >= span); i++)
            at += (size_t)snprintf(text + at, sizeof(text) - at, "%s %d \"%s\" %.2f", i ? "," : "", (int)top[i].tid, top[i].name, (double)top[i].ns / (double)span);
        hk_info("%s", text);
        last = now;
        last_process = process;
    }
    free(before);
    free(host);
}

void hk_bionic_cpu_trace_start(void)
{
    static volatile i32 started;
    hk_thread *t;
    if (hk_atomic_xchg(&started, 1)) return;
    t = hk_thread_start(cpu_observer, NULL, 64u * 1024u, "cpu-observer");
    if (t) {
        hk_thread_detach(t);
        hk_info("cpu: every five seconds, the processor the app had and its busiest threads (the flag trace-cpu)");
    }
}

bool hk_app_threads_give_way(void)
{
    static int give_way = -1;
    if (give_way < 0) give_way = hk_flag("normal-priority") ? 0 : 1;
    return give_way != 0;
}

hk_gthread *hk_gthread_self(void)
{
    void **tp = hk_tls_thread_pointer();
    hk_gthread *t = (hk_gthread *)tp[1];
    if (t) return t;
    t = (hk_gthread *)hk_alloc(sizeof(*t));
    t->tp = tp;
    t->tid = hk_gthread_take_tid(t);
    hk_thread_stack_bounds(&t->stack_lo, &t->stack_hi);
    t->sig_thread = hk_thread_current();
    /* bionic's slots: 1 is the thread, 2 its errno. */
    tp[1] = t;
    tp[2] = &t->gerrno;
    return t;
}

/* ------------------------------------------------------ system properties */

typedef struct prop {
    char *name, *value;
    struct prop *next;
} prop;
static prop *g_props;
static hk_lock g_prop_lock = HK_LOCK_INIT;

void hk_sysprop_set(const char *name, const char *value)
{
    prop *p;
    hk_lock_acquire(&g_prop_lock);
    for (p = g_props; p; p = p->next) {
        if (!strcmp(p->name, name)) {
            free(p->value);
            p->value = hk_strdup(value);
            hk_lock_release(&g_prop_lock);
            return;
        }
    }
    p = (prop *)hk_alloc(sizeof(*p));
    p->name = hk_strdup(name);
    p->value = hk_strdup(value);
    p->next = g_props;
    g_props = p;
    hk_lock_release(&g_prop_lock);
}

const char *hk_sysprop(const char *name)
{
    prop *p;
    const char *v = NULL;
    hk_lock_acquire(&g_prop_lock);
    for (p = g_props; p; p = p->next) {
        if (!strcmp(p->name, name)) {
            v = p->value;
            break;
        }
    }
    hk_lock_release(&g_prop_lock);
    return v;
}

/* The phone's time zone (hk_bionic.h). Kept here as well as in the property, because the properties' defaults are set later than a shell may speak. */
static char g_timezone[64];

void hk_timezone_set(const char *id)
{
    hk_strlcpy(g_timezone, id ? id : "", sizeof(g_timezone));
    hk_sysprop_set("persist.sys.timezone", g_timezone[0] ? g_timezone : "UTC");
}

const char *hk_timezone_id(void) { return g_timezone; }

/* The API level the device says it has: one number for the property, the NDK's AConfiguration and android_get_device_api_level. */
int hk_sdk_level(void)
{
    const char *v = hk_sysprop("ro.build.version.sdk");
    int n = v ? atoi(v) : 0;
    return n > 0 ? n : 28;
}

static void default_props(void)
{
    /* Android 9: the newest release whose devices were still commonly 32-bit. */
    static const char *const d[][2] = {
        { "ro.build.version.sdk", "28" },
        { "ro.build.version.release", "9" },
        { "ro.build.version.codename", "REL" },
        { "ro.build.id", "PQ3A.190801.002" },
        { "ro.build.display.id", "Megler" },
        { "ro.build.type", "user" },
        { "ro.build.tags", "release-keys" },
        { "ro.build.fingerprint", "megler/w10m/w10m:9/PQ3A.190801.002/1:user/release-keys" },
        { "ro.product.model", "Windows Phone" },
        { "ro.product.brand", "Megler" },
        { "ro.product.name", "w10m" },
        { "ro.product.device", "w10m" },
        { "ro.product.board", "w10m" },
        { "ro.product.manufacturer", "Microsoft" },
        { "ro.product.cpu.abi", "armeabi-v7a" },
        { "ro.product.cpu.abi2", "armeabi" },
        { "ro.product.cpu.abilist", "armeabi-v7a,armeabi" },
        { "ro.product.cpu.abilist32", "armeabi-v7a,armeabi" },
        { "ro.product.cpu.abilist64", "" },
        { "ro.hardware", "qcom" },
        { "ro.board.platform", "msm" },
        { "ro.debuggable", "0" },
        { "ro.secure", "1" },
        { "ro.kernel.qemu", "0" },
        { "ro.sf.lcd_density", "320" },
        { "persist.sys.timezone", "UTC" },
        { "persist.sys.locale", "en-US" },
        { "ro.opengles.version", "131072" },
    };
    size_t i;
    for (i = 0; i < HK_COUNT(d); i++) hk_sysprop_set(d[i][0], d[i][1]);
    /* A zone the shell named before this ran is not undone by the default. */
    if (g_timezone[0]) hk_sysprop_set("persist.sys.timezone", g_timezone);
}

/* ------------------------------------------------------------------- init */

void (*hk_guest_exit_hook)(int status);

void hk_bionic_init(void)
{
    if (g_inited) return;
    g_inited = true;
    default_props();
    hk_bionic_register(hk_tab_mem);
    hk_bionic_register(hk_tab_str);
    hk_bionic_register(hk_tab_wchar);
    hk_bionic_register(hk_tab_stdio);
    hk_bionic_register(hk_tab_io);
    hk_bionic_register(hk_tab_misc);
    hk_bionic_register(hk_tab_time);
    hk_bionic_register(hk_tab_math);
    hk_bionic_register(hk_tab_pthread);
    hk_bionic_register(hk_tab_dl);
    hk_bionic_register(hk_tab_log);
    hk_bionic_register(hk_tab_aeabi);
    hk_bionic_register(hk_tab_net);
    hk_bionic_register(hk_tab_netdb);
    hk_bionic_register(hk_tab_zlib);
    hk_bionic_register(hk_tab_ndk);
    hk_bionic_register(hk_tab_native_activity);
    hk_bionic_register(hk_opensles_table());
    hk_bionic_register(hk_media_table());
    hk_bionic_register(hk_aaudio_table());
    hk_bionic_register(hk_ahb_table());
    hk_bionic_register(hk_java_ndk_table());
}
