/*
 * The host layer over Win32: the Windows 10 Mobile app container (ARM32), and
 * desktop Windows for the x64 build of the tests.
 *
 * In the app container only the app partition of the API exists, so executable
 * memory comes from VirtualAllocFromApp / VirtualProtectFromApp (the package
 * declares codeGeneration), files are opened with CreateFile2, and DLLs with
 * LoadPackagedLibrary. Sockets are Winsock 2's, which an app container may
 * use (the package declares internetClient and internetClientServer).
 */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* (Winsock 2's headers before windows.h, which would otherwise bring the first Winsock's.) */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <mstcpip.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_4.h>

#include "mg_host.h"

#include <signal.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
#define MG_UWP 1
#else
#define MG_UWP 0
#endif

/* ---------------------------------------------------------------- memory */

static DWORD prot_of(int prot)
{
    switch (prot) {
    case MG_PROT_R: return PAGE_READONLY;
    case MG_PROT_RW: return PAGE_READWRITE;
    case MG_PROT_RX: return PAGE_EXECUTE_READ;
    default: return PAGE_NOACCESS;
    }
}

static void *valloc(void *p, size_t size, DWORD type, DWORD prot)
{
#if MG_UWP
    return VirtualAllocFromApp(p, size, type, prot);
#else
    return VirtualAlloc(p, size, type, prot);
#endif
}

static BOOL vprotect(void *p, size_t size, DWORD prot)
{
    DWORD old = 0;
#if MG_UWP
    return VirtualProtectFromApp(p, size, prot, &old);
#else
    return VirtualProtect(p, size, prot, &old);
#endif
}

void *mg_mem_reserve(size_t size)
{
    /* PAGE_NOACCESS is refused for a reservation by VirtualAllocFromApp on some builds; read-write is what gecko-w10m reserves with. */
    return valloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
}

void *mg_mem_reserve_at(void *p, size_t size) { return p ? valloc(p, size, MEM_RESERVE, PAGE_READWRITE) : NULL; }

/*
 * Sections: pagefile-backed file mappings, viewed with MapViewOfFile(FromApp), whose offsets are on the 64 KB granularity.
 * A section is made with execute rights where the system grants them (the package's codeGeneration capability), so that a
 * view of it can run code (mg_section_map_code); its other views are read-write or read-only as before. Made with
 * PAGE_READWRITE only, no view of it could ever be made executable: Facebook maps its memfd a second time with PROT_EXEC
 * to run the code it wrote, and faulted executing there.
 */
struct mg_section {
    HANDLE handle;
};

static volatile i32 g_views_kb;

static void views_count(size_t size, int sign) { mg_atomic_add(&g_views_kb, sign * (i32)((size + 1023) >> 10)); }

u32 mg_views_kb(void) { return (u32)g_views_kb; }

mg_section *mg_section_create(u64 size)
{
    mg_section *s;
    HANDLE h;
    if (!size) return NULL;
#if MG_UWP
    h = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, size, NULL);
    if (!h) h = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size, NULL);
#else
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!h) h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
#endif
    if (!h) return NULL;
    s = (mg_section *)calloc(1, sizeof(*s));
    if (!s) {
        CloseHandle(h);
        return NULL;
    }
    s->handle = h;
    return s;
}

void *mg_section_map(mg_section *s, u64 offset, size_t size, bool writable, void **base)
{
    u64 aligned = offset & ~(u64)0xFFFF;
    size_t delta = (size_t)(offset - aligned);
    void *v;
    if (!s || !size) return NULL;
#if MG_UWP
    v = MapViewOfFileFromApp(s->handle, writable ? FILE_MAP_WRITE : FILE_MAP_READ, aligned, size + delta);
#else
    v = MapViewOfFile(s->handle, writable ? FILE_MAP_WRITE : FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)aligned, size + delta);
#endif
    if (!v) return NULL;
    views_count(size + delta, 1);
    if (base) *base = v;
    return (u8 *)v + delta;
}

void *mg_section_map_code(mg_section *s, u64 offset, size_t size, void **base)
{
    u64 aligned = offset & ~(u64)0xFFFF;
    size_t delta = (size_t)(offset - aligned);
    void *v;
    if (!s || !size) return NULL;
#if MG_UWP
    v = MapViewOfFileFromApp(s->handle, FILE_MAP_READ | FILE_MAP_EXECUTE, aligned, size + delta);
#else
    v = MapViewOfFile(s->handle, FILE_MAP_READ | FILE_MAP_EXECUTE, (DWORD)(aligned >> 32), (DWORD)aligned, size + delta);
#endif
    if (!v) return NULL;
    views_count(size + delta, 1);
    if (base) *base = v;
    return (u8 *)v + delta;
}

void mg_section_unmap(void *base, size_t size)
{
    if (!base) return;
    UnmapViewOfFile(base);
    views_count(size, -1);
}

void mg_section_close(mg_section *s)
{
    if (!s) return;
    CloseHandle(s->handle);
    free(s);
}

bool mg_mem_commit(void *p, size_t size, int prot)
{
    /* An executable commit is never allowed: commit writable, flip later. */
    if (prot == MG_PROT_RX) prot = MG_PROT_RW;
    return valloc(p, size, MEM_COMMIT, prot_of(prot)) != NULL;
}

bool mg_mem_protect(void *p, size_t size, int prot)
{
    return vprotect(p, size, prot_of(prot)) != 0;
}

bool mg_mem_decommit(void *p, size_t size) { return VirtualFree(p, size, MEM_DECOMMIT) != 0; }

/* Each run of pages of one state in [p, p + size), in turn: `fn` with the run, whether it is committed, and whether it is the process's own (not a view of a file or a section). */
static bool mem_runs(const void *p, size_t size, bool (*fn)(u8 *at, size_t n, bool committed, bool own, void *user), void *user)
{
    const u8 *at = (const u8 *)p, *end = at + size;
    bool ok = true;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info;
        const u8 *next;
        size_t n;
        if (VirtualQuery((LPCVOID)at, &info, sizeof(info)) != sizeof(info) || !info.RegionSize) return false;
        next = (const u8 *)info.BaseAddress + info.RegionSize;
        n = (size_t)((next < end ? next : end) - at);
        if (!fn((u8 *)at, n, info.State == MEM_COMMIT, info.Type == MEM_PRIVATE, user)) ok = false;
        at += n;
    }
    return ok;
}
static bool protect_run(u8 *at, size_t n, bool committed, bool own, void *user)
{
    (void)own;
    return !committed || vprotect(at, n, *(DWORD *)user) != 0;
}
static bool count_run(u8 *at, size_t n, bool committed, bool own, void *user)
{
    (void)at;
    if (committed && own) *(i64 *)user += (i64)n;
    return true;
}
bool mg_mem_protect_committed(void *p, size_t size, int prot)
{
    DWORD native = prot_of(prot);
    return mem_runs(p, size, protect_run, &native);
}
i64 mg_mem_committed(const void *p, size_t size)
{
    i64 n = 0;
    return mem_runs(p, size, count_run, &n) ? n : -1;
}
void mg_mem_release(void *p, size_t size) { (void)size; VirtualFree(p, 0, MEM_RELEASE); }
void mg_icache_flush(void *p, size_t size) { FlushInstructionCache(GetCurrentProcess(), p, size); }
void *mg_mem_alloc(size_t size) { return valloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

static u64 (*g_limit)(void), (*g_usage)(void);
void mg_mem_set_probe(u64 (*limit)(void), u64 (*usage)(void)) { g_limit = limit; g_usage = usage; }
u64 mg_mem_limit(void) { return g_limit ? g_limit() : 0; }
u64 mg_mem_usage(void) { return g_usage ? g_usage() : 0; }

/* --------------------------------------------------------------- threads */

struct mg_thread {
    HANDLE h;                          /* the creator's, for joining: closed by a join or a detach */
    HANDLE self;                       /* the thread's own, for interrupting it: there for as long as it runs */
    mg_thread_fn fn;
    void *arg;
    DWORD id;
    /* Other threads using `self` right now (self_take): the thread, ending, closes it only once there are none. */
    volatile LONG users;
    /* An interruption under way (mg_thread_interrupt): what the thread was doing, to go back to. */
    volatile LONG interrupted;
    CONTEXT was;
};

static MG_THREAD_LOCAL mg_thread *t_current;

/* A handle of the calling thread's own: GetCurrentThread's means "the caller" to whoever uses it. */
static HANDLE own_handle(void)
{
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS)) return NULL;
    return h;
}

/*
 * Another thread's use of a thread's own handle -- to stop it, read its registers or its times, or interrupt it: taken
 * and given back, so that the thread, ending, closes the handle only once nobody is using it. Closed under a user, the
 * handle made the user's next call one on an invalid handle, which an app's process is ended for (or, here, an exception
 * the handler logged from the profiler's thread, which then stood still: urent, on the page after asking for its SMS
 * code), or one on whatever object was given that number next. NULL when the handle has gone.
 */
static HANDLE self_take(mg_thread *t)
{
    HANDLE h;
    InterlockedIncrement(&t->users);
    h = (HANDLE)InterlockedCompareExchangePointer((PVOID volatile *)&t->self, NULL, NULL);
    if (!h) InterlockedDecrement(&t->users);
    return h;
}

static void self_give(mg_thread *t) { InterlockedDecrement(&t->users); }

/* The thread is about to end: nobody interrupts it from here on (the core has stopped asking by now). */
static void close_own_handle(mg_thread *t)
{
    HANDLE h = (HANDLE)InterlockedExchangePointer((PVOID volatile *)&t->self, NULL);
    /* (A use already under way takes a moment: a stop and a read of the registers, or of the times.) */
    while (InterlockedCompareExchange(&t->users, 0, 0) != 0) SwitchToThread();
    if (h) CloseHandle(h);
}

static void (*volatile g_end_hook)(void);
void mg_thread_set_end_hook(void (*fn)(void)) { g_end_hook = fn; }

static DWORD WINAPI thread_main(LPVOID p)
{
    mg_thread *t = (mg_thread *)p;
    t->id = GetCurrentThreadId();
    t->self = own_handle();
    t_current = t;
    t->fn(t->arg);
    if (g_end_hook) g_end_hook();
    close_own_handle(t);
    return 0;
}

void mg_thread_runs_app_code(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }

mg_thread *mg_thread_current(void)
{
    if (!t_current) {
        mg_thread *t = (mg_thread *)mg_alloc(sizeof(*t));
        t->id = GetCurrentThreadId();
        t->self = own_handle();
        t_current = t;
    }
    return t_current;
}

/* GetThreadTimes and GetProcessTimes count in 100 ns; Windows adds to them at each tick of its clock (some 15.6 ms). */
static u64 filetime_100ns(const FILETIME *f) { return (u64)f->dwHighDateTime << 32 | f->dwLowDateTime; }

u64 mg_thread_cpu_ns(mg_thread *t)
{
    FILETIME created, ended, kernel, user;
    HANDLE h = t ? self_take(t) : GetCurrentThread();
    BOOL ok = h && GetThreadTimes(h, &created, &ended, &kernel, &user);
    if (t && h) self_give(t);
    if (!ok) return 0;
    return (filetime_100ns(&kernel) + filetime_100ns(&user)) * 100ull;
}

u64 mg_process_cpu_ns(void)
{
    FILETIME created, ended, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) return 0;
    return (filetime_100ns(&kernel) + filetime_100ns(&user)) * 100ull;
}

/* -------------------------------------------- interrupting a thread in guest code */

static void (*g_interrupt_fn)(void);
static bool (*g_interrupt_in_guest)(mg_addr pc);

void mg_thread_set_interrupt(void (*fn)(void), bool (*in_guest_code)(mg_addr pc))
{
    g_interrupt_fn = fn;
    g_interrupt_in_guest = in_guest_code;
}

/* Raised by a thread that has finished an interruption, for the vectored handler to put it back where it was. */
#define EXCEPTION_MEGLER_RESUME 0xE04D4701u

#if defined(_M_ARM)
#if WINAPI_FAMILY == WINAPI_FAMILY_APP
/* Left out of the app partition by the SDK's headers, in the phone's kernelbase like any other (see AddVectoredExceptionHandler below). */
WINBASEAPI DWORD WINAPI SuspendThread(HANDLE hThread);
WINBASEAPI BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
WINBASEAPI BOOL WINAPI SetThreadContext(HANDLE hThread, const CONTEXT *lpContext);
#endif

/*
 * Where an interrupted thread finds itself: on its own stack, below where it
 * was. It runs the function and then asks, through an exception, to be put
 * back; nothing here returns.
 */
/* What became of each request, counted: nothing may be logged while another thread is stopped (it may hold the log's lock). */
static volatile LONG g_int_no_handle, g_int_busy, g_int_suspend_failed, g_int_get_failed, g_int_elsewhere, g_int_set_failed, g_int_redirected,
    g_int_landed, g_int_resumed, g_int_error;

void mg_thread_interrupt_report(char *out, size_t size)
{
    snprintf(out, size,
             "%ld redirected, %ld landed, %ld resumed; %ld found the thread elsewhere, %ld while it was still in one; failures: %ld no handle, "
             "%ld SuspendThread, %ld GetThreadContext, %ld SetThreadContext (last error %ld)",
             (long)g_int_redirected, (long)g_int_landed, (long)g_int_resumed, (long)g_int_elsewhere, (long)g_int_busy, (long)g_int_no_handle,
             (long)g_int_suspend_failed, (long)g_int_get_failed, (long)g_int_set_failed, (long)g_int_error);
}

static void interrupt_landing(mg_thread *t)
{
    ULONG_PTR arg = (ULONG_PTR)t;
    InterlockedIncrement(&g_int_landed);
    g_interrupt_fn();
    RaiseException(EXCEPTION_MEGLER_RESUME, 0, 1, &arg);
    for (;;) Sleep(INFINITE);
}

bool mg_thread_interrupt(mg_thread *t)
{
    CONTEXT ctx;
    bool done = false;
    HANDLE h;
    if (!t || !g_interrupt_fn || !g_interrupt_in_guest || t->id == GetCurrentThreadId()) return false;
    h = self_take(t);
    if (!h) {
        InterlockedIncrement(&g_int_no_handle);
        return false;
    }
    /* One at a time for a thread: the place to go back to is kept in one slot. */
    if (InterlockedCompareExchange(&t->interrupted, 1, 0) != 0) {
        InterlockedIncrement(&g_int_busy);
        self_give(t);
        return false;
    }
    if (SuspendThread(h) == (DWORD)-1) {
        InterlockedExchange(&g_int_error, (LONG)GetLastError());
        InterlockedIncrement(&g_int_suspend_failed);
        InterlockedExchange(&t->interrupted, 0);
        self_give(t);
        return false;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) {
        InterlockedExchange(&g_int_error, (LONG)GetLastError());
        InterlockedIncrement(&g_int_get_failed);
    } else if (!g_interrupt_in_guest((mg_addr)(ctx.Pc & ~1u))) {
        InterlockedIncrement(&g_int_elsewhere);
    } else {
        t->was = ctx;
        /* Android code keeps nothing below its stack pointer (a signal may land there at any time); the gap is for good measure. */
        ctx.Sp = (ctx.Sp - 512) & ~7u;
        ctx.R0 = (DWORD)(DWORD_PTR)t;
        ctx.Lr = 0;
        /* Thumb state both ways it can be said, and out of any IT block the thread was in. */
        ctx.Pc = (DWORD)(DWORD_PTR)interrupt_landing | 1u;
        ctx.Cpsr = (ctx.Cpsr & ~0x0600FC00u) | 0x20u;
        done = SetThreadContext(h, &ctx) != 0;
        if (done) InterlockedIncrement(&g_int_redirected);
        else {
            InterlockedExchange(&g_int_error, (LONG)GetLastError());
            InterlockedIncrement(&g_int_set_failed);
        }
    }
    if (!done) InterlockedExchange(&t->interrupted, 0);
    ResumeThread(h);
    self_give(t);
    return done;
}
#else
void mg_thread_interrupt_report(char *out, size_t size) { snprintf(out, size, "not on this processor"); }

