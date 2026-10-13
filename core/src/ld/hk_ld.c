#include "hk_ld_internal.h"

#include <stdio.h>

#include "../host/hk_host.h"
#include "../ndk/hk_egl.h"
#include "../zip/hk_zip.h"

/* ------------------------------------------------------------------ state */

static hk_lock g_lock = HK_LOCK_INIT;      /* the list of libraries; loading itself is serialised by g_load_lock */
static hk_lock g_load_lock = HK_LOCK_INIT;
static u32 g_load_owner;                   /* dlopen from a constructor re-enters on the same thread */
static int g_load_depth;
static hk_lib *g_libs, *g_libs_tail;
static hk_zip g_apks[HK_LD_MAX_APKS];
static int g_napks;
static char *g_dirs[8];
static int g_ndirs;
static char *g_runtime_dirs[4];            /* where the runtime's own libraries are: never searched for an app's */
static int g_nruntime_dirs;
static const char *g_abi;
static int g_verbosity = 1;
static size_t g_unresolved;

typedef struct interposed {
    const char *name;
    void *fn;
} interposed;
static interposed g_interposed[64];
static int g_ninterposed;

static void load_lock(void)
{
    u32 me = hk_thread_self_id();
    if (g_load_owner == me) {
        g_load_depth++;
        return;
    }
    hk_lock_acquire(&g_load_lock);
    g_load_owner = me;
    g_load_depth = 1;
}

static void note_code_range(const u8 *map, size_t size);

static void load_unlock(void)
{
    if (--g_load_depth == 0) {
        g_load_owner = 0;
        hk_lock_release(&g_load_lock);
    }
}

void hk_ld_set_verbosity(int v) { g_verbosity = v; }
size_t hk_ld_unresolved_count(void) { return g_unresolved; }

void hk_ld_interpose(const char *name, void *fn)
{
    if (g_ninterposed < (int)HK_COUNT(g_interposed)) {
        g_interposed[g_ninterposed].name = hk_strdup(name);
        g_interposed[g_ninterposed].fn = fn;
        g_ninterposed++;
    }
}

/* ---------------------------------------------------------------- sources */

bool hk_ld_add_apk(const char *path)
{
    char err[128];
    if (g_napks >= HK_LD_MAX_APKS) return false;
    if (!hk_zip_open(&g_apks[g_napks], path, err, sizeof(err))) {
        hk_err("ld: %s: %s", path, err);
        return false;
    }
    g_napks++;
    g_abi = NULL;
    return true;
}

const struct hk_zip *hk_ld_apk_at(int index) { return index >= 0 && index < g_napks ? &g_apks[index] : NULL; }

void hk_ld_add_search_dir(const char *dir)
{
    if (g_ndirs < (int)HK_COUNT(g_dirs)) g_dirs[g_ndirs++] = hk_strdup(dir);
}

void hk_ld_add_runtime_dir(const char *dir)
{
    int i;
    for (i = 0; i < g_nruntime_dirs; i++)
        if (!strcmp(g_runtime_dirs[i], dir)) return;
    if (g_nruntime_dirs < (int)HK_COUNT(g_runtime_dirs)) g_runtime_dirs[g_nruntime_dirs++] = hk_strdup(dir);
}

static void count_one(const hk_zip_entry *e, void *user)
{
    size_t n = strlen(e->name);
    if (n > 3 && !strcmp(e->name + n - 3, ".so")) ++*(int *)user;
}

/* armeabi-v7a when any APK has it; the older armeabi (ARMv5, which every ARMv7 runs) otherwise. */
const char *hk_ld_abi(void)
{
    static const char *const abis[] = { "armeabi-v7a", "armeabi" };
    int a, i;
    if (g_abi) return g_abi;
    for (a = 0; a < 2; a++) {
        char prefix[40];
        int n = 0;
        snprintf(prefix, sizeof(prefix), "lib/%s/", abis[a]);
        for (i = 0; i < g_napks; i++) hk_zip_each(&g_apks[i], prefix, count_one, &n);
        if (n) {
            g_abi = abis[a];
            return g_abi;
        }
    }
    return NULL;
}

static const hk_zip_entry *apk_lib(const char *name, const hk_zip **in)
{
    const char *abi = hk_ld_abi();
    char path[300];
    int i;
    if (!abi) return NULL;
    snprintf(path, sizeof(path), "lib/%s/%s", abi, name);
    for (i = 0; i < g_napks; i++) {
        const hk_zip_entry *e = hk_zip_find(&g_apks[i], path);
        if (e) {
            *in = &g_apks[i];
            return e;
        }
    }
    return NULL;
}

bool hk_ld_has_lib(const char *name)
{
    const hk_zip *z;
    int i;
    if (apk_lib(name, &z)) return true;
    for (i = 0; i < g_ndirs; i++) {
        char *p = hk_sprintf("%s/%s", g_dirs[i], name);
        hk_stat st;
        bool ok = hk_path_stat(p, &st, NULL) && !st.is_dir;
        free(p);
        if (ok) return true;
    }
    return false;
}

typedef struct libs_walk {
    void (*cb)(const char *name, u64 size, void *user);
    void *user;
    int n;
    size_t skip;
} libs_walk;

static void libs_one(const hk_zip_entry *e, void *user)
{
    libs_walk *w = (libs_walk *)user;
    size_t n = strlen(e->name);
    if (n <= w->skip + 3 || strcmp(e->name + n - 3, ".so") || strchr(e->name + w->skip, '/')) return;
    w->n++;
    if (w->cb) w->cb(e->name + w->skip, e->usize, w->user);
}

int hk_ld_apk_libs(void (*cb)(const char *name, u64 size, void *user), void *user)
{
    const char *abi = hk_ld_abi();
    char prefix[40];
    libs_walk w;
    int i;
    if (!abi) return 0;
    snprintf(prefix, sizeof(prefix), "lib/%s/", abi);
    w.cb = cb;
    w.user = user;
    w.n = 0;
    w.skip = strlen(prefix);
    for (i = 0; i < g_napks; i++) hk_zip_each(&g_apks[i], prefix, libs_one, &w);
    return w.n;
}

