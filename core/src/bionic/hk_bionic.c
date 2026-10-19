/* The symbol registry, errno, guest threads' state and system properties. */
#include "hk_bionic.h"

#include "../hk_core.h"

#include "../ld/hk_ld_internal.h"
#include "../dvm/hk_dvm_internal.h"
#include "../ndk/hk_egl.h"

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
/* Who holds g_thread_lock, since when and where (the hang report): taken and let go only through these two. */
static volatile u32 g_thread_lock_owner;
static volatile u64 g_thread_lock_at;
static const char *volatile g_thread_lock_site;
/* The guest thread prof_sample is stopping while it holds the lock, 0 for none. */
static volatile i32 g_prof_stopping;

static void thread_lock(const char *site)
{
    hk_lock_acquire(&g_thread_lock);
    g_thread_lock_at = hk_time_monotonic_ns();
    g_thread_lock_site = site;
    g_thread_lock_owner = hk_thread_self_id();
}

static void thread_unlock(void)
{
    g_thread_lock_owner = 0;
    hk_lock_release(&g_thread_lock);
}

i32 hk_gthread_take_tid(hk_gthread *thread)
{
    i32 n;
    thread_lock("hk_gthread_take_tid");
    for (n = 0; n < 32760; n++) {
        i32 t = g_tid_next;
        g_tid_next = g_tid_next >= 32760 ? 1 : g_tid_next + 1;
        if (!(g_tid_used[t >> 3] & (1 << (t & 7)))) {
            g_tid_used[t >> 3] |= (u8)(1 << (t & 7));
            g_tid_thread[t] = thread;
            thread_unlock();
            return t;
        }
    }
    thread_unlock();
    hk_fatal("bionic: out of thread ids");
}

void hk_gthread_release_tid(i32 tid)
{
    thread_lock("hk_gthread_release_tid");
    g_tid_used[tid >> 3] &= (u8)~(1 << (tid & 7));
    g_tid_thread[tid] = NULL;
    thread_unlock();
}

hk_thread *hk_gthread_host_of_tid(i32 tid)
{
    /* (A host thread's record lives as long as the process; the guest thread's goes at its join, hence the lock.) */
    hk_thread *h = NULL;
    thread_lock("hk_gthread_host_of_tid");
    if (tid > 0 && tid < (i32)HK_COUNT(g_tid_thread) && g_tid_thread[tid]) h = g_tid_thread[tid]->sig_thread;
    thread_unlock();
    return h;
}

/*
 * The flag trace-cpu (hk_run.c starts it with the app): every five seconds, how much of the processor the app had -- the
 * process, in the phone's cores -- and the eight of its threads that had the most, each by its id and name as a share of
 * one core. Whether an app that is slow is held by one thread (near a whole core, while the process has a quarter of a
 * four-core phone) or by all of them. A thread is counted here once it has run guest code (a Java thread that never
 * called native code is in the process's figure only). And the memory then: what the app has in use against its limit,
 * what its textures held come to (ndk/hk_gltex.c), its address space (free, the largest free stretch, committed), and the
 * graphics memory ANGLE's device has used against its budget -- an app that ends out of memory is followed there. At most
 * 2000 lines a run.
 */
#define CPU_TOP 8

static volatile i32 g_stacks_round;

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
    thread_lock("cpu_round");
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
    thread_unlock();
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
        char text[900];
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
        {
            u32 free_mb = 0, largest = 0, reserved = 0, committed = 0, gpu_used = 0, gpu_budget = 0;
            at += (size_t)snprintf(text + at, sizeof(text) - at, "; memory: %u MB in use of %u, textures held %u MB", (unsigned)(hk_mem_usage() >> 20),
                                   (unsigned)(hk_mem_limit() >> 20), (unsigned)(hk_gltex_held_bytes() >> 20));
            if (hk_host_address_space(&free_mb, &largest, &reserved, &committed) && at < sizeof(text))
                at += (size_t)snprintf(text + at, sizeof(text) - at, ", addresses %u MB free (the largest stretch %u), %u committed", (unsigned)free_mb,
                                       (unsigned)largest, (unsigned)committed);
            if (hk_egl_gpu_memory(&gpu_used, &gpu_budget) && at < sizeof(text))
                snprintf(text + at, sizeof(text) - at, ", graphics memory %u of %u MB", (unsigned)gpu_used, (unsigned)gpu_budget);
        }
        hk_info("%s", text);
        last = now;
        last_process = process;
    }
    free(before);
    free(host);
}

/*
 * The flag trace-native-stacks: every ten seconds, where each of the app's threads that run guest code is -- its pc, its
 * lr and what its stack holds that looks like a way back into a library -- the way a crash report says where the thread
 * that faulted was. Not an unwinding, but it names the function a thread is in and who called it: where a game that
 * stands still waits, and on what. Each thread is stopped only while its registers and the top 8 KB of its stack are
 * copied; what the words are is worked out after it goes on. The round holds the runtime's lock, as the collector and
 * trace-stacks do while they stop threads. Only on Windows (the phone), where a thread is stopped by SuspendThread
 * whatever it is doing: elsewhere hk_thread_suspend sends a signal and waits for its handler, and a guest thread may hold
 * that signal back -- the collector stops Java threads only, which never do; the data app's run under qemu stood still
 * on one. There a round says only that it does not look. At most 60 threads a round, 1500 lines a run.
 */
#define STACK_WORDS 2048

typedef struct native_stack {
    i32 tid;
    char name[32];
    hk_addr pc, lr;
    u32 words[STACK_WORDS];
    int nwords;
} native_stack;

/* "lib+0x1234" for an address of a library of the app's or a module of the host's; false for neither. */
static bool code_at(hk_addr a, char *out, size_t size)
{
    const char *lib = NULL;
    const void *sym = NULL, *base = NULL;
    char module[96];
    hk_addr mbase = 0;
    a &= ~(hk_addr)1;
    if (a < 0x10000) return false;
    if (hk_ld_is_guest_code(a)) {
        hk_ld_symbol_at((const void *)a, &lib, &sym, &base);
        if (!lib || !base) return false;
        snprintf(out, size, "%s+0x%x", lib, (unsigned)(a - (hk_addr)base));
        return true;
    }
    if (hk_host_module_at(a, module, sizeof(module), &mbase) && a != mbase) {
        snprintf(out, size, "%s+0x%x", module, (unsigned)(a - mbase));
        return true;
    }
    return false;
}

