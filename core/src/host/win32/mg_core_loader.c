/*
 * Loads the core's ELF into Megler.exe on desktop Windows (mg_core_loader.h says what and why; docs/DESKTOP.md, "The
 * core as an ELF").
 *
 * The core is linked by tools/build-desktop.sh so that there is little for this to do, and it refuses what it was not
 * built to do rather than half doing it: one shared object of the host's own processor, its loadable segments each on
 * pages of their own (-z separate-loadable-segments), plain relocation tables (no packing, no RELR), no TLS segment (the
 * core's thread-local variables are emulated: __emutls_get_address is mg_crt_bridge.c's), no IFUNCs, no text relocations.
 * Its segments are committed read-write, filled, relocated, and then given their own protection, which is the order an
 * app container allows (VirtualProtectFromApp never makes a page writable and executable at once).
 */
#if defined(MG_ELF_HOST)

#include "mg_core_loader.h"

#include <stdio.h>

#include "../mg_host.h"
#include "../../elf/mg_elf.h"

#if MG_GUEST_LP64
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Phdr Phdr;
typedef Elf64_Dyn Dyn;
typedef Elf64_Sym Sym;
typedef Elf64_Rela Rel;
#define R_SYM(i) ELF64_R_SYM(i)
#define R_TYPE(i) ELF64_R_TYPE(i)
#define ELF_CLASS ELFCLASS64
#define DT_TABLE DT_RELA
#define DT_TABLESZ DT_RELASZ
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Phdr Phdr;
typedef Elf32_Dyn Dyn;
typedef Elf32_Sym Sym;
typedef Elf32_Rel Rel;
#define R_SYM(i) ELF_R_SYM(i)
#define R_TYPE(i) ELF_R_TYPE(i)
#define ELF_CLASS ELFCLASS32
#define DT_TABLE DT_REL
#define DT_TABLESZ DT_RELSZ
#endif

#if MG_ARCH_X86_64
#define MACHINE EM_X86_64
#elif MG_ARCH_ARM64
#define MACHINE EM_AARCH64
#elif MG_ARCH_X86
#define MACHINE EM_386
#else
#define MACHINE EM_ARM
#endif

/* How Windows' code calls the core's initialisers: as Android code calls a function of no arguments. */
#if MG_ARCH_X86_64
typedef void (__attribute__((sysv_abi)) *init_fn)(void);
#else
typedef void (*init_fn)(void);
#endif

static struct {
    u8 *base;                          /* the reservation: the image's lowest address, less its first segment's vaddr */
    size_t size;
    const Sym *symtab;
    const char *strtab;
    u32 nsyms;
} g_core;

static void fail(char *err, size_t errlen, const char *fmt, const char *a, const char *b)
{
    if (err && errlen) snprintf(err, errlen, fmt, a ? a : "", b ? b : "");
}

static void *find_import(const char *name)
{
    const mg_core_import *tables[2];
    int t;
    tables[0] = mg_crt_bridge_imports;
    tables[1] = mg_core_host_imports;
    for (t = 0; t < 2; t++) {
        const mg_core_import *i;
        for (i = tables[t]; i->name; i++)
            if (!strcmp(i->name, name)) return i->addr;
    }
    return NULL;
}

void *mg_core_sym(const char *name)
{
    u32 i;
    if (!g_core.symtab) return NULL;
    for (i = 1; i < g_core.nsyms; i++) {
        const Sym *s = &g_core.symtab[i];
        if (s->st_shndx != SHN_UNDEF && ELF_ST_BIND(s->st_info) != STB_LOCAL && !strcmp(g_core.strtab + s->st_name, name))
            return g_core.base + s->st_value;
    }
    return NULL;
}

bool mg_core_image(mg_addr *base, size_t *size)
{
    if (!g_core.base) return false;
    *base = (mg_addr)g_core.base;
    *size = g_core.size;
    return true;
}