bool mg_thread_interrupt(mg_thread *t)
{
    (void)t;
    return false;
}
#endif

/* ------------------------------------------------- stopping a thread for the collector */

#if WINAPI_FAMILY == WINAPI_FAMILY_APP && !defined(_M_ARM)
WINBASEAPI DWORD WINAPI SuspendThread(HANDLE hThread);
WINBASEAPI BOOL WINAPI GetThreadContext(HANDLE hThread, LPCONTEXT lpContext);
#endif

bool mg_thread_suspend(mg_thread *t, mg_thread_regs *out)
{
    CONTEXT ctx;
    HANDLE h;
    memset(out, 0, sizeof(*out));
    if (!t || t->id == GetCurrentThreadId()) return false;
    /* Given back once it has stopped (or not): stopped, it cannot end, and its handle stays until mg_thread_resume. */
    h = self_take(t);
    if (!h) return false;
    if (SuspendThread(h) == (DWORD)-1) {
        self_give(t);
        return false;
    }
    /* SuspendThread only asks; GetThreadContext returns once the thread has stopped, with its registers. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    if (!GetThreadContext(h, &ctx)) {
        ResumeThread(h);
        self_give(t);
        return false;
    }
    self_give(t);
#if defined(_M_ARM)
    out->sp = ctx.Sp;
    out->pc = ctx.Pc;
    out->flags = ctx.Cpsr;
    out->r[0] = ctx.R0; out->r[1] = ctx.R1; out->r[2] = ctx.R2; out->r[3] = ctx.R3; out->r[4] = ctx.R4; out->r[5] = ctx.R5;
    out->r[6] = ctx.R6; out->r[7] = ctx.R7; out->r[8] = ctx.R8; out->r[9] = ctx.R9; out->r[10] = ctx.R10; out->r[11] = ctx.R11;
    out->r[12] = ctx.R12; out->r[13] = ctx.Lr;
    out->n = 14;
#elif defined(_M_X64)
    out->sp = (mg_addr)ctx.Rsp;
    out->pc = (mg_addr)ctx.Rip;
    out->flags = ctx.EFlags;
    out->r[0] = ctx.Rax; out->r[1] = ctx.Rcx; out->r[2] = ctx.Rdx; out->r[3] = ctx.Rbx; out->r[4] = ctx.Rbp; out->r[5] = ctx.Rsi;
    out->r[6] = ctx.Rdi; out->r[7] = ctx.R8; out->r[8] = ctx.R9; out->r[9] = ctx.R10; out->r[10] = ctx.R11; out->r[11] = ctx.R12;
    out->r[12] = ctx.R13; out->r[13] = ctx.R14; out->r[14] = ctx.R15;
    out->n = 15;
#else
    ResumeThread(h);
    return false;
#endif
    return true;
}

void mg_thread_resume(mg_thread *t)
{
    HANDLE h = t ? self_take(t) : NULL;
    if (!h) return;
    ResumeThread(h);
    self_give(t);
}

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
/* Left out of the app partition by the SDK's headers, in the phone's kernelbase like any other. */
WINBASEAPI HANDLE WINAPI OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId);
#endif

int mg_thread_snapshot_id(u32 id, mg_thread_regs *regs, u32 *words, int max_words)
{
    mg_thread t;
    MEMORY_BASIC_INFORMATION mbi;
    HANDLE h;
    int n = 0;
    memset(regs, 0, sizeof(*regs));
    if (!id || id == GetCurrentThreadId()) return -1;
    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
    if (!h) return -1;
    memset(&t, 0, sizeof(t));
    t.self = h;
    t.id = id;
    if (!mg_thread_suspend(&t, regs)) {
        CloseHandle(h);
        return -1;
    }
    if (regs->sp && VirtualQuery((const void *)(DWORD_PTR)regs->sp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
        mg_addr end = (mg_addr)(DWORD_PTR)mbi.BaseAddress + (mg_addr)mbi.RegionSize, at = regs->sp & ~(mg_addr)3;
        n = (int)((end - at) / 4 < (mg_addr)max_words ? (end - at) / 4 : (mg_addr)max_words);
        memcpy(words, (const void *)(DWORD_PTR)at, (size_t)n * 4);
    }
    ResumeThread(h);
    CloseHandle(h);
    return n;
}

mg_thread *mg_thread_start(mg_thread_fn fn, void *arg, size_t stack_size, const char *name)
{
    mg_thread *t = (mg_thread *)mg_alloc(sizeof(*t));
    (void)name;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack_size, thread_main, t, stack_size ? STACK_SIZE_PARAM_IS_A_RESERVATION : 0, NULL);
    if (!t->h) {
        free(t);
        return NULL;
    }
    return t;
}

void mg_thread_join(mg_thread *t)
{
    WaitForSingleObjectEx(t->h, INFINITE, FALSE);
    CloseHandle(t->h);
    free(t);
}

void mg_thread_detach(mg_thread *t)
{
    /* The thread's main still reads t; it is small and lives as long as the process. */
    CloseHandle(t->h);
    t->h = NULL;
}

void mg_thread_exit(void)
{
    if (g_end_hook) g_end_hook();
    if (t_current) close_own_handle(t_current);
    ExitThread(0);
}
u32 mg_thread_self_id(void) { return GetCurrentThreadId(); }
u32 mg_thread_id(const mg_thread *t) { return t ? (u32)t->id : 0; }
void mg_thread_yield(void) { SwitchToThread(); }

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
/* Left out of the app partition by the SDK's headers, in the phone's kernelbase like any other. */
WINBASEAPI HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName);
#endif

/*
 * Windows ends a wait on the next tick of its timer, and the tick is 15.6 ms
 * unless a process asks for a finer one: on the phone a sleep of one
 * millisecond took sixteen, and Android code sleeps for one millisecond all
 * the time. winmm's timeBeginPeriod is what asks, and winmm is not there for
 * an app; the call under it is in ntdll, which every process has. Asked for
 * once, at the first sleep, and said in the log either way.
 */
static void finer_timer(void)
{
    typedef LONG (NTAPI *set_resolution_fn)(ULONG wanted, BOOLEAN set, PULONG now);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    set_resolution_fn set = ntdll ? (set_resolution_fn)GetProcAddress(ntdll, "NtSetTimerResolution") : NULL;
    ULONG now = 0;
    LONG status = set ? set(10000, TRUE, &now) : -1;       /* in units of 100 ns */
    if (set && status >= 0) mg_info("host: the system timer ticks every %.1f ms now", (double)now / 10000.0);
    else mg_warn("host: a finer system timer was refused (status 0x%lx): short sleeps take up to 16 ms", (unsigned long)status);
}

void mg_sleep_ns(u64 ns)
{
    static volatile LONG asked;
    DWORD ms = (DWORD)((ns + 999999ull) / 1000000ull);
    if (!asked && InterlockedExchange(&asked, 1) == 0) finer_timer();
    Sleep(ms);
}

int mg_cpu_count(void)
{
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

void mg_lock_init(mg_lock *l) { InitializeSRWLock((PSRWLOCK)&l->srw); }
void mg_lock_acquire(mg_lock *l) { AcquireSRWLockExclusive((PSRWLOCK)&l->srw); }
bool mg_lock_try(mg_lock *l) { return TryAcquireSRWLockExclusive((PSRWLOCK)&l->srw) != 0; }
void mg_lock_release(mg_lock *l) { ReleaseSRWLockExclusive((PSRWLOCK)&l->srw); }
void mg_cond_init(mg_cond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)&c->cv); }
void mg_cond_wait(mg_cond *c, mg_lock *l) { SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->cv, (PSRWLOCK)&l->srw, INFINITE, 0); }

bool mg_cond_wait_ns(mg_cond *c, mg_lock *l, u64 ns)
{
    u64 ms = (ns + 999999ull) / 1000000ull;
    if (ms >= 0xFFFFFFFEull) ms = 0xFFFFFFFEull;
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)&c->cv, (PSRWLOCK)&l->srw, (DWORD)ms, 0) != 0;
}

void mg_cond_signal(mg_cond *c) { WakeConditionVariable((PCONDITION_VARIABLE)&c->cv); }
void mg_cond_broadcast(mg_cond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)&c->cv); }

i32 mg_atomic_cas(volatile i32 *p, i32 expect, i32 value) { return (i32)InterlockedCompareExchange((volatile LONG *)p, value, expect); }
i32 mg_atomic_add(volatile i32 *p, i32 delta) { return (i32)InterlockedExchangeAdd((volatile LONG *)p, delta) + delta; }
i32 mg_atomic_xchg(volatile i32 *p, i32 value) { return (i32)InterlockedExchange((volatile LONG *)p, value); }
void *mg_atomic_cas_ptr(void *volatile *p, void *expect, void *value) { return InterlockedCompareExchangePointer(p, value, expect); }
i64 mg_atomic_cas64(volatile i64 *p, i64 expect, i64 value) { return (i64)InterlockedCompareExchange64((volatile LONG64 *)p, value, expect); }

bool mg_futex_wait(volatile i32 *addr, i32 expect, u64 timeout_ns)
{
    u64 ms = (timeout_ns + 999999ull) / 1000000ull;
    if (ms >= 0xFFFFFFFEull) ms = 0xFFFFFFFEull;
    if (WaitOnAddress((volatile VOID *)addr, &expect, sizeof(expect), timeout_ns ? (DWORD)ms : INFINITE)) return true;
    return GetLastError() != ERROR_TIMEOUT;
}

void mg_futex_wake(volatile i32 *addr, bool all)
{
    if (all) WakeByAddressAll((PVOID)addr);
    else WakeByAddressSingle((PVOID)addr);
}

void mg_thread_stack_bounds(void **lo, void **hi)
{
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    *lo = (void *)low;
    *hi = (void *)high;
}

void mg_thread_whole_stack(void)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    ULONG_PTR low = 0, high = 0;
    const size_t page = 4096;
    u8 *bottom, *limit, *lowest;
    GetCurrentThreadStackLimits(&low, &high);
    bottom = (u8 *)low;
    limit = (u8 *)tib->StackLimit;
    /*
     * As the stack would be had it grown all the way by itself: its lowest
     * page never given out, the one above that the guard (so running out is
     * still an exception and not a write into whatever lies below), and
     * everything from there up plain memory. Committed, not touched: it
     * costs no memory until used.
     */
    lowest = bottom + 2 * page;
    if (!bottom || limit <= lowest || limit > (u8 *)high) return;
    if (!valloc(lowest, (size_t)(limit - lowest), MEM_COMMIT, PAGE_READWRITE) ||
        !vprotect(lowest, (size_t)(limit - lowest), PAGE_READWRITE) ||
        !valloc(bottom + page, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
        mg_warn("host: thread %lu's stack could not be made whole (error %lu); Android code with a large frame may end the process",
                (unsigned long)GetCurrentThreadId(), (unsigned long)GetLastError());
        return;
    }
    tib->StackLimit = lowest;
}

u32 mg_tls_alloc(void)
{
    DWORD k = TlsAlloc();
    if (k == TLS_OUT_OF_INDEXES) mg_fatal("out of thread-local slots");
    return k;
}
void *mg_tls_get(u32 key) { return TlsGetValue(key); }
void mg_tls_set(u32 key, void *value) { TlsSetValue(key, value); }

/* ------------------------------------------------- the guest thread pointer
 *
 * Windows on ARM keeps the TEB in TPIDRURW (c13, c0, 2), readable from user
 * mode, and the first 64 TlsAlloc slots are an array inside the TEB. So two
 * instructions fetch a slot into any register without a call:
 *
 *     mrc   p15, 0, rX, c13, c0, 2      ; the TEB
 *     ldr.w rX, [rX, #TlsSlots + 4*slot]
 *
 * TlsSlots is at 0xE10 in the 32-bit TEB. That offset is checked against
 * TlsGetValue before it is trusted, so a build of Windows that moved it makes
 * mg_guest_tp_where fail instead of handing guest code a wrong pointer.
 */
#define MG_TEB_TLS_SLOTS 0xE10u

static DWORD g_tp_slot = TLS_OUT_OF_INDEXES;
static int g_tp_slot_usable = -1;      /* -1 not checked yet */

static DWORD tp_slot(void)
{
    if (g_tp_slot == TLS_OUT_OF_INDEXES) {
        /* Racing first calls would each allocate; the startup path calls this once before any guest thread exists. */
        g_tp_slot = TlsAlloc();
        if (g_tp_slot == TLS_OUT_OF_INDEXES) mg_fatal("no thread-local slot for the guest thread pointer");
    }
    return g_tp_slot;
}

void mg_guest_tp_set(void *tp) { TlsSetValue(tp_slot(), tp); }
void *mg_guest_tp_get(void) { return TlsGetValue(tp_slot()); }

bool mg_guest_tp_where(u32 *opc2, u32 *offset)
{
#if defined(_M_ARM)
    DWORD slot = tp_slot();
    if (g_tp_slot_usable < 0) {
        void *saved = TlsGetValue(slot);
        void *probe = (void *)(uintptr_t)0x5A17C0DEu;
        void **in_teb = (void **)((u8 *)NtCurrentTeb() + MG_TEB_TLS_SLOTS + 4u * slot);
        TlsSetValue(slot, probe);
        g_tp_slot_usable = (slot < 64 && *in_teb == probe) ? 1 : 0;
        TlsSetValue(slot, saved);
        if (!g_tp_slot_usable)
            mg_err("host: TLS slot %lu is not at TEB+0x%X; guest thread-pointer reads cannot be redirected", (unsigned long)slot, MG_TEB_TLS_SLOTS + 4u * slot);
    }
    if (!g_tp_slot_usable) return false;
    *opc2 = 2;
    *offset = MG_TEB_TLS_SLOTS + 4u * slot;
    return true;
#else
    (void)opc2; (void)offset;
    return false;
#endif
}

/* ----------------------------------------------------------------- faults */

static mg_fault_handler g_fault_handler;

bool mg_host_module_at(mg_addr address, char *name, size_t size, mg_addr *base);

#if MG_UWP
/* Left out of the app partition by the SDK's headers, in the phone's kernelbase like any other. */
WINBASEAPI LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
WINBASEAPI BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode);
#endif

/*
 * One line about an exception, said the moment it is seen. Windows ends the
 * process over an exception nothing handles, and what it leaves behind is
 * for Microsoft, not for a log; an end that this layer said nothing about
 * cost a day once (a phone's shell took the dying app's window away, the
 * app's own UI thread fell over that, and that second fault was all the log
 * had).
 */
static void note_exception(const EXCEPTION_POINTERS *ep, const char *what)
{
    char module[96], line[400];
    mg_addr base = 0, pc = (mg_addr)(DWORD_PTR)ep->ExceptionRecord->ExceptionAddress & ~(mg_addr)1;
    size_t n;
    snprintf(line, sizeof(line), "host: %s: exception 0x%08lx at %08x", what, (unsigned long)ep->ExceptionRecord->ExceptionCode, (unsigned)pc);
    n = strlen(line);
    if (mg_host_module_at(pc, module, sizeof(module), &base)) snprintf(line + n, sizeof(line) - n, " (%s+0x%x)", module, (unsigned)(pc - base));
#if defined(_M_ARM)
    {
        mg_addr lr = (mg_addr)ep->ContextRecord->Lr & ~(mg_addr)1;
        n = strlen(line);
        snprintf(line + n, sizeof(line) - n, ", lr %08x sp %08x", (unsigned)lr, (unsigned)ep->ContextRecord->Sp);
        n = strlen(line);
        if (mg_host_module_at(lr, module, sizeof(module), &base)) snprintf(line + n, sizeof(line) - n, " (lr in %s+0x%x)", module, (unsigned)(lr - base));
    }
#endif
    n = strlen(line);
    snprintf(line + n, sizeof(line) - n, ", thread %lu", (unsigned long)GetCurrentThreadId());
    mg_err("%s", line);
}

/*
 * Who was on the way to here, by what the stack holds that looks like a way
 * back into a module: not an unwinding, but it names the callers. One line.
 */
