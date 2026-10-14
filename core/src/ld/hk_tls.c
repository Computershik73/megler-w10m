/*
 * Guest thread-local storage.
 *
 * Each thread that runs guest code has a block that the guest's thread
 * pointer points at: bionic's slots first (code reads a few of them by
 * number), then the static thread-local data of every loaded library, each at
 * the offset it was given when it loaded (variant 1 of the ELF TLS ABI, as on
 * ARM: data above the thread pointer).
 *
 * The block has room for HK_TLS_STATIC_MAX bytes of library data from the
 * start, so a library loaded later -- after threads exist -- still gets its
 * data in every thread without moving anything a thread may be using.
 */
#include "../host/hk_host.h"
#include "hk_ld_internal.h"

typedef struct tls_block {
    struct tls_block *next, *prev;
    void *tcb[HK_TCB_SLOTS];
    u8 data[HK_TLS_STATIC_MAX];
} tls_block;

static hk_lock g_tls_lock = HK_LOCK_INIT;
static tls_block *g_blocks;
static hk_lib *g_tls_libs[64];
static u32 g_ntls_libs;
static u32 g_static_used;

static void init_lib_data(tls_block *b, const hk_lib *lib)
{
    u8 *at = (u8 *)b->tcb + lib->tls_offset;
    if (!lib->tls_ready) return;
    memcpy(at, lib->tls_image, lib->tls_filesz);
    memset(at + lib->tls_filesz, 0, lib->tls_memsz - lib->tls_filesz);
}

bool hk_tls_register(hk_lib *lib)
{
    u32 align = lib->tls_align < 4 ? 4 : lib->tls_align, start;
    hk_lock_acquire(&g_tls_lock);
    /* Offsets are from the thread pointer, so that is what has to be aligned. */
    start = HK_ALIGN_UP(HK_TCB_SLOTS * (u32)sizeof(void *) + g_static_used, align);
    if (start + lib->tls_memsz > HK_TCB_SLOTS * sizeof(void *) + HK_TLS_STATIC_MAX || g_ntls_libs >= HK_COUNT(g_tls_libs) || align > 64) {
        hk_lock_release(&g_tls_lock);
        lib->has_tls = false;
        return false;
    }
    lib->tls_offset = start;
    g_static_used = start + lib->tls_memsz - HK_TCB_SLOTS * (u32)sizeof(void *);
    g_tls_libs[g_ntls_libs++] = lib;
    lib->tls_module = g_ntls_libs;
    hk_lock_release(&g_tls_lock);
    return true;
}

/*
 * The initial data is copied only once the library is relocated: .tdata holds pointers (a thread_local const char *
 * initialised to a string literal), which relocation makes addresses. Copied at registration, as it was, the threads that
 * already existed -- the main thread, always -- kept the library's link-time values: Brave's libchrome.so read its thread
 * name through one and strlen faulted at 0x00c0aa77.
 */
void hk_tls_ready(hk_lib *lib)
{
    tls_block *b;
    if (!lib->has_tls) return;
    hk_lock_acquire(&g_tls_lock);
    lib->tls_ready = true;
    for (b = g_blocks; b; b = b->next) init_lib_data(b, lib);
    hk_lock_release(&g_tls_lock);
}

void **hk_tls_thread_pointer(void)
{
    void **tp = (void **)hk_guest_tp_get();
    tls_block *b;
    u32 i;
    if (tp) return tp;
    /* The thread's first step towards Android code, whoever made it: the app, a driver, the shell. */
    hk_thread_whole_stack();
    b = (tls_block *)hk_alloc(sizeof(*b));
    /* bionic's slot 0 is the thread pointer itself; the others are filled in by the pthread shim as it needs them. */
    b->tcb[0] = b->tcb;
    hk_lock_acquire(&g_tls_lock);
    for (i = 0; i < g_ntls_libs; i++) init_lib_data(b, g_tls_libs[i]);
    b->next = g_blocks;
    if (g_blocks) g_blocks->prev = b;
    g_blocks = b;
    hk_lock_release(&g_tls_lock);
    hk_guest_tp_set(b->tcb);
    return b->tcb;
}

void hk_tls_thread_exit(void)
{
    void **tp = (void **)hk_guest_tp_get();
    tls_block *b;
    if (!tp) return;
    b = (tls_block *)((u8 *)tp - offsetof(tls_block, tcb));
    hk_lock_acquire(&g_tls_lock);
    if (b->prev) b->prev->next = b->next;
    else g_blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    hk_lock_release(&g_tls_lock);
    hk_guest_tp_set(NULL);
    free(b);
}

/* The address of a thread-local variable: module as R_ARM_TLS_DTPMOD32 gave it, offset within that module's block. */
void *hk_tls_get_addr(u32 module, u32 offset)
{
    void **tp = hk_tls_thread_pointer();
    if (!module || module > g_ntls_libs) return NULL;
    return (u8 *)tp + g_tls_libs[module - 1]->tls_offset + offset;
}
