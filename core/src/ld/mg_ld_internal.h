/* The linker's own structures, shared with the patcher (arm/mg_patch.c). */
#ifndef MG_LD_INTERNAL_H
#define MG_LD_INTERNAL_H

#include "../elf/mg_elf.h"
#include "mg_ld.h"

typedef struct mg_svc_site {
    mg_addr pc;               /* bit 0: Thumb */
    struct mg_svc_site *next;
} mg_svc_site;

struct mg_lib {
    char *name, *soname;
    u8 *map;                   /* the whole reservation: island, image, island; or, placed, the image and what follows it */
    size_t map_size;
    /*
     * Loaded where the app said (android_dlopen_ext, ANDROID_DLEXT_RESERVED_ADDRESS): the image is in the app's own
     * reservation, which is the app's to keep and give back, and the island below it, if any, is a reservation of its own.
     */
    bool placed;
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
    mg_lib **needed;           /* NULL for a system library */
    int nneeded;
    mg_lib **scope;            /* itself, then its DT_NEEDED closure breadth-first */
    int nscope;

    const void *exidx;
    int exidx_count;

    bool has_tls;
    bool tls_ready;            /* its initial data relocated: threads' blocks are given it (mg_tls_ready) */
    u32 tls_offset;            /* of this library's block from the guest thread pointer */
    u32 tls_filesz, tls_memsz, tls_align, tls_module;
    const u8 *tls_image;

    /* Code the patcher adds, within a branch's reach of the text: [0] below the image, [1] above it. */
    u8 *island[2];
    size_t island_size[2], island_used[2], island_committed[2];

    bool relocated, sealed, inited, initing;
    int service_scope;         /* service loader namespace; zero for the client and boot */
    bool runtime;              /* one of Android's own (mg_ld_load_runtime), not the app's: the two never see each other */
    mg_ld_stats stats;
    mg_svc_site *svc_sites;    /* immutable once executable; only our registered traps may resume */
    mg_lib *next;
};

/*
 * `size` bytes of island within `reach` bytes of `near` (0 reach: anywhere),
 * writable until the library is sealed. NULL when no island is close enough or
 * they are full.
 */
u8 *mg_island_alloc(mg_lib *lib, mg_addr near_addr, size_t size, size_t reach);

/* arm/mg_patch.c: everything done to one executable segment before it is made executable. */
void mg_patch_segment(mg_lib *lib, u8 *seg, size_t size);
/* How many thread-pointer reads a file's executable bytes hold, to size the islands before mapping. */
u32 mg_patch_count_tp_reads(const u8 *bytes, size_t size);
/* A stub for an import nothing provides: logs the name when called, returns 0. In the library's island. */
mg_addr mg_patch_unresolved_stub(mg_lib *lib, const char *name);

/* bionic/: a system symbol by name, and whether a soname is the system's. */
void *mg_bionic_find(const char *name);
char **mg_bionic_initial_argv(void);   /* bionic/mg_libc_misc.c: argv, envp and the auxiliary vector, as init functions get them */
bool mg_bionic_is_system_lib(const char *soname);

/* Guest thread-local storage (ld/mg_tls.c). */
#define MG_TCB_SLOTS 16
#define MG_TLS_STATIC_MAX 16384u
/* A library's block given its place and module number, before relocation, which needs both. */
bool mg_tls_register(mg_lib *lib);
/* Its initial data, now relocated, copied into every thread's block, and into every block made from now on. */
void mg_tls_ready(mg_lib *lib);
/* The calling thread's guest thread pointer, made on first use. */
void **mg_tls_thread_pointer(void);
void mg_tls_thread_exit(void);
/* The address of a thread-local variable: the module number R_ARM_TLS_DTPMOD32 gave, and an offset in its block. */
void *mg_tls_get_addr(u32 module, u32 offset);

#endif
