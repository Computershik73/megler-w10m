/* The linker's own structures, shared with the patcher (arm/mg_patch.c). */
#ifndef MG_LD_INTERNAL_H
#define MG_LD_INTERNAL_H

#include "../elf/mg_elf.h"
#include "mg_ld.h"

typedef struct mg_svc_site {
    mg_addr pc;               /* bit 0: Thumb */
    struct mg_svc_site *next;
    /* Left as the file has it, a hardware breakpoint on it (mg_patch.c, "left as the file has it"): an svc, or a
       thread-pointer read into tp_reg (-1 for an svc, patched or not). */
    bool watched;
    i8 tp_reg;
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
    mg_addr vmin, vmax;        /* the PT_LOADs' span, page-aligned */
    ElfW(Phdr) *phdr;
    int phnum;
    const ElfW(Dyn) *dyn;

    const char *strtab;
    const ElfW(Sym) *symtab;
    u32 nsyms;
    /* The GNU hash table's bloom filter is of words of the class's width; its buckets and chains are 32-bit words in both. */
    const mg_addr *gnu_bloom;
    const u32 *gnu_buckets, *gnu_chain;
    u32 gnu_nbuckets, gnu_symoffset, gnu_bloom_size, gnu_bloom_shift;
    const u32 *sysv_bucket, *sysv_chain;
    u32 sysv_nbucket, sysv_nchain;

    const mg_reloc *rel, *jmprel;     /* REL on the 32-bit ABIs, RELA on the 64-bit ones */
    size_t relsz, pltrelsz;
    const u8 *android_rel;
    size_t android_relsz;
    const mg_addr *relr;
    size_t relrsz;

    mg_addr init_fn;           /* vaddr, 0 for none */
    const mg_addr *init_array;
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
    mg_addr tls_offset;        /* of this library's block from the guest thread pointer */
    mg_addr tls_filesz, tls_memsz, tls_align;
    u32 tls_module;
    const u8 *tls_image;

    /* Code the patcher adds, within a branch's reach of the text: [0] below the image, [1] above it. */
    u8 *island[2];
    size_t island_size[2], island_used[2], island_committed[2];

    bool relocated, sealed, inited, initing;
    int service_scope;         /* service loader namespace; zero for the client and boot */
    bool runtime;              /* one of Android's own (mg_ld_load_runtime), not the app's: the two never see each other */
    bool go;                   /* one of Go's: its build ID note (mg_ld_lib_is_go) */
    mg_ld_stats stats;
    mg_svc_site *svc_sites;    /* immutable once executable; only our registered traps may resume */
    /*
     * Its executable sections (.text, .plt...), [start, end) by address, from the file's section headers when they agree
     * with its segments: where the patcher looks for instructions. Read-only data shares the executable segment (gold's
     * layout, lld's with --no-rosegment), and a table in libmetasec_ov.so's .rodata holds the word of an ARM svc 0. None
     * known (ncode 0): the executable segments, whole.
     */
    mg_addr code[8][2];
    int ncode;
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
/*
 * x86_64 and 32-bit ARM: the last of the block's slots, which no bionic has, holds the thread's stack for the system calls
 * of libraries' own led out to the core (x86/mg_patch_x86.c; arm/mg_patch.c, a library of Go's), once a library has one:
 * every thread's block gets it then (mg_tls_syscall_stacks), the block of a thread made later as it is made.
 * mg_patch_syscall_stack_new makes one (the thread's own stack's bounds where the host's thread block does not say them,
 * NULL where they are not known), _free gives it back.
 */
#define MG_TCB_SYSCALL 15
void mg_tls_syscall_stacks(void);
void *mg_patch_syscall_stack_new(void *own_lo, void *own_hi);
void mg_patch_syscall_stack_free(void *stack);
#define MG_TLS_STATIC_MAX 16384u
/* A library's block given its place and module number, before relocation, which needs both. */
bool mg_tls_register(mg_lib *lib);
/* Its initial data, now relocated, copied into every thread's block, and into every block made from now on. */
void mg_tls_ready(mg_lib *lib);
/* The calling thread's guest thread pointer, made on first use. */
void **mg_tls_thread_pointer(void);
void mg_tls_thread_exit(void);
/* The address of a thread-local variable: the module number R_ARM_TLS_DTPMOD32 gave, and an offset in its block. */
void *mg_tls_get_addr(u32 module, mg_addr offset);

#endif