static void note_callers(mg_addr sp, const char *what)
{
    char module[96], line[900];
    void *lo = NULL, *hi = NULL;
    mg_addr at, last = 0, base = 0;
    size_t n;
    int found = 0;
    mg_thread_stack_bounds(&lo, &hi);
    sp &= ~(mg_addr)3;
    if (sp < (mg_addr)lo || sp >= (mg_addr)hi) return;
    snprintf(line, sizeof(line), "host: %s, on the stack:", what);
    n = strlen(line);
    for (at = sp; at + 4 <= (mg_addr)hi && at < sp + 16384 && found < 24 && n + 60 < sizeof(line); at += 4) {
        mg_addr word = *(const u32 *)at;
#if defined(_M_ARM)
        /* A way back into Thumb code is odd. */
        if (!(word & 1)) continue;
        word &= ~(mg_addr)1;
#endif
        if (word < 0x10000 || word == last) continue;
        if (!mg_host_module_at(word, module, sizeof(module), &base) || word == base) continue;
        snprintf(line + n, sizeof(line) - n, " %s+0x%x", module, (unsigned)(word - base));
        n = strlen(line);
        last = word;
        found++;
    }
    if (found) mg_err("%s", line);
}

#if defined(_M_ARM)
/*
 * A system call refused with STATUS_INVALID_HANDLE comes back through ntdll's KiRaiseUserExceptionDispatcher, which raises
 * the exception from a frame of its own: the exception record at sp, then the caller's r0, r1 and r11 and the way back, which
 * its epilogue restores (add sp, #N ; pop.w {r0, r1, r11, pc}). Continued the ordinary way, the exception goes back through
 * ZwContinue with a CONTEXT kept on that same stack, and the system refuses one that is aligned to four bytes and not eight
 * (STATUS_DATATYPE_MISALIGNMENT, 0x80000002): the thread then ran on into the next function in ntdll with that number for a
 * pointer and faulted. On urent's WebView every such call was made on a stack aligned to four. So the epilogue is done here
 * instead, on a CONTEXT of this layer's own, aligned, given to NtContinue: the system call returns to its caller with the
 * status the system gave it, as it would have. Only when the code there is that epilogue and the record is at sp; false
 * otherwise, and the exception is continued the ordinary way.
 */
typedef LONG(NTAPI *nt_continue_fn)(CONTEXT *context, BOOLEAN test_alert);
static nt_continue_fn g_nt_continue;
static MG_THREAD_LOCAL CONTEXT *t_return_context;

/* The words of the caller's stack, bare, for the log: whose call it was (a guest library's address is told by the ld lines). */
static void note_words(mg_addr sp, const char *what)
{
    char line[600];
    void *lo = NULL, *hi = NULL;
    size_t n;
    int i;
    mg_thread_stack_bounds(&lo, &hi);
    sp &= ~(mg_addr)3;
    if (sp < (mg_addr)lo || sp >= (mg_addr)hi) return;
    snprintf(line, sizeof(line), "host: %s, sp %08x:", what, (unsigned)sp);
    n = strlen(line);
    for (i = 0; i < 40 && sp + (mg_addr)i * 4 + 4 <= (mg_addr)hi && n + 12 < sizeof(line); i++) {
        snprintf(line + n, sizeof(line) - n, " %08x", (unsigned)((const u32 *)sp)[i]);
        n = strlen(line);
    }
    mg_err("%s", line);
}