static void stacks_round(native_stack *stacks, int *lines)
{
    int n = 0, i, k;
#if !defined(_WIN32)
    if (!*lines) hk_info("stacks: native threads are not looked at on this host (a thread is stopped by a signal here, which it may hold back)");
    ++*lines;
    return;
#endif
    hk_dvm_lock();
    thread_lock("stacks_round");
    for (i = 1; i < (int)HK_COUNT(g_tid_thread) && n < 60; i++) {
        hk_gthread *g = g_tid_thread[i];
        hk_thread *h = g ? g->sig_thread : NULL;
        hk_thread_regs regs;
        native_stack *s = &stacks[n];
        if (!h || h == hk_thread_current() || !hk_thread_suspend(h, &regs)) continue;
        s->tid = i;
        hk_strlcpy(s->name, g->name, sizeof(s->name));
        s->pc = regs.pc;
        s->lr = regs.n > 13 ? regs.r[13] : 0;
        s->nwords = 0;
        if (g->stack_lo && g->stack_hi && regs.sp >= (hk_addr)g->stack_lo && regs.sp < (hk_addr)g->stack_hi) {
            hk_addr top = (hk_addr)g->stack_hi, at = regs.sp & ~(hk_addr)3;
            s->nwords = (int)((top - at) / 4 < STACK_WORDS ? (top - at) / 4 : STACK_WORDS);
            memcpy(s->words, (const void *)at, (size_t)s->nwords * 4);
        }
        hk_thread_resume(h);
        n++;
    }
    thread_unlock();
    hk_dvm_unlock();
    for (i = 0; i < n && *lines < 1500; i++) {
        native_stack *s = &stacks[i];
        char text[900], where[128];
        size_t at;
        int found = 0;
        u32 last = 0;
        at = (size_t)snprintf(text, sizeof(text), "stacks: native thread %d \"%s\": pc ", (int)s->tid, s->name);
        at += (size_t)snprintf(text + at, sizeof(text) - at, "%s", code_at(s->pc, where, sizeof(where)) ? where : "?");
        at += (size_t)snprintf(text + at, sizeof(text) - at, ", lr %s; on the stack:", code_at(s->lr, where, sizeof(where)) ? where : "?");
        for (k = 0; k < s->nwords && found < 14 && at + 64 < sizeof(text); k++) {
            u32 w = s->words[k] & ~1u;
            if (w == last || !code_at(w, where, sizeof(where))) continue;
            at += (size_t)snprintf(text + at, sizeof(text) - at, "%s %s", found ? "," : "", where);
            last = w;
            found++;
        }
        hk_info("%s", text);
        ++*lines;
    }
}

static void stacks_observer(void *unused)
{
    native_stack *stacks = (native_stack *)hk_alloc(sizeof(native_stack) * 60);
    int lines = 0;
    (void)unused;
    while (lines < 1500) {
        hk_sleep_ns(10000000000ull);
        if (lines < 1500 && hk_atomic_add(&g_stacks_round, 1) >= 0) hk_info("stacks: native round, every thread that runs guest code (the flag trace-native-stacks)");
        stacks_round(stacks, &lines);
    }
    free(stacks);
}

void hk_bionic_native_stacks_start(void)
{
    static volatile i32 started;
    hk_thread *t;
    if (hk_atomic_xchg(&started, 1)) return;
#if !defined(_WIN32)
    /* (Said at once, not at the first round ten seconds on: a run may be over before it.) */
    hk_info("stacks: native threads are not looked at on this host (a thread is stopped by a signal here, which it may hold back)");
    return;
#endif
    t = hk_thread_start(stacks_observer, NULL, 64u * 1024u, "native-stacks-observer");
    if (t) hk_thread_detach(t);
}

/*
 * The flag trace-profile: where the app's busiest threads spend their time, sampled -- what a game that is slow with
 * the graphics processor idle is doing instead. Every second the three guest threads that had the most of the processor
 * (a tenth of a core at least) are chosen, and each is stopped about sixty times a second for as long as its pc and the
 * top 8 KB of its stack take to copy (no lock is taken while it is stopped). A sample whose pc is in the system's own
 * code (ntdll, KERNELBASE, kernel32, and win32u, whose system calls are the graphics driver's into the kernel) is a wait --
 * a thread stopped in a system call is in the kernel, blocked nearly always -- and
 * is put to the first return address on its stack that is neither the system's nor the layer's (Megler.exe, whose locks
 * and condition variables the app's waits go through): what it waits for, in ANGLE, the driver or the app. And every
 * sample is put to the innermost frame of ANGLE's (libGLESv2.dll) it is under -- its pc, or the first return address
 * into ANGLE on its stack: the ANGLE function a stretch of time in Direct3D, the driver or the kernel below it is
 * spent for (a race of Asphalt 9's had two thirds of its render thread in the driver's calls into the kernel, which
 * alone says nothing of why). Every ten seconds the log says, for each thread sampled then, its share of a core and its
 * samples by module, running and waiting; and since the start the six addresses it ran at, waited from and was under
 * ANGLE at most. LocalState\profile.txt has every
 * address of every thread with its count, rewritten every ten seconds: tools/profile-names.py names them by function
 * with the build's maps (ANGLE's, the layer's, an app library's exported symbols). Only on Windows, as
 * trace-native-stacks. At most 1000 lines a run.
 */
#define PROF_THREADS 3
#define PROF_SLOTS 8192u
#define PROF_WORDS 2048
#define PROF_MODULES 96
#define PROF_TOP 6

enum { PROF_OTHER, PROF_SYSTEM, PROF_LAYER };

typedef struct prof_module {
    char name[48];
    hk_addr base;
    int kind;
    bool gl;                           /* ANGLE's libGLESv2.dll: what a sample is "under" */
    int ncode;                         /* its executable sections (a DLL's, from its PE header); -1: an app's library, asked of the loader */
    hk_addr code_lo[4], code_hi[4];
} prof_module;

