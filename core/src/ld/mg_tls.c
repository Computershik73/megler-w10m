/*
 * Guest thread-local storage.
 *
 * Each thread that runs guest code has a block that the guest's thread
 * pointer points at: bionic's slots first (code reads a few of them by
 * number), then the static thread-local data of every loaded library, each at
 * the offset it was given when it loaded (variant 1 of the ELF TLS ABI, as on
 * ARM: data above the thread pointer).
 *
 * The block has room for MG_TLS_STATIC_MAX bytes of library data from the
 * start, so a library loaded later -- after threads exist -- still gets its
 * data in every thread without moving anything a thread may be using.
 */
#include "../host/mg_host.h"
#include "mg_ld_internal.h"

typedef struct tls_block {
    struct tls_block *next, *prev;
    void *tcb[MG_TCB_SLOTS];
    u8 data[MG_TLS_STATIC_MAX];
} tls_block;

static mg_lock g_tls_lock = MG_LOCK_INIT;
static tls_block *g_blocks;
static mg_lib *g_tls_libs[64];
static u32 g_ntls_libs;
static u32 g_static_used;

static void init_lib_data(tls_block *b, const mg_lib *lib)
{
    u8 *at = (u8 *)b->tcb + lib->tls_offset;
    if (!lib->tls_ready) return;
    memcpy(at, lib->tls_image, lib->tls_filesz);
    memset(at + lib->tls_filesz, 0, lib->tls_memsz - lib->tls_filesz);
}

bool mg_tls_register(mg_lib *lib)
{
    mg_addr align = lib->tls_align < 4 ? 4 : lib->tls_align, start;
    mg_lock_acquire(&g_tls_lock);
    /* Offsets are from the thread pointer, so that is what has to be aligned. */
    start = MG_ALIGN_UP((mg_addr)(MG_TCB_SLOTS * sizeof(void *) + g_static_used), align);
    if (start + lib->tls_memsz > MG_TCB_SLOTS * sizeof(void *) + MG_TLS_STATIC_MAX || g_ntls_libs >= MG_COUNT(g_tls_libs) || align > 64) {
        mg_lock_release(&g_tls_lock);
        lib->has_tls = false;
        return false;
    }
    lib->tls_offset = start;
    g_static_used = (u32)(start + lib->tls_memsz - MG_TCB_SLOTS * sizeof(void *));
    g_tls_libs[g_ntls_libs++] = lib;
    lib->tls_module = g_ntls_libs;
    mg_lock_release(&g_tls_lock);
    return true;
}

/*
 * The initial data is copied only once the library is relocated: .tdata holds pointers (a thread_local const char *
 * initialised to a string literal), which relocation makes addresses. Copied at registration, as it was, the threads that
 * already existed -- the main thread, always -- kept the library's link-time values: Brave's libchrome.so read its thread
 * name through one and strlen faulted at 0x00c0aa77.
 */
void mg_tls_ready(mg_lib *lib)
{
    tls_block *b;
    if (!lib->has_tls) return;
    mg_lock_acquire(&g_tls_lock);
    lib->tls_ready = true;
    for (b = g_blocks; b; b = b->next) init_lib_data(b, lib);
    mg_lock_release(&g_tls_lock);
}

/*
 * The stack guard, bionic's slot 5, which code of x86_64, x86 and ARM64 reads at every function with an array on its
 * stack, from the thread pointer (32-bit ARM's code reads the global __stack_chk_guard instead). One value for the
 * process, as bionic's, given to every thread as it first runs guest code: in its block, and in the host's own slot where
 * the patched reads find it (mg_guest_guard_set, x86/mg_patch_x86.c).
 */
static mg_addr stack_guard(void)
{
    static volatile mg_addr guard;
    if (!guard) {
        mg_addr g = 0;
        mg_random(&g, sizeof(g));
        /* (A zero byte at the bottom, as bionic's has, which ends a string overrun before it reaches the rest.) */
        guard = (g & ~(mg_addr)0xff) | 0x100;
    }
    return guard;
}

#if MG_ARCH_X86_64 && MG_GUEST_CODE
/* Whether every block gets a stack for system calls (MG_TCB_SYSCALL): since a library has one of its own. */
static bool g_syscall_stacks;

void mg_tls_syscall_stacks(void)
{
    tls_block *b;
    mg_lock_acquire(&g_tls_lock);
    if (!g_syscall_stacks) {
        g_syscall_stacks = true;
        /* (Another thread's own stack is not known here: the host's thread block says it, where the entry looks.) */
        for (b = g_blocks; b; b = b->next)
            if (!b->tcb[MG_TCB_SYSCALL]) b->tcb[MG_TCB_SYSCALL] = mg_patch_syscall_stack_new(NULL, NULL);
    }
    mg_lock_release(&g_tls_lock);
}
#endif

void **mg_tls_thread_pointer(void)
{
    void **tp = (void **)mg_guest_tp_get();
    tls_block *b;
    u32 i;
    if (tp) return tp;
    /* The thread's first step towards Android code, whoever made it: the app, a driver, the shell. */
    mg_thread_whole_stack();
    b = (tls_block *)mg_alloc(sizeof(*b));
    /* bionic's slot 0 is the thread pointer itself; the others are filled in by the pthread shim as it needs them. */
    b->tcb[0] = b->tcb;
    b->tcb[5] = (void *)stack_guard();
    mg_guest_guard_set(stack_guard());
    mg_lock_acquire(&g_tls_lock);
    for (i = 0; i < g_ntls_libs; i++) init_lib_data(b, g_tls_libs[i]);
    b->next = g_blocks;
    if (g_blocks) g_blocks->prev = b;
    g_blocks = b;
#if MG_ARCH_X86_64 && MG_GUEST_CODE
    if (g_syscall_stacks) {
        void *lo = NULL, *hi = NULL;
        mg_thread_stack_bounds(&lo, &hi);
        b->tcb[MG_TCB_SYSCALL] = mg_patch_syscall_stack_new(lo, hi);
    }
#endif
    mg_lock_release(&g_tls_lock);
    mg_guest_tp_set(b->tcb);
    return b->tcb;
}

void mg_tls_thread_exit(void)
{
    void **tp = (void **)mg_guest_tp_get();
    tls_block *b;
    if (!tp) return;
    b = (tls_block *)((u8 *)tp - offsetof(tls_block, tcb));
    mg_lock_acquire(&g_tls_lock);
    if (b->prev) b->prev->next = b->next;
    else g_blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    mg_lock_release(&g_tls_lock);
    mg_guest_tp_set(NULL);
#if MG_ARCH_X86_64 && MG_GUEST_CODE
    if (b->tcb[MG_TCB_SYSCALL]) mg_patch_syscall_stack_free(b->tcb[MG_TCB_SYSCALL]);
#endif
    free(b);
}

/* The address of a thread-local variable: module as R_ARM_TLS_DTPMOD32 gave it, offset within that module's block. */
void *mg_tls_get_addr(u32 module, mg_addr offset)
{
    void **tp = mg_tls_thread_pointer();
    if (!module || module > g_ntls_libs) return NULL;
    return (u8 *)tp + g_tls_libs[module - 1]->tls_offset + offset;
}
