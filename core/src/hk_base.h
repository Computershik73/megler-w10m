/*
 * What every file of the core starts from: fixed-width types, the few things the
 * three compilers spell differently, and the log.
 *
 * The core is C11 that three compilers build: MSVC for ARM (the phone), GCC for
 * arm-linux-gnueabihf (the same code under qemu-arm, where the tests run guest
 * code) and MSVC for x64 (this PC, for what needs no ARM). So: no GNU
 * extensions, no arithmetic on void pointers, no variable-length arrays.
 */
#ifndef HK_BASE_H
#define HK_BASE_H

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
#define HK_THREAD_LOCAL __declspec(thread)
#define HK_NOINLINE __declspec(noinline)
#define HK_NORETURN __declspec(noreturn)
#define HK_PRINTF(fmt, args)
#else
#define HK_THREAD_LOCAL __thread
#define HK_NOINLINE __attribute__((noinline))
#define HK_NORETURN __attribute__((noreturn))
#define HK_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

/* Whether this build can run guest code at all: a 32-bit ARM host. */
#if defined(_M_ARM) || defined(__arm__)
#define HK_HOST_ARM32 1
#else
#define HK_HOST_ARM32 0
#endif

#define HK_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define HK_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define HK_ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Guest addresses are host addresses: one address space, 32 bits wide on the phone. */
typedef uintptr_t hk_addr;

/* ------------------------------------------------------------------- log */

enum { HK_LOG_DEBUG, HK_LOG_INFO, HK_LOG_WARN, HK_LOG_ERROR };

void hk_log(int level, const char *fmt, ...) HK_PRINTF(2, 3);
void hk_logv(int level, const char *fmt, va_list ap);
/* Where lines go besides stderr / the debugger: the shell shows and saves them. */
typedef void (*hk_log_sink)(int level, const char *line, void *user);
void hk_log_set_sink(hk_log_sink sink, void *user);
void hk_log_set_level(int min_level);
/*
 * For a watchdog (the hang report, hk_bionic.c): the host thread that holds the log's lock, since when (monotonic ns), and
 * the line it is writing; false when nobody holds it. hk_log_unlocked hands a line to the host and the sink without the
 * lock, for saying that the lock itself stands still; nothing else may use it.
 */
bool hk_log_holder(u32 *thread, u64 *since_ns, char *line, size_t size);
void hk_log_unlocked(int level, const char *line);
/* The trace: a line into a ring that only a crash report prints (hk_base.c). Cheap enough for every call that matters. */
void hk_trace(const char *fmt, ...) HK_PRINTF(1, 2);
/* Prints the newest `newest` lines of it (0: all it holds) to the log. */
void hk_trace_dump(int level, int newest);
/* Once per distinct text: for things that are stubbed and would repeat every frame. */
void hk_note_once(const char *fmt, ...) HK_PRINTF(1, 2);

#define hk_dbg(...) hk_log(HK_LOG_DEBUG, __VA_ARGS__)
#define hk_info(...) hk_log(HK_LOG_INFO, __VA_ARGS__)
#define hk_warn(...) hk_log(HK_LOG_WARN, __VA_ARGS__)
#define hk_err(...) hk_log(HK_LOG_ERROR, __VA_ARGS__)

HK_NORETURN void hk_fatal(const char *fmt, ...) HK_PRINTF(1, 2);

/* ---------------------------------------------------------------- memory */

void *hk_alloc(size_t n);              /* zeroed; never NULL (fatal) */
void *hk_realloc(void *p, size_t n);
char *hk_strdup(const char *s);
char *hk_sprintf(const char *fmt, ...) HK_PRINTF(1, 2); /* malloc'd */
size_t hk_strlcpy(char *dst, const char *src, size_t cap);

/* ------------------------------------------------------- unaligned reads */

static inline u16 hk_rd16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 hk_rd32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline void hk_wr16(void *p, u16 v) { memcpy(p, &v, 2); }
static inline void hk_wr32(void *p, u32 v) { memcpy(p, &v, 4); }

/* Floats cross the guest boundary as their bits (see docs/ARCHITECTURE.md, "Floating point"). */
static inline float hk_f(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline u32 hk_fb(float f) { u32 b; memcpy(&b, &f, 4); return b; }
static inline double hk_d(u64 bits) { double d; memcpy(&d, &bits, 8); return d; }
static inline u64 hk_db(double d) { u64 b; memcpy(&b, &d, 8); return b; }

#endif /* HK_BASE_H */