typedef struct prof_slot {
    i32 tid;                           /* 0: free */
    hk_thread *host;
    char name[32];
    u64 chosen_at;                     /* when it was last among the busiest */
    u32 samples, waits, unknown;       /* since it was first chosen; unknown: waits whose caller was not found */
    u32 w_samples, w_waits, w_unknown, w_run_unknown;   /* this window's */
    u64 w_cpu_ns;
    u32 w_run_mod[PROF_MODULES], w_wait_mod[PROF_MODULES];
    hk_addr run_addr[PROF_SLOTS], wait_addr[PROF_SLOTS], under_addr[PROF_SLOTS];
    u32 run_count[PROF_SLOTS], wait_count[PROF_SLOTS], under_count[PROF_SLOTS];
} prof_slot;

static prof_module *g_pmod;
static int g_npmod;
static u16 *g_ppage;                   /* each 64 KB of the address space: its module's index + 1; 0xFFFF none; 0 not looked at */
static volatile i32 g_prof_lines;

static bool prof_line_left(void) { return hk_atomic_add(&g_prof_lines, 1) <= 1000; }

static int prof_kind(const char *name)
{
    static const char *const system[] = { "ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll" };
    char low[48];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof(low); i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    for (i = 0; i < HK_COUNT(system); i++)
        if (!strcmp(low, system[i])) return PROF_SYSTEM;
    return !strcmp(low, "megler.exe") ? PROF_LAYER : PROF_OTHER;
}

/* A DLL's executable sections, from the PE header Windows mapped at its base. (A word on a stack that points into a module
   is a return address only if it points into its code: one into its data -- a lock, a table -- is a pointer, and was taken
   for a frame of ANGLE's in half of a race's samples.) */
static void prof_code_ranges(prof_module *m)
{
    const u8 *b = (const u8 *)m->base;
    u32 pe = 0, i, nsec, opt;
    u16 v16;
    m->ncode = 0;
    if (!b || b[0] != 'M' || b[1] != 'Z') return;
    memcpy(&pe, b + 0x3C, 4);
    if (pe > 4096 || memcmp(b + pe, "PE\0\0", 4) != 0) return;
    memcpy(&v16, b + pe + 6, 2);
    nsec = v16;
    memcpy(&v16, b + pe + 20, 2);
    opt = v16;
    for (i = 0; i < nsec && m->ncode < 4; i++) {
        const u8 *sh = b + pe + 24 + opt + i * 40;
        u32 vsize, va, flags;
        memcpy(&vsize, sh + 8, 4);
        memcpy(&va, sh + 12, 4);
        memcpy(&flags, sh + 36, 4);
        if (flags & 0x20000000u) {     /* IMAGE_SCN_MEM_EXECUTE */
            m->code_lo[m->ncode] = m->base + va;
            m->code_hi[m->ncode] = m->base + va + vsize;
            m->ncode++;
        }
    }
}

/* Whether `a`, in module `m`, is in its code: what a return address is. */
static bool prof_is_code(int m, hk_addr a)
{
    int i;
    if (g_pmod[m].ncode < 0) return hk_ld_is_guest_code(a);
    if (!g_pmod[m].ncode) return true;
    for (i = 0; i < g_pmod[m].ncode; i++)
        if (a >= g_pmod[m].code_lo[i] && a < g_pmod[m].code_hi[i]) return true;
    return false;
}