static bool return_from_raise_user_exception(EXCEPTION_POINTERS *ep, bool say)
{
    const u16 *code = (const u16 *)((DWORD_PTR)ep->ExceptionRecord->ExceptionAddress & ~(DWORD_PTR)1);
    CONTEXT *c = ep->ContextRecord, *to;
    const DWORD *saved;
    DWORD sp;
    MEMORY_BASIC_INFORMATION mbi;
    if (!g_nt_continue) return false;
    if (!VirtualQuery(code, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || !(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    /* add sp, #imm7*4 ; pop.w {r0, r1, r11, pc} -- and the record where the dispatcher put it, at sp. */
    if ((code[0] & 0xFF80u) != 0xB000u || code[1] != 0xE8BDu || code[2] != 0x8803u) return false;
    if ((DWORD_PTR)ep->ExceptionRecord != c->Sp) return false;
    sp = c->Sp + (DWORD)(code[0] & 0x7Fu) * 4u;
    saved = (const DWORD *)(DWORD_PTR)sp;
    if (!t_return_context) t_return_context = (CONTEXT *)_aligned_malloc(sizeof(CONTEXT), 16);
    if (!t_return_context) return false;
    to = t_return_context;
    *to = *c;
    to->R0 = saved[0];
    to->R1 = saved[1];
    to->R11 = saved[2];
    to->Pc = saved[3];
    to->Lr = saved[3];
    to->Sp = sp + 16;
    /* Its state from bit 0 of Pc, and the T bit to agree. */
    to->Cpsr = (to->Pc & 1u) ? (to->Cpsr | 0x20u) : (to->Cpsr & ~0x20u);
    if (say) note_words(to->Sp, "the caller's stack");
    g_nt_continue(to, FALSE);
    return false; /* (NtContinue does not come back unless it refused the context.) */
}
#endif

/* Not for anyone to handle: C++ exceptions, a thread's name for a debugger, debug output, WinRT's error bookkeeping. */
static bool exception_is_traffic(DWORD code)
{
    /* Bit 29 marks a code as an application's own (0xE06D7363 is the C++ one); the top two bits are its severity. */
    return (code & 0x20000000u) != 0 || (code >> 30) < 2;
}

#if defined(_M_ARM)
/*
 * A thread out of stack cannot report it: the report needs stack. It is sent
 * to a stack kept for the purpose, the first one to need it, and says it
 * from there. It does not come back.
 */
static u8 g_spare_stack[96 * 1024];
static volatile LONG g_spare_stack_taken;
static EXCEPTION_RECORD g_overflow_record;
static CONTEXT g_overflow_context;

static __declspec(noinline) void overflow_report(void)
{
    EXCEPTION_POINTERS ep;
    mg_fault f;
    ep.ExceptionRecord = &g_overflow_record;
    ep.ContextRecord = &g_overflow_context;
    note_exception(&ep, "a thread ran out of stack");
    memset(&f, 0, sizeof(f));
    f.kind = MG_FAULT_OTHER;
    f.code = g_overflow_record.ExceptionCode;
    f.regs = (u32 *)&g_overflow_context.R0;
    f.cpsr = (u32 *)&g_overflow_context.Cpsr;
    /* Reports, and keeps a thread of the app's for good. */
    if (g_fault_handler) g_fault_handler(&f);
    /* One of the shell's own threads, then: the process cannot go on without it. */
    TerminateProcess(GetCurrentProcess(), (UINT)EXCEPTION_STACK_OVERFLOW);
    for (;;) Sleep(INFINITE);
}

static void overflow_landing(void)
{
    /* The stack's bounds as Windows knows them, for whatever checks them (stack probes do) from here on. */
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    tib->StackBase = g_spare_stack + sizeof(g_spare_stack);
    tib->StackLimit = g_spare_stack;
    overflow_report();
}
#endif

/* The last word, when nothing took an exception and Windows is about to end the process. */
static LONG WINAPI on_unhandled(EXCEPTION_POINTERS *ep)
{
    note_exception(ep, "nothing handled this and Windows ends the process");
    return EXCEPTION_CONTINUE_SEARCH;
}

/* abort(): the C runtime's way out, terminate()'s too. It ends the process without an exception anyone sees. */
static void on_abort(int sig)
{
    volatile u32 here = 0;
    (void)sig;
    mg_err("host: abort() was called on thread %lu; Windows ends the process now", (unsigned long)GetCurrentThreadId());
    note_callers((mg_addr)(DWORD_PTR)&here, "abort()");
}

/*
 * The C runtime, handed an argument it will not take (a mode string, a
 * descriptor, a time out of range), ends the process on the spot unless
 * told otherwise. Android's does not: the call fails and says why. So does
 * this one from here on, and the log says who asked.
 */
static void on_invalid_parameter(const wchar_t *expression, const wchar_t *function, const wchar_t *file, unsigned int line, uintptr_t reserved)
{
    volatile u32 here = 0;
    static volatile LONG said;
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    if (InterlockedIncrement(&said) > 20) return;
    mg_err("host: the C runtime refused an argument on thread %lu (the call fails; it does not end the process)", (unsigned long)GetCurrentThreadId());
    note_callers((mg_addr)(DWORD_PTR)&here, "the refused call");
}

static void on_pure_call(void)
{
    volatile u32 here = 0;
    mg_err("host: a pure virtual function was called on thread %lu (an object used while being made or after being destroyed)",
           (unsigned long)GetCurrentThreadId());
    note_callers((mg_addr)(DWORD_PTR)&here, "the pure call");
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    mg_fault f;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
#if defined(_M_ARM)
    if (er->ExceptionCode == EXCEPTION_MEGLER_RESUME && er->NumberParameters == 1) {
        /* A thread back from an interruption: every register as it was. Its state is taken from bit 0 of Pc. */
        mg_thread *t = (mg_thread *)er->ExceptionInformation[0];
        *ep->ContextRecord = t->was;
        if (ep->ContextRecord->Cpsr & 0x20u) ep->ContextRecord->Pc |= 1u;
        else ep->ContextRecord->Pc &= ~1u;
        InterlockedIncrement(&g_int_resumed);
        InterlockedExchange(&t->interrupted, 0);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif
    memset(&f, 0, sizeof(f));
    f.code = er->ExceptionCode;
    switch (er->ExceptionCode) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        f.kind = MG_FAULT_ILLEGAL;
        break;
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        f.kind = MG_FAULT_ACCESS;
        if (er->NumberParameters >= 2) {
            f.write = er->ExceptionInformation[0] == 1;
            f.exec = er->ExceptionInformation[0] == 8;
            f.address = (mg_addr)er->ExceptionInformation[1];
        }
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
        f.kind = MG_FAULT_OTHER;
        break;
    case 0xC0000008u: /* STATUS_INVALID_HANDLE */
        /*
         * A handle that was not open, used or closed: an app's process has the system check every handle strictly, and
         * says so with this exception, which nothing further down takes -- the search for a handler then went up into
         * Android code, which has no unwinding tables, and ended the process with 0x80000002 (urent's WebView did, on the
         * page after asking for its SMS code). On Linux the same call fails with EBADF and the program goes on. Continued,
         * the exception makes the system call return STATUS_INVALID_HANDLE to its caller, which fails as on Linux. The
         * first few are said, with who called.
         */
        if (!(er->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
            static volatile LONG said;
            const bool say = InterlockedIncrement(&said) <= 8;
            if (say) {
                note_exception(ep, "continued (a handle that was not open: the call fails, as Linux's EBADF)");
#if defined(_M_ARM)
                note_callers((mg_addr)ep->ContextRecord->Sp, "the invalid handle");
#endif
            }
#if defined(_M_ARM)
            /* (Returns only when the frame is not the dispatcher's.) */
            return_from_raise_user_exception(ep, say);
#endif
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        f.kind = MG_FAULT_OTHER;
        break;
#if defined(_M_ARM)
    case EXCEPTION_STACK_OVERFLOW:
        if (InterlockedCompareExchange(&g_spare_stack_taken, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
        g_overflow_record = *er;
        g_overflow_context = *ep->ContextRecord;
        ep->ContextRecord->Sp = ((DWORD)(DWORD_PTR)(g_spare_stack + sizeof(g_spare_stack)) - 64) & ~7u;
        ep->ContextRecord->Lr = 0;
        ep->ContextRecord->Pc = (DWORD)(DWORD_PTR)overflow_landing | 1u;
        ep->ContextRecord->Cpsr = (ep->ContextRecord->Cpsr & ~0x0600FC00u) | 0x20u;
        return EXCEPTION_CONTINUE_EXECUTION;
#endif
    default:
        if (er->ExceptionCode == 0xE06D7363u) {
            /*
             * A C++ exception, from the shell or from a library. One that is
             * caught is nothing; one that is not, or that meets Android
             * code's frames on its way up (nothing can unwind those), ends
             * in terminate() and a process gone with no other word.
             */
            /*
             * Only on a thread that runs Android code, and only the first
             * few: the system's own libraries throw and catch them all day
             * on the shell's threads (DirectWrite does for every font it
             * looks up), and that is nobody's business.
             */
            {
                static volatile LONG said;
                if (mg_guest_tp_get() && InterlockedIncrement(&said) <= 12) {
                    note_exception(ep, "a C++ exception was thrown on a thread of the app's (harmless if something catches it)");
#if defined(_M_ARM)
                    note_callers((mg_addr)ep->ContextRecord->Sp, "the C++ exception");
#endif
                }
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (exception_is_traffic(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
        /*
         * A breakpoint (what a compiler's trap and an assertion come to),
         * arithmetic, a bad handle... In Android code nothing further down
         * can take it, since that code has no handlers Windows knows of, so
         * it is a fault like any other. Elsewhere something may, so it is
         * noted and passed on.
         */
        f.kind = MG_FAULT_OTHER;
        if (!g_interrupt_in_guest || !g_interrupt_in_guest((mg_addr)(DWORD_PTR)er->ExceptionAddress & ~(mg_addr)1)) {
            note_exception(ep, "passed on");
            return EXCEPTION_CONTINUE_SEARCH;
        }
        break;
    }
#if defined(_M_ARM)
    /* R0..R12, Sp, Lr, Pc are sixteen consecutive ULONGs in the ARM CONTEXT, Cpsr the next. */
    f.regs = (u32 *)&ep->ContextRecord->R0;
    f.cpsr = (u32 *)&ep->ContextRecord->Cpsr;
#endif
    if (g_fault_handler && g_fault_handler(&f)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

void mg_fault_resume_at(mg_fault *f, mg_addr target)
{
    if (!f->regs) return;
    /* Bit 0 stays in Pc: that, not the T bit, is what the thread's state is taken from when the context is resumed. */
    f->regs[15] = (u32)target;
    if (f->cpsr) *f->cpsr = (target & 1) ? (*f->cpsr | 0x20u) : (*f->cpsr & ~0x20u);
}

#if MG_UWP
/*
 * The SDK's headers leave this out of the app partition, but the function is
 * in the phone's kernelbase like any other (OneCoreUAP.lib resolves it). A
 * vectored handler is the only kind that sees a fault in guest code first:
 * guest code has no unwind tables for a frame-based handler to be found by.
 */
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
#endif

void mg_fault_set_handler(mg_fault_handler h)
{
    static bool installed;
    g_fault_handler = h;
    if (!installed) {
        installed = true;
        if (!AddVectoredExceptionHandler(1, on_exception)) mg_err("host: no vectored exception handler; faults in guest code cannot be repaired");
#if defined(_M_ARM)
        {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            g_nt_continue = ntdll ? (nt_continue_fn)(void *)GetProcAddress(ntdll, "NtContinue") : NULL;
        }
#endif
        SetUnhandledExceptionFilter(on_unhandled);
        signal(SIGABRT, on_abort);
        _set_invalid_parameter_handler(on_invalid_parameter);
        _set_purecall_handler(on_pure_call);
    }
}

/* ------------------------------------------------------------------- time */

static u64 filetime_to_unix_ns(FILETIME ft)
{
    u64 t = ((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;     /* 100 ns since 1601 */
    if (t < 116444736000000000ull) return 0;
    return (t - 116444736000000000ull) * 100ull;
}

u64 mg_time_monotonic_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (u64)(now.QuadPart / freq.QuadPart) * 1000000000ull +
           (u64)(now.QuadPart % freq.QuadPart) * 1000000000ull / (u64)freq.QuadPart;
}

u64 mg_time_ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (u64)now.QuadPart;
}

u64 mg_ticks_ns(u64 ticks)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    return ticks / (u64)freq.QuadPart * 1000000000ull + ticks % (u64)freq.QuadPart * 1000000000ull / (u64)freq.QuadPart;
}

u64 mg_time_realtime_ns(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return filetime_to_unix_ns(ft);
}

i32 mg_time_utc_offset(i64 unix_seconds, bool *dst)
{
    /* The zone's rules for the current year, applied to the moment asked about. */
    TIME_ZONE_INFORMATION tz;
    SYSTEMTIME utc, local;
    FILETIME fu, fl;
    u64 t = (u64)(unix_seconds * 10000000ll) + 116444736000000000ull, lt;
    DWORD id = GetTimeZoneInformation(&tz);
    fu.dwLowDateTime = (DWORD)t;
    fu.dwHighDateTime = (DWORD)(t >> 32);
    if (!FileTimeToSystemTime(&fu, &utc) || !SystemTimeToTzSpecificLocalTime(&tz, &utc, &local) ||
        !SystemTimeToFileTime(&local, &fl)) {
        if (dst) *dst = id == TIME_ZONE_ID_DAYLIGHT;
        return -(i32)(tz.Bias + (id == TIME_ZONE_ID_DAYLIGHT ? tz.DaylightBias : tz.StandardBias)) * 60;
    }
    lt = ((u64)fl.dwHighDateTime << 32) | fl.dwLowDateTime;
    {
        i32 off = (i32)(((i64)lt - (i64)t) / 10000000ll);
        if (dst) *dst = off != -(i32)(tz.Bias + tz.StandardBias) * 60;
        return off;
    }
}

/* ------------------------------------------------------------------ files */

struct mg_file {
    HANDLE h;
    bool append;
};

void *mg_file_map(mg_file *f, u64 offset, size_t size, void **base, size_t *mapped)
{
    u64 aligned = offset & ~(u64)0xFFFF;
    size_t delta = (size_t)(offset - aligned);
    HANDLE m;
    void *v;
    if (!f || !size) return NULL;
#if MG_UWP
    m = CreateFileMappingFromApp(f->h, NULL, PAGE_READONLY, 0, NULL);
#else
    m = CreateFileMappingW(f->h, NULL, PAGE_READONLY, 0, 0, NULL);
#endif
    if (!m) return NULL;
#if MG_UWP
    v = MapViewOfFileFromApp(m, FILE_MAP_READ, aligned, size + delta);
#else
    v = MapViewOfFile(m, FILE_MAP_READ, (DWORD)(aligned >> 32), (DWORD)aligned, size + delta);
#endif
    CloseHandle(m);                    /* the view keeps the mapping */
    if (!v) return NULL;
    views_count(size + delta, 1);
    *base = v;
    *mapped = size + delta;
    return (u8 *)v + delta;
}

static int err_of(DWORD e)
{
    switch (e) {
    case ERROR_SUCCESS: return MG_E_OK;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_PATHNAME: case ERROR_INVALID_NAME:
        return MG_E_NOENT;
    case ERROR_ACCESS_DENIED: case ERROR_WRITE_PROTECT: return MG_E_ACCESS;
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return MG_E_BUSY;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return MG_E_EXIST;
    case ERROR_DIRECTORY: return MG_E_NOTDIR;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return MG_E_NOSPC;
    case ERROR_DIR_NOT_EMPTY: return MG_E_NOTEMPTY;
    case ERROR_INVALID_PARAMETER: return MG_E_INVAL;
    case ERROR_NOT_SAME_DEVICE: return MG_E_XDEV;
    default: return MG_E_IO;
    }
}
#define SET_ERR() do { if (err) *err = err_of(GetLastError()); } while (0)

/*
 * A drive's absolute path that Win32 would refuse for its length, as "\\?\" and the whole path, which Win32 takes up to
 * 32767 characters: MAX_PATH (260 with the terminating zero) for a file, 12 less for CreateDirectory, which keeps room for
 * an 8.3 name. Apps' own names can be that long: Instagram's Superpack unpacks a library whose path in the app's data folder
 * is 262 characters, and stopped at "could not open file to write". "\\?\" takes every part as a name, so "." and ".." parts
 * are resolved here. Shorter paths, and relative ones, are returned as they were: Win32's own handling of names, which the
 * rest of this file relies on, stays theirs. Frees `w` when it returns another string.
 */
static wchar_t *long_path(wchar_t *w)
{
    size_t n = wcslen(w), i = 2, o;
    wchar_t *l;
    if (n < MAX_PATH - 12 || !((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) || w[1] != L':' || w[2] != L'\\')
        return w;
    l = (wchar_t *)mg_alloc(sizeof(wchar_t) * (n + 6));
    memcpy(l, L"\\\\?\\", 4 * sizeof(wchar_t));
    l[4] = w[0];
    l[5] = L':';
    o = 6;
    while (i < n) {
        size_t j, len;
        while (i < n && w[i] == L'\\') i++;
        for (j = i; j < n && w[j] != L'\\'; j++) {}
        len = j - i;
        if (len == 2 && w[i] == L'.' && w[i + 1] == L'.') {
            while (o > 6 && l[o - 1] != L'\\') o--;
            if (o > 6) o--;
        } else if (len && !(len == 1 && w[i] == L'.')) {
            l[o++] = L'\\';
            memcpy(l + o, w + i, len * sizeof(wchar_t));
            o += len;
        }
        i = j;
    }
    if (o == 6) l[o++] = L'\\';
    l[o] = 0;
    free(w);
    return l;
}

/* UTF-8 with forward slashes to what Win32 takes, with the names in it that Windows refuses mapped (mg_host.h); malloc'd. */
static wchar_t *wide_path(const char *path)
{
    char *mapped = mg_name_to_host(path, MG_NAME_HOST);
    int n = MultiByteToWideChar(CP_UTF8, 0, mapped, -1, NULL, 0);
    wchar_t *w, *p;
    if (n <= 0) n = 1;
    w = (wchar_t *)mg_alloc(sizeof(wchar_t) * (size_t)(n + 1));
    MultiByteToWideChar(CP_UTF8, 0, mapped, -1, w, n);
    free(mapped);
    for (p = w; *p; p++)
        if (*p == L'/') *p = L'\\';
    /* A trailing separator makes several calls fail ("C:\dir\"); a bare root keeps its own. */
    n = (int)wcslen(w);
    while (n > 3 && w[n - 1] == L'\\') w[--n] = 0;
    return long_path(w);
}

static char *utf8_of(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)mg_alloc((size_t)(n > 0 ? n : 1) + 1);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* A foreign path's handle (mg_host.h), which the file's owner hands over; NULL with *err set. */
static HANDLE foreign_handle(const char *path, int flags, int kind, int *err)
{
    HANDLE h = (HANDLE)mg_foreign_open(path, flags, kind, err);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = MG_E_IO;
        return NULL;
    }
    return h;
}

/* ------------------------------------------------ files by handle (mg_host.h) */

#define HANDLE_FILES 8

static SRWLOCK g_by_handle_lock = SRWLOCK_INIT;
static struct {
    char *path;
    void *(*open)(void *user);
    void *user;
} g_by_handle[HANDLE_FILES];

static bool same_path(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '/' ? '\\' : (*a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a);
        char y = *b == '/' ? '\\' : (*b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b);
        if (x != y) return false;
    }
    return !*a && !*b;
}

bool mg_file_by_handle(const char *path, void *(*open)(void *user), void *user)
{
    int i, free_at = -1;
    bool done = false;
    if (!path) return false;
    AcquireSRWLockExclusive(&g_by_handle_lock);
    for (i = 0; i < HANDLE_FILES; i++) {
        if (g_by_handle[i].path && same_path(g_by_handle[i].path, path)) {
            free(g_by_handle[i].path);
            memset(&g_by_handle[i], 0, sizeof(g_by_handle[i]));
        }
        if (!g_by_handle[i].path && free_at < 0) free_at = i;
    }
    if (!open) {
        done = true;
    } else if (free_at >= 0) {
        g_by_handle[free_at].path = mg_strdup(path);
        g_by_handle[free_at].open = open;
        g_by_handle[free_at].user = user;
        done = true;
    }
    ReleaseSRWLockExclusive(&g_by_handle_lock);
    return done;
}

/* A new handle of a file by handle, INVALID_HANDLE_VALUE when it is none, NULL when its owner gave none. */
static HANDLE by_handle(const char *path)
{
    void *(*open)(void *) = NULL;
    void *user = NULL;
    int i;
    AcquireSRWLockShared(&g_by_handle_lock);
    for (i = 0; i < HANDLE_FILES; i++)
        if (g_by_handle[i].path && same_path(g_by_handle[i].path, path)) {
            open = g_by_handle[i].open;
            user = g_by_handle[i].user;
        }
    ReleaseSRWLockShared(&g_by_handle_lock);
    if (!open) return INVALID_HANDLE_VALUE;
    {
        HANDLE h = (HANDLE)open(user);
        return h == INVALID_HANDLE_VALUE ? NULL : h;
    }
}

mg_file *mg_file_open(const char *path, int flags, int *err)
{
    wchar_t *w;
    DWORD access = 0, disp;
    HANDLE h;
    mg_file *f;
    h = path ? by_handle(path) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        if (!h || (flags & (MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC | MG_O_APPEND))) {
            if (h) CloseHandle(h);
            if (err) *err = h ? MG_E_ACCESS : MG_E_IO;
            return NULL;
        }
        f = (mg_file *)mg_alloc(sizeof(*f));
        f->h = h;
        f->append = false;
        if (err) *err = MG_E_OK;
        return f;
    }
    if (mg_path_is_foreign(path)) {
        h = foreign_handle(path, flags, MG_FOREIGN_FILE, err);
        if (!h) return NULL;
        f = (mg_file *)mg_alloc(sizeof(*f));
        f->h = h;
        f->append = (flags & MG_O_APPEND) != 0;
        return f;
    }
    w = wide_path(path);
    if (flags & MG_O_READ) access |= GENERIC_READ;
    if (flags & MG_O_WRITE) access |= GENERIC_WRITE;
    if (!access) access = GENERIC_READ;
    if ((flags & MG_O_CREATE) && (flags & MG_O_EXCL)) disp = CREATE_NEW;
    else if ((flags & MG_O_CREATE) && (flags & MG_O_TRUNC)) disp = CREATE_ALWAYS;
    else if (flags & MG_O_CREATE) disp = OPEN_ALWAYS;
    else if (flags & MG_O_TRUNC) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;
    h = CreateFile2(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disp, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(w, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                free(w);
                if (err) *err = MG_E_ISDIR;
                return NULL;
            }
        }
        free(w);
        if (err) *err = err_of(e);
        return NULL;
    }
    free(w);
    f = (mg_file *)mg_alloc(sizeof(*f));
    f->h = h;
    f->append = (flags & MG_O_APPEND) != 0;
    if (err) *err = MG_E_OK;
    return f;
}

void mg_file_close(mg_file *f)
{
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

i64 mg_file_read(mg_file *f, void *buf, size_t n, int *err)
{
    DWORD got = 0;
    if (!ReadFile(f->h, buf, (DWORD)n, &got, NULL)) {
        if (GetLastError() == ERROR_HANDLE_EOF || GetLastError() == ERROR_BROKEN_PIPE) return 0;
        SET_ERR();
        return -1;
    }
    return (i64)got;
}

i64 mg_file_write(mg_file *f, const void *buf, size_t n, int *err)
{
    DWORD put = 0;
    if (f->append) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        SetFilePointerEx(f->h, zero, NULL, FILE_END);
    }
    if (!WriteFile(f->h, buf, (DWORD)n, &put, NULL)) { SET_ERR(); return -1; }
    return (i64)put;
}

static bool tell(mg_file *f, LARGE_INTEGER *pos)
{
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return SetFilePointerEx(f->h, zero, pos, FILE_CURRENT) != 0;
}

i64 mg_file_pread(mg_file *f, void *buf, size_t n, u64 offset, int *err)
{
    LARGE_INTEGER saved, at;
    DWORD got = 0;
    BOOL ok;
    DWORD e;
    if (!tell(f, &saved)) { SET_ERR(); return -1; }
    at.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->h, at, NULL, FILE_BEGIN)) { SET_ERR(); return -1; }
    ok = ReadFile(f->h, buf, (DWORD)n, &got, NULL);
    e = GetLastError();
    SetFilePointerEx(f->h, saved, NULL, FILE_BEGIN);
    if (!ok) {
        if (e == ERROR_HANDLE_EOF) return 0;
        if (err) *err = err_of(e);
        return -1;
    }
    return (i64)got;
}

i64 mg_file_pwrite(mg_file *f, const void *buf, size_t n, u64 offset, int *err)
{
    LARGE_INTEGER saved, at;
    DWORD put = 0;
    BOOL ok;
    DWORD e;
    if (!tell(f, &saved)) { SET_ERR(); return -1; }
    at.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(f->h, at, NULL, FILE_BEGIN)) { SET_ERR(); return -1; }
    ok = WriteFile(f->h, buf, (DWORD)n, &put, NULL);
    e = GetLastError();
    SetFilePointerEx(f->h, saved, NULL, FILE_BEGIN);
    if (!ok) {
        if (err) *err = err_of(e);
        return -1;
    }
    return (i64)put;
}

i64 mg_file_seek(mg_file *f, i64 offset, int whence, int *err)
{
    LARGE_INTEGER to, now;
    to.QuadPart = offset;
    if (!SetFilePointerEx(f->h, to, &now, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END)) {
        if (err) *err = GetLastError() == ERROR_NEGATIVE_SEEK ? MG_E_INVAL : err_of(GetLastError());
        return -1;
    }
    return (i64)now.QuadPart;
}

bool mg_file_truncate(mg_file *f, u64 size, int *err)
{
    LARGE_INTEGER saved, at;
    BOOL ok;
    if (!tell(f, &saved)) { SET_ERR(); return false; }
    at.QuadPart = (LONGLONG)size;
    ok = SetFilePointerEx(f->h, at, NULL, FILE_BEGIN) && SetEndOfFile(f->h);
    if (!ok) SET_ERR();
    SetFilePointerEx(f->h, saved, NULL, FILE_BEGIN);
    return ok != 0;
}

bool mg_file_sync(mg_file *f) { return FlushFileBuffers(f->h) != 0; }

static bool stat_handle(HANDLE h, mg_stat *st)
{
    FILE_BASIC_INFO bi;
    FILE_STANDARD_INFO si;
    FILETIME ft;
    memset(st, 0, sizeof(*st));
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si))) {
        /*
         * The older call, for a handle that will not answer the newer one:
         * it is the one a phone was seen to answer for a file of the user's
         * music, handed over by Windows' broker.
         */
        BY_HANDLE_FILE_INFORMATION all;
        if (!GetFileInformationByHandle(h, &all)) return false;
        st->is_dir = (all.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st->size = ((u64)all.nFileSizeHigh << 32) | all.nFileSizeLow;
        st->mtime_ns = (i64)filetime_to_unix_ns(all.ftLastWriteTime);
        st->atime_ns = (i64)filetime_to_unix_ns(all.ftLastAccessTime);
        st->ctime_ns = (i64)filetime_to_unix_ns(all.ftCreationTime);
        return true;
    }
    st->is_dir = si.Directory != 0;
    st->size = (u64)si.EndOfFile.QuadPart;
    if (GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) {
        ft.dwLowDateTime = bi.LastWriteTime.LowPart; ft.dwHighDateTime = (DWORD)bi.LastWriteTime.HighPart;
        st->mtime_ns = (i64)filetime_to_unix_ns(ft);
        ft.dwLowDateTime = bi.LastAccessTime.LowPart; ft.dwHighDateTime = (DWORD)bi.LastAccessTime.HighPart;
        st->atime_ns = (i64)filetime_to_unix_ns(ft);
        ft.dwLowDateTime = bi.ChangeTime.LowPart; ft.dwHighDateTime = (DWORD)bi.ChangeTime.HighPart;
        st->ctime_ns = (i64)filetime_to_unix_ns(ft);
    }
    return true;
}

bool mg_file_stat(mg_file *f, mg_stat *st) { return stat_handle(f->h, st); }

bool mg_path_stat(const char *path, mg_stat *st, int *err)
{
    wchar_t *w;
    WIN32_FILE_ATTRIBUTE_DATA a;
    BOOL ok;
    if (mg_path_is_foreign(path)) {
        HANDLE h = foreign_handle(path, MG_O_READ, MG_FOREIGN_ANY, err);
        if (!h) return false;
        ok = stat_handle(h, st);
        if (!ok) SET_ERR();
        CloseHandle(h);
        return ok != 0;
    }
    w = wide_path(path);
    ok = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    if (!ok) SET_ERR();
    free(w);
    if (!ok) return false;
    memset(st, 0, sizeof(*st));
    st->is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->size = ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    st->mtime_ns = (i64)filetime_to_unix_ns(a.ftLastWriteTime);
    st->atime_ns = (i64)filetime_to_unix_ns(a.ftLastAccessTime);
    st->ctime_ns = (i64)filetime_to_unix_ns(a.ftCreationTime);
    return true;
}

bool mg_path_mkdir(const char *path, int *err)
{
    wchar_t *w;
    BOOL ok;
    if (mg_path_is_foreign(path)) return mg_foreign_mkdir(path, err);
    w = wide_path(path);
    ok = CreateDirectoryW(w, NULL);
    if (!ok) SET_ERR();
    free(w);
    return ok != 0;
}

bool mg_path_remove(const char *path, int *err)
{
    wchar_t *w;
    BOOL ok;
    if (mg_path_is_foreign(path)) return mg_foreign_remove(path, false, err);
    w = wide_path(path);
    ok = DeleteFileW(w);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            /* A read-only file: POSIX unlink does not care about the file's own mode. */
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW(w, GetFileExInfoStandard, &a) && (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (err) *err = MG_E_ISDIR;
                free(w);
                return false;
            }
            if (SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL)) ok = DeleteFileW(w);
            if (!ok) e = GetLastError();
        }
        if (!ok && err) *err = err_of(e);
    }
    free(w);
    return ok != 0;
}

bool mg_path_rmdir(const char *path, int *err)
{
    wchar_t *w;
    BOOL ok;
    if (mg_path_is_foreign(path)) return mg_foreign_remove(path, true, err);
    w = wide_path(path);
    ok = RemoveDirectoryW(w);
    if (!ok) SET_ERR();
    free(w);
    return ok != 0;
}

/*
 * A file that a rename has put aside (replace_in_use) keeps a name that begins so until the last handle and view of it are
 * gone; listings leave such names out.
 */
static const wchar_t ASIDE_PREFIX[] = L".megler-replaced-";

static bool is_aside(const wchar_t *name)
{
    return !wcsncmp(name, ASIDE_PREFIX, sizeof(ASIDE_PREFIX) / sizeof(wchar_t) - 1);
}

/*
 * POSIX rename replaces a file that is open or mapped, and those who have it keep the old one. Windows refuses to replace a
 * file anyone has open, even when every open shared deletion, as every open here does: Google's GmsCore writes its Chimera
 * config so, over the live one it has mapped, and without the new config none of its modules was loaded on the phone. So the
 * file in the way is renamed aside, which an open shared for deletion allows, the move is made, and the file aside is deleted:
 * it goes when its last handle and view do. One that cannot be deleted yet is hidden, and deleted by a later listing of its
 * folder. Whatever fails, the file in the way is put back.
 */
static BOOL replace_in_use(const wchar_t *a, const wchar_t *b)
{
    static volatile LONG counter;
    WIN32_FILE_ATTRIBUTE_DATA from, in_way;
    const wchar_t *slash = wcsrchr(b, L'\\');
    size_t dir = slash ? (size_t)(slash - b) + 1 : 0;
    wchar_t *aside;
    DWORD why;
    BOOL ok;
    if (!GetFileAttributesExW(a, GetFileExInfoStandard, &from) || !GetFileAttributesExW(b, GetFileExInfoStandard, &in_way)
        || (in_way.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    aside = (wchar_t *)mg_alloc(sizeof(wchar_t) * (dir + 64));
    memcpy(aside, b, sizeof(wchar_t) * dir);
    swprintf(aside + dir, 64, L"%ls%08lx-%08lx-%08lx", ASIDE_PREFIX, (unsigned long)GetCurrentProcessId(),
             (unsigned long)InterlockedIncrement(&counter), (unsigned long)GetTickCount());
    if (!MoveFileExW(b, aside, 0)) {
        free(aside);
        return FALSE;
    }
    ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        why = GetLastError();
        MoveFileExW(aside, b, 0);
        SetLastError(why);
    } else if (!DeleteFileW(aside)) {
        SetFileAttributesW(aside, in_way.dwFileAttributes | FILE_ATTRIBUTE_HIDDEN);
    }
    free(aside);
    return ok;
}

bool mg_path_rename(const char *from, const char *to, int *err)
{
    wchar_t *a, *b;
    BOOL ok;
    if (mg_path_is_foreign(from) || mg_path_is_foreign(to)) return mg_foreign_rename(from, to, err);
    a = wide_path(from);
    b = wide_path(to);
    ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_USER_MAPPED_FILE) {
            ok = replace_in_use(a, b);
            if (!ok) SetLastError(e);
        }
    }
    if (!ok) SET_ERR();
    free(a);
    free(b);
    return ok != 0;
}