/* One relocation: false, with the name said, when its symbol is nowhere. */
static bool relocate_one(const Rel *r, char *missing, size_t missing_len, int *nmissing)
{
    u32 type = (u32)R_TYPE(r->r_info), sym = (u32)R_SYM(r->r_info);
    mg_addr *place = (mg_addr *)(g_core.base + r->r_offset);
    mg_addr s = 0;
#if MG_GUEST_LP64
    mg_addr addend = (mg_addr)r->r_addend;
#else
    mg_addr addend = *place;       /* REL: the addend is what the place holds */
#endif
    if (sym) {
        const Sym *e = &g_core.symtab[sym];
        if (e->st_shndx != SHN_UNDEF) s = (mg_addr)(g_core.base + e->st_value);
        else {
            const char *name = g_core.strtab + e->st_name;
            s = (mg_addr)find_import(name);
            if (!s && ELF_ST_BIND(e->st_info) != STB_WEAK) {
                size_t k = strlen(missing);
                if (k + strlen(name) + 3 < missing_len) snprintf(missing + k, missing_len - k, "%s%s", k ? ", " : "", name);
                (*nmissing)++;
                return false;
            }
        }
    }
    switch (type) {
#if MG_ARCH_X86_64
    case R_X86_64_NONE: break;
    case R_X86_64_RELATIVE: *place = (mg_addr)g_core.base + addend; break;
    case R_X86_64_64: *place = s + addend; break;
    case R_X86_64_GLOB_DAT: case R_X86_64_JUMP_SLOT: *place = s; break;
#elif MG_ARCH_ARM64
    case R_AARCH64_NONE: break;
    case R_AARCH64_RELATIVE: *place = (mg_addr)g_core.base + addend; break;
    case R_AARCH64_ABS64: case R_AARCH64_GLOB_DAT: case R_AARCH64_JUMP_SLOT: *place = s + addend; break;
#elif MG_ARCH_X86
    case R_386_NONE: break;
    case R_386_RELATIVE: *place = (mg_addr)g_core.base + addend; break;
    case R_386_32: *place = s + addend; break;
    case R_386_GLOB_DAT: case R_386_JMP_SLOT: *place = s; break;
#endif
    default: {
        char t[16];
        snprintf(t, sizeof(t), "%u", (unsigned)type);
        if (strlen(missing) + 40 < missing_len) {
            size_t k = strlen(missing);
            snprintf(missing + k, missing_len - k, "%sa relocation of type %s", k ? ", " : "", t);
        }
        (*nmissing)++;
        return false;
    }
    }
    return true;
}

static int prot_of(u32 flags)
{
    if (flags & PF_X) return MG_PROT_RX;
    if (flags & PF_W) return MG_PROT_RW;
    return MG_PROT_R;
}

/*
 * The core's file, whole, or NULL with what stood in the way in `why`. (The first start of a package registered from its
 * folder a moment before -- developer mode, tools/build-appx.sh's stage folder registered again over the last build -- is
 * refused it, "access denied", for as long as that process lives; the next start reads it.)
 */
static u8 *read_core(const char *path, size_t *size, int *why)
{
    mg_file *f = mg_file_open(path, MG_O_READ, why);
    if (!f) return NULL;
    mg_file_close(f);
    return mg_file_read_all(path, size);
}