static bool prof_kind_gl(const char *name)
{
    char low[48];
    size_t i;
    for (i = 0; name[i] && i + 1 < sizeof(low); i++) low[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    low[i] = 0;
    return !strcmp(low, "libglesv2.dll");
}

/* The module an address is in, by its 64 KB (no two modules share one: Windows maps them at that granularity, as the
   layer's loader does an app's libraries); -1 for none. Only the profiler's thread calls it. */
static int prof_module_of(hk_addr a, hk_addr *off)
{
    bool cached = (a >> 16) < 65536u;
    u32 page = (u32)(a >> 16);
    int m = -1;
    if (cached && g_ppage[page]) {
        if (g_ppage[page] == 0xFFFF) return -1;
        m = g_ppage[page] - 1;
    } else {
        const char *lib = NULL;
        const void *sym = NULL, *base = NULL;
        char name[96];
        hk_addr mbase = 0;
        bool found = false, guest = false;
        int i;
        if (hk_ld_is_guest_code(a)) {
            hk_ld_symbol_at((const void *)a, &lib, &sym, &base);
            if (lib && base) {
                hk_strlcpy(name, lib, sizeof(name));
                mbase = (hk_addr)base;
                found = guest = true;
            }
        } else if (hk_host_module_at(a, name, sizeof(name), &mbase)) {
            found = true;
        }
        if (found) {
            for (i = 0; i < g_npmod; i++)
                if (g_pmod[i].base == mbase && !strcmp(g_pmod[i].name, name)) break;
            if (i == g_npmod && g_npmod < PROF_MODULES) {
                hk_strlcpy(g_pmod[i].name, name, sizeof(g_pmod[i].name));
                g_pmod[i].base = mbase;
                g_pmod[i].kind = prof_kind(g_pmod[i].name);
                g_pmod[i].gl = prof_kind_gl(g_pmod[i].name);
                if (guest) g_pmod[i].ncode = -1;
                else prof_code_ranges(&g_pmod[i]);
                g_npmod++;
            }
            if (i < g_npmod) m = i;
        }
        if (cached) g_ppage[page] = m >= 0 ? (u16)(m + 1) : 0xFFFF;
    }
    if (m >= 0 && off) *off = a - g_pmod[m].base;
    return m;
}

static void prof_add(hk_addr *addr, u32 *count, hk_addr a)
{
    u32 h = (u32)(a >> 1) * 2654435761u, i;
    for (i = 0; i < 64; i++) {
        u32 at = (h + i) & (PROF_SLOTS - 1);
        if (addr[at] == a) {
            count[at]++;
            return;
        }
        if (!addr[at]) {
            addr[at] = a;
            count[at] = 1;
            return;
        }
    }
    /* (Nothing free near it: the sample is in the totals only.) */
}

static void prof_sample(prof_slot *s, u32 *words)
{
    hk_gthread *g;
    hk_thread_regs regs;
    hk_addr pc;
    int n = 0, k, m;
    thread_lock("prof_sample");
    g = s->tid > 0 && s->tid < (i32)HK_COUNT(g_tid_thread) ? g_tid_thread[s->tid] : NULL;
    g_prof_stopping = s->tid;
    if (!g || g->sig_thread != s->host || !hk_thread_suspend(s->host, &regs)) {
        g_prof_stopping = 0;
        thread_unlock();
        return;
    }
    g_prof_stopping = 0;
    if (g->stack_lo && g->stack_hi && regs.sp >= (hk_addr)g->stack_lo && regs.sp < (hk_addr)g->stack_hi) {
        hk_addr top = (hk_addr)g->stack_hi, at = regs.sp & ~(hk_addr)3;
        n = (int)((top - at) / 4 < PROF_WORDS ? (top - at) / 4 : PROF_WORDS);
        memcpy(words, (const void *)at, (size_t)n * 4);
    }
    hk_thread_resume(s->host);
    thread_unlock();
    pc = regs.pc & ~(hk_addr)1;
    s->samples++;
    s->w_samples++;
    m = pc >= 0x10000 ? prof_module_of(pc, NULL) : -1;
    if (m >= 0 && g_pmod[m].gl) {
        prof_add(s->under_addr, s->under_count, pc);
    } else {
        for (k = 0; k < n; k++) {
            hk_addr w = (hk_addr)(words[k] & ~1u);
            int wm;
            if (w < 0x10000) continue;
            wm = prof_module_of(w, NULL);
            if (wm >= 0 && g_pmod[wm].gl && prof_is_code(wm, w)) {
                prof_add(s->under_addr, s->under_count, w);
                break;
            }
        }
    }
    if (m >= 0 && g_pmod[m].kind == PROF_SYSTEM) {
        int from = -1;
        hk_addr w = 0;
        for (k = 0; k < n && from < 0; k++) {
            int wm;
            w = (hk_addr)(words[k] & ~1u);
            if (w < 0x10000) continue;
            wm = prof_module_of(w, NULL);
            if (wm >= 0 && g_pmod[wm].kind == PROF_OTHER && prof_is_code(wm, w)) from = wm;
        }
        s->waits++;
        s->w_waits++;
        if (from >= 0) {
            prof_add(s->wait_addr, s->wait_count, w);
            s->w_wait_mod[from]++;
        } else {
            s->unknown++;
            s->w_unknown++;
        }
    } else {
        prof_add(s->run_addr, s->run_count, pc);
        if (m >= 0) s->w_run_mod[m]++;
        else s->w_run_unknown++;
    }
}

/* "module 12%" for each module with 2 % of the samples or more, the most first. */
static size_t prof_modules_text(char *out, size_t size, const u32 *by_mod, u32 total)
{
    bool said[PROF_MODULES];
    size_t at = 0;
    int i, found = 0;
    memset(said, 0, sizeof(said));
    out[0] = 0;
    for (;;) {
        int best = -1;
        for (i = 0; i < g_npmod; i++)
            if (!said[i] && by_mod[i] && (best < 0 || by_mod[i] > by_mod[best])) best = i;
        if (best < 0 || by_mod[best] * 50 < total || at + 64 >= size) break;
        said[best] = true;
        at += (size_t)snprintf(out + at, size - at, "%s%s %u%%", found++ ? ", " : "", g_pmod[best].name, (unsigned)(by_mod[best] * 100 / total));
    }
    return at;
}

/* The `PROF_TOP` addresses with the most samples, as "module+0x1234 3.1%". */
static size_t prof_top_text(char *out, size_t size, const hk_addr *addr, const u32 *count, u32 total)
{
    u32 took[PROF_TOP];
    size_t at = 0;
    int t, found = 0;
    u32 i;
    out[0] = 0;
    for (t = 0; t < PROF_TOP; t++) {
        u32 best = PROF_SLOTS;
        int k;
        hk_addr off = 0;
        int m;
        for (i = 0; i < PROF_SLOTS; i++) {
            bool before = false;
            if (!addr[i]) continue;
            for (k = 0; k < t; k++)
                if (took[k] == i) before = true;
            if (!before && (best == PROF_SLOTS || count[i] > count[best])) best = i;
        }
        if (best == PROF_SLOTS || at + 80 >= size) break;
        took[t] = best;
        m = prof_module_of(addr[best], &off);
        at += (size_t)snprintf(out + at, size - at, "%s%s+0x%x %.1f%%", found++ ? ", " : "", m >= 0 ? g_pmod[m].name : "?", (unsigned)off,
                               total ? 100.0 * count[best] / total : 0.0);
    }
    return at;
}

static size_t prof_dump_hist(char *out, size_t size, const char *what, const hk_addr *addr, const u32 *count)
{
    size_t at = 0;
    u32 i;
    for (i = 0; i < PROF_SLOTS && at + 96 < size; i++) {
        hk_addr off = 0;
        int m;
        if (!addr[i]) continue;
        m = prof_module_of(addr[i], &off);
        at += (size_t)snprintf(out + at, size - at, "%s %s 0x%x %u\n", what, m >= 0 ? g_pmod[m].name : "?", (unsigned)(m >= 0 ? off : addr[i]),
                               (unsigned)count[i]);
    }
    return at;
}

const char *megler_data_dir(void);

static void prof_report(prof_slot *slots, double seconds, double since)
{
    char mods[600], waits[600], under[600];
    size_t cap = 4096, at = 0;
    char *file;
    int i;
    u32 k;
    for (i = 0; i < PROF_THREADS; i++) {
        prof_slot *s = &slots[i];
        size_t n;
        if (!s->tid || !s->w_samples) continue;
        n = prof_modules_text(mods, sizeof(mods), s->w_run_mod, s->w_samples);
        if (s->w_run_unknown * 50 >= s->w_samples)
            snprintf(mods + n, sizeof(mods) - n, "%soutside any module %u%%", n ? ", " : "", (unsigned)(s->w_run_unknown * 100 / s->w_samples));
        n = prof_modules_text(waits, sizeof(waits), s->w_wait_mod, s->w_samples);
        if (s->w_unknown * 50 >= s->w_samples)
            snprintf(waits + n, sizeof(waits) - n, "%sa caller not found %u%%", n ? ", " : "", (unsigned)(s->w_unknown * 100 / s->w_samples));
        if (prof_line_left())
            hk_info("profile: thread %d \"%s\", %.2f of a core: %u samples in %.1f s; running %u%% (%s), waiting %u%% (for %s)", (int)s->tid, s->name,
                    (double)s->w_cpu_ns / (seconds * 1e9), (unsigned)s->w_samples, seconds, (unsigned)((s->w_samples - s->w_waits) * 100 / s->w_samples),
                    mods[0] ? mods : "-", (unsigned)(s->w_waits * 100 / s->w_samples), waits[0] ? waits : "-");
        prof_top_text(mods, sizeof(mods), s->run_addr, s->run_count, s->samples);
        prof_top_text(waits, sizeof(waits), s->wait_addr, s->wait_count, s->samples);
        prof_top_text(under, sizeof(under), s->under_addr, s->under_count, s->samples);
        if (prof_line_left())
            hk_info("profile: thread %d since it was first sampled: ran most at %s; waited most for %s; under ANGLE most at %s", (int)s->tid,
                    mods[0] ? mods : "-", waits[0] ? waits : "-", under[0] ? under : "-");
        s->w_samples = s->w_waits = s->w_unknown = s->w_run_unknown = 0;
        s->w_cpu_ns = 0;
        memset(s->w_run_mod, 0, sizeof(s->w_run_mod));
        memset(s->w_wait_mod, 0, sizeof(s->w_wait_mod));
    }
    /* The whole of it, for tools/profile-names.py. */
    for (i = 0; i < PROF_THREADS; i++)
        for (k = 0; k < PROF_SLOTS; k++) cap += (slots[i].run_addr[k] ? 96 : 0) + (slots[i].wait_addr[k] ? 96 : 0) + (slots[i].under_addr[k] ? 96 : 0);
    file = (char *)hk_alloc(cap);
    at += (size_t)snprintf(file + at, cap - at, "# Megler trace-profile, %.0f s after it began: samples by address (module, offset in it, count); "
                                                "run: where a thread was, wait: what a thread in the system's code waited for, "
                                                "under: the innermost frame of ANGLE's a sample was under\n", since);
    for (i = 0; i < PROF_THREADS; i++) {
        prof_slot *s = &slots[i];
        if (!s->tid) continue;
        at += (size_t)snprintf(file + at, cap - at, "thread %d \"%s\" samples %u waits %u unknown %u\n", (int)s->tid, s->name, (unsigned)s->samples,
                               (unsigned)s->waits, (unsigned)s->unknown);
        at += prof_dump_hist(file + at, cap - at, "run", s->run_addr, s->run_count);
        at += prof_dump_hist(file + at, cap - at, "wait", s->wait_addr, s->wait_count);
        at += prof_dump_hist(file + at, cap - at, "under", s->under_addr, s->under_count);
    }
    {
        char path[700];
        snprintf(path, sizeof(path), "%s/profile.txt", megler_data_dir());
        if (!hk_file_write_all(path, file, at) && prof_line_left()) hk_info("profile: %s could not be written", path);
    }
    free(file);
    /* Addresses that were in no module are looked at again: a library may have been loaded there since. */
    for (k = 0; k < 65536u; k++)
        if (g_ppage[k] == 0xFFFF) g_ppage[k] = 0;
}

/* Every second: the busiest guest threads, each given a slot (kept while it stays among them; the one chosen longest ago
   gives its slot up). */
static void prof_choose(prof_slot *slots, u64 *before, hk_thread **host, u64 span, u64 now)
{
    struct { i32 tid; u64 ns; hk_thread *h; char name[32]; } top[PROF_THREADS];
    int n = 0, i, k;
    memset(top, 0, sizeof(top));
    thread_lock("prof_choose");
    for (i = 1; i < (int)HK_COUNT(g_tid_thread); i++) {
        hk_gthread *g = g_tid_thread[i];
        hk_thread *h = g ? g->sig_thread : NULL;
        u64 ns, d;
        if (!h || h == hk_thread_current()) continue;
        ns = hk_thread_cpu_ns(h);
        d = host[i] == h && ns >= before[i] ? ns - before[i] : 0;
        before[i] = ns;
        host[i] = h;
        for (k = 0; k < PROF_THREADS; k++)
            if (slots[k].tid == i && slots[k].host == h) slots[k].w_cpu_ns += d;
        if (d * 10 < span) continue;
        for (k = n; k > 0 && top[k - 1].ns < d; k--)
            if (k < PROF_THREADS) top[k] = top[k - 1];
        if (k < PROF_THREADS) {
            top[k].tid = i;
            top[k].ns = d;
            top[k].h = h;
            hk_strlcpy(top[k].name, g->name, sizeof(top[k].name));
            if (n < PROF_THREADS) n++;
        }
    }
    thread_unlock();
    for (i = 0; i < n; i++) {
        int at = -1, oldest = -1;
        for (k = 0; k < PROF_THREADS; k++) {
            if (slots[k].tid == top[i].tid && slots[k].host == top[i].h) at = k;
            if (oldest < 0 || slots[k].chosen_at < slots[oldest].chosen_at) oldest = k;
        }
        if (at < 0) {
            for (k = 0; k < PROF_THREADS && at < 0; k++)
                if (!slots[k].tid) at = k;
            if (at < 0) at = oldest;
            memset(&slots[at], 0, sizeof(slots[at]));
            slots[at].tid = top[i].tid;
            slots[at].host = top[i].h;
            hk_strlcpy(slots[at].name, top[i].name, sizeof(slots[at].name));
            slots[at].w_cpu_ns = top[i].ns;
        }
        slots[at].chosen_at = now;
    }
}

static void prof_observer(void *unused)
{
    prof_slot *slots = (prof_slot *)hk_alloc(sizeof(prof_slot) * PROF_THREADS);
    u64 *before = (u64 *)hk_alloc(sizeof(u64) * HK_COUNT(g_tid_thread));
    hk_thread **host = (hk_thread **)hk_alloc(sizeof(hk_thread *) * HK_COUNT(g_tid_thread));
    u32 *words = (u32 *)hk_alloc(PROF_WORDS * 4);
    u64 began = hk_time_monotonic_ns(), chose = began, reported = began;
    int i;
    (void)unused;
    g_pmod = (prof_module *)hk_alloc(sizeof(prof_module) * PROF_MODULES);
    g_ppage = (u16 *)hk_alloc(sizeof(u16) * 65536u);
    prof_choose(slots, before, host, ~0ull, began);
    for (;;) {
        u64 now;
        hk_sleep_ns(10000000ull);
        now = hk_time_monotonic_ns();
        if (now - chose >= 1000000000ull) {
            /* (Only the threads still chosen within the last ten seconds are sampled.) */
            prof_choose(slots, before, host, now - chose, now);
            chose = now;
        }
        for (i = 0; i < PROF_THREADS; i++)
            if (slots[i].tid && now - slots[i].chosen_at < 10000000000ull) prof_sample(&slots[i], words);
        if (now - reported >= 10000000000ull) {
            prof_report(slots, (double)(now - reported) / 1e9, (double)(now - began) / 1e9);
            reported = now;
        }
    }
}

void hk_bionic_profile_start(void)
{
    static volatile i32 started;
    hk_thread *t;
    if (hk_atomic_xchg(&started, 1)) return;
#if !defined(_WIN32)
    if (prof_line_left()) hk_info("profile: threads are not sampled on this host (a thread is stopped by a signal here, which it may hold back)");
    return;
#endif
    t = hk_thread_start(prof_observer, NULL, 64u * 1024u, "profile-observer");
    if (t) {
        hk_thread_detach(t);
        if (prof_line_left()) hk_info("profile: sampling the busiest threads sixty times a second, said every ten seconds (the flag trace-profile)");
    }
}

/*
 * The hang report, in every run on Windows: a watchdog that looks every two seconds at who holds the log's lock and the
 * guest threads' lock. Nothing the layer does holds either for long, so one held by the same thread for ten seconds is an
 * app that stands still -- a deadlock, which says nothing of itself: the core's lines stop (each waits for the log's lock)
 * while the shell's go on. Once for each such stand-still it says who holds which lock since when and what it was doing
 * (the line it was writing; the function that took the threads' lock, and the thread trace-profile was stopping), and
 * where each thread that runs guest code is: pc, lr and its stack's way back, as trace-native-stacks says. The threads are
 * found and stopped without either lock -- while another thread holds the threads' lock no thread's record can go, and
 * each is stopped only while its registers and stack are copied. A holder that is not one of them (a thread of the
 * system's, which the exception handler logs from as from any other) is looked at by its id, deeper: its stack holds the
 * exception it is handling and the frames of whoever raised it. Said first as bare addresses in LocalState\hang.txt (to
 * name them may need a lock the stand-still holds), then with module and offset there and in the log, past its lock.
 */
#define HANG_SECONDS 10
#define HANG_THREADS 60
#define HANG_WORDS 1024
#define HANG_HOLDER_WORDS 4096

typedef struct hang_thread {
    i32 tid;
    u32 host_id;
    char name[32];
    hk_addr pc, lr;
    u32 words[HANG_WORDS];
    int nwords;
} hang_thread;

/* A holder that is not a guest thread, by its host id. */
typedef struct hang_holder {
    u32 host_id;
    hk_addr pc, lr;
    u32 words[HANG_HOLDER_WORDS];
    int nwords;
} hang_holder;

static size_t hang_add(char *out, size_t cap, size_t at, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (at >= cap) return at;
    va_start(ap, fmt);
    n = vsnprintf(out + at, cap - at, fmt, ap);
    va_end(ap);
    return n < 0 ? at : (at + (size_t)n < cap ? at + (size_t)n : cap - 1);
}

static void hang_write(const char *text, size_t size)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/hang.txt", megler_data_dir());
    hk_file_write_all(path, text, size);
}

