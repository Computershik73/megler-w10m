/*
 * What every file of the core starts from: fixed-width types, the few things the
 * three compilers spell differently, and the log.
 *
 * The core is C11 that three compilers build: MSVC for ARM (the phone), GCC for
 * arm-linux-gnueabihf (the same code under qemu-arm, where the tests run guest
 * code) and MSVC for x64 (this PC, for what needs no ARM). So: no GNU
 * extensions, no arithmetic on void pointers, no variable-length arrays.
 *
 * And clang, for desktop Windows on x64, ARM64 and x86 (docs/DESKTOP.md): there
 * the core is compiled for Android's own ABI of that processor
 * (--target=x86_64-linux-android and so on) into an ELF library of its own,
 * which Megler.exe loads (host/win32/mg_core_loader.c). Its C is then of the
 * guest's ABI throughout -- `long`, `va_list`, `wchar_t`, how floats are passed
 * -- and guest code calls it as it calls bionic. What crosses to Windows is
 * named: the host layer's functions (mg_host.h), Megler's API (megler.h), and
 * the C library's functions the core uses, which mg_core_loader.c binds to
 * Windows' (host/win32/mg_crt_bridge.c). MG_CORE_ELF says a file is compiled so.
 */
#ifndef MG_BASE_H
#define MG_BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

#if defined(_MSC_VER)
#define MG_THREAD_LOCAL __declspec(thread)
#define MG_NOINLINE __declspec(noinline)
#define MG_NORETURN __declspec(noreturn)
#define MG_PRINTF(fmt, args)
#else
#define MG_THREAD_LOCAL __thread
#define MG_NOINLINE __attribute__((noinline))
#define MG_NORETURN __attribute__((noreturn))
#define MG_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

/*
 * The processor, which is the guest's as well: nothing here runs code of one
 * processor on another. Android's ABI of each: armeabi-v7a, arm64-v8a, x86_64,
 * x86.
 */
#if defined(_M_ARM) || defined(__arm__)
#define MG_ARCH_ARM32 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define MG_ARCH_ARM64 1
#elif defined(_M_X64) || defined(__x86_64__)
#define MG_ARCH_X86_64 1
#elif defined(_M_IX86) || defined(__i386__)
#define MG_ARCH_X86 1
#else
#error "a processor Megler does not know"
#endif
#ifndef MG_ARCH_ARM32
#define MG_ARCH_ARM32 0
#endif
#ifndef MG_ARCH_ARM64
#define MG_ARCH_ARM64 0
#endif
#ifndef MG_ARCH_X86_64
#define MG_ARCH_X86_64 0
#endif
#ifndef MG_ARCH_X86
#define MG_ARCH_X86 0
#endif
#define MG_GUEST_LP64 (MG_ARCH_ARM64 || MG_ARCH_X86_64)
#if MG_ARCH_ARM32
#define MG_GUEST_ABI "armeabi-v7a"
#elif MG_ARCH_ARM64
#define MG_GUEST_ABI "arm64-v8a"
#elif MG_ARCH_X86_64
#define MG_GUEST_ABI "x86_64"
#else
#define MG_GUEST_ABI "x86"
#endif
/* The machine uname() says, which libcore makes os.arch of. */
#define MG_GUEST_MACHINE (MG_ARCH_ARM32 ? "armv7l" : MG_ARCH_ARM64 ? "aarch64" : MG_ARCH_X86_64 ? "x86_64" : "i686")

/* The 32-bit ARM host: the phone, and qemu-arm's Linux. What is ARM32's alone (Thumb, the kernel's helpers, the JIT) asks this. */
#define MG_HOST_ARM32 MG_ARCH_ARM32

/*
 * Whether this build can run guest code at all. On 32-bit ARM, where the core
 * bridges Android's softfp convention to the host's (docs/ARCHITECTURE.md,
 * "Two ABIs in one process"); and wherever the core's own ABI is Android's: the
 * desktop builds (MG_CORE_ELF), and Linux on x86_64, ARM64 and x86, whose ABI
 * is Android's too (the tests). Not MSVC's x64 build of the tests: Windows'
 * calling convention is not Android's there.
 */