/*
 * A directory read through a handle to it, which is all there is of a
 * foreign one: no pattern, no path. The entries come a bufferful at a time.
 */
static bool list_handle(HANDLE h, mg_dir_cb cb, void *user, int *err)
{
    enum { LIST_BYTES = 16384 };
    /* (Not on the stack: the caller may be a thread of the app's, with a stack of the app's choosing.) */
    FILE_ID_BOTH_DIR_INFO *buffer = (FILE_ID_BOTH_DIR_INFO *)mg_alloc(LIST_BYTES);
    bool first = true, ok = true;
    for (;;) {
        const FILE_ID_BOTH_DIR_INFO *e = buffer;
        if (!GetFileInformationByHandleEx(h, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buffer, LIST_BYTES)) {
            DWORD why = GetLastError();
            /* The end; or, at the first call, a directory with nothing at all in it (a volume's root has not even "."). */
            if (why != ERROR_NO_MORE_FILES && !(first && why == ERROR_FILE_NOT_FOUND)) {
                if (err) *err = err_of(why);
                ok = false;
            }
            break;
        }
        first = false;
        for (;;) {
            size_t n = e->FileNameLength / sizeof(wchar_t);
            if (!(n == 1 && e->FileName[0] == L'.') && !(n == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.')) {
                int bytes = WideCharToMultiByte(CP_UTF8, 0, e->FileName, (int)n, NULL, 0, NULL, NULL);
                char *name = (char *)mg_alloc((size_t)(bytes > 0 ? bytes : 0) + 1);
                if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, e->FileName, (int)n, name, bytes, NULL, NULL);
                mg_name_from_host(name);
                if (name[0]) cb(name, (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
                free(name);
            }
            if (!e->NextEntryOffset) break;
            e = (const FILE_ID_BOTH_DIR_INFO *)((const u8 *)e + e->NextEntryOffset);
        }
    }
    free(buffer);
    return ok;
}

bool mg_path_list(const char *path, mg_dir_cb cb, void *user, int *err)
{
    wchar_t *w, *dir;
    size_t len;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (mg_path_is_foreign(path)) {
        bool ok;
        h = foreign_handle(path, MG_O_READ, MG_FOREIGN_DIR, err);
        if (!h) return false;
        ok = list_handle(h, cb, user, err);
        CloseHandle(h);
        return ok;
    }
    /* The pattern's '*' after the mapping, which would make it a name's. */
    dir = wide_path(path);
    len = wcslen(dir);
    w = (wchar_t *)mg_alloc(sizeof(wchar_t) * (len + 3));
    memcpy(w, dir, sizeof(wchar_t) * len);
    if (len && dir[len - 1] != L'\\') w[len++] = L'\\';
    w[len++] = L'*';
    w[len] = 0;
    free(dir);
    h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(w);
        if (e == ERROR_FILE_NOT_FOUND) {
            /* An existing but empty directory answers this way on some volumes. */
            mg_stat st;
            if (mg_path_stat(path, &st, NULL) && st.is_dir) return true;
        }
        if (err) *err = err_of(e);
        return false;
    }
    do {
        char *name;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (is_aside(fd.cFileName)) {
            /* A file a rename put aside: deleted once nothing has it any more (a delete still pending leaves it listed). */
            size_t n = len - 1, k = wcslen(fd.cFileName);
            wchar_t *full = (wchar_t *)mg_alloc(sizeof(wchar_t) * (n + k + 1));
            memcpy(full, w, sizeof(wchar_t) * n);
            memcpy(full + n, fd.cFileName, sizeof(wchar_t) * (k + 1));
            DeleteFileW(full);
            free(full);
            continue;
        }
        name = utf8_of(fd.cFileName);
        mg_name_from_host(name);
        cb(name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(w);
    return true;
}

bool mg_map_open(mg_map *m, const char *path)
{
    HANDLE file, mapping;
    LARGE_INTEGER size;
    void *view;
    FILE_STANDARD_INFO si;
    memset(m, 0, sizeof(*m));
    if (mg_path_is_foreign(path)) {
        file = foreign_handle(path, MG_O_READ, MG_FOREIGN_FILE, NULL);
        if (!file) return false;
    } else {
        wchar_t *w = wide_path(path);
        file = CreateFile2(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, NULL);
        free(w);
        if (file == INVALID_HANDLE_VALUE) return false;
    }
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &si, sizeof(si)) || si.EndOfFile.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }
    size = si.EndOfFile;
#if MG_UWP
    mapping = CreateFileMappingFromApp(file, NULL, PAGE_READONLY, 0, NULL);
#else
    mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
#endif
    if (!mapping) {
        CloseHandle(file);
        return false;
    }
#if MG_UWP
    view = MapViewOfFileFromApp(mapping, FILE_MAP_READ, 0, 0);
#else
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
#endif
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    m->data = (const u8 *)view;
    m->size = (u64)size.QuadPart;
    m->h1 = file;
    m->h2 = mapping;
    return true;
}

bool mg_path_space(const char *path, u64 *total, u64 *available)
{
    wchar_t *w;
    ULARGE_INTEGER mine, all, unused;
    BOOL ok;
    if (mg_path_is_foreign(path)) return false;
    w = wide_path(path);
    ok = GetDiskFreeSpaceExW(w, &mine, &all, &unused);
    free(w);
    if (!ok) return false;
    *total = all.QuadPart;
    *available = mine.QuadPart;
    return true;
}

void mg_map_close(mg_map *m)
{
    if (m->data) UnmapViewOfFile(m->data);
    if (m->h2) CloseHandle((HANDLE)m->h2);
    if (m->h1) CloseHandle((HANDLE)m->h1);
    memset(m, 0, sizeof(*m));
}

/* ---------------------------------------------------------------- sockets
 *
 * Every socket is non-blocking and has an event Winsock sets on each of its
 * network events (WSAEventSelect), which a wait of the thread pool watches.
 * When it fires the events are taken (WSAEnumNetworkEvents, which resets the
 * event), what they say that select() cannot is kept -- how a connection
 * attempt ended, that the other side closed -- and whoever mg_sock_set_notify
 * named is told. What holds at a given moment is select()'s answer with no
 * wait. WSAPoll is not used: on the Windows these phones have (before 10
 * 2004) it never reports a connection attempt that failed.
 *
 * Winsock's events are edges, and some are armed again only by a call that
 * fails: room to send (FD_WRITE) is announced only after a send that found
 * none. So a send that took part of what it was given is asked again with
 * the rest, until it has taken all of it or found no room (mg_sock_send).
 */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif

struct mg_sock {
    SOCKET s;
    WSAEVENT ev;
    PTP_WAIT wait;
    int family, type;
    volatile LONG closing;
    volatile LONG connecting;          /* an attempt is under way: what select() then puts among the exceptions is its failure */
    volatile LONG connect_error;       /* how the last one ended, as FD_CONNECT said, until asked for (a WSA error) */
    volatile LONG failed;              /* an attempt failed: the socket is closed as far as anyone polling it can tell */
    volatile LONG error_pending;       /* ... and its error has not been asked for yet */
    volatile LONG peer_closed;         /* FD_CLOSE: the other side sends no more */
    volatile LONG reset;               /* ... because the connection was reset or aborted */
    int reuseaddr;                     /* kept here, not given to Windows (mg_sock_set) */
    struct mg_ping *ping;              /* a ping socket's own (ping_open): no Windows socket then */
};

static void (*volatile g_sock_notify)(void);
void mg_sock_set_notify(void (*changed)(void)) { g_sock_notify = changed; }

static bool wsa_start(void)
{
    static volatile LONG state;        /* 0 not yet, 1 started, -1 refused */
    if (!state) {
        WSADATA d;
        int e = WSAStartup(MAKEWORD(2, 2), &d);
        if (e) mg_err("host: Winsock did not start (error %d): there are no sockets", e);
        InterlockedExchange(&state, e ? -1 : 1);
    }
    return state == 1;
}

static int net_err(int e)
{
    switch (e) {
    case 0: return MG_NE_OK;
    case WSAEWOULDBLOCK: return MG_NE_AGAIN;
    case WSAEINPROGRESS: return MG_NE_INPROGRESS;
    case WSAEALREADY: return MG_NE_ALREADY;
    case WSAEISCONN: return MG_NE_ISCONN;
    case WSAENOTCONN: return MG_NE_NOTCONN;
    case WSAECONNREFUSED: return MG_NE_CONNREFUSED;
    case WSAECONNRESET: return MG_NE_CONNRESET;
    case WSAECONNABORTED: return MG_NE_CONNABORTED;
    case WSAETIMEDOUT: return MG_NE_TIMEDOUT;
    case WSAENETUNREACH: return MG_NE_NETUNREACH;
    case WSAEHOSTUNREACH: case WSAEHOSTDOWN: return MG_NE_HOSTUNREACH;
    case WSAENETDOWN: return MG_NE_NETDOWN;
    case WSAENETRESET: return MG_NE_NETRESET;
    case WSAEADDRINUSE: return MG_NE_ADDRINUSE;
    case WSAEADDRNOTAVAIL: return MG_NE_ADDRNOTAVAIL;
    case WSAEAFNOSUPPORT: case WSAEPFNOSUPPORT: return MG_NE_AFNOSUPPORT;
    case WSAEPROTONOSUPPORT: case WSAEPROTOTYPE: return MG_NE_PROTONOSUPPORT;
    case WSAESOCKTNOSUPPORT: return MG_NE_SOCKTNOSUPPORT;
    case WSAEOPNOTSUPP: return MG_NE_OPNOTSUPP;
    case WSAENOPROTOOPT: return MG_NE_NOPROTOOPT;
    case WSAEINVAL: return MG_NE_INVAL;
    case WSAEACCES: return MG_NE_ACCES;
    case WSAEMSGSIZE: return MG_NE_MSGSIZE;
    case WSAENOBUFS: case WSA_NOT_ENOUGH_MEMORY: return MG_NE_NOBUFS;
    case WSAEMFILE: return MG_NE_MFILE;
    case WSAESHUTDOWN: return MG_NE_SHUTDOWN;
    case WSAEDESTADDRREQ: return MG_NE_DESTADDRREQ;
    case WSAEFAULT: return MG_NE_FAULT;
    default: return MG_NE_IO;
    }
}

static int addr_to_native(const mg_sockaddr *a, SOCKADDR_STORAGE *ss)
{
    memset(ss, 0, sizeof(*ss));
    if (a->family == MG_AF_INET) {
        SOCKADDR_IN *in = (SOCKADDR_IN *)ss;
        in->sin_family = AF_INET;
        in->sin_port = htons(a->port);
        memcpy(&in->sin_addr, a->addr, 4);
        return (int)sizeof(*in);
    }
    if (a->family == MG_AF_INET6) {
        SOCKADDR_IN6 *in6 = (SOCKADDR_IN6 *)ss;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(a->port);
        in6->sin6_flowinfo = a->flowinfo;
        memcpy(&in6->sin6_addr, a->addr, 16);
        in6->sin6_scope_id = a->scope_id;
        return (int)sizeof(*in6);
    }
    return 0;
}

static void addr_from_native(const struct sockaddr *sa, int len, mg_sockaddr *a)
{
    memset(a, 0, sizeof(*a));
    if (!sa) return;
    if (sa->sa_family == AF_INET && len >= (int)sizeof(SOCKADDR_IN)) {
        const SOCKADDR_IN *in = (const SOCKADDR_IN *)sa;
        a->family = MG_AF_INET;
        a->port = ntohs(in->sin_port);
        memcpy(a->addr, &in->sin_addr, 4);
    } else if (sa->sa_family == AF_INET6 && len >= (int)sizeof(SOCKADDR_IN6)) {
        const SOCKADDR_IN6 *in6 = (const SOCKADDR_IN6 *)sa;
        a->family = MG_AF_INET6;
        a->port = ntohs(in6->sin6_port);
        a->flowinfo = in6->sin6_flowinfo;
        memcpy(a->addr, &in6->sin6_addr, 16);
        a->scope_id = in6->sin6_scope_id;
    }
}

/* The thread pool's wait fired: the socket's network events, taken and kept as far as select() cannot say them later, and the news passed on. */
static VOID CALLBACK sock_event(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WAIT wait, TP_WAIT_RESULT result)
{
    mg_sock *k = (mg_sock *)context;
    void (*notify)(void) = g_sock_notify;
    WSANETWORKEVENTS ne;
    (void)instance;
    (void)result;
    if (k->closing) return;
    memset(&ne, 0, sizeof(ne));
    if (WSAEnumNetworkEvents(k->s, k->ev, &ne) == 0) {
        if (ne.lNetworkEvents & FD_CONNECT) {
            int e = ne.iErrorCode[FD_CONNECT_BIT];
            if (e) {
                InterlockedExchange(&k->connect_error, e);
                InterlockedExchange(&k->error_pending, 1);
                InterlockedExchange(&k->failed, 1);
            }
            InterlockedExchange(&k->connecting, 0);
        }
        if (ne.lNetworkEvents & FD_CLOSE) {
            if (ne.iErrorCode[FD_CLOSE_BIT]) InterlockedExchange(&k->reset, 1);
            InterlockedExchange(&k->peer_closed, 1);
        }
    } else {
        WSAResetEvent(k->ev);
    }
    if (notify) notify();
    if (!k->closing) SetThreadpoolWait(wait, k->ev, NULL);
}

static mg_sock *sock_wrap(SOCKET s, int family, int type, int *err)
{
    mg_sock *k = (mg_sock *)mg_alloc(sizeof(*k));
    k->s = s;
    k->family = family;
    k->type = type;
    k->ev = WSACreateEvent();
    k->wait = k->ev != WSA_INVALID_EVENT ? CreateThreadpoolWait(sock_event, k, NULL) : NULL;
    /* (An accepted socket comes with the listening one's events, and its event: this gives it its own.) */
    if (!k->wait || WSAEventSelect(s, k->ev, FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE) != 0) {
        int e = WSAGetLastError();
        *err = e ? net_err(e) : MG_NE_NOBUFS;
        if (k->wait) CloseThreadpoolWait(k->wait);
        if (k->ev != WSA_INVALID_EVENT) WSACloseEvent(k->ev);
        closesocket(s);
        free(k);
        return NULL;
    }
    SetThreadpoolWait(k->wait, k->ev, NULL);
    return k;
}

/*
 * Ping sockets: Linux's socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) (and AF_INET6 with IPPROTO_ICMPV6), through which a
 * program without privileges sends ICMP echo requests and reads the replies. Android's InetAddress.isReachable tries one
 * first (libcore's Inet6AddressImpl.icmpEcho), then a TCP connection to port 7, which nothing answers; urent asks
 * isReachable of its hosts every few seconds and says "no internet connection" when none answers. Windows has no such
 * socket (ICMP takes a raw one, for administrators), but it sends echoes for an app: IcmpSendEcho2 and Icmp6SendEcho2.
 *
 * So the socket is made here: a request written to it (type 8, or 128 for ICMPv6, then the code, checksum, identifier,
 * sequence and data) becomes an echo sent by Windows on a thread of the pool, a second at most; its reply is kept, and
 * reading the socket gives it as Linux does, the ICMP header and the data with no IP header, the identifier the socket's
 * own (Linux puts its "port" there, which getsockname says) and the sequence the request's. No reply in time: nothing to
 * read, as on Linux. One echo at a time: a request written while one is under way is taken and not sent.
 */
#define PING_DATA 2048

typedef struct mg_ping {
    HANDLE icmp;                       /* IcmpCreateFile's or Icmp6CreateFile's */
    PTP_WORK work;                     /* the echo under way, on the pool */
    u16 ident;                         /* the identifier Linux gives the socket: its port */
    volatile LONG busy;                /* an echo is under way */
    volatile LONG ready;               /* its reply is in `reply`, not read yet */
    mg_sockaddr to;                    /* where the echo goes */
    mg_sockaddr peer;                  /* connect's; family 0: none */
    u8 request[8 + PING_DATA];
    u32 request_len;
    u8 reply[8 + PING_DATA];
    u32 reply_len;
} mg_ping;

static u16 internet_checksum(const u8 *p, u32 n)
{
    u32 sum = 0, i;
    for (i = 0; i + 1 < n; i += 2) sum += (u32)p[i] << 8 | p[i + 1];
    if (n & 1) sum += (u32)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static VOID CALLBACK ping_work(PTP_CALLBACK_INSTANCE instance, PVOID context, PTP_WORK work)
{
    mg_sock *k = (mg_sock *)context;
    mg_ping *p = k->ping;
    /* Room for a reply structure, the data echoed and an ICMP error's 8 bytes, as the functions ask. */
    u8 replies[sizeof(ICMPV6_ECHO_REPLY) + sizeof(ICMP_ECHO_REPLY) + PING_DATA + 64];
    const u8 *data = p->request + 8;
    WORD size = (WORD)(p->request_len - 8);
    DWORD n;
    bool ok = false;
    (void)instance;
    (void)work;
    if (k->family == MG_AF_INET) {
        IPAddr to;
        memcpy(&to, p->to.addr, 4);
        n = IcmpSendEcho2(p->icmp, NULL, NULL, NULL, to, (LPVOID)data, size, NULL, replies, sizeof(replies), 1000);
        ok = n > 0 && ((const ICMP_ECHO_REPLY *)replies)->Status == IP_SUCCESS;
    } else {
        SOCKADDR_IN6 from, to;
        memset(&from, 0, sizeof(from));
        memset(&to, 0, sizeof(to));
        from.sin6_family = to.sin6_family = AF_INET6;
        memcpy(&to.sin6_addr, p->to.addr, 16);
        to.sin6_scope_id = p->to.scope_id;
        n = Icmp6SendEcho2(p->icmp, NULL, NULL, NULL, &from, &to, (LPVOID)data, size, NULL, replies, sizeof(replies), 1000);
        ok = n > 0 && ((const ICMPV6_ECHO_REPLY *)replies)->Status == IP_SUCCESS;
    }
    if (ok && !k->closing) {
        u16 sum;
        p->reply[0] = k->family == MG_AF_INET ? 0 : 129;   /* an echo reply */
        p->reply[1] = 0;
        p->reply[2] = p->reply[3] = 0;
        p->reply[4] = (u8)(p->ident >> 8);
        p->reply[5] = (u8)p->ident;
        p->reply[6] = p->request[6];
        p->reply[7] = p->request[7];
        memcpy(p->reply + 8, data, size);
        p->reply_len = 8u + size;
        /* (ICMPv6's checksum takes the IP addresses in; nobody reading a ping socket checks it.) */
        if (k->family == MG_AF_INET) {
            sum = internet_checksum(p->reply, p->reply_len);
            p->reply[2] = (u8)(sum >> 8);
            p->reply[3] = (u8)sum;
        }
        InterlockedExchange(&p->ready, 1);
    }
    InterlockedExchange(&p->busy, 0);
    if (ok && g_sock_notify) g_sock_notify();
}

static mg_sock *ping_open(int family, int type, int *err)
{
    static volatile LONG next;
    mg_sock *k = (mg_sock *)mg_alloc(sizeof(*k));
    mg_ping *p = (mg_ping *)mg_alloc(sizeof(*p));
    LONG id;
    k->s = INVALID_SOCKET;
    k->ev = WSA_INVALID_EVENT;
    k->family = family;
    k->type = type;
    k->ping = p;
    p->icmp = family == MG_AF_INET ? IcmpCreateFile() : Icmp6CreateFile();
    p->work = p->icmp != INVALID_HANDLE_VALUE ? CreateThreadpoolWork(ping_work, k, NULL) : NULL;
    if (p->icmp == INVALID_HANDLE_VALUE || !p->work) {
        *err = p->icmp == INVALID_HANDLE_VALUE ? MG_NE_PROTONOSUPPORT : MG_NE_NOBUFS;
        if (p->icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(p->icmp);
        free(p);
        free(k);
        return NULL;
    }
    if (!next) InterlockedCompareExchange(&next, (LONG)((GetCurrentProcessId() * 2654435761u ^ GetTickCount()) & 0xFFFF), 0);
    do id = InterlockedIncrement(&next) & 0xFFFF;
    while (!id);
    p->ident = (u16)id;
    return k;
}

static void ping_close(mg_sock *k)
{
    mg_ping *p = k->ping;
    InterlockedExchange(&k->closing, 1);
    WaitForThreadpoolWorkCallbacks(p->work, FALSE);
    CloseThreadpoolWork(p->work);
    IcmpCloseHandle(p->icmp);
    free(p);
    free(k);
}

static i32 ping_send(mg_sock *k, const void *buf, u32 n, const mg_sockaddr *to, int *err)
{
    mg_ping *p = k->ping;
    const u8 *b = (const u8 *)buf;
    const mg_sockaddr *dest = to ? to : p->peer.family ? &p->peer : NULL;
    if (!dest) {
        *err = MG_NE_DESTADDRREQ;
        return -1;
    }
    if (dest->family != k->family) {
        *err = MG_NE_AFNOSUPPORT;
        return -1;
    }
    /* Linux takes echo requests only, a whole header at least. */
    if (n < 8 || b[0] != (k->family == MG_AF_INET ? 8 : 128) || b[1] != 0) {
        *err = MG_NE_INVAL;
        return -1;
    }
    if (n > 8 + PING_DATA) {
        *err = MG_NE_MSGSIZE;
        return -1;
    }
    if (InterlockedCompareExchange(&p->busy, 1, 0) != 0) return (i32)n;
    p->to = *dest;
    memcpy(p->request, b, n);
    p->request_len = n;
    InterlockedExchange(&p->ready, 0);
    SubmitThreadpoolWork(p->work);
    return (i32)n;
}

static i32 ping_recv(mg_sock *k, void *buf, u32 n, int flags, mg_sockaddr *from, int *said, int *err)
{
    mg_ping *p = k->ping;
    u32 len;
    if (!p->ready) {
        *err = MG_NE_AGAIN;
        return -1;
    }
    len = p->reply_len < n ? p->reply_len : n;
    memcpy(buf, p->reply, len);
    if (said && len < p->reply_len) *said = MG_MSG_TRUNCATED;
    if (from) {
        *from = p->to;
        from->port = 0;
    }
    if (!(flags & MG_MSG_PEEK)) InterlockedExchange(&p->ready, 0);
    return (i32)len;
}

static bool ping_name(mg_sock *k, bool peer, mg_sockaddr *a, int *err)
{
    mg_ping *p = k->ping;
    if (peer) {
        if (!p->peer.family) {
            *err = MG_NE_NOTCONN;
            return false;
        }
        *a = p->peer;
        return true;
    }
    memset(a, 0, sizeof(*a));
    a->family = k->family;
    a->port = p->ident;
    return true;
}

mg_sock *mg_sock_open(int family, int type, int protocol, int *err)
{
    int af = family == MG_AF_INET ? AF_INET : family == MG_AF_INET6 ? AF_INET6 : 0;
    int st = type == MG_SOCK_STREAM ? SOCK_STREAM : type == MG_SOCK_DGRAM ? SOCK_DGRAM : type == MG_SOCK_RAW ? SOCK_RAW : 0;
    SOCKET s;
    if (!wsa_start()) {
        *err = MG_NE_NETDOWN;
        return NULL;
    }
    if (!af || !st) {
        *err = !af ? MG_NE_AFNOSUPPORT : MG_NE_SOCKTNOSUPPORT;
        return NULL;
    }
    if (st == SOCK_DGRAM && ((af == AF_INET && protocol == IPPROTO_ICMP) || (af == AF_INET6 && protocol == IPPROTO_ICMPV6))) return ping_open(family, type, err);
    s = WSASocketW(af, st, protocol, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) {
        *err = net_err(WSAGetLastError());
        return NULL;
    }
    if (af == AF_INET6) {
        /* Linux's default, and Android counts on it: libcore makes every socket an IPv6 one and reaches IPv4 addresses through it. */
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&off, sizeof(off));
    }
    if (st == SOCK_DGRAM) {
        /* Windows would answer a later receive with WSAECONNRESET when something sent was refused by its destination (ICMP port unreachable); Linux says nothing of it on a socket with no peer. */
        BOOL no = FALSE;
        DWORD got = 0;
        WSAIoctl(s, SIO_UDP_CONNRESET, &no, sizeof(no), NULL, 0, &got, NULL, NULL);
    }
    return sock_wrap(s, family, type, err);
}

void mg_sock_close(mg_sock *k)
{
    u_long blocking = 0;
    if (!k) return;
    if (k->ping) {
        ping_close(k);
        return;
    }
    InterlockedExchange(&k->closing, 1);
    /* Twice: a callback still running when the wait was first taken away may have set it again before it saw `closing`. */
    SetThreadpoolWait(k->wait, NULL, NULL);
    WaitForThreadpoolWaitCallbacks(k->wait, TRUE);
    SetThreadpoolWait(k->wait, NULL, NULL);
    WaitForThreadpoolWaitCallbacks(k->wait, TRUE);
    CloseThreadpoolWait(k->wait);
    /* Blocking again for the close itself: with a linger time set, a non-blocking socket refuses to be closed (WSAEWOULDBLOCK) and stays open. */
    WSAEventSelect(k->s, NULL, 0);
    ioctlsocket(k->s, FIONBIO, &blocking);
    closesocket(k->s);
    WSACloseEvent(k->ev);
    free(k);
}

bool mg_sock_bind(mg_sock *k, const mg_sockaddr *a, int *err)
{
    SOCKADDR_STORAGE ss;
    int len;
    if (k->ping) {
        /* (A port asked for is the identifier, as on Linux.) */
        if (a->port) k->ping->ident = a->port;
        return true;
    }
    len = addr_to_native(a, &ss);
    if (!len) {
        *err = MG_NE_AFNOSUPPORT;
        return false;
    }
    if (bind(k->s, (const struct sockaddr *)&ss, len) != 0) {
        *err = net_err(WSAGetLastError());
        return false;
    }
    return true;
}

bool mg_sock_listen(mg_sock *k, int backlog, int *err)
{
    if (k->ping) {
        *err = MG_NE_OPNOTSUPP;
        return false;
    }
    if (backlog < 1) backlog = 1;
    if (listen(k->s, backlog) != 0) {
        int e = WSAGetLastError();
        if (e == WSAEINVAL) {
            /* Not bound: Linux binds it to a port of its choosing first, Windows refuses. */
            mg_sockaddr any;
            memset(&any, 0, sizeof(any));
            any.family = k->family;
            if (mg_sock_bind(k, &any, err) && listen(k->s, backlog) == 0) return true;
            e = WSAGetLastError();
        }
        *err = net_err(e);
        return false;
    }
    return true;
}

mg_sock *mg_sock_accept(mg_sock *k, mg_sockaddr *peer, int *err)
{
    if (k->ping) {
        *err = MG_NE_OPNOTSUPP;
        return NULL;
    }
    SOCKADDR_STORAGE ss;
    int len = (int)sizeof(ss);
    SOCKET n = accept(k->s, (struct sockaddr *)&ss, &len);
    if (n == INVALID_SOCKET) {
        *err = net_err(WSAGetLastError());
        return NULL;
    }
    if (peer) addr_from_native((const struct sockaddr *)&ss, len, peer);
    return sock_wrap(n, k->family, k->type, err);
}

static bool is_loopback(const mg_sockaddr *a)
{
    static const u8 one[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    if (a->family == MG_AF_INET) return a->addr[0] == 127;
    return a->family == MG_AF_INET6 && (!memcmp(a->addr, one, 16) || (!memcmp(a->addr, mapped, 12) && a->addr[12] == 127));
}

bool mg_sock_connect(mg_sock *k, const mg_sockaddr *a, int *err)
{
    SOCKADDR_STORAGE ss;
    int len, e;
    if (k->ping) {
        if (a->family && a->family != k->family) {
            *err = MG_NE_AFNOSUPPORT;
            return false;
        }
        k->ping->peer = *a;
        return true;
    }
    if (!a->family) {
        /* No peer any more: Windows takes an address of zeros for that. */
        memset(&ss, 0, sizeof(ss));
        ss.ss_family = (ADDRESS_FAMILY)(k->family == MG_AF_INET6 ? AF_INET6 : AF_INET);
        len = k->family == MG_AF_INET6 ? (int)sizeof(SOCKADDR_IN6) : (int)sizeof(SOCKADDR_IN);
    } else if (!(len = addr_to_native(a, &ss))) {
        *err = MG_NE_AFNOSUPPORT;
        return false;
    }
    if (k->type == MG_SOCK_STREAM && a->family && is_loopback(a)) {
        /*
         * Windows answers a connection to a port of this machine that nobody
         * listens on only after sending its opening segment twice more, a
         * second or so later; Linux says ECONNREFUSED at once. With no
         * retransmission of that segment the refusal comes at once here too.
         * Only for this machine's own addresses: on a real network a lost
         * opening segment has to be sent again.
         */
        TCP_INITIAL_RTO_PARAMETERS p;
        DWORD got = 0;
        p.Rtt = TCP_INITIAL_RTO_UNSPECIFIED_RTT;
        p.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
        WSAIoctl(k->s, SIO_TCP_INITIAL_RTO, &p, sizeof(p), NULL, 0, &got, NULL, NULL);
    }
    if (k->type == MG_SOCK_DGRAM) {
        /*
         * A datagram socket with a peer of its own hears of that peer's refusal (an ICMP port or host unreachable), as
         * Linux's does: its next receive or send fails, with ECONNREFUSED (mg_net.c). Without a peer it hears nothing of it,
         * as on Linux (mg_sock_open turns that off). A QUIC client connects its socket and, told so, goes over to TCP at
         * once; not told, it waited out its handshake's time (Facebook's start, behind a network that refuses UDP).
         */
        BOOL tell = a->family != 0;
        DWORD got = 0;
        WSAIoctl(k->s, SIO_UDP_CONNRESET, &tell, sizeof(tell), NULL, 0, &got, NULL, NULL);
    }
    InterlockedExchange(&k->connect_error, 0);
    InterlockedExchange(&k->connecting, 1);
    if (connect(k->s, (const struct sockaddr *)&ss, len) == 0) {
        InterlockedExchange(&k->connecting, 0);
        return true;
    }
    e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK) {
        *err = MG_NE_INPROGRESS;
        return false;
    }
    InterlockedExchange(&k->connecting, 0);
    *err = net_err(e);
    return false;
}

static int native_flags(int flags)
{
    return ((flags & MG_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & MG_MSG_OOB) ? MSG_OOB : 0) | ((flags & MG_MSG_DONTROUTE) ? MSG_DONTROUTE : 0);
}

i32 mg_sock_send(mg_sock *k, const void *buf, u32 n, int flags, const mg_sockaddr *to, int *err)
{
    SOCKADDR_STORAGE ss;
    int len = 0, f = native_flags(flags);
    u32 done = 0;
    if (k->ping) return ping_send(k, buf, n, to, err);
    if (to && !(len = addr_to_native(to, &ss))) {
        *err = MG_NE_AFNOSUPPORT;
        return -1;
    }
    for (;;) {
        int chunk = n - done > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)(n - done), r;
        r = to ? sendto(k->s, (const char *)buf + done, chunk, f, (const struct sockaddr *)&ss, len) : send(k->s, (const char *)buf + done, chunk, f);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (done) return (i32)done;
            *err = net_err(e);
            return -1;
        }
        done += (u32)r;
        /* (A datagram is never sent twice.) */
        if (done >= n || k->type != MG_SOCK_STREAM || r == 0) return (i32)done;
    }
}

