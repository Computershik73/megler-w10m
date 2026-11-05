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
/* A function of the core's that Megler.exe looks up by name in the core's ELF and calls (besides megler.h's). */
#if defined(MG_CORE_ELF)
#define MG_EXPORT MG_HOSTCB __attribute__((visibility("default")))
#else
#define MG_EXPORT
#endif

#define MG_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MG_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define MG_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Guest addresses are host addresses: one address space, 32 bits wide on the phone. */
typedef uintptr_t mg_addr;

/* ------------------------------------------------------------------- log */

enum { MG_LOG_DEBUG, MG_LOG_INFO, MG_LOG_WARN, MG_LOG_ERROR };

void mg_log(int level, const char *fmt, ...) MG_PRINTF(2, 3);
void mg_logv(int level, const char *fmt, va_list ap);
/* Where lines go besides stderr / the debugger: the shell shows and saves them. */
typedef void (*mg_log_sink)(int level, const char *line, void *user);
void mg_log_set_sink(mg_log_sink sink, void *user);
void mg_log_set_level(int min_level);
/*
 * For a watchdog (the hang report, mg_bionic.c): the host thread that holds the log's lock, since when (monotonic ns), and
 * the line it is writing; false when nobody holds it. mg_log_unlocked hands a line to the host and the sink without the
 * lock, for saying that the lock itself stands still; nothing else may use it.
 */
bool mg_log_holder(u32 *thread, u64 *since_ns, char *line, size_t size);
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

/* Floats cross the guest boundary as their bits (see docs/ARCHITECTURE.md, "Floating point"). */
static inline float mg_f(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline u32 mg_fb(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static inline double mg_d(u64 bits) { double d; memcpy(&d, &bits, 8); return d; }
static inline u64 mg_db(double d) { u64 b; memcpy(&b, &d, 8); return b; }

#endif /* MG_BASE_H */