#if MG_ARCH_ARM32 || (!defined(_MSC_VER) && !defined(_WIN32))
#define MG_GUEST_CODE 1
#else
#define MG_GUEST_CODE 0
#endif
/*
 * The native libraries an app's APKs are read and loaded for: 32-bit ARM's (armeabi-v7a, then the older armeabi, which
 * every ARMv7 runs) on the phone and in the builds that run no guest code, which look at APKs as the phone does; the
 * processor's own ABI alone elsewhere (MG_GUEST_ABI: no other processor's code is translated, docs/DESKTOP.md).
 */
#define MG_APK_ABI_ARM (MG_ARCH_ARM32 || !MG_GUEST_CODE)
/* That ABI's name, and the second one Android's Build.CPU_ABI2 names (none on the desktop's processors). */
#if MG_APK_ABI_ARM
#define MG_APK_ABI "armeabi-v7a"
#define MG_APK_ABI2 "armeabi"
#else
#define MG_APK_ABI MG_GUEST_ABI
#define MG_APK_ABI2 ""
#endif
/* Floats cross the guest boundary as their bits in integer registers (Android's softfp, 32-bit ARM only). */
#define MG_GUEST_SOFTFP MG_ARCH_ARM32

/*
 * The operating system: Windows, whether the file is compiled by MSVC for it or
 * as part of the core's ELF for it (MG_CORE_ELF, where the compiler says Linux).
 */
#if defined(_WIN32) || defined(MG_CORE_ELF)
#define MG_HOST_WINDOWS 1
#else
#define MG_HOST_WINDOWS 0
#endif

/*
 * The boundary between the core's ELF and Windows (docs/DESKTOP.md):
 *
 *   MG_HOSTAPI  on the host layer's functions (mg_host.h), which are Windows'
 *               code called by the core;
 *   MG_HOSTCB   on a function of the core's that Windows' code calls through a
 *               pointer (a thread's entry, a fault handler), and on the type of
 *               that pointer.
 *
 * Only x86_64 needs them as a calling convention: Windows passes arguments in
 * rcx, rdx, r8, r9 and keeps rsi, rdi and xmm6-15, Android in rdi, rsi, rdx,
 * rcx, r8, r9 and keeps none of those. ARM64's conventions are the same AAPCS64
 * for every call that is not variadic, and none of these is. On x86 both are
 * cdecl, but Android's code takes the stack as 16-byte aligned where Windows'
 * leaves it at 4: a call from Windows realigns it (force_align_arg_pointer).
 * Everywhere else they are nothing.
 */
#if defined(MG_CORE_ELF) && MG_ARCH_X86_64
#define MG_HOSTAPI __attribute__((ms_abi))
#define MG_HOSTCB __attribute__((ms_abi))
#elif defined(MG_CORE_ELF) && MG_ARCH_X86
#define MG_HOSTAPI
#define MG_HOSTCB __attribute__((force_align_arg_pointer))
#else
#define MG_HOSTAPI
#define MG_HOSTCB
#endif
/*
 * And EGL's and OpenGL ES's, which are ANGLE's, Windows' code:
 *
 *   MG_GLAPI    on the type of a pointer to one of ANGLE's functions, and on a stand-in for one (the tests);
 *   MG_GLCB     on a function of the core's that ANGLE calls through a GL_APIENTRY or EGLAPIENTRY pointer (a debug
 *               message callback).
 *
 * The Khronos headers give them KHRONOS_APIENTRY, which on Windows is __stdcall: on x86 the callee takes its arguments
 * off the stack, where Android's cdecl leaves them to the caller. On x86_64 that is Windows' one convention, and ARM64's
 * are the same AAPCS64 again. Guest code and the core call them through the adapters of ndk/mg_gl_adapters.c.
 */
#if defined(MG_CORE_ELF) && MG_ARCH_X86_64
#define MG_GLAPI __attribute__((ms_abi))
#define MG_GLCB __attribute__((ms_abi))
#elif defined(MG_CORE_ELF) && MG_ARCH_X86
#define MG_GLAPI __attribute__((stdcall))
#define MG_GLCB __attribute__((stdcall, force_align_arg_pointer))
#else
#define MG_GLAPI
#define MG_GLCB
#endif
/* A function of the core's that Megler.exe looks up by name in the core's ELF and calls (besides megler.h's). */
#if defined(MG_CORE_ELF)
#define MG_EXPORT MG_HOSTCB __attribute__((visibility("default")))
#else
#define MG_EXPORT
#endif