i32 mg_sock_recv(mg_sock *k, void *buf, u32 n, int flags, mg_sockaddr *from, int *said, int *err)
{
    SOCKADDR_STORAGE ss;
    int len = (int)sizeof(ss), r;
    if (said) *said = 0;
    if (k->ping) return ping_recv(k, buf, n, flags, from, said, err);
    memset(&ss, 0, sizeof(ss));
    r = recvfrom(k->s, (char *)buf, n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n, native_flags(flags), from ? (struct sockaddr *)&ss : NULL, from ? &len : NULL);
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSAEMSGSIZE) {
            *err = net_err(e);
            return -1;
        }
        /* A datagram larger than the buffer: Windows fills it and drops the rest, as Linux does, but calls that an error. */
        r = n > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)n;
        if (said) *said = MG_MSG_TRUNCATED;
    }
    if (from) addr_from_native((const struct sockaddr *)&ss, len, from);
    return r;
}

bool mg_sock_shutdown(mg_sock *k, int how, int *err)
{
    if (k->ping) return true;
    if (shutdown(k->s, how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH) != 0) {
        *err = net_err(WSAGetLastError());
        return false;
    }
    return true;
}

bool mg_sock_name(mg_sock *k, bool peer, mg_sockaddr *a, int *err)
{
    SOCKADDR_STORAGE ss;
    int len = (int)sizeof(ss), r;
    if (k->ping) return ping_name(k, peer, a, err);
    r = peer ? getpeername(k->s, (struct sockaddr *)&ss, &len) : getsockname(k->s, (struct sockaddr *)&ss, &len);
    if (r != 0) {
        int e = WSAGetLastError();
        if (!peer && e == WSAEINVAL) {
            /* Not bound yet: Linux says its family's wildcard address and port 0, Windows refuses. */
            memset(a, 0, sizeof(*a));
            a->family = k->family;
            return true;
        }
        *err = net_err(e);
        return false;
    }
    addr_from_native((const struct sockaddr *)&ss, len, a);
    return true;
}