bool mg_core_load(const char *path, char *err, size_t errlen)
{
    size_t size = 0;
    int why = 0;
    u8 *file = read_core(path, &size, &why);
    const Ehdr *eh = (const Ehdr *)file;
    const Phdr *ph;
    const Dyn *dyn = NULL;
    mg_addr lo = (mg_addr)-1, hi = 0;
    const u8 *rel = NULL, *jmprel = NULL;
    size_t relsz = 0, jmprelsz = 0, init_array_sz = 0;
    mg_addr init_array = 0, init = 0;
    const u32 *hash = NULL;
    char missing[1024];
    int nmissing = 0, i;
    if (g_core.base) return true;
    mg_crt_bridge_init();
    if (!file) {
        fail(err, errlen, "the core (%s) could not be read%s", path,
             why == MG_E_NOENT || why == MG_E_NOTDIR ? ": there is no such file" : why == MG_E_ACCESS ? ": access denied"
             : why == MG_E_BUSY ? ": another process holds it" : NULL);
        return false;
    }
    if (size < sizeof(Ehdr) || memcmp(eh->e_ident, "\177ELF", 4) || eh->e_ident[4] != ELF_CLASS || eh->e_type != ET_DYN
        || eh->e_machine != MACHINE || eh->e_phoff + (size_t)eh->e_phnum * sizeof(Phdr) > size) {
        fail(err, errlen, "%s is not the core of this processor (an ELF shared object for %s)", path, MG_GUEST_ABI);
        free(file);
        return false;
    }
    ph = (const Phdr *)(file + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_TLS) {
            fail(err, errlen, "%s has thread-local data of its own, which this loader does not lay out (build it with -femulated-tls)%s", path, NULL);
            free(file);
            return false;
        }
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = (mg_addr)ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = (mg_addr)(ph[i].p_vaddr + ph[i].p_memsz);
    }
    lo = MG_ALIGN_DOWN(lo, (mg_addr)MG_PAGE);
    hi = MG_ALIGN_UP(hi, (mg_addr)MG_PAGE);
    g_core.size = MG_ALIGN_UP((size_t)(hi - lo), (size_t)65536);
    g_core.base = (u8 *)mg_mem_reserve(g_core.size);
    if (!g_core.base || !mg_mem_commit(g_core.base, (size_t)(hi - lo), MG_PROT_RW)) {
        fail(err, errlen, "no memory for the core (%s)%s", path, NULL);
        free(file);
        if (g_core.base) mg_mem_release(g_core.base, g_core.size);
        memset(&g_core, 0, sizeof(g_core));
        return false;
    }
    g_core.base -= lo;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const Dyn *)(g_core.base + ph[i].p_vaddr);
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) {
            fail(err, errlen, "%s is cut short%s", path, NULL);
            free(file);
            return false;
        }
        memcpy(g_core.base + ph[i].p_vaddr, file + ph[i].p_offset, (size_t)ph[i].p_filesz);
    }
    if (!dyn) {
        fail(err, errlen, "%s has no dynamic section%s", path, NULL);
        free(file);
        return false;
    }
    for (; dyn->d_tag != DT_NULL; dyn++) {
        mg_addr v = (mg_addr)dyn->d_val;
        switch (dyn->d_tag) {
        case DT_SYMTAB: g_core.symtab = (const Sym *)(g_core.base + v); break;
        case DT_STRTAB: g_core.strtab = (const char *)(g_core.base + v); break;
        case DT_HASH: hash = (const u32 *)(g_core.base + v); break;
        case DT_TABLE: rel = g_core.base + v; break;
        case DT_TABLESZ: relsz = (size_t)v; break;
        case DT_JMPREL: jmprel = g_core.base + v; break;
        case DT_PLTRELSZ: jmprelsz = (size_t)v; break;
        case DT_INIT: init = v; break;
        case DT_INIT_ARRAY: init_array = v; break;
        case DT_INIT_ARRAYSZ: init_array_sz = (size_t)v; break;
        case DT_RELR: case DT_ANDROID_REL: case DT_ANDROID_RELA: case DT_ANDROID_RELR:
            fail(err, errlen, "%s has packed relocations, which this loader does not read (link with --pack-dyn-relocs=none)%s", path, NULL);
            free(file);
            return false;
        default: break;
        }
    }
    if (!g_core.symtab || !g_core.strtab || !hash) {
        fail(err, errlen, "%s has no symbol table, or no DT_HASH (link with --hash-style=both)%s", path, NULL);
        free(file);
        return false;
    }
    g_core.nsyms = hash[1];
    missing[0] = 0;
    {
        size_t k;
        for (k = 0; k + sizeof(Rel) <= relsz; k += sizeof(Rel)) relocate_one((const Rel *)(rel + k), missing, sizeof(missing), &nmissing);
        for (k = 0; k + sizeof(Rel) <= jmprelsz; k += sizeof(Rel)) relocate_one((const Rel *)(jmprel + k), missing, sizeof(missing), &nmissing);
    }
    if (nmissing) {
        char what[32];
        snprintf(what, sizeof(what), "%d", nmissing);
        fail(err, errlen, "the core imports what Megler.exe does not have (%s): %s", what, missing);
        free(file);
        return false;
    }
    /* Each segment its own protection now (and the part that was relocated read-only, RELRO, read-only). */
    for (i = 0; i < eh->e_phnum; i++) {
        mg_addr a, b;
        if (ph[i].p_type != PT_LOAD && ph[i].p_type != PT_GNU_RELRO) continue;
        a = MG_ALIGN_DOWN((mg_addr)ph[i].p_vaddr, (mg_addr)MG_PAGE);
        b = MG_ALIGN_UP((mg_addr)(ph[i].p_vaddr + ph[i].p_memsz), (mg_addr)MG_PAGE);
        if (ph[i].p_type == PT_GNU_RELRO) {
            /* (Only its whole pages: the rest shares a page with writable data.) */
            b = MG_ALIGN_DOWN((mg_addr)(ph[i].p_vaddr + ph[i].p_memsz), (mg_addr)MG_PAGE);
            if (b > a) mg_mem_protect(g_core.base + a, (size_t)(b - a), MG_PROT_R);
            continue;
        }
        if (!mg_mem_protect(g_core.base + a, (size_t)(b - a), prot_of(ph[i].p_flags))) {
            fail(err, errlen, "the core's code could not be made executable (%s): is the codeGeneration capability granted?%s", path, NULL);
            free(file);
            return false;
        }
        if (ph[i].p_flags & PF_X) mg_icache_flush(g_core.base + a, (size_t)(b - a));
    }
    free(file);
    /* Every stub's slot, before anything of the core runs: an initialiser of its may log, and the log is the core's. */
    {
        const mg_core_export *x;
        missing[0] = 0;
        for (x = mg_core_exports; x->name; x++) {
            *x->slot = mg_core_sym(x->name);
            if (!*x->slot) {
                size_t k = strlen(missing);
                if (k + strlen(x->name) + 3 < sizeof(missing)) snprintf(missing + k, sizeof(missing) - k, "%s%s", k ? ", " : "", x->name);
                nmissing++;
            }
        }
        if (nmissing) {
            fail(err, errlen, "the core does not export what Megler.exe calls: %s%s", missing, NULL);
            return false;
        }
    }
    if (init) ((init_fn)(g_core.base + init))();
    if (init_array) {
        const mg_addr *f = (const mg_addr *)(g_core.base + init_array);
        size_t n = init_array_sz / sizeof(mg_addr), k;
        for (k = 0; k < n; k++)
            if (f[k] && f[k] != (mg_addr)-1) ((init_fn)f[k])();
    }
    mg_host_base_use_core_log();
    return true;
}

#endif /* MG_ELF_HOST */