/*
 * A library's file: `*owned` says whether the bytes are ours to free. The
 * app's are in the APKs and then the folders named for them; the runtime's
 * only in the folders named for the runtime.
 */
static const u8 *read_lib_file(const char *name, bool runtime, size_t *size, bool *owned)
{
    const hk_zip *z = NULL;
    const hk_zip_entry *e = runtime ? NULL : apk_lib(name, &z);
    char **dirs = runtime ? g_runtime_dirs : g_dirs;
    int ndirs = runtime ? g_nruntime_dirs : g_ndirs, i;
    if (e) {
        *owned = true;
        return hk_zip_read(z, e, 0x40000000u, size);
    }
    for (i = 0; i < ndirs; i++) {
        char *p = hk_sprintf("%s/%s", dirs[i], name);
        u8 *b = hk_file_read_all(p, size);
        free(p);
        if (b) {
            *owned = true;
            return b;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- islands */

u8 *hk_island_alloc(hk_lib *lib, hk_addr near_addr, size_t size, size_t reach)
{
    int k;
    size = HK_ALIGN_UP(size, 4);
    if (lib->sealed) return NULL;
    /* The upper island first: it is the one every library has. */
    for (k = 1; k >= 0; k--) {
        u8 *at;
        size_t need;
        if (!lib->island[k] || lib->island_used[k] + size > lib->island_size[k]) continue;
        at = lib->island[k] + lib->island_used[k];
        if (reach) {
            hk_addr lo = (hk_addr)at, hi = (hk_addr)at + size;
            hk_addr d1 = lo > near_addr ? lo - near_addr : near_addr - lo;
            hk_addr d2 = hi > near_addr ? hi - near_addr : near_addr - hi;
            if (d1 >= reach || d2 >= reach) continue;
        }
        need = HK_ALIGN_UP(lib->island_used[k] + size, HK_PAGE);
        if (need > lib->island_committed[k]) {
            if (!hk_mem_commit(lib->island[k] + lib->island_committed[k], need - lib->island_committed[k], HK_PROT_RW)) return NULL;
            lib->island_committed[k] = need;
        }
        lib->island_used[k] += size;
        return at;
    }
    return NULL;
}

/* ---------------------------------------------------------------- symbols */

static u32 gnu_hash(const char *s)
{
    u32 h = 5381;
    for (; *s; s++) h = h * 33 + (u8)*s;
    return h;
}

static u32 sysv_hash(const char *s)
{
    u32 h = 0, g;
    for (; *s; s++) {
        h = (h << 4) + (u8)*s;
        g = h & 0xf0000000u;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

static bool sym_exported(const Elf32_Sym *s)
{
    int bind = ELF_ST_BIND(s->st_info);
    if (s->st_shndx == SHN_UNDEF) return false;
    if (bind != STB_GLOBAL && bind != STB_WEAK) return false;
    /* STV_HIDDEN (2) and STV_INTERNAL (1) never leave the library. */
    if ((s->st_other & 3) == 1 || (s->st_other & 3) == 2) return false;
    return true;
}

static const Elf32_Sym *lib_lookup(const hk_lib *lib, const char *name, u32 gh, u32 sh)
{
    if (lib->gnu_buckets) {
        u32 word = lib->gnu_bloom[(gh / 32) % lib->gnu_bloom_size];
        u32 mask = (1u << (gh % 32)) | (1u << ((gh >> lib->gnu_bloom_shift) % 32));
        u32 i;
        if ((word & mask) != mask) return NULL;
        i = lib->gnu_buckets[gh % lib->gnu_nbuckets];
        if (i < lib->gnu_symoffset) return NULL;
        for (;; i++) {
            u32 hh = lib->gnu_chain[i - lib->gnu_symoffset];
            const Elf32_Sym *s = &lib->symtab[i];
            if ((hh | 1) == (gh | 1) && sym_exported(s) && !strcmp(lib->strtab + s->st_name, name)) return s;
            if (hh & 1) break;
        }
        return NULL;
    }
    if (lib->sysv_bucket && lib->sysv_nbucket) {
        u32 i;
        for (i = lib->sysv_bucket[sh % lib->sysv_nbucket]; i && i < lib->sysv_nchain; i = lib->sysv_chain[i]) {
            const Elf32_Sym *s = &lib->symtab[i];
            if (sym_exported(s) && !strcmp(lib->strtab + s->st_name, name)) return s;
        }
    }
    return NULL;
}

static hk_addr sym_address(const hk_lib *lib, const Elf32_Sym *s)
{
    hk_addr a = (hk_addr)lib->base + s->st_value;
    if (ELF_ST_TYPE(s->st_info) == STT_GNU_IFUNC) a = HK_GFN(hk_addr (*)(void), a)();
    return a;
}

/* Who defines `name` for an import of `lib`: its own scope, then the system, then any library of its own kind. */
static bool resolve(hk_lib *lib, const char *name, hk_addr *addr, const Elf32_Sym **sym, hk_lib **def)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    void *sys;
    hk_lib *l;
    int i;
    *sym = NULL;
    *def = NULL;
    for (i = 0; i < g_ninterposed; i++) {
        if (!strcmp(g_interposed[i].name, name)) {
            *addr = (hk_addr)g_interposed[i].fn;
            return true;
        }
    }
    for (i = 0; i < lib->nscope; i++) {
        const Elf32_Sym *s = lib_lookup(lib->scope[i], name, gh, sh);
        if (s) {
            *sym = s;
            *def = lib->scope[i];
            *addr = ELF_ST_TYPE(s->st_info) == STT_TLS ? s->st_value : sym_address(lib->scope[i], s);
            return true;
        }
    }
    sys = hk_bionic_find(name);
    /* EGL and OpenGL ES are ANGLE's, found (and ANGLE loaded) only when something asks for them. */
    if (!sys) sys = hk_egl_resolve(name);
    if (sys) {
        *addr = (hk_addr)sys;
        return true;
    }
    for (l = g_libs; l; l = l->next) {
        const Elf32_Sym *s = l->runtime == lib->runtime ? lib_lookup(l, name, gh, sh) : NULL;
        if (s) {
            *sym = s;
            *def = l;
            *addr = ELF_ST_TYPE(s->st_info) == STT_TLS ? s->st_value : sym_address(l, s);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ relocations */

typedef struct reloc_ctx {
    hk_lib *lib;
    /* The last symbol resolved: a library's relocations come in runs against one symbol. */
    u32 cached_index;
    bool cached_ok;
    hk_addr cached_addr;
    const Elf32_Sym *cached_sym;
    hk_lib *cached_def;
    bool failed;
} reloc_ctx;

static bool reloc_symbol(reloc_ctx *c, u32 index, bool want_fn, hk_addr *addr, const Elf32_Sym **sym, hk_lib **def)
{
    hk_lib *lib = c->lib;
    const Elf32_Sym *own;
    const char *name;
    if (c->cached_ok && c->cached_index == index) {
        *addr = c->cached_addr;
        *sym = c->cached_sym;
        *def = c->cached_def;
        return true;
    }
    if (index >= lib->nsyms && lib->nsyms) {
        hk_err("ld: %s: relocation against symbol %u of %u", lib->name, index, lib->nsyms);
        return false;
    }
    own = &lib->symtab[index];
    name = lib->strtab + own->st_name;
    if (!resolve(lib, name, addr, sym, def)) {
        *sym = NULL;
        *def = NULL;
        if (ELF_ST_BIND(own->st_info) == STB_WEAK) {
            *addr = 0;
        } else {
            g_unresolved++;
            lib->stats.unresolved++;
            if (g_verbosity >= 2) hk_warn("ld: %s: unresolved %s %s", lib->name, want_fn ? "function" : "symbol", name);
            /* A function gets a stub that says so when called; data gets NULL, and a note now. */
            *addr = want_fn || ELF_ST_TYPE(own->st_info) == STT_FUNC ? hk_patch_unresolved_stub(lib, name) : 0;
            if (!*addr && g_verbosity < 2) hk_warn("ld: %s: unresolved data symbol %s", lib->name, name);
        }
    }
    c->cached_ok = true;
    c->cached_index = index;
    c->cached_addr = *addr;
    c->cached_sym = *sym;
    c->cached_def = *def;
    return true;
}

static void apply_reloc(reloc_ctx *c, u32 r_offset, u32 r_info)
{
    hk_lib *lib = c->lib;
    u32 type = ELF_R_TYPE(r_info), index = ELF_R_SYM(r_info);
    u8 *place;
    hk_addr s = 0;
    const Elf32_Sym *sym = NULL;
    hk_lib *def = NULL;
    u32 addend;
    if (type == R_ARM_NONE) return;
    if (r_offset < lib->vmin || r_offset > lib->vmax - 4) {
        hk_err("ld: %s: relocation outside the image (0x%x)", lib->name, r_offset);
        c->failed = true;
        return;
    }
    place = lib->base + r_offset;
    addend = hk_rd32(place);
    if (index && !reloc_symbol(c, index, type == R_ARM_JUMP_SLOT, &s, &sym, &def)) {
        c->failed = true;
        return;
    }
    switch (type) {
    case R_ARM_RELATIVE:
        hk_wr32(place, (u32)((hk_addr)lib->base + addend));
        break;
    case R_ARM_ABS32:
        hk_wr32(place, (u32)(s + addend));
        break;
    case R_ARM_REL32:
        hk_wr32(place, (u32)(s + addend - (hk_addr)place));
        break;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
        hk_wr32(place, (u32)s);
        break;
    case R_ARM_IRELATIVE:
        hk_wr32(place, (u32)HK_GFN(hk_addr (*)(void), (hk_addr)lib->base + addend)());
        break;
    case R_ARM_TLS_DTPMOD32:
        hk_wr32(place, index ? (def ? def->tls_module : 0) : lib->tls_module);
        break;
    case R_ARM_TLS_DTPOFF32:
        hk_wr32(place, (u32)(s + addend));
        break;
    case R_ARM_TLS_TPOFF32: {
        hk_lib *owner = index ? def : lib;
        if (!owner || !owner->has_tls) {
            hk_err("ld: %s: thread-local relocation against a library with no thread-local data", lib->name);
            c->failed = true;
            break;
        }
        hk_wr32(place, (u32)(owner->tls_offset + s + addend));
        break;
    }
    default:
        hk_err("ld: %s: relocation type %u is not supported", lib->name, type);
        c->failed = true;
        break;
    }
}

static bool sleb(const u8 **p, const u8 *end, i32 *out)
{
    i32 v = 0;
    u32 shift = 0;
    u8 b;
    do {
        if (*p >= end || shift > 28) return false;
        b = *(*p)++;
        v |= (i32)((u32)(b & 0x7f) << shift);
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) v |= -(i32)(1u << shift);
    *out = v;
    return true;
}

/* Android's packed relocations ("APS2"): groups of entries sharing an info word or an offset step. */
static bool apply_packed(reloc_ctx *c, const u8 *data, u32 size)
{
    const u8 *p = data + 4, *end = data + size;
    i32 count, offset, n = 0;
    if (size < 4 || memcmp(data, "APS2", 4)) {
        hk_err("ld: %s: packed relocations in an unknown format", c->lib->name);
        return false;
    }
    if (!sleb(&p, end, &count) || !sleb(&p, end, &offset)) return false;
    while (n < count) {
        i32 group, flags, delta = 0, info = 0, i, skip;
        if (!sleb(&p, end, &group) || !sleb(&p, end, &flags)) return false;
        if ((flags & 2) && !sleb(&p, end, &delta)) return false;      /* grouped by offset delta */
        if ((flags & 1) && !sleb(&p, end, &info)) return false;       /* grouped by info */
        if ((flags & 8) && (flags & 4) && !sleb(&p, end, &skip)) return false;   /* a shared addend: REL keeps none */
        for (i = 0; i < group && n < count; i++, n++) {
            if (flags & 2) offset += delta;
            else {
                i32 d;
                if (!sleb(&p, end, &d)) return false;
                offset += d;
            }
            if (!(flags & 1) && !sleb(&p, end, &info)) return false;
            if ((flags & 8) && !(flags & 4) && !sleb(&p, end, &skip)) return false;
            apply_reloc(c, (u32)offset, (u32)info);
        }
    }
    return true;
}

/* RELR: an address, then bitmaps each covering the 31 words that follow the last one. */
static void apply_relr(hk_lib *lib, const u32 *relr, u32 size)
{
    u32 n = size / 4, i, where = 0;
    for (i = 0; i < n; i++) {
        u32 e = relr[i];
        if (!(e & 1)) {
            if (e < lib->vmin || e > lib->vmax - 4) continue;
            hk_wr32(lib->base + e, hk_rd32(lib->base + e) + (u32)(hk_addr)lib->base);
            where = e + 4;
        } else {
            u32 bit;
            for (bit = 0; bit < 31; bit++) {
                if ((e >> (bit + 1)) & 1) {
                    u32 at = where + bit * 4;
                    if (at >= lib->vmin && at <= lib->vmax - 4) hk_wr32(lib->base + at, hk_rd32(lib->base + at) + (u32)(hk_addr)lib->base);
                }
            }
            where += 31 * 4;
        }
    }
}

static bool relocate(hk_lib *lib)
{
    reloc_ctx c;
    u32 i;
    memset(&c, 0, sizeof(c));
    c.lib = lib;
    if (lib->relr) apply_relr(lib, lib->relr, lib->relrsz);
    if (lib->android_rel && !apply_packed(&c, lib->android_rel, lib->android_relsz)) {
        hk_err("ld: %s: packed relocations are damaged", lib->name);
        return false;
    }
    for (i = 0; i < lib->relsz / sizeof(Elf32_Rel); i++) apply_reloc(&c, lib->rel[i].r_offset, lib->rel[i].r_info);
    for (i = 0; i < lib->pltrelsz / sizeof(Elf32_Rel); i++) apply_reloc(&c, lib->jmprel[i].r_offset, lib->jmprel[i].r_info);
    return !c.failed;
}

/* ---------------------------------------------------------------- mapping */

static bool in_image(const hk_lib *lib, u32 vaddr, u32 size)
{
    return vaddr >= lib->vmin && vaddr <= lib->vmax && size <= lib->vmax - vaddr;
}

static bool parse_dynamic(hk_lib *lib)
{
    const Elf32_Dyn *d;
    u32 strsz = 0, hash = 0, gnu = 0, symtab = 0, strtab = 0, rel = 0, jmprel = 0, arel = 0, relr = 0, init_array = 0, soname = 0;
    u32 nneeded = 0, i;
    bool have_soname = false;
    if (!lib->dyn) {
        hk_err("ld: %s: no dynamic section", lib->name);
        return false;
    }
    for (d = lib->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_NEEDED: nneeded++; break;
        case DT_STRTAB: strtab = d->d_val; break;
        case DT_STRSZ: strsz = d->d_val; break;
        case DT_SYMTAB: symtab = d->d_val; break;
        case DT_HASH: hash = d->d_val; break;
        case DT_GNU_HASH: gnu = d->d_val; break;
        case DT_REL: rel = d->d_val; break;
        case DT_RELSZ: lib->relsz = d->d_val; break;
        case DT_JMPREL: jmprel = d->d_val; break;
        case DT_PLTRELSZ: lib->pltrelsz = d->d_val; break;
        case DT_PLTREL:
            if (d->d_val != DT_REL) {
                hk_err("ld: %s: RELA relocations in a 32-bit ARM library", lib->name);
                return false;
            }
            break;
        case DT_RELA:
            hk_err("ld: %s: RELA relocations in a 32-bit ARM library", lib->name);
            return false;
        case DT_ANDROID_REL: arel = d->d_val; break;
        case DT_ANDROID_RELSZ: lib->android_relsz = d->d_val; break;
        case DT_RELR: case DT_ANDROID_RELR: relr = d->d_val; break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: lib->relrsz = d->d_val; break;
        case DT_INIT: lib->init_fn = d->d_val; break;
        case DT_INIT_ARRAY: init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: lib->init_array_count = d->d_val / 4; break;
        case DT_SONAME: soname = d->d_val; have_soname = true; break;
        default: break;
        }
    }
    if (!strtab || !symtab || !in_image(lib, strtab, strsz) || !in_image(lib, symtab, 16)) {
        hk_err("ld: %s: symbol or string table missing", lib->name);
        return false;
    }
    lib->strtab = (const char *)lib->base + strtab;
    lib->symtab = (const Elf32_Sym *)(lib->base + symtab);
    if (rel && in_image(lib, rel, lib->relsz)) lib->rel = (const Elf32_Rel *)(lib->base + rel); else lib->relsz = 0;
    if (jmprel && in_image(lib, jmprel, lib->pltrelsz)) lib->jmprel = (const Elf32_Rel *)(lib->base + jmprel); else lib->pltrelsz = 0;
    if (arel && in_image(lib, arel, lib->android_relsz)) lib->android_rel = lib->base + arel; else lib->android_relsz = 0;
    if (relr && in_image(lib, relr, lib->relrsz)) lib->relr = (const u32 *)(lib->base + relr); else lib->relrsz = 0;
    if (init_array && in_image(lib, init_array, lib->init_array_count * 4)) lib->init_array = (const u32 *)(lib->base + init_array);
    else lib->init_array_count = 0;

    if (gnu && in_image(lib, gnu, 16)) {
        const u32 *g = (const u32 *)(lib->base + gnu);
        lib->gnu_nbuckets = g[0];
        lib->gnu_symoffset = g[1];
        lib->gnu_bloom_size = g[2];
        lib->gnu_bloom_shift = g[3];
        lib->gnu_bloom = g + 4;
        lib->gnu_buckets = lib->gnu_bloom + lib->gnu_bloom_size;
        lib->gnu_chain = lib->gnu_buckets + lib->gnu_nbuckets;
        if (!lib->gnu_nbuckets || !lib->gnu_bloom_size || !in_image(lib, gnu, 16 + 4 * (lib->gnu_bloom_size + lib->gnu_nbuckets))) {
            lib->gnu_buckets = NULL;
        } else {
            /* The table does not say how many symbols there are: the last chain of the highest bucket ends them. */
            u32 top = 0;
            for (i = 0; i < lib->gnu_nbuckets; i++)
                if (lib->gnu_buckets[i] > top) top = lib->gnu_buckets[i];
            if (top >= lib->gnu_symoffset) {
                while (!(lib->gnu_chain[top - lib->gnu_symoffset] & 1)) top++;
                lib->nsyms = top + 1;
            } else {
                lib->nsyms = lib->gnu_symoffset;
            }
        }
    }
    if (hash && in_image(lib, hash, 8)) {
        const u32 *h = (const u32 *)(lib->base + hash);
        if (in_image(lib, hash, 8 + 4 * (h[0] + h[1]))) {
            lib->sysv_nbucket = h[0];
            lib->sysv_nchain = h[1];
            lib->sysv_bucket = h + 2;
            lib->sysv_chain = h + 2 + h[0];
            if (lib->sysv_nchain > lib->nsyms) lib->nsyms = lib->sysv_nchain;
        }
    }
    if (!lib->gnu_buckets && !lib->sysv_bucket) {
        hk_err("ld: %s: no symbol hash table", lib->name);
        return false;
    }
    if (have_soname && soname < strsz) lib->soname = hk_strdup(lib->strtab + soname);
    lib->needed_names = (char **)hk_alloc(sizeof(char *) * (nneeded + 1));
    lib->needed = (hk_lib **)hk_alloc(sizeof(hk_lib *) * (nneeded + 1));
    for (d = lib->dyn; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_NEEDED && d->d_val < strsz) lib->needed_names[lib->nneeded++] = hk_strdup(lib->strtab + d->d_val);
    return true;
}

/* How much island a library may need, from its file alone: PLT entries and thread-pointer reads, plus a stub per import. */
static size_t island_estimate(const u8 *file, size_t size, const Elf32_Ehdr *eh)
{
    const Elf32_Phdr *ph = (const Elf32_Phdr *)(file + eh->e_phoff);
    size_t need = 8192;
    int i;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && ph[i].p_offset <= size && ph[i].p_filesz <= size - ph[i].p_offset)
            need += (size_t)hk_patch_count_tp_reads(file + ph[i].p_offset, ph[i].p_filesz) * 16;
        if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset <= size && ph[i].p_filesz <= size - ph[i].p_offset) {
            const Elf32_Dyn *d = (const Elf32_Dyn *)(file + ph[i].p_offset);
            u32 n = ph[i].p_filesz / sizeof(Elf32_Dyn), k;
            for (k = 0; k < n && d[k].d_tag != DT_NULL; k++) {
                /* 12 bytes of thunk per PLT entry, 16 of stub should its import be missing; other relocations may need stubs too. */
                if (d[k].d_tag == DT_PLTRELSZ) need += (size_t)(d[k].d_val / 8) * 28;
                if (d[k].d_tag == DT_RELSZ) need += (size_t)(d[k].d_val / 8) / 4;
            }
        }
    }
    return HK_ALIGN_UP(need, 65536);
}

static hk_lib *map_library(const char *name, const u8 *file, size_t size)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)file;
    const Elf32_Phdr *ph;
    hk_lib *lib;
    u32 vmin = 0xFFFFFFFFu, vmax = 0;
    size_t isl, image, below;
    int i;
    if (size < sizeof(Elf32_Ehdr) || memcmp(eh->e_ident, "\177ELF", 4)) {
        hk_err("ld: %s: not an ELF file", name);
        return NULL;
    }
    if (eh->e_ident[4] != 1 || eh->e_ident[5] != 1 || eh->e_machine != EM_ARM || eh->e_type != ET_DYN) {
        hk_err("ld: %s: not a 32-bit ARM shared object (class %u, machine %u, type %u)", name, eh->e_ident[4], eh->e_machine, eh->e_type);
        return NULL;
    }
    if (eh->e_phentsize != sizeof(Elf32_Phdr) || !eh->e_phnum || eh->e_phoff > size || (size_t)eh->e_phnum * sizeof(Elf32_Phdr) > size - eh->e_phoff) {
        hk_err("ld: %s: program headers outside the file", name);
        return NULL;
    }
    ph = (const Elf32_Phdr *)(file + eh->e_phoff);
    for (i = 0; i < eh->e_phnum; i++) {
        u32 lo, hi;
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset || ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_vaddr > 0x7FFFFFFFu || ph[i].p_memsz > 0x7FFFFFFFu - ph[i].p_vaddr) {
            hk_err("ld: %s: a segment lies outside the file", name);
            return NULL;
        }
        lo = HK_ALIGN_DOWN(ph[i].p_vaddr, HK_PAGE);
        hi = HK_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, HK_PAGE);
        if (lo < vmin) vmin = lo;
        if (hi > vmax) vmax = hi;
    }
    if (vmax <= vmin) {
        hk_err("ld: %s: nothing to load", name);
        return NULL;
    }

    lib = (hk_lib *)hk_alloc(sizeof(*lib));
    lib->name = hk_strdup(name);
    lib->vmin = vmin;
    lib->vmax = vmax;
    lib->phnum = eh->e_phnum;
    lib->phdr = (Elf32_Phdr *)hk_alloc(sizeof(Elf32_Phdr) * eh->e_phnum);
    memcpy(lib->phdr, ph, sizeof(Elf32_Phdr) * eh->e_phnum);

    /*
     * One reservation: [island] image [island]. A Thumb branch reaches 16 MB,
     * so a library whose image is larger than that gets an island on both
     * sides, and each patched instruction uses the nearer one.
     */
    isl = island_estimate(file, size, eh);
    image = HK_ALIGN_UP((size_t)(vmax - vmin), 65536);
    below = image > 14u * 1024 * 1024 ? isl : 0;
    lib->map_size = below + image + isl;
    lib->map = (u8 *)hk_mem_reserve(lib->map_size);
    if (!lib->map) {
        hk_err("ld: %s: no address space for %u MB", name, (unsigned)(lib->map_size >> 20));
        goto bad;
    }
    lib->base = lib->map + below - vmin;
    if (below) {
        lib->island[0] = lib->map;
        lib->island_size[0] = below;
    }
    lib->island[1] = lib->map + below + image;
    lib->island_size[1] = isl;

    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        u32 lo, hi;
        if (p->p_type == PT_DYNAMIC) lib->dyn = (const Elf32_Dyn *)(lib->base + p->p_vaddr);
        if (p->p_type == PT_ARM_EXIDX) {
            lib->exidx = lib->base + p->p_vaddr;
            lib->exidx_count = (int)(p->p_memsz / 8);
        }
        if (p->p_type != PT_LOAD) continue;
        lo = HK_ALIGN_DOWN(p->p_vaddr, HK_PAGE);
        hi = HK_ALIGN_UP(p->p_vaddr + p->p_memsz, HK_PAGE);
        if (!hk_mem_commit(lib->base + lo, hi - lo, HK_PROT_RW)) {
            hk_err("ld: %s: out of memory mapping a %u KB segment", name, (hi - lo) >> 10);
            goto bad;
        }
        memcpy(lib->base + p->p_vaddr, file + p->p_offset, p->p_filesz);
    }
    if (lib->dyn && !in_image(lib, (u32)((const u8 *)lib->dyn - lib->base), 8)) lib->dyn = NULL;
    if (!parse_dynamic(lib)) goto bad;
    return lib;