int mg_sock_ready(mg_sock *k)
{
    fd_set r, w, x;
    struct timeval none = { 0, 0 };
    int out = 0;
    if (k->ping) return MG_SOCK_OUT | (k->ping->ready ? MG_SOCK_IN : 0);
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&x);
    FD_SET(k->s, &r);
    FD_SET(k->s, &w);
    FD_SET(k->s, &x);
    if (select(0, &r, &w, &x, &none) > 0) {
        if (FD_ISSET(k->s, &r)) out |= MG_SOCK_IN;
        if (FD_ISSET(k->s, &w)) out |= MG_SOCK_OUT;
        /* Among the exceptions while connecting: the attempt failed (otherwise it is urgent data, which nothing here waits for). */
        if (FD_ISSET(k->s, &x) && k->connecting && !k->failed) {
            InterlockedExchange(&k->error_pending, 1);
            InterlockedExchange(&k->failed, 1);
            InterlockedExchange(&k->connecting, 0);
        }
    }
    /* As Linux has a socket whose attempt failed: closed both ways, the error there until asked for. */
    if (k->failed) out |= MG_SOCK_IN | MG_SOCK_OUT | MG_SOCK_HUP | (k->error_pending ? MG_SOCK_ERR : 0);
    if (k->peer_closed) out |= MG_SOCK_IN | MG_SOCK_RDHUP;
    if (k->reset) out |= MG_SOCK_ERR | MG_SOCK_HUP;
    return out;
}

int mg_sock_error(mg_sock *k)
{
    int e = 0, len = (int)sizeof(e);
    LONG kept;
    if (k->ping) return MG_NE_OK;
    /* Windows says a failed attempt's error every time it is asked; Linux once, and 0 after. */
    if (k->failed && !k->error_pending) return MG_NE_OK;
    kept = InterlockedExchange(&k->connect_error, 0);
    if (getsockopt(k->s, SOL_SOCKET, SO_ERROR, (char *)&e, &len) != 0) e = 0;
    if (!e) e = (int)kept;
    if (e) InterlockedExchange(&k->failed, 1);
    InterlockedExchange(&k->error_pending, 0);
    return net_err(e);
}

i32 mg_sock_pending(mg_sock *k)
{
    u_long n = 0;
    if (k->ping) return k->ping->ready ? (i32)k->ping->reply_len : 0;
    if (ioctlsocket(k->s, FIONREAD, &n) != 0) return 0;
    return n > 0x7FFFFFFFu ? 0x7FFFFFFF : (i32)n;
}

/* Windows says only whether urgent data is waiting, not whether the next byte is its mark: never, here. */
bool mg_sock_at_mark(mg_sock *k) { (void)k; return false; }

/* The level and name of an option as Windows has it; 0 for one it has none of. */
static int option_of(int option, int *level)
{
    *level = SOL_SOCKET;
    switch (option) {
    case MG_SO_KEEPALIVE: return SO_KEEPALIVE;
    case MG_SO_BROADCAST: return SO_BROADCAST;
    case MG_SO_OOBINLINE: return SO_OOBINLINE;
    case MG_SO_RCVBUF: return SO_RCVBUF;
    case MG_SO_SNDBUF: return SO_SNDBUF;
    case MG_SO_DONTROUTE: return SO_DONTROUTE;
    case MG_SO_LINGER: return SO_LINGER;
    }
    *level = IPPROTO_TCP;
    switch (option) {
    case MG_TCP_NODELAY: return TCP_NODELAY;
    case MG_TCP_KEEPIDLE: return TCP_KEEPIDLE;
    case MG_TCP_KEEPINTVL: return TCP_KEEPINTVL;
    case MG_TCP_KEEPCNT: return TCP_KEEPCNT;
    case MG_TCP_MAXSEG: return TCP_MAXSEG;
    }
    *level = IPPROTO_IP;
    switch (option) {
    case MG_IP_TOS: return IP_TOS;
    case MG_IP_TTL: return IP_TTL;
    case MG_IP_MULTICAST_TTL: return IP_MULTICAST_TTL;
    case MG_IP_MULTICAST_LOOP: return IP_MULTICAST_LOOP;
    case MG_IP_MULTICAST_IF: return IP_MULTICAST_IF;
    }
    *level = IPPROTO_IPV6;
    switch (option) {
    case MG_IPV6_V6ONLY: return IPV6_V6ONLY;
    case MG_IPV6_UNICAST_HOPS: return IPV6_UNICAST_HOPS;
    case MG_IPV6_MULTICAST_HOPS: return IPV6_MULTICAST_HOPS;
    case MG_IPV6_MULTICAST_LOOP: return IPV6_MULTICAST_LOOP;
    case MG_IPV6_MULTICAST_IF: return IPV6_MULTICAST_IF;
    case MG_IPV6_TCLASS: return IPV6_TCLASS;
    }
    return 0;
}

/* What setsockopt's refusal means: for the options of TCP's keep-alive, which Windows has only from 10 1709, that this one does not know them. */
static int option_err(int option, int e)
{
    if ((option == MG_TCP_KEEPIDLE || option == MG_TCP_KEEPINTVL || option == MG_TCP_KEEPCNT) && (e == WSAEINVAL || e == WSAENOPROTOOPT)) return MG_NE_NOPROTOOPT;
    return net_err(e);
}

bool mg_sock_set(mg_sock *k, int option, int value, int *err)
{
    int level, name;
    DWORD dw = (DWORD)value;
    struct linger lg;
    const char *p = (const char *)&dw;
    int len = (int)sizeof(dw);
    /* A ping socket's options (its time to live, its buffers) are Windows' own for each echo. */
    if (k->ping) return true;
    if (option == MG_SO_REUSEADDR) {
        /*
         * Linux's SO_REUSEADDR lets a server bind a port that connections of
         * its last run still hold in TIME_WAIT, which Windows allows without
         * being asked. Windows' own lets a second socket take a port another
         * one is listening on, which Linux's never does: it is kept here and
         * not passed on.
         */
        k->reuseaddr = value != 0;
        return true;
    }
    if (!(name = option_of(option, &level))) {
        *err = MG_NE_NOPROTOOPT;
        return false;
    }
    if (option == MG_SO_LINGER) {
        lg.l_onoff = value >= 0;
        lg.l_linger = (u_short)(value < 0 ? 0 : value > 65535 ? 65535 : value);
        p = (const char *)&lg;
        len = (int)sizeof(lg);
    }
    if (setsockopt(k->s, level, name, p, len) != 0) {
        *err = option_err(option, WSAGetLastError());
        return false;
    }
    return true;
}

bool mg_sock_get(mg_sock *k, int option, int *value, int *err)
{
    int level, name, len;
    DWORD dw = 0;
    struct linger lg;
    if (k->ping) {
        *value = 0;
        return true;
    }
    if (option == MG_SO_REUSEADDR) {
        *value = k->reuseaddr;
        return true;
    }
    if (!(name = option_of(option, &level))) {
        *err = MG_NE_NOPROTOOPT;
        return false;
    }
    if (option == MG_SO_LINGER) {
        memset(&lg, 0, sizeof(lg));
        len = (int)sizeof(lg);
        if (getsockopt(k->s, level, name, (char *)&lg, &len) != 0) {
            *err = net_err(WSAGetLastError());
            return false;
        }
        *value = lg.l_onoff ? (int)lg.l_linger : -1;
        return true;
    }
    len = (int)sizeof(dw);
    if (getsockopt(k->s, level, name, (char *)&dw, &len) != 0) {
        *err = option_err(option, WSAGetLastError());
        return false;
    }
    /* (Some come back as a single byte.) */
    *value = len == 1 ? (int)(dw & 0xFF) : (int)dw;
    return true;
}

bool mg_sock_multicast(mg_sock *k, bool join, const mg_sockaddr *group, const mg_sockaddr *iface, u32 index, int *err)
{
    int r;
    if (k->ping) {
        *err = MG_NE_OPNOTSUPP;
        return false;
    }
    if (group->family == MG_AF_INET) {
        IP_MREQ m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.imr_multiaddr, group->addr, 4);
        if (iface && iface->family == MG_AF_INET) memcpy(&m.imr_interface, iface->addr, 4);
        else if (index) m.imr_interface.s_addr = htonl(index);  /* Windows' way of naming an interface by its index here: 0.0.0.index */
        r = setsockopt(k->s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, (const char *)&m, sizeof(m));
    } else if (group->family == MG_AF_INET6) {
        IPV6_MREQ m;
        memset(&m, 0, sizeof(m));
        memcpy(&m.ipv6mr_multiaddr, group->addr, 16);
        m.ipv6mr_interface = index;
        r = setsockopt(k->s, IPPROTO_IPV6, join ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP, (const char *)&m, sizeof(m));
    } else {
        *err = MG_NE_AFNOSUPPORT;
        return false;
    }
    if (r != 0) {
        *err = net_err(WSAGetLastError());
        return false;
    }
    return true;
}

/* ---------------------------------------------------- names and interfaces */

static wchar_t *wide_of(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)mg_alloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static int eai_of(int e)
{
    switch (e) {
    case WSAHOST_NOT_FOUND: return MG_EAI_NONAME;
    case WSATRY_AGAIN: return MG_EAI_AGAIN;
    case WSANO_RECOVERY: return MG_EAI_FAIL;
    case WSANO_DATA: return MG_EAI_NODATA;
    case WSAEAFNOSUPPORT: return MG_EAI_FAMILY;
    case WSA_NOT_ENOUGH_MEMORY: return MG_EAI_MEMORY;
    default: return MG_EAI_FAIL;
    }
}