#define MG_COUNT(a) (sizeof(a) / sizeof((a)[0]))
/*
 * x rounded up or down to a multiple of a, a power of two. The mask is as wide as x and a together (0 * (x) brings x's type
 * in; x has no side effects where these are used): with a of 32 bits, as MG_PAGE is (4096u), and a 64-bit x, a mask of a's
 * width alone cleared x's upper 32 bits. On a 64-bit processor a mapping of 4 GB or more was made a size of 0 so, and the
 * pages of Chromium's 16 GB PartitionAlloc pool were none of the app's: a page madvise gave back was never committed again
 * when written (the WebView's first page, on x64).
 */
#define MG_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1 + 0 * (x)))
#define MG_ALIGN_DOWN(x, a) ((x) & ~((a) - 1 + 0 * (x)))

/* Guest addresses are host addresses: one address space, 32 bits wide on the phone. */
typedef uintptr_t mg_addr;
/* A code address's own bits: on 32-bit ARM bit 0 of a pointer to code says Thumb, and is not part of the address. */
#if MG_ARCH_ARM32
#define MG_CODE_ADDR(a) ((mg_addr)(a) & ~(mg_addr)1)
#else
#define MG_CODE_ADDR(a) ((mg_addr)(a))
#endif

/* ------------------------------------------------------------------- log */

enum { MG_LOG_DEBUG, MG_LOG_INFO, MG_LOG_WARN, MG_LOG_ERROR };

void mg_log(int level, const char *fmt, ...) MG_PRINTF(2, 3);
void mg_logv(int level, const char *fmt, va_list ap);
/*
 * Where lines go besides stderr / the debugger: the shell shows and saves them. Called past the log's lock, from any thread
 * and from several at once: a sink that writes a line out to a file there holds up no other thread's lines.
 */
typedef void (*mg_log_sink)(int level, const char *line, void *user);
void mg_log_set_sink(mg_log_sink sink, void *user);
/* The sink now (the tests', which put one of their own in its place for a while). */
void mg_log_get_sink(mg_log_sink *sink, void **user);
void mg_log_set_level(int min_level);
/* ------------------------------------------------------- a lock's holder */

/*
 * Who holds one of the layer's locks, since when and doing what, for the hang report (bionic/mg_bionic.c), which looks at
 * it from a thread of its own without taking the lock: the log's, the guest threads' and the mappings'. The holder writes
 * the record as it takes the lock and as it lets it go, and nothing else writes it (the lock keeps its writers one at a
 * time). A look gets it whole or not at all: `seq` is odd while it is written and moves on with every hold, since the
 * time is two words on a 32-bit processor and the owner, the time and what it does are one hold's.
 */
typedef struct mg_holder {
    volatile i32 seq;
    volatile u32 owner;           /* the host thread's id (mg_thread_self_id); 0: nobody */
    volatile u64 since;           /* mg_time_monotonic_ns() as it was taken */
    const char *volatile what;    /* where it was taken, or the line being written (in the holder's frame while it holds the lock) */
} mg_holder;
#define MG_HOLDER_INIT { 0, 0, 0, NULL }

void mg_holder_take(mg_holder *h, const char *what);
void mg_holder_let_go(mg_holder *h);
/* Whether the calling thread is the holder: a fault met under the lock, whose handling would take it again. */
bool mg_holder_mine(const mg_holder *h);

typedef struct mg_hold_seen {
    u32 owner;                    /* 0: nobody held it */
    u64 since, held_ns;           /* when the hold began, and how long it had lasted when it was seen */
    const char *what;
} mg_hold_seen;
/*
 * A look at a lock's holder from another thread; false (and all of `seen` 0) when nobody holds it. The clock is read after
 * the record, and the record again after the clock: `held_ns` is a time the hold had really lasted. Read the other way
 * round -- the clock first, then a hold that began a moment later -- the unsigned difference was 2^64 ns, the
 * "18446744073.7 s" of two users' hang reports (Viber on 0.1.0.669, Tuya Smart on 671). `what` is copied into `text` (of
 * `size` bytes; none when 0) while the hold is still the one seen.
 */
bool mg_holder_look(mg_holder *h, mg_hold_seen *seen, char *text, size_t size);
/* How long a hold begun at `since` had lasted at `now`: none when `now` is the earlier of the two. */
static inline u64 mg_held_ns(u64 since, u64 now) { return now > since ? now - since : 0; }

/*
 * For a watchdog (the hang report, mg_bionic.c): a look at who holds the log's lock, with the line it is writing copied into
 * `line`; false when nobody holds it. mg_log_unlocked hands a line to the host and the sink without the lock, for saying
 * that the lock itself stands still; nothing else may use it.
 */