/* `threads`, `holders` and `text` are the watchdog's, made at its start: the allocator's lock may be what stands still. */
static void hang_report(hang_thread *threads, hang_holder *holders, const u32 *holder_ids, const char *what, char *text, size_t text_cap)
{
    int nholders = 0;
    char where[128];
    size_t at = 0, mark;
    int n = 0, i, k;
    bool locked = false;
    /* The threads under g_thread_lock if it comes free within a second; without it if it does not: held by another, no record can go meanwhile. */
    for (k = 0; k < 50 && !(locked = hk_lock_try(&g_thread_lock)); k++) hk_sleep_ns(20000000ull);
    for (i = 1; i < (int)HK_COUNT(g_tid_thread) && n < HANG_THREADS; i++) {
        hk_gthread *g = g_tid_thread[i];
        hk_thread *h = g ? g->sig_thread : NULL;
        hk_thread_regs regs;
        hang_thread *s = &threads[n];
        if (!h || h == hk_thread_current() || !hk_thread_suspend(h, &regs)) continue;
        s->tid = i;
        s->host_id = hk_thread_id(h);
        memcpy(s->name, g->name, sizeof(s->name));
        s->name[sizeof(s->name) - 1] = 0;
        s->pc = regs.pc;
        s->lr = regs.n > 13 ? regs.r[13] : 0;
        s->nwords = 0;
        if (g->stack_lo && g->stack_hi && regs.sp >= (hk_addr)g->stack_lo && regs.sp < (hk_addr)g->stack_hi) {
            hk_addr top = (hk_addr)g->stack_hi, from = regs.sp & ~(hk_addr)3;
            s->nwords = (int)((top - from) / 4 < HANG_WORDS ? (top - from) / 4 : HANG_WORDS);
            memcpy(s->words, (const void *)from, (size_t)s->nwords * 4);
        }
        hk_thread_resume(h);
        n++;
    }
    if (locked) hk_lock_release(&g_thread_lock);
    /* The holders that are none of those threads. */
    for (k = 0; k < 2; k++) {
        hk_thread_regs regs;
        hang_holder *hd = &holders[nholders];
        bool guest = false;
        if (!holder_ids[k] || (k == 1 && holder_ids[1] == holder_ids[0])) continue;
        for (i = 0; i < n && !guest; i++) guest = threads[i].host_id == holder_ids[k];
        if (guest) continue;
        hd->nwords = hk_thread_snapshot_id(holder_ids[k], &regs, hd->words, HANG_HOLDER_WORDS);
        if (hd->nwords < 0) continue;
        hd->host_id = holder_ids[k];
        hd->pc = regs.pc;
        hd->lr = regs.n > 13 ? regs.r[13] : 0;
        nholders++;
    }
    at = hang_add(text, text_cap, at, "hang: %s\n", what);
    mark = at;
    /* First the bare addresses: the words of each stack that may be code, unnamed. */
    for (i = 0; i < n; i++) {
        hang_thread *s = &threads[i];
        int shown = 0;
        at = hang_add(text, text_cap, at, "hang: thread %d \"%s\" (host %u): pc 0x%x, lr 0x%x; on the stack:", (int)s->tid, s->name, (unsigned)s->host_id,
                      (unsigned)s->pc, (unsigned)s->lr);
        for (k = 0; k < s->nwords && shown < 24; k++)
            if (s->words[k] >= 0x10000u && (hk_ld_is_guest_code(s->words[k] & ~1u) || (s->words[k] & 1u))) {
                at = hang_add(text, text_cap, at, " 0x%x", (unsigned)s->words[k]);
                shown++;
            }
        at = hang_add(text, text_cap, at, "\n");
    }
    for (i = 0; i < nholders; i++) {
        hang_holder *hd = &holders[i];
        int shown = 0;
        at = hang_add(text, text_cap, at, "hang: host thread %u, not one of the app's: pc 0x%x, lr 0x%x; on the stack:", (unsigned)hd->host_id, (unsigned)hd->pc,
                      (unsigned)hd->lr);
        for (k = 0; k < hd->nwords && shown < 64; k++)
            if (hd->words[k] >= 0x10000u && (hd->words[k] & 1u)) {
                at = hang_add(text, text_cap, at, " 0x%x", (unsigned)hd->words[k]);
                shown++;
            }
        at = hang_add(text, text_cap, at, "\n");
    }
    hang_write(text, at);
    /* Then named, in the file and in the log. */
    at = mark;
    for (i = 0; i < n; i++) {
        hang_thread *s = &threads[i];
        u32 last = 0;
        int found = 0;
        at = hang_add(text, text_cap, at, "hang: thread %d \"%s\" (host %u): pc %s", (int)s->tid, s->name, (unsigned)s->host_id,
                      code_at(s->pc, where, sizeof(where)) ? where : "?");
        at = hang_add(text, text_cap, at, ", lr %s; on the stack:", code_at(s->lr, where, sizeof(where)) ? where : "?");
        for (k = 0; k < s->nwords && found < 14; k++) {
            u32 w = s->words[k] & ~1u;
            if (w == last || !code_at(w, where, sizeof(where))) continue;
            at = hang_add(text, text_cap, at, "%s %s", found ? "," : "", where);
            last = w;
            found++;
        }
        at = hang_add(text, text_cap, at, "\n");
    }
    for (i = 0; i < nholders; i++) {
        hang_holder *hd = &holders[i];
        u32 last = 0;
        int found = 0;
        at = hang_add(text, text_cap, at, "hang: host thread %u, not one of the app's: pc %s", (unsigned)hd->host_id,
                      code_at(hd->pc, where, sizeof(where)) ? where : "?");
        at = hang_add(text, text_cap, at, ", lr %s; on the stack:", code_at(hd->lr, where, sizeof(where)) ? where : "?");
        for (k = 0; k < hd->nwords && found < 48; k++) {
            u32 w = hd->words[k] & ~1u;
            if (w == last || !code_at(w, where, sizeof(where))) continue;
            at = hang_add(text, text_cap, at, "%s %s", found ? "," : "", where);
            last = w;
            found++;
        }
        at = hang_add(text, text_cap, at, "\n");
    }
    hang_write(text, at);
    /* The log, line by line, past its lock (which may be what stands still). */
    {
        char *line = text, *end;
        while (*line) {
            end = strchr(line, '\n');
            if (end) *end = 0;
            hk_log_unlocked(HK_LOG_WARN, line);
            if (!end) break;
            line = end + 1;
        }
    }
}