bad:
    if (lib->map) hk_mem_release(lib->map, lib->map_size);
    free(lib->phdr);
    free(lib->name);
    free(lib);
    return NULL;
}

/* Patch what cannot run as it is, then give every segment its final protection. Nothing is written to the text after this. */
static bool seal(hk_lib *lib)
{
    int i, k;
    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) hk_patch_segment(lib, lib->base + p->p_vaddr, p->p_filesz);
    }
    lib->sealed = true;
    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        u32 lo, hi;
        int prot;
        if (p->p_type != PT_LOAD) continue;
        lo = HK_ALIGN_DOWN(p->p_vaddr, HK_PAGE);
        hi = HK_ALIGN_UP(p->p_vaddr + p->p_memsz, HK_PAGE);
        if ((p->p_flags & PF_X) && (p->p_flags & PF_W))
            hk_warn("ld: %s: a segment is both writable and executable; it becomes executable only, and code that rewrites itself will fault", lib->name);
        prot = (p->p_flags & PF_X) ? HK_PROT_RX : (p->p_flags & PF_W) ? HK_PROT_RW : HK_PROT_R;
        if (prot != HK_PROT_RW && !hk_mem_protect(lib->base + lo, hi - lo, prot)) {
            hk_err("ld: %s: cannot make a segment %s -- is the codeGeneration capability granted?", lib->name, prot == HK_PROT_RX ? "executable" : "read-only");
            return false;
        }
        if (prot == HK_PROT_RX) hk_icache_flush(lib->base + lo, hi - lo);
    }
    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        if (p->p_type == PT_GNU_RELRO) {
            /* Whole pages inside the range only: the last one is shared with data that stays writable. */
            u32 lo = HK_ALIGN_UP(p->p_vaddr, HK_PAGE), hi = HK_ALIGN_DOWN(p->p_vaddr + p->p_memsz, HK_PAGE);
            if (hi > lo && in_image(lib, lo, hi - lo)) hk_mem_protect(lib->base + lo, hi - lo, HK_PROT_R);
        }
    }
    for (k = 0; k < 2; k++) {
        if (!lib->island_committed[k]) continue;
        if (!hk_mem_protect(lib->island[k], lib->island_committed[k], HK_PROT_RX)) {
            hk_err("ld: %s: cannot make the island executable", lib->name);
            return false;
        }
        hk_icache_flush(lib->island[k], lib->island_committed[k]);
    }
    return true;
}