bool mg_log_holder(mg_hold_seen *seen, char *line, size_t size);
void mg_log_unlocked(int level, const char *line);
/* The trace: a line into a ring that only a crash report prints (mg_base.c). Cheap enough for every call that matters. */
void mg_trace(const char *fmt, ...) MG_PRINTF(1, 2);
/* Prints the newest `newest` lines of it (0: all it holds) to the log. */
void mg_trace_dump(int level, int newest);
/* Once per distinct text: for things that are stubbed and would repeat every frame. */
void mg_note_once(const char *fmt, ...) MG_PRINTF(1, 2);

#define mg_dbg(...) mg_log(MG_LOG_DEBUG, __VA_ARGS__)
#define mg_info(...) mg_log(MG_LOG_INFO, __VA_ARGS__)
#define mg_warn(...) mg_log(MG_LOG_WARN, __VA_ARGS__)
#define mg_err(...) mg_log(MG_LOG_ERROR, __VA_ARGS__)

MG_NORETURN void mg_fatal(const char *fmt, ...) MG_PRINTF(1, 2);

/* ---------------------------------------------------------------- memory */

void *mg_alloc(size_t n);              /* zeroed; never NULL (fatal) */
void *mg_realloc(void *p, size_t n);
char *mg_strdup(const char *s);
char *mg_sprintf(const char *fmt, ...) MG_PRINTF(1, 2); /* malloc'd */
size_t mg_strlcpy(char *dst, const char *src, size_t cap);

/* ------------------------------------------------------- unaligned reads */

static inline u16 mg_rd16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 mg_rd32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline void mg_wr16(void *p, u16 v) { memcpy(p, &v, 2); }
static inline void mg_wr32(void *p, u32 v) { memcpy(p, &v, 4); }

/*
 * Floats cross the guest boundary as their bits on 32-bit ARM (see docs/ARCHITECTURE.md, "Floating point"). MSVC for ARM
 * makes a memcpy between an integer and a float a store to the stack and a load from it, into the other register file --
 * at every float a wrapper takes and every one it gives back (`str r0, [sp]; vldr s0, [sp]` before libm's call, `vstr s0,
 * [sp]; ldr r0, [sp]` after it, in package 668's g_roundf) -- so there the bits are moved by its intrinsics, one `vmov`
 * each. GCC makes the memcpy a `vmov` itself.
 */
#if defined(_MSC_VER) && defined(_M_ARM)
#include <intrin.h>
static inline float mg_f(u32 bits) { return _CopyFloatFromInt32((__int32)bits); }
static inline u32 mg_fb(float f) { return (u32)_CopyInt32FromFloat(f); }
static inline double mg_d(u64 bits) { return _CopyDoubleFromInt64((__int64)bits); }
static inline u64 mg_db(double d) { return (u64)_CopyInt64FromDouble(d); }
#else
static inline float mg_f(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline u32 mg_fb(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static inline double mg_d(u64 bits) { double d; memcpy(&d, &bits, 8); return d; }
static inline u64 mg_db(double d) { u64 b; memcpy(&b, &d, 8); return b; }
#endif

/*
 * A float or a double as guest code passes it to the core and takes it back, in a function the guest calls or a guest
 * function the core calls: its bits in an integer on 32-bit ARM, where Android passes floats in the integer registers
 * (softfp) and the host in VFP registers; the value itself everywhere else, where the core's convention is the guest's
 * (docs/DESKTOP.md). mg_gf / mg_gd take one as a value, mg_gfb / mg_gdb make one of a value. A wrapper written with these
 * is right for both.
 */
#if MG_GUEST_SOFTFP
typedef u32 gfloat;
typedef u64 gdouble;
static inline float mg_gf(gfloat x) { return mg_f(x); }
static inline double mg_gd(gdouble x) { return mg_d(x); }
static inline gfloat mg_gfb(float f) { return mg_fb(f); }
#define mg_gdb(d) mg_db(d)
#else
typedef float gfloat;
typedef double gdouble;
static inline float mg_gf(gfloat x) { return x; }
static inline double mg_gd(gdouble x) { return x; }
static inline gfloat mg_gfb(float f) { return f; }
#define mg_gdb(d) ((gdouble)(d))
#endif

#endif /* MG_BASE_H */
