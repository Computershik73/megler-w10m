/* The linker's own structures, shared with the patcher (arm/hk_patch.c). */
#ifndef HK_LD_INTERNAL_H
#define HK_LD_INTERNAL_H

#include "../elf/hk_elf.h"
#include "hk_ld.h"

struct hk_lib {
    char *name, *soname;
    u8 *map;                   /* the whole reservation: island, image, island */
    size_t map_size;
    u8 *base;                  /* load bias: where vaddr 0 is */
    u32 vmin, vmax;            /* the PT_LOADs' span, page-aligned */
    Elf32_Phdr *phdr;
    int phnum;
    const Elf32_Dyn *dyn;

    const char *strtab;
    const Elf32_Sym *symtab;
    u32 nsyms;
    const u32 *gnu_bloom, *gnu_buckets, *gnu_chain;
    u32 gnu_nbuckets, gnu_symoffset, gnu_bloom_size, gnu_bloom_shift;
    const u32 *sysv_bucket, *sysv_chain;
    u32 sysv_nbucket, sysv_nchain;

    const Elf32_Rel *rel, *jmprel;
    u32 relsz, pltrelsz;
    const u8 *android_rel;
    u32 android_relsz;
    const u32 *relr;
    u32 relrsz;

    u32 init_fn;               /* vaddr, 0 for none */
    const u32 *init_array;
    u32 init_array_count;

    char **needed_names;
    hk_lib **needed;           /* NULL for a system library */
    int nneeded;
    hk_lib **scope;            /* itself, then its DT_NEEDED closure breadth-first */
    int nscope;

    const void *exidx;
    int exidx_count;

    bool has_tls;
    u32 tls_offset;            /* of this library's block from the guest thread pointer */
    u32 tls_filesz, tls_memsz, tls_align, tls_module;
    const u8 *tls_image;

    /* Code the patcher adds, within a branch's reach of the text: [0] below the image, [1] above it. */
    u8 *island[2];
    size_t island_size[2], island_used[2], island_committed[2];

    bool relocated, sealed, inited, initing;
    bool runtime;              /* one of Android's own (hk_ld_load_runtime), not the app's: the two never see each other */
    hk_ld_stats stats;
    hk_lib *next;
};

/*
 * `size` bytes of island within `reach` bytes of `near` (0 reach: anywhere),
 * writable until the library is sealed. NULL when no island is close enough or
 * they are full.
 */
u8 *hk_island_alloc(hk_lib *lib, hk_addr near_addr, size_t size, size_t reach);

/* arm/hk_patch.c: everything done to one executable segment before it is made executable. */
void hk_patch_segment(hk_lib *lib, u8 *seg, size_t size);
/* How many thread-pointer reads a file's executable bytes hold, to size the islands before mapping. */
u32 hk_patch_count_tp_reads(const u8 *bytes, size_t size);
/* A stub for an import nothing provides: logs the name when called, returns 0. In the library's island. */
hk_addr hk_patch_unresolved_stub(hk_lib *lib, const char *name);

/* bionic/: a system symbol by name, and whether a soname is the system's. */
void *hk_bionic_find(const char *name);
bool hk_bionic_is_system_lib(const char *soname);

/* Guest thread-local storage (ld/hk_tls.c). */
#define HK_TCB_SLOTS 16
#define HK_TLS_STATIC_MAX 16384u
bool hk_tls_register(hk_lib *lib);
/* The calling thread's guest thread pointer, made on first use. */
void **hk_tls_thread_pointer(void);
void hk_tls_thread_exit(void);
/* The address of a thread-local variable: the module number R_ARM_TLS_DTPMOD32 gave, and an offset in its block. */
void *hk_tls_get_addr(u32 module, u32 offset);

#endif
