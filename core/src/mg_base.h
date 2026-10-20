/*
 * What every file of the core starts from: fixed-width types, the few things the
 * three compilers spell differently, and the log.
 *
 * The core is C11 that three compilers build: MSVC for ARM (the phone), GCC for
 * arm-linux-gnueabihf (the same code under qemu-arm, where the tests run guest
 * code) and MSVC for x64 (this PC, for what needs no ARM). So: no GNU
 * extensions, no arithmetic on void pointers, no variable-length arrays.
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

/* Whether this build can run guest code at all: a 32-bit ARM host. */
#if defined(_M_ARM) || defined(__arm__)
#define MG_HOST_ARM32 1
#else
#define MG_HOST_ARM32 0
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