static void build_scope(hk_lib *lib)
{
    /* Breadth-first over DT_NEEDED, each library once. */
    int cap = 16, head = 0, i, k;
    lib->scope = (hk_lib **)hk_alloc(sizeof(hk_lib *) * cap);
    lib->scope[0] = lib;
    lib->nscope = 1;
    while (head < lib->nscope) {
        hk_lib *l = lib->scope[head++];
        for (i = 0; i < l->nneeded; i++) {
            hk_lib *n = l->needed[i];
            bool seen = false;
            if (!n) continue;
            for (k = 0; k < lib->nscope; k++)
                if (lib->scope[k] == n) seen = true;
            if (seen) continue;
            if (lib->nscope == cap) {
                cap *= 2;
                lib->scope = (hk_lib **)hk_realloc(lib->scope, sizeof(hk_lib *) * cap);
            }
            lib->scope[lib->nscope++] = n;
        }
    }
}

static void count_functions(hk_lib *lib)
{
    u32 i;
    for (i = 1; i < lib->nsyms; i++) {
        const Elf32_Sym *s = &lib->symtab[i];
        if (s->st_shndx == SHN_UNDEF || ELF_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (s->st_value & 1) lib->stats.thumb_functions++;
        else lib->stats.arm_functions++;
    }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static hk_lib *find_lib(const char *name, bool runtime)
{
    hk_lib *l;
    name = base_name(name);
    hk_lock_acquire(&g_lock);
    for (l = g_libs; l; l = l->next)
        if (l->runtime == runtime && (!strcmp(l->name, name) || (l->soname && !strcmp(l->soname, name)))) break;
    hk_lock_release(&g_lock);
    return l;
}

hk_lib *hk_ld_find_lib(const char *name) { return find_lib(name, false); }
hk_lib *hk_ld_find_runtime_lib(const char *name) { return find_lib(name, true); }
bool hk_ld_lib_is_runtime(const hk_lib *lib) { return lib && lib->runtime; }

/* `host_path`: the library is that file (an app's own, loaded by its path); otherwise it is looked for by name where its kind is kept. */
static hk_lib *load_lib(const char *name, bool runtime, const char *host_path)
{
    hk_lib *lib;
    const u8 *file;
    size_t size = 0;
    bool owned = false;
    int i;
    name = base_name(name);
    if (hk_bionic_is_system_lib(name)) return NULL;
    load_lock();
    lib = find_lib(name, runtime);
    if (lib) {
        load_unlock();
        return lib;
    }
    if (host_path) {
        file = hk_file_read_all(host_path, &size);
        owned = true;
    } else {
        file = read_lib_file(name, runtime, &size, &owned);
    }
    if (!file) {
        if (host_path) hk_err("ld: %s cannot be read", host_path);
        else hk_err("ld: %s: not found in %s", name, runtime ? "the Android root's libraries" : "the APK");
        load_unlock();
        return NULL;
    }
    lib = map_library(name, file, size);
    if (owned) free((void *)file);
    if (!lib) {
        load_unlock();
        return NULL;
    }
    lib->runtime = runtime;
    /* Listed before its dependencies load, so a cycle finds it instead of loading it again. */
    hk_lock_acquire(&g_lock);
    if (g_libs_tail) g_libs_tail->next = lib;
    else g_libs = lib;
    g_libs_tail = lib;
    hk_lock_release(&g_lock);

    for (i = 0; i < lib->nneeded; i++) {
        if (hk_bionic_is_system_lib(lib->needed_names[i])) continue;
        /* Where this one came from, and nowhere else: an app's library is never handed one of the runtime's, nor the other way round. */
        lib->needed[i] = load_lib(lib->needed_names[i], runtime, NULL);
        if (!lib->needed[i]) hk_warn("ld: %s needs %s, which did not load", lib->name, lib->needed_names[i]);
    }
    build_scope(lib);
    for (i = 0; i < lib->phnum; i++) {
        if (lib->phdr[i].p_type == PT_TLS) {
            lib->has_tls = true;
            lib->tls_filesz = lib->phdr[i].p_filesz;
            lib->tls_memsz = lib->phdr[i].p_memsz;
            lib->tls_align = lib->phdr[i].p_align ? lib->phdr[i].p_align : 1;
            lib->tls_image = lib->base + lib->phdr[i].p_vaddr;
            if (!hk_tls_register(lib)) hk_err("ld: %s: no room for %u bytes of thread-local data", lib->name, lib->tls_memsz);
        }
    }
    if (!relocate(lib)) hk_err("ld: %s: relocation failed; the library may not work", lib->name);
    lib->relocated = true;
    count_functions(lib);
    if (!seal(lib)) {
        load_unlock();
        return NULL;
    }
    note_code_range(lib->map, lib->map_size);
    if (g_verbosity >= 1) {
        hk_info("ld: %s at %p (%u KB), %u Thumb / %u ARM functions, %u thread-pointer reads and %u PLT entries rewritten%s",
                lib->name, (void *)(lib->base + lib->vmin), (lib->vmax - lib->vmin) >> 10, lib->stats.thumb_functions, lib->stats.arm_functions,
                lib->stats.tp_reads, lib->stats.plt_entries, lib->stats.unresolved ? ", with unresolved imports" : "");
        if (lib->stats.unresolved) hk_warn("ld: %s: %u imports are not provided", lib->name, lib->stats.unresolved);
        if (lib->stats.tp_reads_failed) hk_err("ld: %s: %u thread-pointer reads are out of an island's reach", lib->name, lib->stats.tp_reads_failed);
    }
    load_unlock();
    return lib;
}

hk_lib *hk_ld_load(const char *name) { return load_lib(name, false, NULL); }
hk_lib *hk_ld_load_runtime(const char *name) { return load_lib(name, true, NULL); }

hk_lib *hk_ld_load_file(const char *host_path)
{
    const char *slash = strrchr(host_path, '/'), *back = strrchr(host_path, '\\');
    const char *name = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : host_path;
    return load_lib(name, false, host_path);
}

bool hk_ld_init(hk_lib *lib)
{
    static char *argv[] = { (char *)"app_process", NULL };
    static char *envp[] = { NULL };
    u32 i;
    int k;
    if (!lib || lib->inited || lib->initing) return true;
    load_lock();
    lib->initing = true;
    for (k = 0; k < lib->nneeded; k++)
        if (lib->needed[k]) hk_ld_init(lib->needed[k]);
    if (lib->init_fn) HK_GFN(void (*)(int, char **, char **), (hk_addr)lib->base + lib->init_fn)(1, argv, envp);
    for (i = 0; i < lib->init_array_count; i++) {
        u32 fn = lib->init_array[i];
        if (!fn || fn == 0xFFFFFFFFu) continue;
        HK_GFN(void (*)(int, char **, char **), fn)(1, argv, envp);
    }
    lib->inited = true;
    lib->initing = false;
    load_unlock();
    return true;
}

/* ---------------------------------------------------------------- queries */

void *hk_ld_sym(hk_lib *lib, const char *name)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    const Elf32_Sym *s;
    hk_lib *l;
    if (lib) {
        s = lib_lookup(lib, name, gh, sh);
        return s ? (void *)sym_address(lib, s) : NULL;
    }
    /* dlsym(RTLD_DEFAULT) from the app's code: the app's libraries, then the system. */
    for (l = g_libs; l; l = l->next) {
        s = l->runtime ? NULL : lib_lookup(l, name, gh, sh);
        if (s) return (void *)sym_address(l, s);
    }
    {
        void *sys = hk_bionic_find(name);
        return sys ? sys : hk_egl_resolve(name);
    }
}

void *hk_ld_sym_among(bool runtime, const char *name, hk_lib **found_in)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    hk_lib *l;
    if (found_in) *found_in = NULL;
    for (l = g_libs; l; l = l->next) {
        const Elf32_Sym *s = l->runtime == runtime ? lib_lookup(l, name, gh, sh) : NULL;
        if (s && ELF_ST_TYPE(s->st_info) != STT_TLS) {
            if (found_in) *found_in = l;
            return (void *)sym_address(l, s);
        }
    }
    return NULL;
}

