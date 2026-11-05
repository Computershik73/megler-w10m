/*
 * bionic's types as the guest test libraries use them, for the ABI each is built for (tools/build-guest.sh):
 * armeabi-v7a, arm64-v8a, x86_64 or x86. No NDK header is used, so what differs between the four -- the width of
 * size_t and long, the sizes of pthread's objects and of jmp_buf -- is spelled out here as bionic's headers have it.
 * On armeabi-v7a each is the type the libraries declared before they were built for other ABIs, so that what they
 * build to there stays byte for byte what it was.
 */
#ifndef T_ABI_H
#define T_ABI_H

typedef __SIZE_TYPE__ size_t;          /* unsigned int on the 32-bit ABIs, unsigned long on the 64-bit ones */
typedef __PTRDIFF_TYPE__ ssize_t;      /* int, long: bionic's ssize_t */
typedef long pthread_t;
typedef int pthread_key_t;
typedef int pthread_once_t;

#if defined(__LP64__)
/* bionic's LP64 sizes, 40 and 48 bytes (the core reads the first word of each); their initialisers as its macros give them. */
typedef struct { int private_[10]; } pthread_mutex_t;
typedef struct { int private_[12]; } pthread_cond_t;
#define T_MUTEX_INITIALIZER {{0}}
#define T_RECURSIVE_MUTEX_INITIALIZER {{0x4000}}
typedef struct { unsigned flags; void *stack_base; size_t stack_size, guard_size; int sched_policy, sched_priority; char reserved[16]; } pthread_attr_t;
#else
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
#define T_MUTEX_INITIALIZER 0
#define T_RECURSIVE_MUTEX_INITIALIZER 0x4000
typedef struct { int flags; void *stack_base; size_t stack_size, guard_size; int sched_policy, sched_priority; } pthread_attr_t;
#endif

/* bionic's _JBLEN of each processor: 256, 256, 88 and 40 bytes. */
#if defined(__arm__)
typedef long jmp_buf[64];
#elif defined(__aarch64__)
typedef long jmp_buf[32];
#elif defined(__x86_64__)
typedef long jmp_buf[11];
#else
typedef long jmp_buf[10];
#endif

/* The kernel's and bionic's `unsigned long` where the 32-bit ABIs' libraries said `unsigned`: a sigset_t, an auxv entry. */
#if defined(__LP64__)
typedef unsigned long t_word;
#else
typedef unsigned t_word;
#endif

#endif /* T_ABI_H */