static void hang_observer(void *unused)
{
    hang_thread *threads = (hang_thread *)hk_alloc(sizeof(hang_thread) * HANG_THREADS);
    hang_holder *holders = (hang_holder *)hk_alloc(sizeof(hang_holder) * 2);
    char *text = (char *)hk_alloc(64u * 1024u);
    u32 said_log = 0, said_threads = 0;
    u64 said_log_at = 0, said_threads_at = 0;
    int reports = 0;
    (void)unused;
    while (reports < 5) {
        u32 log_owner = 0, thread_owner;
        u64 log_since = 0, thread_since, now;
        char line[300], what[900];
        u32 holder_ids[2];
        const char *site;
        size_t at = 0;
        bool log_stuck, threads_stuck;
        hk_sleep_ns(2000000000ull);
        now = hk_time_monotonic_ns();
        log_stuck = hk_log_holder(&log_owner, &log_since, line, sizeof(line)) && now - log_since >= HANG_SECONDS * 1000000000ull;
        thread_owner = g_thread_lock_owner;
        thread_since = g_thread_lock_at;
        site = g_thread_lock_site;
        threads_stuck = thread_owner && thread_owner == g_thread_lock_owner && now - thread_since >= HANG_SECONDS * 1000000000ull;
        /* Once for each stand-still: the same holder since the same moment is the one already said. */
        if (log_stuck && log_owner == said_log && log_since == said_log_at) log_stuck = false;
        if (threads_stuck && thread_owner == said_threads && thread_since == said_threads_at) threads_stuck = false;
        if (!log_stuck && !threads_stuck) continue;
        what[0] = 0;
        holder_ids[0] = holder_ids[1] = 0;
        if (hk_log_holder(&log_owner, &log_since, line, sizeof(line)) && now - log_since >= HANG_SECONDS * 1000000000ull) {
            at = hang_add(what, sizeof(what), at, "the log's lock held %.1f s by host thread %u, writing \"%s\"; ", (double)(now - log_since) / 1e9,
                          (unsigned)log_owner, line);
            said_log = log_owner;
            said_log_at = log_since;
            holder_ids[0] = log_owner;
        }
        if (thread_owner && now - thread_since >= HANG_SECONDS * 1000000000ull) {
            i32 stopping = g_prof_stopping;
            at = hang_add(what, sizeof(what), at, "the guest threads' lock held %.1f s by host thread %u, in %s", (double)(now - thread_since) / 1e9,
                          (unsigned)thread_owner, site ? site : "?");
            if (stopping) at = hang_add(what, sizeof(what), at, " (stopping guest thread %d for trace-profile)", (int)stopping);
            at = hang_add(what, sizeof(what), at, "; ");
            said_threads = thread_owner;
            said_threads_at = thread_since;
            holder_ids[1] = thread_owner;
        }
        hang_add(what, sizeof(what), at, "the app stands still");
        hang_report(threads, holders, holder_ids, what, text, 64u * 1024u);
        reports++;
    }
    free(text);
    free(holders);
    free(threads);
}