void *hk_ld_sym_deep(hk_lib *lib, const char *name, hk_lib **found_in)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    int i;
    for (i = 0; i < lib->nscope; i++) {
        const Elf32_Sym *s = lib_lookup(lib->scope[i], name, gh, sh);
        if (s) {
            if (found_in) *found_in = lib->scope[i];
            return (void *)sym_address(lib->scope[i], s);
        }
    }
    if (found_in) *found_in = NULL;
    return NULL;
}

/*
 * Where the libraries are, for hk_ld_is_guest_code: asked about a thread that
 * has been stopped wherever it was, possibly in the middle of loading a
 * library, so it takes no lock. Entries are only ever added (a library stays
 * for the life of the process), each written before it is counted.
 */
static struct {
    hk_addr lo, hi;
} g_code_ranges[512];
static volatile i32 g_code_range_count;

static void note_code_range(const u8 *map, size_t size)
{
    i32 n = g_code_range_count;
    if (n >= (i32)HK_COUNT(g_code_ranges)) return;
    g_code_ranges[n].lo = (hk_addr)map;
    g_code_ranges[n].hi = (hk_addr)map + size;
    hk_atomic_add(&g_code_range_count, 1);
}

bool hk_ld_is_guest_code(hk_addr pc)
{
    i32 n = g_code_range_count, i;
    for (i = 0; i < n; i++)
        if (pc >= g_code_ranges[i].lo && pc < g_code_ranges[i].hi) return true;
    return false;
}