int mg_net_resolve(const char *name, int family, int flags, mg_sockaddr **addrs, int *count, char **canonical)
{
    ADDRINFOW hints, *res = NULL, *ai;
    mg_sockaddr *out = NULL;
    wchar_t *w;
    int e, got = 0, cap = 0, i;
    *addrs = NULL;
    *count = 0;
    if (canonical) *canonical = NULL;
    if (!wsa_start()) return MG_EAI_SYSTEM;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family == MG_AF_INET ? AF_INET : family == MG_AF_INET6 ? AF_INET6 : AF_UNSPEC;
    /* (One answer for each address: with no type every address would come once for each.) */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = ((flags & MG_RESOLVE_CANONICAL) ? AI_CANONNAME : 0) | ((flags & MG_RESOLVE_ADDRCONFIG) ? AI_ADDRCONFIG : 0);
    w = wide_of(name);
    e = GetAddrInfoW(w, NULL, &hints, &res);
    free(w);
    if (e) return eai_of(e);
    for (ai = res; ai; ai = ai->ai_next) {
        mg_sockaddr a;
        addr_from_native(ai->ai_addr, (int)ai->ai_addrlen, &a);
        if (!a.family) continue;
        for (i = 0; i < got && memcmp(&out[i], &a, sizeof(a)); i++) { }
        if (i < got) continue;
        if (got == cap) {
            cap = cap ? cap * 2 : 8;
            out = (mg_sockaddr *)mg_realloc(out, sizeof(*out) * (size_t)cap);
        }
        out[got++] = a;
    }
    if (canonical && res && res->ai_canonname) *canonical = utf8_of(res->ai_canonname);
    FreeAddrInfoW(res);
    if (!got) {
        free(out);
        if (canonical) {
            free(*canonical);
            *canonical = NULL;
        }
        return MG_EAI_NODATA;
    }
    *addrs = out;
    *count = got;
    return MG_EAI_OK;
}

int mg_net_name_of(const mg_sockaddr *a, char *name, size_t size)
{
    SOCKADDR_STORAGE ss;
    wchar_t host[NI_MAXHOST];
    int len = addr_to_native(a, &ss), e;
    char *u;
    if (!wsa_start()) return MG_EAI_SYSTEM;
    if (!len) return MG_EAI_FAMILY;
    e = GetNameInfoW((const SOCKADDR *)&ss, len, host, NI_MAXHOST, NULL, 0, NI_NAMEREQD);
    if (e) return eai_of(e);
    u = utf8_of(host);
    mg_strlcpy(name, u, size);
    free(u);
    return MG_EAI_OK;
}

int mg_net_interfaces(mg_netif *out, int max)
{
    ULONG size = 16 * 1024, r = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *list = NULL, *ad;
    int n = 0, tries;
    for (tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        list = (IP_ADAPTER_ADDRESSES *)mg_realloc(list, size);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME, NULL, list, &size);
    }
    if (r != NO_ERROR) {
        free(list);
        return r == ERROR_NO_DATA ? 0 : -1;
    }
    for (ad = list; ad; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *u;
        mg_netif *f;
        if (n >= max) {
            n++;
            continue;
        }
        f = &out[n++];
        memset(f, 0, sizeof(*f));
        f->index = ad->IfIndex ? ad->IfIndex : ad->Ipv6IfIndex;
        switch (ad->IfType) {
        case IF_TYPE_SOFTWARE_LOOPBACK: f->kind = MG_IF_LOOPBACK; break;
        case IF_TYPE_IEEE80211: f->kind = MG_IF_WIFI; break;
        case IF_TYPE_ETHERNET_CSMACD: f->kind = MG_IF_ETHERNET; break;
        case IF_TYPE_WWANPP: case IF_TYPE_WWANPP2: f->kind = MG_IF_CELLULAR; break;
        case IF_TYPE_TUNNEL: f->kind = MG_IF_TUNNEL; break;
        default: f->kind = MG_IF_OTHER; break;
        }
        if (ad->OperStatus == IfOperStatusUp) f->flags |= MG_IFF_UP | MG_IFF_RUNNING;
        if (!(ad->Flags & IP_ADAPTER_NO_MULTICAST) && f->kind != MG_IF_LOOPBACK) f->flags |= MG_IFF_MULTICAST;
        if (f->kind == MG_IF_WIFI || f->kind == MG_IF_ETHERNET) f->flags |= MG_IFF_BROADCAST;
        if (ad->IfType == IF_TYPE_PPP || f->kind == MG_IF_CELLULAR) f->flags |= MG_IFF_POINTOPOINT;
        /* (The loopback says it has no limit; Linux's has 65536.) */
        f->mtu = ad->Mtu == 0xFFFFFFFFu ? 65536 : ad->Mtu;
        if (ad->PhysicalAddressLength > 0 && ad->PhysicalAddressLength <= sizeof(f->hw)) {
            memcpy(f->hw, ad->PhysicalAddress, ad->PhysicalAddressLength);
            f->hw_len = (int)ad->PhysicalAddressLength;
        }
        for (u = ad->FirstUnicastAddress; u && f->naddrs < MG_NETIF_ADDRS; u = u->Next) {
            mg_sockaddr a;
            addr_from_native(u->Address.lpSockaddr, u->Address.iSockaddrLength, &a);
            if (!a.family) continue;
            f->addrs[f->naddrs].addr = a;
            f->addrs[f->naddrs].prefix = u->OnLinkPrefixLength;
            f->naddrs++;
        }
    }
    free(list);
    return n;
}

/*
 * Windows says fec0:0:0:ffff::1 to ::3, site-local addresses an old draft gave name servers, for an interface that has no
 * IPv6 server of its own: nothing answers there.
 */
static bool dns_placeholder(const SOCKADDR *sa)
{
    static const u8 site[14] = { 0xfe, 0xc0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0 };
    const SOCKADDR_IN6 *in6 = (const SOCKADDR_IN6 *)sa;
    return sa->sa_family == AF_INET6 && !memcmp(in6->sin6_addr.s6_addr, site, sizeof(site)) && in6->sin6_addr.s6_addr[14] == 0
           && in6->sin6_addr.s6_addr[15] >= 1 && in6->sin6_addr.s6_addr[15] <= 3;
}

int mg_net_dns_servers(char (*out)[48], int max)
{
    ULONG size = 16 * 1024, r = ERROR_BUFFER_OVERFLOW;
    IP_ADAPTER_ADDRESSES *list = NULL, *ad;
    int n = 0, tries, pass, i;
    for (tries = 0; tries < 4 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        list = (IP_ADAPTER_ADDRESSES *)mg_realloc(list, size);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME | GAA_FLAG_INCLUDE_GATEWAYS, NULL,
                                 list, &size);
    }
    if (r != NO_ERROR) {
        free(list);
        return 0;
    }
    /* The interfaces with a way out first (Wi-Fi or the cell network as it is now), then any other that is up. */
    for (pass = 0; pass < 2; pass++)
        for (ad = list; ad; ad = ad->Next) {
            IP_ADAPTER_DNS_SERVER_ADDRESS *d;
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if ((ad->FirstGatewayAddress != NULL) != (pass == 0)) continue;
            for (d = ad->FirstDnsServerAddress; d && n < max; d = d->Next) {
                const SOCKADDR *sa = d->Address.lpSockaddr;
                char text[48] = { 0 };
                bool seen = false;
                if (!sa || dns_placeholder(sa)) continue;
                if (sa->sa_family == AF_INET) inet_ntop(AF_INET, &((const SOCKADDR_IN *)sa)->sin_addr, text, sizeof(text));
                else if (sa->sa_family == AF_INET6) inet_ntop(AF_INET6, &((const SOCKADDR_IN6 *)sa)->sin6_addr, text, sizeof(text));
                if (!text[0]) continue;
                for (i = 0; i < n && !seen; i++) seen = !strcmp(out[i], text);
                if (!seen) mg_strlcpy(out[n++], text, sizeof(out[0]));
            }
        }
    free(list);
    return n;
}

/* ------------------------------------------------------ host shared libraries */

void *mg_dl_open(const char *name)
{
    wchar_t *w = wide_path(name);
    HMODULE h;
#if MG_UWP
    h = LoadPackagedLibrary(w, 0);
#else
    h = LoadLibraryW(w);
#endif
    free(w);
    return (void *)h;
}

void *mg_dl_sym(void *lib, const char *name) { return (void *)GetProcAddress((HMODULE)lib, name); }

#if WINAPI_FAMILY == WINAPI_FAMILY_APP
/* Left out of the app partition by the SDK's headers, in the phone's kernelbase like any other. */
WINBASEAPI BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE *phModule);
WINBASEAPI DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);
#endif

bool mg_host_module_at(mg_addr address, char *name, size_t size, mg_addr *base)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    const wchar_t *file;
    DWORD n;
    /* FROM_ADDRESS | UNCHANGED_REFCOUNT */
    if (!GetModuleHandleExW(0x4 | 0x2, (LPCWSTR)address, &module) || !module) return false;
    n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    for (file = path + n; file > path && file[-1] != L'\\' && file[-1] != L'/'; file--) { }
    WideCharToMultiByte(CP_UTF8, 0, file, -1, name, (int)size, NULL, NULL);
    if (size) name[size - 1] = 0;
    if (base) *base = (mg_addr)module;
    return true;
}

/* ------------------------------------------------------------------- misc */

int mg_host_d3d11_feature_level(void *device)
{
    return device ? (int)ID3D11Device_GetFeatureLevel((ID3D11Device *)device) : 0;
}

u32 mg_host_d3d11_removed_reason(void *device)
{
    return device ? (u32)ID3D11Device_GetDeviceRemovedReason((ID3D11Device *)device) : 0;
}

bool mg_host_d3d11_options(void *device, bool *constant_buffer_offsets, bool *constant_buffer_partial_updates)
{
    D3D11_FEATURE_DATA_D3D11_OPTIONS o;
    memset(&o, 0, sizeof(o));
    if (!device || FAILED(ID3D11Device_CheckFeatureSupport((ID3D11Device *)device, D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o)))) return false;
    *constant_buffer_offsets = o.ConstantBufferOffsetting != FALSE;
    *constant_buffer_partial_updates = o.ConstantBufferPartialUpdate != FALSE;
    return true;
}

bool mg_host_address_space(u32 *free_mb, u32 *largest_free_mb, u32 *reserved_mb, u32 *committed_mb)
{
    MEMORY_BASIC_INFORMATION info;
    SYSTEM_INFO si;
    u64 free_bytes = 0, largest = 0, reserved = 0, committed = 0;
    mg_addr at, end;
    GetNativeSystemInfo(&si);
    at = (mg_addr)si.lpMinimumApplicationAddress;
    end = (mg_addr)si.lpMaximumApplicationAddress;
    while (at < end && VirtualQuery((LPCVOID)at, &info, sizeof(info)) == sizeof(info) && info.RegionSize) {
        if (info.State == MEM_FREE) {
            free_bytes += info.RegionSize;
            if (info.RegionSize > largest) largest = info.RegionSize;
        } else if (info.State == MEM_RESERVE) {
            reserved += info.RegionSize;
        } else {
            committed += info.RegionSize;
        }
        at = (mg_addr)info.BaseAddress + info.RegionSize;
    }
    *free_mb = (u32)(free_bytes >> 20);
    *largest_free_mb = (u32)(largest >> 20);
    *reserved_mb = (u32)(reserved >> 20);
    *committed_mb = (u32)(committed >> 20);
    return true;
}

bool mg_host_free_stretches(u32 *mb, int n)
{
    MEMORY_BASIC_INFORMATION info;
    SYSTEM_INFO si;
    mg_addr at, end;
    int i, k;
    for (i = 0; i < n; i++) mb[i] = 0;
    GetNativeSystemInfo(&si);
    at = (mg_addr)si.lpMinimumApplicationAddress;
    end = (mg_addr)si.lpMaximumApplicationAddress;
    while (at < end && VirtualQuery((LPCVOID)at, &info, sizeof(info)) == sizeof(info) && info.RegionSize) {
        u32 size = (u32)(info.RegionSize >> 20);
        if (info.State == MEM_FREE && n > 0 && size > mb[n - 1]) {
            /* In its place among the largest so far. */
            for (k = n - 1; k > 0 && mb[k - 1] < size; k--) mb[k] = mb[k - 1];
            mb[k] = size;
        }
        at = (mg_addr)info.BaseAddress + info.RegionSize;
    }
    return true;
}

/* (Spelled out: the library that holds DXGI's interface ids is not among those the app links.) */
static const GUID g_iid_dxgi_device = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID g_iid_dxgi_adapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };

bool mg_host_gpu_memory(void *device, u32 *budget_mb, u32 *used_mb)
{
    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIAdapter3 *adapter3 = NULL;
    DXGI_QUERY_VIDEO_MEMORY_INFO info;
    bool ok = false;
    if (!device) return false;
    memset(&info, 0, sizeof(info));
    if (SUCCEEDED(ID3D11Device_QueryInterface((ID3D11Device *)device, &g_iid_dxgi_device, (void **)&dxgi)) && dxgi &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi, &adapter)) && adapter &&
        SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &g_iid_dxgi_adapter3, (void **)&adapter3)) && adapter3 &&
        SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        *budget_mb = (u32)(info.Budget >> 20);
        *used_mb = (u32)(info.CurrentUsage >> 20);
        ok = true;
    }
    if (adapter3) IDXGIAdapter3_Release(adapter3);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi) IDXGIDevice_Release(dxgi);
    return ok;
}

/* ID3D11Multithread (d3d11_4.h, Windows 10 1703; the same id as ID3D10Multithread), declared here: only its lock is needed. */
static const GUID g_iid_d3d11_device = { 0xdb6f6ddb, 0xac77, 0x4e88, { 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40 } };
static const GUID g_iid_multithread = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };
typedef struct mt_vtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(void *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(void *);
    ULONG(STDMETHODCALLTYPE *Release)(void *);
    void(STDMETHODCALLTYPE *Enter)(void *);
    void(STDMETHODCALLTYPE *Leave)(void *);
    BOOL(STDMETHODCALLTYPE *SetMultithreadProtected)(void *, BOOL);
    BOOL(STDMETHODCALLTYPE *GetMultithreadProtected)(void *);
} mt_vtbl;
typedef struct mt_object {
    const mt_vtbl *v;
} mt_object;

bool mg_host_keyed_mutex(void *mutex, bool acquire, u32 ms)
{
    IDXGIKeyedMutex *km = (IDXGIKeyedMutex *)mutex;
    ID3D11Device *device = NULL;
    mt_object *mt = NULL;
    HRESULT hr;
    if (!km) return false;
    if (SUCCEEDED(IDXGIKeyedMutex_GetDevice(km, &g_iid_d3d11_device, (void **)&device)) && device &&
        SUCCEEDED(ID3D11Device_QueryInterface(device, &g_iid_multithread, (void **)&mt)) && mt) {
        if (!mt->v->GetMultithreadProtected(mt)) mt->v->SetMultithreadProtected(mt, TRUE);
        mt->v->Enter(mt);
    }
    hr = acquire ? IDXGIKeyedMutex_AcquireSync(km, 0, ms) : IDXGIKeyedMutex_ReleaseSync(km, 0);
    if (mt) {
        mt->v->Leave(mt);
        mt->v->Release(mt);
    }
    if (device) ID3D11Device_Release(device);
    return hr == S_OK;
}

void mg_random(void *buf, size_t n)
{
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        size_t i;
        u64 x = mg_time_monotonic_ns() ^ ((u64)GetCurrentThreadId() << 32);
        for (i = 0; i < n; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            ((u8 *)buf)[i] = (u8)x;
        }
    }
}

void mg_host_print(const char *line)
{
    /*
     * To a debugger, when one is attached. OutputDebugStringA raises an exception and catches it, for every line: with no
     * debugger that reaches nobody and costs a dispatch, and on the phone the dispatch itself faulted in ntdll (an access
     * at 80000002) on a thread of urent's WebView, inside the log's lock, which stood every other thread still.
     */
    if (IsDebuggerPresent()) {
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
    }
#if !MG_UWP
    fputs(line, stderr);
    fputc('\n', stderr);
#endif
}

const char *mg_host_name(void)
{
#if defined(_M_ARM)
    return "win32-arm";
#elif defined(_M_X64)
    return "win32-x64";
#else
    return "win32";
#endif
}

#endif /* _WIN32 */