void hk_bionic_hang_watch_start(void)
{
    static volatile i32 started;
    hk_thread *t;
    if (hk_atomic_xchg(&started, 1)) return;
#if !defined(_WIN32)
    return;
#endif
    t = hk_thread_start(hang_observer, NULL, 64u * 1024u, "hang-observer");
    if (t) hk_thread_detach(t);
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

/* The phone's language and region (hk_bionic.h), kept here as the time zone is. */
static char g_locale[40] = "en-US";

static bool alpha(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }

void hk_locale_set(const char *tag)
{
    size_t i, first = 0;
    bool ok = tag != NULL && tag[0] != 0 && strlen(tag) < sizeof(g_locale);
    for (i = 0; ok && tag[i]; i++) {
        char ch = tag[i] == '_' ? '-' : tag[i];
        ok = alpha(ch) || (ch >= '0' && ch <= '9') || ch == '-';
        if (ch == '-' && !first) first = i;
    }
    if (ok) {
        if (!first) first = strlen(tag);
        ok = first >= 2 && first <= 3 && alpha(tag[0]) && alpha(tag[1]) && (first == 2 || alpha(tag[2]));
    }
    if (!ok) tag = "en-US";
    hk_strlcpy(g_locale, tag, sizeof(g_locale));
    for (i = 0; g_locale[i]; i++)
        if (g_locale[i] == '_') g_locale[i] = '-';
    hk_sysprop_set("persist.sys.locale", g_locale);
    hk_sysprop_set("ro.product.locale", g_locale);
}

const char *hk_locale_tag(void) { return g_locale; }

void hk_locale_parts(char *language, size_t language_size, char *region, size_t region_size)
{
    const char *p = g_locale;
    size_t n = 0;
    language[0] = region[0] = 0;
    while (p[n] && p[n] != '-') n++;
    if (language_size) {
        size_t i;
        for (i = 0; i < n && i + 1 < language_size; i++) language[i] = (char)(p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
        language[i] = 0;
    }
    /* The region: the first subtag after the language of two letters or three digits (a script's four letters come between). */
    while (p[n] == '-') {
        const char *s = p + n + 1;
        size_t len = 0;
        while (s[len] && s[len] != '-') len++;
        if ((len == 2 && alpha(s[0]) && alpha(s[1])) || (len == 3 && s[0] >= '0' && s[0] <= '9')) {
            size_t i;
            for (i = 0; i < len && i + 1 < region_size; i++) region[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
            region[i] = 0;
            return;
        }
        n += 1 + len;
    }
}

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
        { "ro.build.fingerprint", "google/sailfish/sailfish:9/PQ3A.190801.002/1:user/release-keys" },
        /* The device: Google's Pixel of 2016, whose Snapdragon 821 has the phone's graphics processor, the Adreno 530; the Java
           framework's Build names it too (framework-overlay/android/os/Build.java). A game chooses its detail by it. */
        { "ro.product.model", "Pixel" },
        { "ro.product.brand", "google" },
        { "ro.product.name", "sailfish" },
        { "ro.product.device", "sailfish" },
        { "ro.product.board", "sailfish" },
        { "ro.product.manufacturer", "Google" },
        { "ro.product.cpu.abi", "armeabi-v7a" },
        { "ro.product.cpu.abi2", "armeabi" },
        { "ro.product.cpu.abilist", "armeabi-v7a,armeabi" },
        { "ro.product.cpu.abilist32", "armeabi-v7a,armeabi" },
        { "ro.product.cpu.abilist64", "" },
        { "ro.hardware", "sailfish" },
        { "ro.board.platform", "msm8996" },
        { "ro.debuggable", "0" },
        { "ro.secure", "1" },
        { "ro.kernel.qemu", "0" },
        { "ro.sf.lcd_density", "320" },
        { "persist.sys.timezone", "UTC" },
        { "persist.sys.locale", "en-US" },
        { "ro.product.locale", "en-US" },
        { "ro.opengles.version", "131072" },
    };
    size_t i;
    for (i = 0; i < HK_COUNT(d); i++) hk_sysprop_set(d[i][0], d[i][1]);
    /* A zone the shell named before this ran is not undone by the default. */
    if (g_timezone[0]) hk_sysprop_set("persist.sys.timezone", g_timezone);
    /* (Likewise the language and region.) */
    hk_sysprop_set("persist.sys.locale", g_locale);
    hk_sysprop_set("ro.product.locale", g_locale);
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