hk_lib *hk_ld_lib_of(const void *addr)
{
    hk_lib *l;
    for (l = g_libs; l; l = l->next)
        if ((const u8 *)addr >= l->map && (const u8 *)addr < l->map + l->map_size) return l;
    return NULL;
}

void *hk_ld_lib_base(const hk_lib *lib) { return lib->base; }
const char *hk_ld_lib_name(const hk_lib *lib) { return lib->name; }
void hk_ld_get_stats(const hk_lib *lib, hk_ld_stats *out) { *out = lib->stats; }

const char *hk_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr, const void **lib_base)
{
    hk_lib *lib = hk_ld_lib_of(addr);
    const Elf32_Sym *best = NULL;
    u32 off, i;
    if (lib_name) *lib_name = lib ? lib->name : NULL;
    if (sym_addr) *sym_addr = NULL;
    if (lib_base) *lib_base = lib ? lib->base : NULL;
    if (!lib || (const u8 *)addr < lib->base + lib->vmin || (const u8 *)addr >= lib->base + lib->vmax) return NULL;
    off = (u32)((const u8 *)addr - lib->base);
    for (i = 1; i < lib->nsyms; i++) {
        const Elf32_Sym *s = &lib->symtab[i];
        u32 v = s->st_value & ~1u;
        int t = ELF_ST_TYPE(s->st_info);
        if (s->st_shndx == SHN_UNDEF || (t != STT_FUNC && t != STT_OBJECT)) continue;
        if (v <= off && (!best || v > (best->st_value & ~1u))) best = s;
    }
    if (!best) return NULL;
    /* A symbol with a size only names addresses inside it. */
    if (best->st_size && off >= (best->st_value & ~1u) + best->st_size) return NULL;
    if (sym_addr) *sym_addr = lib->base + (best->st_value & ~1u);
    return lib->strtab + best->st_name;
}

int hk_ld_iterate(hk_ld_phdr_cb cb, void *user)
{
    hk_lib *l;
    int r = 0;
    for (l = g_libs; l && !r; l = l->next) r = cb((hk_addr)l->base, l->name, l->phdr, (unsigned)l->phnum, user);
    return r;
}

const void *hk_ld_find_exidx(hk_addr pc, int *count)
{
    hk_lib *l = hk_ld_lib_of((const void *)pc);
    if (!l || !l->exidx) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = l->exidx_count;
    return l->exidx;
}

/* ------------------------------------------------------------- guest code */

hk_addr hk_code_entry(hk_addr fn) { return fn; }

/*
 * A rewritten PLT entry is one ARM instruction, `ldr pc, [pc, #-4]`, and the
 * address it loads. If the thread is interrupted exactly there, Windows
 * resumes it in Thumb state, where those bytes are an undefined instruction
 * (see arm/hk_patch.c). Finish the jump the instruction was about to make.
 */
bool hk_ld_plt_target(hk_addr pc, hk_addr *target)
{
    hk_lib *lib = hk_ld_lib_of((const void *)pc);
    int i;
    if (!lib || (pc & 3)) return false;
    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        const u8 *lo = lib->base + p->p_vaddr;
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if ((const u8 *)pc < lo || (const u8 *)pc + 8 > lo + p->p_filesz) continue;
        if (hk_rd32((const void *)pc) != 0xE51FF004u) return false;
        *target = hk_rd32((const void *)(pc + 4));
        return true;
    }
    return false;
}
