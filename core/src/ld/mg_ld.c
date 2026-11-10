#include "mg_ld_internal.h"

#include <stdio.h>

#include "../host/mg_host.h"
#include "../ndk/mg_egl.h"
#include "../zip/mg_zip.h"

/* ------------------------------------------------------------------ state */

static mg_lock g_lock = MG_LOCK_INIT;      /* the list of libraries; loading itself is serialised by g_load_lock */
static mg_lock g_load_lock = MG_LOCK_INIT;
static u32 g_load_owner;                   /* dlopen from a constructor re-enters on the same thread */
static int g_load_depth;
static mg_lib *g_libs, *g_libs_tail;
/* What the images the loader mapped hold in committed memory: their segments, read from their files (mg_ld_image_bytes). */
static u64 g_image_bytes;
static mg_zip g_apks[MG_LD_MAX_APKS];
static int g_napks;
static mg_zip g_service_apks[MG_LD_MAX_SCOPES];
static bool g_service_apk_ready[MG_LD_MAX_SCOPES];
static MG_THREAD_LOCAL int g_service_scope;
int mg_ld_scope(void) { return g_service_scope; }
int mg_ld_set_scope(int scope) { int old = g_service_scope; g_service_scope = scope; return old; }
bool mg_ld_add_service_apk(const char *path, int scope) {
    char why[160];
    if (scope <= 0 || scope >= MG_LD_MAX_SCOPES) return false;
    if (g_service_apk_ready[scope]) return true;
    if (!mg_zip_open(&g_service_apks[scope], path, why, sizeof(why))) return false;
    g_service_apk_ready[scope] = true;
    return true;
}
static char *g_dirs[32];
static int g_ndirs;
static char *g_runtime_dirs[4];            /* where the runtime's own libraries are: never searched for an app's */
static int g_nruntime_dirs;
static const char *g_abi;
/*
 * Where the next library is to go (mg_ld_load_placed): the app's range [next, end), for the first library loaded and, when
 * recursive, for the ones it needs. Read and written under the load lock. `failed` says why the library did not fit.
 */
static struct {
    bool active, recursive, hint, first;
    u8 *next, *end;
    char failed[160];
} g_place;
static int g_verbosity = 1;
static size_t g_unresolved;
/*
 * While a library's relocations are applied in two passes (mg_ld.c's relocate), this is true during the first: an IFUNC
 * symbol then resolves to its resolver's address without the resolver being run. The resolvers are the library's own code,
 * which runs only in the second pass, after the text is made executable. Read and written under the load lock.
 */
static bool g_defer_ifunc;

typedef struct interposed {
    const char *name;
    void *fn;
} interposed;
static interposed g_interposed[64];
static int g_ninterposed;

static void load_lock(void)
{
    u32 me = mg_thread_self_id();
    if (g_load_owner == me) {
        g_load_depth++;
        return;
    }
    mg_lock_acquire(&g_load_lock);
    g_load_owner = me;
    g_load_depth = 1;
}

static void note_code_range(const u8 *map, size_t size);

static void load_unlock(void)
{
    if (--g_load_depth == 0) {
        g_load_owner = 0;
        mg_lock_release(&g_load_lock);
    }
}

void mg_ld_set_verbosity(int v) { g_verbosity = v; }
size_t mg_ld_unresolved_count(void) { return g_unresolved; }

void mg_ld_interpose(const char *name, void *fn)
{
    if (g_ninterposed < (int)MG_COUNT(g_interposed)) {
        g_interposed[g_ninterposed].name = mg_strdup(name);
        g_interposed[g_ninterposed].fn = fn;
        g_ninterposed++;
    }
}

/* ---------------------------------------------------------------- sources */

bool mg_ld_add_apk(const char *path)
{
    char err[128];
    if (g_napks >= MG_LD_MAX_APKS) return false;
    if (!mg_zip_open(&g_apks[g_napks], path, err, sizeof(err))) {
        mg_err("ld: %s: %s", path, err);
        return false;
    }
    g_napks++;
    g_abi = NULL;
    return true;
}

const struct mg_zip *mg_ld_apk_at(int index) { return index >= 0 && index < g_napks ? &g_apks[index] : NULL; }

void mg_ld_add_search_dir(const char *dir)
{
    int i;
    if (!dir || !dir[0]) return;
    load_lock();
    for (i = 0; i < g_ndirs; i++)
        if (!strcmp(g_dirs[i], dir)) { load_unlock(); return; }
    if (g_ndirs < (int)MG_COUNT(g_dirs)) g_dirs[g_ndirs++] = mg_strdup(dir);
    load_unlock();
}

void mg_ld_add_runtime_dir(const char *dir)
{
    int i;
    for (i = 0; i < g_nruntime_dirs; i++)
        if (!strcmp(g_runtime_dirs[i], dir)) return;
    if (g_nruntime_dirs < (int)MG_COUNT(g_runtime_dirs)) g_runtime_dirs[g_nruntime_dirs++] = mg_strdup(dir);
}

static void count_one(const mg_zip_entry *e, void *user)
{
    size_t n = strlen(e->name);
    if (n > 3 && !strcmp(e->name + n - 3, ".so")) ++*(int *)user;
}

/*
 * The ABIs whose libraries are loaded, the best first (MG_APK_ABI_ARM): armeabi-v7a when any APK has it, the older armeabi
 * (ARMv5, which every ARMv7 runs) otherwise; elsewhere the processor's own.
 */
#if MG_APK_ABI_ARM
static const char *const g_abis[] = { "armeabi-v7a", "armeabi" };
#else
static const char *const g_abis[] = { MG_GUEST_ABI };
#endif

const char *mg_ld_abi(void)
{
    int a, i;
    if (g_abi) return g_abi;
    for (a = 0; a < (int)MG_COUNT(g_abis); a++) {
        char prefix[40];
        int n = 0;
        snprintf(prefix, sizeof(prefix), "lib/%s/", g_abis[a]);
        for (i = 0; i < g_napks; i++) mg_zip_each(&g_apks[i], prefix, count_one, &n);
        if (n) {
            g_abi = g_abis[a];
            return g_abi;
        }
    }
    return NULL;
}

static const mg_zip_entry *apk_lib(const char *name, const mg_zip **in)
{
    const char *abi = mg_ld_abi();
    char path[300];
    int i;
    if (g_service_scope) {
        const mg_zip_entry *e = NULL;
        int a;
        if (g_service_scope >= MG_LD_MAX_SCOPES || !g_service_apk_ready[g_service_scope]) return NULL;
        *in = &g_service_apks[g_service_scope];
        for (a = 0; a < (int)MG_COUNT(g_abis) && !e; a++) {
            snprintf(path, sizeof(path), "lib/%s/%s", g_abis[a], name);
            e = mg_zip_find(*in, path);
        }
        return e;
    }
    if (!abi) return NULL;
    snprintf(path, sizeof(path), "lib/%s/%s", abi, name);
    for (i = 0; i < g_napks; i++) {
        const mg_zip_entry *e = mg_zip_find(&g_apks[i], path);
        if (e) {
            *in = &g_apks[i];
            return e;
        }
    }
    return NULL;
}

bool mg_ld_has_lib(const char *name)
{
    const mg_zip *z;
    int i;
    bool found = false;
    load_lock();
    if (apk_lib(name, &z)) { load_unlock(); return true; }
    if (g_service_scope) { load_unlock(); return false; }
    for (i = 0; i < g_ndirs; i++) {
        char *p = mg_sprintf("%s/%s", g_dirs[i], name);
        mg_stat st;
        bool ok = mg_path_stat(p, &st, NULL) && !st.is_dir;
        free(p);
        if (ok) { found = true; break; }
    }
    load_unlock();
    return found;
}

int mg_ld_file_is_apk_lib(const char *name, const char *host_path)
{
    const mg_zip *z = NULL;
    const mg_zip_entry *e;
    u32 want_crc = 0, crc = 0;
    u64 want_size = 0, size = 0;
    mg_stat st;
    load_lock();
    e = apk_lib(name, &z);
    if (e) {
        want_crc = e->crc;
        want_size = e->usize;
    }
    load_unlock();
    /* (Summed outside the lock: an app's library may be megabytes, and other threads load theirs meanwhile.) */
    if (!e || !mg_path_stat(host_path, &st, NULL) || st.is_dir) return -1;
    if (st.size != want_size) return 0;
    if (!mg_zip_file_crc(host_path, &crc, &size)) return -1;
    return crc == want_crc && size == want_size ? 1 : 0;
}

/*
 * A library's file: `*owned` says whether the bytes are ours to free. The
 * app's are in the APKs and then the folders named for them; the runtime's
 * only in the folders named for the runtime.
 */
/*
 * Where a library's bytes are: a buffer of the whole file, or an APK's stored (not deflated) entry, read where it lies a piece
 * at a time. The WebView's library is 70 MB stored in its APK: read into a buffer first, it needed that much address space
 * again beside its image, and Play Store, with Google Play services hosted, had not both (its sign-in page had no WebView).
 */
typedef struct lib_source {
    const u8 *file;             /* the whole file, or NULL */
    const mg_zip *zip;          /* else the APK, */
    u64 at;                     /* and where the entry's bytes begin in it */
    size_t size;
} lib_source;

static bool src_read(const lib_source *s, u64 off, void *dst, size_t n)
{
    if (off > s->size || n > s->size - off) return false;
    if (!n) return true;
    if (s->file) {
        memcpy(dst, s->file + off, n);
        return true;
    }
    return mg_zip_pread(s->zip, s->at + off, dst, n);
}

/* The library's source (lib_source): a stored entry of an APK as it lies, else the file's bytes read (*owned: to be freed). */
static bool read_lib_file(const char *name, bool runtime, lib_source *src, bool *owned)
{
    const mg_zip *z = NULL;
    const mg_zip_entry *e = runtime ? NULL : apk_lib(name, &z);
    char **dirs = runtime ? g_runtime_dirs : g_dirs;
    int ndirs = runtime ? g_nruntime_dirs : g_service_scope ? 0 : g_ndirs, i;
    u64 at;
    size_t size = 0;
    memset(src, 0, sizeof(*src));
    *owned = false;
    if (e) {
        if (e->method == 0 && e->usize < 0x40000000u && mg_zip_stored_at(z, e, &at)) {
            src->zip = z;
            src->at = at;
            src->size = (size_t)e->usize;
            return true;
        }
        src->file = mg_zip_read(z, e, 0x40000000u, &size);
        src->size = size;
        *owned = src->file != NULL;
        return src->file != NULL;
    }
    for (i = 0; i < ndirs; i++) {
        char *p = mg_sprintf("%s/%s", dirs[i], name);
        u8 *b = mg_file_read_all(p, &size);
        free(p);
        if (b) {
            src->file = b;
            src->size = size;
            *owned = true;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- islands */

u8 *mg_island_alloc(mg_lib *lib, mg_addr near_addr, size_t size, size_t reach)
{
    int k;
    size = MG_ALIGN_UP(size, 4);
    if (lib->sealed) return NULL;
    /* The upper island first: it is the one every library has. */
    for (k = 1; k >= 0; k--) {
        u8 *at;
        size_t need;
        if (!lib->island[k] || lib->island_used[k] + size > lib->island_size[k]) continue;
        at = lib->island[k] + lib->island_used[k];
        if (reach) {
            mg_addr lo = (mg_addr)at, hi = (mg_addr)at + size;
            mg_addr d1 = lo > near_addr ? lo - near_addr : near_addr - lo;
            mg_addr d2 = hi > near_addr ? hi - near_addr : near_addr - hi;
            if (d1 >= reach || d2 >= reach) continue;
        }
        need = MG_ALIGN_UP(lib->island_used[k] + size, MG_PAGE);
        if (need > lib->island_committed[k]) {
            if (!mg_mem_commit(lib->island[k] + lib->island_committed[k], need - lib->island_committed[k], MG_PROT_RW)) return NULL;
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

static bool sym_exported(const ElfW(Sym) *s)
{
    int bind = ELF_ST_BIND(s->st_info);
    if (s->st_shndx == SHN_UNDEF) return false;
    if (bind != STB_GLOBAL && bind != STB_WEAK) return false;
    /* STV_HIDDEN (2) and STV_INTERNAL (1) never leave the library. */
    if ((s->st_other & 3) == 1 || (s->st_other & 3) == 2) return false;
    return true;
}

static const ElfW(Sym) *lib_lookup(const mg_lib *lib, const char *name, u32 gh, u32 sh)
{
    if (lib->gnu_buckets) {
        enum { BITS = sizeof(mg_addr) * 8 };
        mg_addr word = lib->gnu_bloom[(gh / BITS) % lib->gnu_bloom_size];
        mg_addr mask = ((mg_addr)1 << (gh % BITS)) | ((mg_addr)1 << ((gh >> lib->gnu_bloom_shift) % BITS));
        u32 i;
        if ((word & mask) != mask) return NULL;
        i = lib->gnu_buckets[gh % lib->gnu_nbuckets];
        if (i < lib->gnu_symoffset) return NULL;
        for (;; i++) {
            u32 hh = lib->gnu_chain[i - lib->gnu_symoffset];
            const ElfW(Sym) *s = &lib->symtab[i];
            if ((hh | 1) == (gh | 1) && sym_exported(s) && !strcmp(lib->strtab + s->st_name, name)) return s;
            if (hh & 1) break;
        }
        return NULL;
    }
    if (lib->sysv_bucket && lib->sysv_nbucket) {
        u32 i;
        for (i = lib->sysv_bucket[sh % lib->sysv_nbucket]; i && i < lib->sysv_nchain; i = lib->sysv_chain[i]) {
            const ElfW(Sym) *s = &lib->symtab[i];
            if (sym_exported(s) && !strcmp(lib->strtab + s->st_name, name)) return s;
        }
    }
    return NULL;
}

static mg_addr sym_address(const mg_lib *lib, const ElfW(Sym) *s)
{
    mg_addr a = (mg_addr)lib->base + s->st_value;
    /* An IFUNC's resolver runs on the first pass only when nothing deferred it; otherwise its address is the resolver's. */
    if (ELF_ST_TYPE(s->st_info) == STT_GNU_IFUNC && !g_defer_ifunc) a = MG_GFN(mg_addr (*)(void), a)();
    return a;
}

/* Who defines `name` for an import of `lib`: its own scope, then the system, then any library of its own kind. */
static bool resolve(mg_lib *lib, const char *name, mg_addr *addr, const ElfW(Sym) **sym, mg_lib **def)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    void *sys;
    mg_lib *l;
    int i;
    *sym = NULL;
    *def = NULL;
    for (i = 0; i < g_ninterposed; i++) {
        if (!strcmp(g_interposed[i].name, name)) {
            *addr = (mg_addr)g_interposed[i].fn;
            return true;
        }
    }
    for (i = 0; i < lib->nscope; i++) {
        const ElfW(Sym) *s = lib_lookup(lib->scope[i], name, gh, sh);
        if (s) {
            *sym = s;
            *def = lib->scope[i];
            *addr = ELF_ST_TYPE(s->st_info) == STT_TLS ? s->st_value : sym_address(lib->scope[i], s);
            return true;
        }
    }
    sys = mg_bionic_find(name);
    /* EGL and OpenGL ES are ANGLE's, found (and ANGLE loaded) only when something asks for them. */
    if (!sys) sys = mg_egl_resolve(name);
    if (sys) {
        *addr = (mg_addr)sys;
        return true;
    }
    for (l = g_libs; l; l = l->next) {
        const ElfW(Sym) *s = l->runtime == lib->runtime && l->service_scope == lib->service_scope ? lib_lookup(l, name, gh, sh) : NULL;
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
    mg_lib *lib;
    /* The last symbol resolved: a library's relocations come in runs against one symbol. */
    u32 cached_index;
    bool cached_ok;
    mg_addr cached_addr;
    const ElfW(Sym) *cached_sym;
    mg_lib *cached_def;
    bool failed;
    /* 1: the relocations that run no guest code; 2: IFUNC and IRELATIVE, whose resolvers run on the now-executable text. */
    int phase;
} reloc_ctx;

/*
 * An import nothing provides, kept by its name in the library's string table the first time: several relocations may ask
 * for one name, and it is counted once. False when it was kept already.
 */
static bool keep_unresolved(mg_lib *lib, const char *name)
{
    u32 i;
    for (i = 0; i < lib->stats.unresolved; i++)
        if (!strcmp(lib->unresolved[i], name)) return false;
    if (lib->stats.unresolved == lib->unresolved_cap) {
        lib->unresolved_cap = lib->unresolved_cap ? lib->unresolved_cap * 2 : 8;
        lib->unresolved = (const char **)mg_realloc((void *)lib->unresolved, lib->unresolved_cap * sizeof(*lib->unresolved));
    }
    lib->unresolved[lib->stats.unresolved++] = name;
    return true;
}

/*
 * The names of a library's imports nothing provides, for its lines in the log as it loads: as many as fit in `cap` (the
 * first at least in part), then how many more there are.
 */
static void unresolved_names(const mg_lib *lib, char *out, size_t cap)
{
    enum { MORE = 24 };                                       /* room kept for ", and 4294967295 more" */
    size_t n = 0;
    u32 i, said = 0;
    out[0] = 0;
    for (i = 0; i < lib->stats.unresolved; i++) {
        size_t len = strlen(lib->unresolved[i]);
        if (n + 2 + len + MORE >= cap) break;
        n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? ", " : "", lib->unresolved[i]);
        said++;
    }
    if (!said && lib->stats.unresolved) {
        /* (A name too long for the line, a C++ one's: its start.) */
        n = (size_t)snprintf(out, cap - MORE, "%.*s...", (int)(cap - MORE - 4), lib->unresolved[0]);
        said = 1;
    }
    if (said < lib->stats.unresolved) snprintf(out + n, cap - n, ", and %u more", (unsigned)(lib->stats.unresolved - said));
}

static bool reloc_symbol(reloc_ctx *c, u32 index, bool want_fn, mg_addr *addr, const ElfW(Sym) **sym, mg_lib **def)
{
    mg_lib *lib = c->lib;
    const ElfW(Sym) *own;
    const char *name;
    if (c->cached_ok && c->cached_index == index) {
        *addr = c->cached_addr;
        *sym = c->cached_sym;
        *def = c->cached_def;
        return true;
    }
    if (index >= lib->nsyms && lib->nsyms) {
        mg_err("ld: %s: relocation against symbol %u of %u", lib->name, index, lib->nsyms);
        return false;
    }
    own = &lib->symtab[index];
    name = lib->strtab + own->st_name;
    if (!resolve(lib, name, addr, sym, def)) {
        *sym = NULL;
        *def = NULL;
        if (ELF_ST_BIND(own->st_info) == STB_WEAK) {
            *addr = 0;
        } else if (c->phase != 1) {
            /* Counted, said and given its stub on the first pass: the second applies only what resolvers give, which this is not. */
            *addr = 0;
        } else {
            /* (Each name counted once, and said with the others in the library's line as it loads: mg_ld_load.) */
            bool first = keep_unresolved(lib, name);
            if (first) g_unresolved++;
            if (first && g_verbosity >= 2) mg_warn("ld: %s: unresolved %s %s", lib->name, want_fn ? "function" : "symbol", name);
            /* A function gets a stub that says so when called; data gets NULL, and a note now. */
            *addr = want_fn || ELF_ST_TYPE(own->st_info) == STT_FUNC ? mg_patch_unresolved_stub(lib, name) : 0;
            if (!*addr && first && g_verbosity < 2) mg_warn("ld: %s: unresolved data symbol %s", lib->name, name);
        }
    }
    c->cached_ok = true;
    c->cached_index = index;
    c->cached_addr = *addr;
    c->cached_sym = *sym;
    c->cached_def = *def;
    return true;
}

/* A word of the guest's at `place`: 32 bits on the 32-bit ABIs, 64 on the others. */
static mg_addr rd_word(const u8 *place) { mg_addr v; memcpy(&v, place, sizeof(v)); return v; }
static void wr_word(u8 *place, mg_addr v) { memcpy(place, &v, sizeof(v)); }

#if MG_ARCH_ARM64 && MG_GUEST_CODE
/*
 * ARM64's TLS descriptors (R_AARCH64_TLSDESC), its compilers' way to a thread-local variable of another module or of the
 * global-dynamic kind: two words, a function and its argument. Code calls the function with x0 the descriptor and adds
 * what it returns in x0 to its thread pointer; the function may change no other register. Every library's thread-local
 * block is in this layer's static block (ld/mg_tls.c), so one function serves them all: it returns the descriptor's
 * second word, the variable's offset from the thread pointer.
 */
void mg_tlsdesc_static(void);
__asm__(".text\n"
        ".p2align 2\n"
        ".globl mg_tlsdesc_static\n"
        ".hidden mg_tlsdesc_static\n"
        ".type mg_tlsdesc_static, %function\n"
        "mg_tlsdesc_static:\n"
        "ldr x0, [x0, #8]\n"
        "ret\n"
        ".size mg_tlsdesc_static, .-mg_tlsdesc_static\n");
#endif

/*
 * One relocation. `addend` is RELA's (the 64-bit ABIs), or for REL (the 32-bit ones) what the place holds, read here when
 * `has_addend` is false. The values are the same on every processor, but for TLS: a thread pointer offset is from the
 * guest's thread pointer to the variable, which is this layer's own block (ld/mg_tls.c) and not x86's variant II, and
 * code adds it to the pointer whichever way the architecture lays its static TLS out.
 */
static void apply_reloc(reloc_ctx *c, mg_addr r_offset, mg_addr r_info, mg_addr addend, bool has_addend)
{
    mg_lib *lib = c->lib;
    u32 type = (u32)ELFW_R_TYPE(r_info), index = (u32)ELFW_R_SYM(r_info);
    u8 *place;
    mg_addr s = 0;
    const ElfW(Sym) *sym = NULL;
    mg_lib *def = NULL;
    if (type == MG_R_NONE) return;
    if (r_offset < lib->vmin || r_offset > lib->vmax - sizeof(mg_addr)) {
        mg_err("ld: %s: relocation outside the image (0x%llx)", lib->name, (unsigned long long)r_offset);
        c->failed = true;
        return;
    }
    place = lib->base + r_offset;
    if (!has_addend) addend = rd_word(place);
    if (index && !reloc_symbol(c, index, type == MG_R_JUMP_SLOT, &s, &sym, &def)) {
        c->failed = true;
        return;
    }
    /*
     * A relocation whose value a resolver gives -- IRELATIVE, or one against an IFUNC symbol -- is applied on the second
     * pass, when the text that holds the resolver is executable; every other relocation on the first. (In the first pass,
     * sym_address returned the resolver's own address, not its result, so sym still tells an IFUNC apart.)
     */
    if (((type == MG_R_IRELATIVE) || (sym && ELF_ST_TYPE(sym->st_info) == STT_GNU_IFUNC)) != (c->phase == 2)) return;
    if (type == MG_R_RELATIVE) {
        wr_word(place, (mg_addr)lib->base + addend);
    } else if (type == MG_R_ABS) {
        wr_word(place, s + addend);
    } else if (type == MG_R_PCREL) {
        wr_word(place, s + addend - (mg_addr)place);
    } else if (type == MG_R_GLOB_DAT || type == MG_R_JUMP_SLOT) {
        /* (RELA's addend is added on ARM64, as its psABI says; x86_64's is 0.) */
        wr_word(place, MG_RELOC_RELA ? s + addend : s);
    } else if (type == MG_R_IRELATIVE) {
        wr_word(place, MG_GFN(mg_addr (*)(void), (mg_addr)lib->base + addend)());
    } else if (type == MG_R_DTPMOD) {
        wr_word(place, index ? (def ? def->tls_module : 0) : lib->tls_module);
    } else if (type == MG_R_DTPOFF) {
        wr_word(place, s + addend);
    } else if (type == MG_R_TPOFF) {
        mg_lib *owner = index ? def : lib;
        if (!owner || !owner->has_tls) {
            mg_err("ld: %s: thread-local relocation against a library with no thread-local data", lib->name);
            c->failed = true;
            return;
        }
        wr_word(place, owner->tls_offset + s + addend);
#if MG_ARCH_ARM64 && MG_GUEST_CODE
    } else if (type == MG_R_TLSDESC) {
        mg_lib *owner = index ? def : lib;
        if (!owner || !owner->has_tls || r_offset > lib->vmax - 2 * sizeof(mg_addr)) {
            mg_err("ld: %s: a TLS descriptor against a library with no thread-local data, or outside the image", lib->name);
            c->failed = true;
            return;
        }
        wr_word(place, (mg_addr)mg_tlsdesc_static);
        wr_word(place + sizeof(mg_addr), owner->tls_offset + s + addend);
#endif
    } else {
        mg_err("ld: %s: relocation type %u is not supported", lib->name, type);
        c->failed = true;
    }
}

static bool sleb(const u8 **p, const u8 *end, i64 *out)
{
    u64 v = 0;
    u32 shift = 0;
    u8 b;
    do {
        if (*p >= end || shift > 63) return false;
        b = *(*p)++;
        v |= (u64)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= ~(u64)0 << shift;
    *out = (i64)v;
    return true;
}

/* Android's packed relocations ("APS2"): groups of entries sharing an info word or an offset step. */
static bool apply_packed(reloc_ctx *c, const u8 *data, u32 size)
{
    const u8 *p = data + 4, *end = data + size;
    i64 count, offset, n = 0, addend = 0;
    if (size < 4 || memcmp(data, "APS2", 4)) {
        mg_err("ld: %s: packed relocations in an unknown format", c->lib->name);
        return false;
    }
    if (!sleb(&p, end, &count) || !sleb(&p, end, &offset)) return false;
    while (n < count) {
        i64 group, flags, delta = 0, info = 0, i, d;
        if (!sleb(&p, end, &group) || !sleb(&p, end, &flags)) return false;
        if ((flags & 2) && !sleb(&p, end, &delta)) return false;      /* grouped by offset delta */
        if ((flags & 1) && !sleb(&p, end, &info)) return false;       /* grouped by info */
        /* An addend shared by the group, or (below) one for each: deltas from the last, RELA's only (REL's are in place). */
        if ((flags & 8) && (flags & 4)) {
            if (!sleb(&p, end, &d)) return false;
            addend += d;
        } else if (!(flags & 8)) {
            addend = 0;
        }
        for (i = 0; i < group && n < count; i++, n++) {
            if (flags & 2) offset += delta;
            else {
                if (!sleb(&p, end, &d)) return false;
                offset += d;
            }
            if (!(flags & 1) && !sleb(&p, end, &info)) return false;
            if ((flags & 8) && !(flags & 4)) {
                if (!sleb(&p, end, &d)) return false;
                addend += d;
            }
            apply_reloc(c, (mg_addr)offset, (mg_addr)info, (mg_addr)addend, MG_RELOC_RELA != 0);
        }
    }
    return true;
}

/* RELR: an address, then bitmaps each covering the words that follow the last one (31 on the 32-bit ABIs, 63 on the others). */
static void apply_relr(mg_lib *lib, const mg_addr *relr, size_t size)
{
    enum { W = sizeof(mg_addr), BITS = sizeof(mg_addr) * 8 - 1 };
    size_t n = size / W, i;
    mg_addr where = 0;
    for (i = 0; i < n; i++) {
        mg_addr e = relr[i];
        if (!(e & 1)) {
            if (e < lib->vmin || e > lib->vmax - W) continue;
            wr_word(lib->base + e, rd_word(lib->base + e) + (mg_addr)lib->base);
            where = e + W;
        } else {
            u32 bit;
            for (bit = 0; bit < BITS; bit++) {
                if ((e >> (bit + 1)) & 1) {
                    mg_addr at = where + bit * W;
                    if (at >= lib->vmin && at <= lib->vmax - W) wr_word(lib->base + at, rd_word(lib->base + at) + (mg_addr)lib->base);
                }
            }
            where += BITS * W;
        }
    }
}

/*
 * Relocations in two passes. Pass 1 (phase 1) applies all that read or write only data -- the great majority -- while the
 * text is still writable and not yet made executable. Between the passes the caller patches the text and makes it
 * executable (seal_text). Pass 2 (phase 2) applies the few whose value a resolver computes: IRELATIVE, and relocations
 * against an IFUNC symbol. Those resolvers are the library's own functions; they must run on executable text, and they
 * write the GOT, which is still writable until the RELRO is sealed afterwards. On a device that enforces W^X (the phone)
 * running a resolver before its text is executable faults; qemu, which reads guest pages rather than executing them, does
 * not, so this showed only on the phone.
 */
static bool relocate(mg_lib *lib, int phase)
{
    reloc_ctx c;
    size_t i;
    memset(&c, 0, sizeof(c));
    c.lib = lib;
    c.phase = phase;
    g_defer_ifunc = phase == 1;
    if (phase == 1 && lib->relr) apply_relr(lib, lib->relr, lib->relrsz);   /* RELR is all base-relative: no resolvers */
    if (lib->android_rel && !apply_packed(&c, lib->android_rel, lib->android_relsz)) {
        mg_err("ld: %s: packed relocations are damaged", lib->name);
        g_defer_ifunc = false;
        return false;
    }
#if MG_RELOC_RELA
    for (i = 0; i < lib->relsz / sizeof(mg_reloc); i++) apply_reloc(&c, lib->rel[i].r_offset, lib->rel[i].r_info, (mg_addr)lib->rel[i].r_addend, true);
    for (i = 0; i < lib->pltrelsz / sizeof(mg_reloc); i++)
        apply_reloc(&c, lib->jmprel[i].r_offset, lib->jmprel[i].r_info, (mg_addr)lib->jmprel[i].r_addend, true);
#else
    for (i = 0; i < lib->relsz / sizeof(mg_reloc); i++) apply_reloc(&c, lib->rel[i].r_offset, lib->rel[i].r_info, 0, false);
    for (i = 0; i < lib->pltrelsz / sizeof(mg_reloc); i++) apply_reloc(&c, lib->jmprel[i].r_offset, lib->jmprel[i].r_info, 0, false);
#endif
    g_defer_ifunc = false;
    return !c.failed;
}

/* ---------------------------------------------------------------- mapping */

static bool in_image(const mg_lib *lib, mg_addr vaddr, mg_addr size)
{
    return vaddr >= lib->vmin && vaddr <= lib->vmax && size <= lib->vmax - vaddr;
}

static bool parse_dynamic(mg_lib *lib)
{
    const ElfW(Dyn) *d;
    mg_addr strsz = 0, hash = 0, gnu = 0, symtab = 0, strtab = 0, rel = 0, jmprel = 0, arel = 0, relr = 0, init_array = 0, soname = 0;
    u32 nneeded = 0, i;
    bool have_soname = false;
    if (!lib->dyn) {
        mg_err("ld: %s: no dynamic section", lib->name);
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
#if MG_RELOC_RELA
        case DT_RELA: rel = d->d_val; break;
        case DT_RELASZ: lib->relsz = d->d_val; break;
        case DT_REL:
            mg_err("ld: %s: REL relocations in a library of a 64-bit ABI, which has RELA", lib->name);
            return false;
        case DT_ANDROID_RELA: arel = d->d_val; break;
        case DT_ANDROID_RELASZ: lib->android_relsz = d->d_val; break;
#else
        case DT_REL: rel = d->d_val; break;
        case DT_RELSZ: lib->relsz = d->d_val; break;
        case DT_RELA:
            mg_err("ld: %s: RELA relocations in a library of a 32-bit ABI, which has REL", lib->name);
            return false;
        case DT_ANDROID_REL: arel = d->d_val; break;
        case DT_ANDROID_RELSZ: lib->android_relsz = d->d_val; break;
#endif
        case DT_JMPREL: jmprel = d->d_val; break;
        case DT_PLTRELSZ: lib->pltrelsz = d->d_val; break;
        case DT_PLTREL:
            if (d->d_val != (MG_RELOC_RELA ? DT_RELA : DT_REL)) {
                mg_err("ld: %s: its PLT's relocations are not of its ABI's kind", lib->name);
                return false;
            }
            break;
        case DT_RELR: case DT_ANDROID_RELR: relr = d->d_val; break;
        case DT_RELRSZ: case DT_ANDROID_RELRSZ: lib->relrsz = d->d_val; break;
        case DT_INIT: lib->init_fn = d->d_val; break;
        case DT_INIT_ARRAY: init_array = d->d_val; break;
        case DT_INIT_ARRAYSZ: lib->init_array_count = (u32)(d->d_val / sizeof(mg_addr)); break;
        case DT_SONAME: soname = d->d_val; have_soname = true; break;
        default: break;
        }
    }
    if (!strtab || !symtab || !in_image(lib, strtab, strsz) || !in_image(lib, symtab, 16)) {
        mg_err("ld: %s: symbol or string table missing", lib->name);
        return false;
    }
    lib->strtab = (const char *)lib->base + strtab;
    lib->symtab = (const ElfW(Sym) *)(lib->base + symtab);
    if (rel && in_image(lib, rel, lib->relsz)) lib->rel = (const mg_reloc *)(lib->base + rel); else lib->relsz = 0;
    if (jmprel && in_image(lib, jmprel, lib->pltrelsz)) lib->jmprel = (const mg_reloc *)(lib->base + jmprel); else lib->pltrelsz = 0;
    if (arel && in_image(lib, arel, lib->android_relsz)) lib->android_rel = lib->base + arel; else lib->android_relsz = 0;
    if (relr && in_image(lib, relr, lib->relrsz)) lib->relr = (const mg_addr *)(lib->base + relr); else lib->relrsz = 0;
    if (init_array && in_image(lib, init_array, lib->init_array_count * sizeof(mg_addr))) lib->init_array = (const mg_addr *)(lib->base + init_array);
    else lib->init_array_count = 0;

    if (gnu && in_image(lib, gnu, 16)) {
        const u32 *g = (const u32 *)(lib->base + gnu);
        lib->gnu_nbuckets = g[0];
        lib->gnu_symoffset = g[1];
        lib->gnu_bloom_size = g[2];
        lib->gnu_bloom_shift = g[3];
        lib->gnu_bloom = (const mg_addr *)(g + 4);
        lib->gnu_buckets = (const u32 *)(lib->gnu_bloom + lib->gnu_bloom_size);
        lib->gnu_chain = lib->gnu_buckets + lib->gnu_nbuckets;
        if (!lib->gnu_nbuckets || !lib->gnu_bloom_size
            || !in_image(lib, gnu, 16 + sizeof(mg_addr) * lib->gnu_bloom_size + 4 * (mg_addr)lib->gnu_nbuckets)) {
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
        mg_err("ld: %s: no symbol hash table", lib->name);
        return false;
    }
    if (have_soname && soname < strsz) lib->soname = mg_strdup(lib->strtab + soname);
    lib->needed_names = (char **)mg_alloc(sizeof(char *) * (nneeded + 1));
    lib->needed = (mg_lib **)mg_alloc(sizeof(mg_lib *) * (nneeded + 1));
    for (d = lib->dyn; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_NEEDED && d->d_val < strsz) lib->needed_names[lib->nneeded++] = mg_strdup(lib->strtab + d->d_val);
    return true;
}

/* Thread-pointer reads in [off, off + size) of the source, as mg_patch_count_tp_reads counts them in one buffer. */
static size_t count_tp_reads(const lib_source *src, u64 off, size_t size)
{
    enum { PIECE = 1 << 20 };
    size_t done = 0, n = 0;
    u8 *buf;
    if (src->file) return mg_patch_count_tp_reads(src->file + off, size);
    /* A piece at a time, each read 2 bytes longer, so that an instruction across a piece's end is counted once, in that piece
       (a window is 4 bytes, taken every 2; PIECE is a multiple of 4, which keeps the 32-bit ARM ones' alignment). */
    buf = (u8 *)mg_alloc(PIECE + 2);
    while (done < size) {
        size_t want = size - done > PIECE + 2 ? PIECE + 2 : size - done;
        if (!src_read(src, off + done, buf, want)) break;
        n += mg_patch_count_tp_reads(buf, want);
        done += PIECE;
    }
    free(buf);
    return n;
}

/* How much island a library may need, from its file alone: PLT entries and thread-pointer reads, plus a stub per import. */
static size_t island_estimate(const lib_source *src, const ElfW(Phdr) *ph, int phnum)
{
    size_t need = 8192, size = src->size;
    int i;
    for (i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_X) && ph[i].p_offset <= size && ph[i].p_filesz <= size - ph[i].p_offset)
            need += count_tp_reads(src, ph[i].p_offset, ph[i].p_filesz) * 16;
        if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset <= size && ph[i].p_filesz <= size - ph[i].p_offset) {
            ElfW(Dyn) *d = (ElfW(Dyn) *)mg_alloc(ph[i].p_filesz + sizeof(ElfW(Dyn)));
            size_t n = (size_t)(ph[i].p_filesz / sizeof(ElfW(Dyn))), k;
            if (!src_read(src, ph[i].p_offset, d, ph[i].p_filesz)) n = 0;
            for (k = 0; k < n && d[k].d_tag != DT_NULL; k++) {
                /* 12 bytes of thunk per PLT entry, 16 of stub should its import be missing; other relocations may need stubs too. */
                if (d[k].d_tag == DT_PLTRELSZ) need += (size_t)(d[k].d_val / sizeof(mg_reloc)) * 28;
                if (d[k].d_tag == (MG_RELOC_RELA ? DT_RELASZ : DT_RELSZ)) need += (size_t)(d[k].d_val / sizeof(mg_reloc)) / 4;
            }
            free(d);
        }
    }
    return MG_ALIGN_UP(need, 65536);
}

#if MG_ARCH_ARM64
static int cmp_mark(const void *a, const void *b)
{
    const mg_addr *x = (const mg_addr *)a, *y = (const mg_addr *)b;
    return x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0;
}

/*
 * ARM64: the spans of data among the library's executable sections (mg_lib's data), from its symbol table's mapping
 * symbols -- local, untyped, $d (or $d.<any>) where data starts and $x where instructions start again, as the AArch64
 * ELF ABI tells a disassembler the two apart -- where it still has a symbol table. Hand-written assembly keeps its
 * constants and banners beside its code (BoringSSL's ChaCha20 its "expand 32-byte k", SHA-256 its table), and a word of
 * them may read as an instruction that writes x18. A table of more than 4 MB (an app's library left unstripped) is not
 * read at every load: nothing is left out of such a library's count.
 */
static void read_data_spans(mg_lib *lib, const lib_source *src, const ElfW(Shdr) *sh, size_t n)
{
    const ElfW(Shdr) *tab = NULL, *str;
    ElfW(Sym) *syms;
    char *names;
    mg_addr (*marks)[3];                /* address, 1 where data starts, the end of its section */
    size_t i, count, nmarks = 0;
    for (i = 0; i < n && !tab; i++)
        if (sh[i].sh_type == SHT_SYMTAB && sh[i].sh_entsize == sizeof(ElfW(Sym)) && sh[i].sh_link < n) tab = &sh[i];
    if (!tab) return;
    str = &sh[tab->sh_link];
    count = (size_t)(tab->sh_size / sizeof(ElfW(Sym)));
    if (str->sh_type != SHT_STRTAB || !count || !str->sh_size || tab->sh_size > (4u << 20) || str->sh_size > (4u << 20)) return;
    syms = (ElfW(Sym) *)mg_alloc(count * sizeof(ElfW(Sym)));
    names = (char *)mg_alloc((size_t)str->sh_size + 1);
    marks = (mg_addr(*)[3])mg_alloc(count * sizeof(*marks));
    if (src_read(src, tab->sh_offset, syms, count * sizeof(ElfW(Sym))) && src_read(src, str->sh_offset, names, (size_t)str->sh_size)) {
        names[str->sh_size] = 0;
        for (i = 0; i < count; i++) {
            const ElfW(Sym) *s = &syms[i];
            const char *name = s->st_name < str->sh_size ? names + s->st_name : "";
            const ElfW(Shdr) *in;
            if (ELF_ST_BIND(s->st_info) != STB_LOCAL || ELF_ST_TYPE(s->st_info) != STT_NOTYPE || name[0] != '$'
                || (name[1] != 'd' && name[1] != 'x') || (name[2] && name[2] != '.') || s->st_shndx == SHN_UNDEF || s->st_shndx >= n)
                continue;
            in = &sh[s->st_shndx];
            if (in->sh_type != SHT_PROGBITS || (in->sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) != (SHF_ALLOC | SHF_EXECINSTR)) continue;
            marks[nmarks][0] = (mg_addr)s->st_value;
            marks[nmarks][1] = name[1] == 'd';
            marks[nmarks][2] = (mg_addr)(in->sh_addr + in->sh_size);
            nmarks++;
        }
        qsort(marks, nmarks, sizeof(*marks), cmp_mark);
        /* Each $d to the next $x of its section (or the section's end); a run of $d is one span. */
        for (i = 0; i < nmarks;) {
            mg_addr start = marks[i][0], end = marks[i][2];
            size_t j = i + 1;
            if (!marks[i][1]) {
                i++;
                continue;
            }
            while (j < nmarks && marks[j][0] < end && marks[j][1]) j++;
            if (j < nmarks && marks[j][0] < end) end = marks[j][0];
            if (start < end) {
                if (!(lib->ndata & 15)) lib->data = (mg_addr(*)[2])mg_realloc(lib->data, (size_t)(lib->ndata + 16) * sizeof(*lib->data));
                lib->data[lib->ndata][0] = start;
                lib->data[lib->ndata][1] = end;
                lib->ndata++;
            }
            i = j;
        }
    }
    free(marks);
    free(names);
    free(syms);
}
#endif

/*
 * The library's executable sections from its section headers (mg_lib's code). Each must lie in an executable segment's
 * bytes from the file, at the address that segment gives its offset; headers that do not (stripped, or made up to
 * mislead a disassembler), or more than eight such sections, and none is known: the segments are then looked at whole.
 * On ARM64, where they are known, the spans of data among them too (read_data_spans).
 */
static void read_code_sections(mg_lib *lib, const lib_source *src, const ElfW(Ehdr) *eh)
{
    size_t n = eh->e_shnum, size = src->size, i;
    ElfW(Shdr) *sh;
    int found = 0, k;
    if (!eh->e_shoff || eh->e_shentsize != sizeof(ElfW(Shdr)) || !n || n > 4096 || eh->e_shoff > size
        || n * sizeof(ElfW(Shdr)) > size - eh->e_shoff)
        return;
    sh = (ElfW(Shdr) *)mg_alloc(n * sizeof(ElfW(Shdr)));
    if (!src_read(src, eh->e_shoff, sh, n * sizeof(ElfW(Shdr)))) {
        free(sh);
        return;
    }
    for (i = 0; i < n && found >= 0; i++) {
        const ElfW(Shdr) *s = &sh[i];
        bool inside = false;
        if (s->sh_type != SHT_PROGBITS || (s->sh_flags & (SHF_ALLOC | SHF_EXECINSTR)) != (SHF_ALLOC | SHF_EXECINSTR) || !s->sh_size) continue;
        for (k = 0; k < lib->phnum && !inside; k++) {
            const ElfW(Phdr) *p = &lib->phdr[k];
            inside = p->p_type == PT_LOAD && (p->p_flags & PF_X) && s->sh_addr >= p->p_vaddr && s->sh_size <= p->p_filesz
                && s->sh_addr - p->p_vaddr <= p->p_filesz - s->sh_size && s->sh_offset >= p->p_offset
                && s->sh_offset - p->p_offset == s->sh_addr - p->p_vaddr;
        }
        if (!inside || found == 8) {
            found = -1;
            break;
        }
        lib->code[found][0] = s->sh_addr;
        lib->code[found][1] = s->sh_addr + s->sh_size;
        found++;
    }
    lib->ncode = found > 0 ? found : 0;
#if MG_ARCH_ARM64
    if (lib->ncode) read_data_spans(lib, src, sh, n);
#endif
    free(sh);
}

/* Note `name` of `type` among the notes in [p, p + size), each name and description padded to `align`. */
static bool has_note(const u8 *p, size_t size, size_t align, const char *name, u32 type)
{
    size_t n = strlen(name) + 1;
    while (size >= 12) {
        u32 namesz, descsz, kind;
        size_t whole;
        memcpy(&namesz, p, 4);
        memcpy(&descsz, p + 4, 4);
        memcpy(&kind, p + 8, 4);
        if (namesz > size || descsz > size) return false;
        whole = 12 + MG_ALIGN_UP((size_t)namesz, align) + MG_ALIGN_UP((size_t)descsz, align);
        if (whole > size) return false;
        if (kind == type && namesz >= n && namesz <= MG_ALIGN_UP(n, 4) && !memcmp(p + 12, name, n)) return true;
        p += whole;
        size -= whole;
    }
    return false;
}

/*
 * Go's build ID note (owner "Go", type 4), which every library of Go's has in a PT_NOTE of its first segment; looked for
 * only where a segment has the notes' bytes from the file (elsewhere the image is not committed).
 */
static bool go_note(const mg_lib *lib)
{
    int i, k;
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *note = &lib->phdr[i];
        if (note->p_type != PT_NOTE) continue;
        for (k = 0; k < lib->phnum; k++) {
            const ElfW(Phdr) *load = &lib->phdr[k];
            if (load->p_type == PT_LOAD && note->p_vaddr >= load->p_vaddr && note->p_filesz <= load->p_filesz &&
                note->p_vaddr - load->p_vaddr <= load->p_filesz - note->p_filesz &&
                has_note(lib->base + note->p_vaddr, (size_t)note->p_filesz, note->p_align == 8 ? 8 : 4, "Go", 4))
                return true;
        }
    }
    return false;
}

bool mg_ld_lib_is_go(const mg_lib *lib) { return lib && lib->go; }

static mg_lib *map_library(const char *name, const lib_source *src)
{
    ElfW(Ehdr) ehdr;
    const ElfW(Ehdr) *eh = &ehdr;
    ElfW(Phdr) *ph;
    mg_lib *lib;
    mg_addr vmin = (mg_addr)-1, vmax = 0;
    size_t isl, image, below, size = src->size;
    u64 committed = 0;
    int i;
    if (size < sizeof(ElfW(Ehdr)) || !src_read(src, 0, &ehdr, sizeof(ehdr)) || memcmp(eh->e_ident, "\177ELF", 4)) {
        mg_err("ld: %s: not an ELF file", name);
        return NULL;
    }
    if (eh->e_ident[4] != MG_ELFCLASS || eh->e_ident[5] != 1 || eh->e_machine != MG_EM || eh->e_type != ET_DYN) {
        mg_err("ld: %s: not a shared object of %s (class %u, machine %u, type %u)", name, MG_GUEST_ABI, eh->e_ident[4], eh->e_machine, eh->e_type);
        return NULL;
    }
    if (eh->e_phentsize != sizeof(ElfW(Phdr)) || !eh->e_phnum || eh->e_phoff > size || (size_t)eh->e_phnum * sizeof(ElfW(Phdr)) > size - eh->e_phoff) {
        mg_err("ld: %s: program headers outside the file", name);
        return NULL;
    }
    ph = (ElfW(Phdr) *)mg_alloc(sizeof(ElfW(Phdr)) * eh->e_phnum);
    if (!src_read(src, eh->e_phoff, ph, sizeof(ElfW(Phdr)) * eh->e_phnum)) {
        mg_err("ld: %s: its program headers cannot be read", name);
        free(ph);
        return NULL;
    }
    for (i = 0; i < eh->e_phnum; i++) {
        mg_addr lo, hi;
        /* (An image's addresses within 2 GB of its start: a 32-bit process has no more, and no library is larger.) */
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset > size || ph[i].p_filesz > size - ph[i].p_offset || ph[i].p_filesz > ph[i].p_memsz ||
            ph[i].p_vaddr > 0x7FFFFFFFu || ph[i].p_memsz > 0x7FFFFFFFu - ph[i].p_vaddr) {
            mg_err("ld: %s: a segment lies outside the file", name);
            free(ph);
            return NULL;
        }
        lo = MG_ALIGN_DOWN(ph[i].p_vaddr, MG_PAGE);
        hi = MG_ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, MG_PAGE);
        if (lo < vmin) vmin = lo;
        if (hi > vmax) vmax = hi;
    }
    if (vmax <= vmin) {
        mg_err("ld: %s: nothing to load", name);
        free(ph);
        return NULL;
    }

    lib = (mg_lib *)mg_alloc(sizeof(*lib));
    lib->name = mg_strdup(name);
    lib->vmin = vmin;
    lib->vmax = vmax;
    lib->phnum = eh->e_phnum;
    lib->phdr = ph;
    read_code_sections(lib, src, eh);

    /*
     * One reservation: [island] image [island]. A Thumb branch reaches 16 MB,
     * so a library whose image is larger than that gets an island on both
     * sides, and each patched instruction uses the nearer one.
     */
    isl = island_estimate(src, ph, eh->e_phnum);
    image = MG_ALIGN_UP((size_t)(vmax - vmin), 65536);
    below = image > 14u * 1024 * 1024 ? isl : 0;
    if (g_place.active && (g_place.first || g_place.recursive)) {
        /*
         * Where the app reserved (mg_ld_load_placed): the image starts at the range's start, as bionic puts it, and the
         * upper island follows it in the range. The island below, for a large image, is a reservation of its own just
         * under the range, within a branch's reach of the image's beginning.
         */
        size_t need = MG_ALIGN_UP((size_t)(vmax - vmin), MG_PAGE) + isl;
        g_place.first = false;
        if (need > (size_t)(g_place.end - g_place.next)) {
            snprintf(g_place.failed, sizeof(g_place.failed), "\"%s\" needs %u KB and the reserved space has %u KB", name, (unsigned)(need >> 10),
                     (unsigned)((size_t)(g_place.end - g_place.next) >> 10));
            if (!g_place.hint) goto bad;
        } else {
            u8 *at = g_place.next;
            image = MG_ALIGN_UP((size_t)(vmax - vmin), MG_PAGE);
            lib->placed = true;
            lib->map = at;
            lib->map_size = image + isl;
            lib->base = at - vmin;
            lib->island[1] = at + image;
            lib->island_size[1] = isl;
            if (below) {
                /*
                 * Below the range, as near as it can be had. When nothing near is free (the app's reservation lies against
                 * another mapping), there is none: what the patcher adds then all goes above the image, and a patch in the
                 * image's first 16 MB that cannot reach it fails, with a line in the log (mg_island_alloc).
                 */
                u8 *want = (u8 *)MG_ALIGN_DOWN((mg_addr)at - below, (mg_addr)65536);
                int k;
                for (k = 0; k < 64 && !lib->island[0]; k++) lib->island[0] = (u8 *)mg_mem_reserve_at(want - (size_t)k * 65536, below);
                if (lib->island[0]) lib->island_size[0] = below;
                else mg_warn("ld: %s: no room below the reserved space for the code the patcher adds; it goes above the image", name);
            }
            /* The range is the app's (an anonymous mapping, all of it committed); the image's pages are made writable to fill. */
            g_place.next = (u8 *)MG_ALIGN_UP((mg_addr)(at + image + isl), (mg_addr)MG_PAGE);
        }
    }
    if (!lib->placed) {
        lib->map_size = below + image + isl;
        lib->map = (u8 *)mg_mem_reserve(lib->map_size);
        if (!lib->map) {
            u32 free_mb = 0, largest = 0, reserved = 0, committed = 0, stretch[4] = { 0, 0, 0, 0 };
            mg_err("ld: %s: no address space for %u MB", name, (unsigned)(lib->map_size >> 20));
            if (mg_host_address_space(&free_mb, &largest, &reserved, &committed))
                mg_err("ld:   the process has %u MB free (the largest stretch %u MB), %u MB reserved, %u MB committed, %u MB of it views",
                       (unsigned)free_mb, (unsigned)largest, (unsigned)reserved, (unsigned)committed, (unsigned)(mg_views_kb() >> 10));
            if (mg_host_free_stretches(stretch, 4))
                mg_err("ld:   the largest free stretches: %u, %u, %u and %u MB", (unsigned)stretch[0], (unsigned)stretch[1],
                       (unsigned)stretch[2], (unsigned)stretch[3]);
            goto bad;
        }
        lib->base = lib->map + below - vmin;
        if (below) {
            lib->island[0] = lib->map;
            lib->island_size[0] = below;
        }
        lib->island[1] = lib->map + below + image;
        lib->island_size[1] = isl;
    }

    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        mg_addr lo, hi;
        if (p->p_type == PT_DYNAMIC) lib->dyn = (const ElfW(Dyn) *)(lib->base + p->p_vaddr);
        if (MG_ARCH_ARM32 && p->p_type == PT_ARM_EXIDX) {
            lib->exidx = lib->base + p->p_vaddr;
            lib->exidx_count = (int)(p->p_memsz / 8);
        }
        if (p->p_type != PT_LOAD) continue;
        lo = MG_ALIGN_DOWN(p->p_vaddr, MG_PAGE);
        hi = MG_ALIGN_UP(p->p_vaddr + p->p_memsz, MG_PAGE);
        if (!mg_mem_commit(lib->base + lo, hi - lo, MG_PROT_RW) || (lib->placed && !mg_mem_protect(lib->base + lo, hi - lo, MG_PROT_RW))) {
            mg_err("ld: %s: out of memory mapping a %u KB segment", name, (unsigned)((hi - lo) >> 10));
            goto bad;
        }
        if (!src_read(src, p->p_offset, lib->base + p->p_vaddr, p->p_filesz)) {
            mg_err("ld: %s: a %u KB segment cannot be read", name, (unsigned)(p->p_filesz >> 10));
            goto bad;
        }
        committed += hi - lo;
    }
    lib->go = go_note(lib);
    if (lib->dyn && !in_image(lib, (mg_addr)((const u8 *)lib->dyn - lib->base), 8)) lib->dyn = NULL;
    if (!parse_dynamic(lib)) goto bad;
    /* (A placed image is in the app's own mapping, a reservation of no access to it, which mg_mmap_usage does not count.) */
    mg_lock_acquire(&g_lock);
    g_image_bytes += committed;
    mg_lock_release(&g_lock);
    return lib;
bad:
    /* (A placed image's range is the app's: only what the loader reserved itself is given back.) */
    if (lib->placed) {
        if (lib->island[0]) mg_mem_release(lib->island[0], lib->island_size[0]);
        mg_mem_protect(lib->map, lib->map_size, MG_PROT_NONE);
    } else if (lib->map) {
        mg_mem_release(lib->map, lib->map_size);
    }
    free(lib->phdr);
    free(lib->name);
    free(lib->data);
    free(lib);
    return NULL;
}

/* Patch what cannot run as it is, then give every segment its final protection. Nothing is written to the text after this. */
/*
 * Patch the executable text (ARM to Thumb, the PLT, thread-pointer reads) and make each segment its final protection:
 * text read-execute, the rest read-only or read-write. After this the text runs; the IFUNC resolvers of relocate's second
 * pass need it. The RELRO is not yet sealed and the islands are not yet executable (seal_protect does those last, once the
 * resolvers have written the GOT). The library is not marked sealed here: the second pass may still add island stubs.
 */
static bool seal_text(mg_lib *lib)
{
    int i;
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & PF_X)) mg_patch_segment(lib, lib->base + p->p_vaddr, p->p_filesz);
    }
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        mg_addr lo, hi;
        int prot;
        if (p->p_type != PT_LOAD) continue;
        lo = MG_ALIGN_DOWN(p->p_vaddr, MG_PAGE);
        hi = MG_ALIGN_UP(p->p_vaddr + p->p_memsz, MG_PAGE);
        if ((p->p_flags & PF_X) && (p->p_flags & PF_W))
            mg_warn("ld: %s: a segment is both writable and executable; it becomes executable only, and code that rewrites itself will fault", lib->name);
        prot = (p->p_flags & PF_X) ? MG_PROT_RX : (p->p_flags & PF_W) ? MG_PROT_RW : MG_PROT_R;
        if (prot != MG_PROT_RW && !mg_mem_protect(lib->base + lo, hi - lo, prot)) {
            mg_err("ld: %s: cannot make a segment %s -- is the codeGeneration capability granted?", lib->name, prot == MG_PROT_RX ? "executable" : "read-only");
            return false;
        }
        if (prot == MG_PROT_RX) mg_icache_flush(lib->base + lo, hi - lo);
    }
    return true;
}

/* Seal the RELRO read-only and make the islands executable: the last step, once every relocation (the IFUNC ones included) is done. */
static bool seal_protect(mg_lib *lib)
{
    int i, k;
    lib->sealed = true;
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        if (p->p_type == PT_GNU_RELRO) {
            /* Whole pages inside the range only: the last one is shared with data that stays writable. */
            mg_addr lo = MG_ALIGN_UP((mg_addr)p->p_vaddr, (mg_addr)MG_PAGE), hi = MG_ALIGN_DOWN((mg_addr)(p->p_vaddr + p->p_memsz), (mg_addr)MG_PAGE);
            if (hi > lo && in_image(lib, lo, hi - lo)) mg_mem_protect(lib->base + lo, hi - lo, MG_PROT_R);
        }
    }
    for (k = 0; k < 2; k++) {   /* the patch islands */
        if (!lib->island_committed[k]) continue;
        if (!mg_mem_protect(lib->island[k], lib->island_committed[k], MG_PROT_RX)) {
            mg_err("ld: %s: cannot make the island executable", lib->name);
            return false;
        }
        mg_icache_flush(lib->island[k], lib->island_committed[k]);
    }
    return true;
}

static void build_scope(mg_lib *lib)
{
    /* Breadth-first over DT_NEEDED, each library once. */
    int cap = 16, head = 0, i, k;
    lib->scope = (mg_lib **)mg_alloc(sizeof(mg_lib *) * cap);
    lib->scope[0] = lib;
    lib->nscope = 1;
    while (head < lib->nscope) {
        mg_lib *l = lib->scope[head++];
        for (i = 0; i < l->nneeded; i++) {
            mg_lib *n = l->needed[i];
            bool seen = false;
            if (!n) continue;
            for (k = 0; k < lib->nscope; k++)
                if (lib->scope[k] == n) seen = true;
            if (seen) continue;
            if (lib->nscope == cap) {
                cap *= 2;
                lib->scope = (mg_lib **)mg_realloc(lib->scope, sizeof(mg_lib *) * cap);
            }
            lib->scope[lib->nscope++] = n;
        }
    }
}

static void count_functions(mg_lib *lib)
{
    u32 i;
    for (i = 1; i < lib->nsyms; i++) {
        const ElfW(Sym) *s = &lib->symtab[i];
        if (s->st_shndx == SHN_UNDEF || ELF_ST_TYPE(s->st_info) != STT_FUNC) continue;
        if (!MG_ARCH_ARM32) continue;
        if (s->st_value & 1) lib->stats.thumb_functions++;
        else lib->stats.arm_functions++;
    }
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static mg_lib *find_lib(const char *name, bool runtime)
{
    mg_lib *l;
    name = base_name(name);
    mg_lock_acquire(&g_lock);
    for (l = g_libs; l; l = l->next)
        if (l->runtime == runtime && l->service_scope == (runtime ? 0 : g_service_scope) && (!strcmp(l->name, name) || (l->soname && !strcmp(l->soname, name)))) break;
    mg_lock_release(&g_lock);
    return l;
}

mg_lib *mg_ld_find_lib(const char *name) { return find_lib(name, false); }
mg_lib *mg_ld_find_runtime_lib(const char *name) { return find_lib(name, true); }
bool mg_ld_lib_is_runtime(const mg_lib *lib) { return lib && lib->runtime; }

/* `host_path`: the library is that file (an app's own, loaded by its path); otherwise it is looked for by name where its kind is kept. */
static mg_lib *load_lib(const char *name, bool runtime, const char *host_path)
{
    mg_lib *lib;
    lib_source src;
    size_t size = 0;
    bool owned = false, found;
    int i;
    name = base_name(name);
    if (mg_bionic_is_system_lib(name)) return NULL;
    load_lock();
    lib = find_lib(name, runtime);
    if (lib) {
        load_unlock();
        return lib;
    }
    if (host_path) {
        memset(&src, 0, sizeof(src));
        src.file = mg_file_read_all(host_path, &size);
        src.size = size;
        owned = found = src.file != NULL;
    } else {
        found = read_lib_file(name, runtime, &src, &owned);
    }
    if (!found) {
        if (host_path) mg_err("ld: %s cannot be read", host_path);
        else mg_err("ld: %s: not found in %s, or it cannot be read", name, runtime ? "the Android root's libraries" : "the APK");
        load_unlock();
        return NULL;
    }
    lib = map_library(name, &src);
    if (owned) free((void *)src.file);
    if (!lib) {
        load_unlock();
        return NULL;
    }
    lib->runtime = runtime;
    lib->service_scope = runtime ? 0 : g_service_scope;
    /* Listed before its dependencies load, so a cycle finds it instead of loading it again. */
    mg_lock_acquire(&g_lock);
    if (g_libs_tail) g_libs_tail->next = lib;
    else g_libs = lib;
    g_libs_tail = lib;
    mg_lock_release(&g_lock);

    for (i = 0; i < lib->nneeded; i++) {
        if (mg_bionic_is_system_lib(lib->needed_names[i])) continue;
        /* Where this one came from, and nowhere else: an app's library is never handed one of the runtime's, nor the other way round. */
        lib->needed[i] = load_lib(lib->needed_names[i], runtime, NULL);
        if (!lib->needed[i]) mg_warn("ld: %s needs %s, which did not load", lib->name, lib->needed_names[i]);
    }
    build_scope(lib);
    for (i = 0; i < lib->phnum; i++) {
        if (lib->phdr[i].p_type == PT_TLS) {
            lib->has_tls = true;
            lib->tls_filesz = lib->phdr[i].p_filesz;
            lib->tls_memsz = lib->phdr[i].p_memsz;
            lib->tls_align = lib->phdr[i].p_align ? lib->phdr[i].p_align : 1;
            lib->tls_image = lib->base + lib->phdr[i].p_vaddr;
            if (!mg_tls_register(lib)) mg_err("ld: %s: no room for %u bytes of thread-local data", lib->name, (unsigned)lib->tls_memsz);
        }
    }
    /*
     * Relocate, patch, relocate again: the IFUNC resolvers (relocate's second pass) run on text that seal_text has just
     * made executable, which a device enforcing W^X needs (see relocate). seal_protect is last, once the resolvers have
     * written the GOT.
     */
    if (!relocate(lib, 1)) mg_err("ld: %s: relocation failed; the library may not work", lib->name);
    if (!seal_text(lib)) {
        load_unlock();
        return NULL;
    }
    if (!relocate(lib, 2)) mg_err("ld: %s: an IFUNC relocation failed; the library may not work", lib->name);
    lib->relocated = true;
    mg_tls_ready(lib);
    count_functions(lib);
    if (!seal_protect(lib)) {
        load_unlock();
        return NULL;
    }
    note_code_range(lib->map, lib->map_size);
    if (lib->placed && lib->island[0]) note_code_range(lib->island[0], lib->island_size[0]);
    if (g_verbosity >= 1) {
        mg_info("ld: %s at %p (%u KB), %u Thumb / %u ARM functions, %u thread-pointer reads and %u PLT entries rewritten%s",
                lib->name, (void *)(lib->base + lib->vmin), (unsigned)((lib->vmax - lib->vmin) >> 10), lib->stats.thumb_functions, lib->stats.arm_functions,
                lib->stats.tp_reads, lib->stats.plt_entries, lib->stats.unresolved ? ", with unresolved imports" : "");
        if (lib->stats.unresolved) {
            /* Which, by name: the first ones, as many as the line has room for (DANA's libjabe.so had one, and said only "1"). */
            char names[320];
            unresolved_names(lib, names, sizeof(names));
            mg_warn("ld: %s: %u imports are not provided: %s", lib->name, lib->stats.unresolved, names);
        }
#if MG_ARCH_ARM32
        if (lib->stats.tp_reads_failed) mg_err("ld: %s: %u thread-pointer reads are out of an island's reach", lib->name, lib->stats.tp_reads_failed);
        /* (arm/mg_patch.c: a library of Go's, whose calls are made on its goroutines' small stacks.) */
        if (lib->stats.svc_led_out)
            mg_info("ld: %s: %u of its %u system calls by number led out to the core, made on a stack of the thread's for them", lib->name,
                    lib->stats.svc_led_out, lib->stats.svc_sites);
#elif MG_ARCH_X86_64 || MG_ARCH_X86
        /* (x86/mg_patch_x86.c: libcrypto's 16 were all data among its code -- ChaCha's "expand 32-byte k", AES's tables.) */
        if (lib->stats.tp_reads_failed)
            mg_warn("ld: %s: %u reads of the thread pointer's segment at other places than bionic's two slots are left as they are "
                    "(data among the code reads as such, too)", lib->name, lib->stats.tp_reads_failed);
#else
        if (lib->stats.tp_reads_failed) mg_err("ld: %s: %u accesses of the thread pointer are left as they are", lib->name, lib->stats.tp_reads_failed);
#endif
    }
    load_unlock();
    return lib;
}

mg_lib *mg_ld_load(const char *name) { return load_lib(name, false, NULL); }

bool mg_ld_placed_overlap(const void *p, size_t size, u8 **lo, u8 **hi)
{
    const u8 *a = (const u8 *)p, *b = a + size;
    mg_lib *l;
    bool found = false;
    mg_lock_acquire(&g_lock);
    for (l = g_libs; l && !found; l = l->next) {
        if (!l->placed || b <= l->map || a >= l->map + l->map_size) continue;
        *lo = a > l->map ? (u8 *)a : l->map;
        *hi = b < l->map + l->map_size ? (u8 *)b : l->map + l->map_size;
        found = true;
    }
    mg_lock_release(&g_lock);
    return found;
}

mg_lib *mg_ld_load_placed(const char *name, void *at, size_t size, bool hint, bool recursive, char *why, size_t why_size)
{
    mg_lib *lib;
    why[0] = 0;
    load_lock();
    if (g_place.active) {
        /* (A constructor's own android_dlopen_ext inside one: loaded where it goes.) */
        load_unlock();
        return mg_ld_load(name);
    }
    g_place.active = true;
    g_place.first = true;
    g_place.recursive = recursive;
    g_place.hint = hint;
    g_place.next = (u8 *)at;
    g_place.end = (u8 *)at + size;
    g_place.failed[0] = 0;
    lib = load_lib(name, false, NULL);
    g_place.active = false;
    if (!lib && g_place.failed[0]) snprintf(why, why_size, "dlopen failed: %s", g_place.failed);
    else if (lib && g_place.failed[0] && hint) mg_info("ld: %s: loaded elsewhere (%s)", name, g_place.failed);
    load_unlock();
    return lib;
}
mg_lib *mg_ld_load_runtime(const char *name) { return load_lib(name, true, NULL); }

mg_lib *mg_ld_load_file(const char *host_path)
{
    const char *slash = strrchr(host_path, '/'), *back = strrchr(host_path, '\\');
    const char *name = back && (!slash || back > slash) ? back + 1 : slash ? slash + 1 : host_path;
    mg_lib *lib;
    load_lock();
    /* Libraries unpacked by an app's native loader need their siblings too.
     * Keep this in the app namespace; service scopes must not inherit it. */
    if (!g_service_scope && name > host_path) {
        size_t n = (size_t)(name - host_path - 1);
        char *dir = (char *)mg_alloc(n + 1);
        memcpy(dir, host_path, n);
        dir[n] = 0;
        mg_ld_add_search_dir(dir);
        free(dir);
    }
    lib = load_lib(name, false, host_path);
    load_unlock();
    return lib;
}

bool mg_ld_init(mg_lib *lib)
{
    /* (argv, envp and the auxiliary vector in one block, as the kernel puts them: Go's runtime looks past envp for it.) */
    char **argv = mg_bionic_initial_argv(), **envp = argv + 2;
    u32 i;
    int k;
    if (!lib || lib->inited || lib->initing) return true;
    load_lock();
    lib->initing = true;
    for (k = 0; k < lib->nneeded; k++)
        if (lib->needed[k]) mg_ld_init(lib->needed[k]);
    if (lib->init_fn) MG_GFN(void (*)(int, char **, char **), (mg_addr)lib->base + lib->init_fn)(1, argv, envp);
    for (i = 0; i < lib->init_array_count; i++) {
        mg_addr fn = lib->init_array[i];
        if (!fn || fn == (mg_addr)-1) continue;
        MG_GFN(void (*)(int, char **, char **), fn)(1, argv, envp);
    }
    lib->inited = true;
    lib->initing = false;
    load_unlock();
    return true;
}

/* ---------------------------------------------------------------- queries */

void *mg_ld_sym(mg_lib *lib, const char *name)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    const ElfW(Sym) *s;
    mg_lib *l;
    if (lib) {
        s = lib_lookup(lib, name, gh, sh);
        return s ? (void *)sym_address(lib, s) : NULL;
    }
    /* dlsym(RTLD_DEFAULT) from the app's code: the app's libraries, then the system. */
    for (l = g_libs; l; l = l->next) {
        s = l->runtime || l->service_scope != g_service_scope ? NULL : lib_lookup(l, name, gh, sh);
        if (s) return (void *)sym_address(l, s);
    }
    {
        void *sys = mg_bionic_find(name);
        return sys ? sys : mg_egl_resolve(name);
    }
}

void *mg_ld_sym_among(bool runtime, const char *name, mg_lib **found_in)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    mg_lib *l;
    if (found_in) *found_in = NULL;
    for (l = g_libs; l; l = l->next) {
        const ElfW(Sym) *s = l->runtime == runtime && l->service_scope == (runtime ? 0 : g_service_scope) ? lib_lookup(l, name, gh, sh) : NULL;
        if (s && ELF_ST_TYPE(s->st_info) != STT_TLS) {
            if (found_in) *found_in = l;
            return (void *)sym_address(l, s);
        }
    }
    return NULL;
}

void *mg_ld_sym_deep(mg_lib *lib, const char *name, mg_lib **found_in)
{
    u32 gh = gnu_hash(name), sh = sysv_hash(name);
    int i;
    for (i = 0; i < lib->nscope; i++) {
        const ElfW(Sym) *s = lib_lookup(lib->scope[i], name, gh, sh);
        if (s) {
            if (found_in) *found_in = lib->scope[i];
            return (void *)sym_address(lib->scope[i], s);
        }
    }
    if (found_in) *found_in = NULL;
    return NULL;
}

/*
 * Where the libraries are, for mg_ld_is_guest_code: asked about a thread that
 * has been stopped wherever it was, possibly in the middle of loading a
 * library, so it takes no lock. Entries are only ever added (a library stays
 * for the life of the process), each written before it is counted.
 */
static struct {
    mg_addr lo, hi;
} g_code_ranges[512];
static volatile i32 g_code_range_count;

static void note_code_range(const u8 *map, size_t size)
{
    i32 n = g_code_range_count;
    if (n >= (i32)MG_COUNT(g_code_ranges)) return;
    g_code_ranges[n].lo = (mg_addr)map;
    g_code_ranges[n].hi = (mg_addr)map + size;
    mg_atomic_add(&g_code_range_count, 1);
}

bool mg_ld_is_guest_code(mg_addr pc)
{
    i32 n = g_code_range_count, i;
    for (i = 0; i < n; i++)
        if (pc >= g_code_ranges[i].lo && pc < g_code_ranges[i].hi) return true;
    return false;
}

mg_lib *mg_ld_lib_of(const void *addr)
{
    mg_lib *l;
    for (l = g_libs; l; l = l->next)
        if (((const u8 *)addr >= l->map && (const u8 *)addr < l->map + l->map_size) ||
            (l->placed && l->island[0] && (const u8 *)addr >= l->island[0] && (const u8 *)addr < l->island[0] + l->island_size[0]))
            return l;
    return NULL;
}

void *mg_ld_lib_base(const mg_lib *lib) { return lib->base; }
size_t mg_ld_lib_extent(const mg_lib *lib) { return lib->map_size + (lib->placed ? lib->island_size[0] : 0); }
const char *mg_ld_lib_name(const mg_lib *lib) { return lib->name; }
void mg_ld_get_stats(const mg_lib *lib, mg_ld_stats *out) { *out = lib->stats; }

u64 mg_ld_reserved_bytes(void)
{
    mg_lib *l;
    u64 n = 0;
    mg_lock_acquire(&g_lock);
    for (l = g_libs; l; l = l->next)
        if (l->map) n += mg_host_reserved_in(l->map, l->map_size);
    mg_lock_release(&g_lock);
    return n;
}

u64 mg_ld_image_bytes(void)
{
    u64 n;
    mg_lock_acquire(&g_lock);
    n = g_image_bytes;
    mg_lock_release(&g_lock);
    return n;
}

const char *mg_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr, const void **lib_base)
{
    mg_lib *lib = mg_ld_lib_of(addr);
    const ElfW(Sym) *best = NULL;
    mg_addr off;
    u32 i;
    if (lib_name) *lib_name = lib ? lib->name : NULL;
    if (sym_addr) *sym_addr = NULL;
    if (lib_base) *lib_base = lib ? lib->base : NULL;
    if (!lib || (const u8 *)addr < lib->base + lib->vmin || (const u8 *)addr >= lib->base + lib->vmax) return NULL;
    off = (mg_addr)((const u8 *)addr - lib->base);
    for (i = 1; i < lib->nsyms; i++) {
        const ElfW(Sym) *s = &lib->symtab[i];
        mg_addr v = MG_CODE_ADDR((mg_addr)s->st_value);
        int t = ELF_ST_TYPE(s->st_info);
        if (s->st_shndx == SHN_UNDEF || (t != STT_FUNC && t != STT_OBJECT)) continue;
        if (v <= off && (!best || v > MG_CODE_ADDR((mg_addr)best->st_value))) best = s;
    }
    if (!best) return NULL;
    /* A symbol with a size only names addresses inside it. */
    if (best->st_size && off >= MG_CODE_ADDR((mg_addr)best->st_value) + best->st_size) return NULL;
    if (sym_addr) *sym_addr = lib->base + MG_CODE_ADDR((mg_addr)best->st_value);
    return lib->strtab + best->st_name;
}

int mg_ld_iterate(mg_ld_phdr_cb cb, void *user)
{
    mg_lib *l;
    int r = 0;
    for (l = g_libs; l && !r; l = l->next) r = cb((mg_addr)l->base, l->name, l->phdr, (unsigned)l->phnum, user);
    return r;
}

const void *mg_ld_find_exidx(mg_addr pc, int *count)
{
    mg_lib *l = mg_ld_lib_of((const void *)pc);
    if (!l || !l->exidx) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = l->exidx_count;
    return l->exidx;
}

/* ------------------------------------------------------------- guest code */

/*
 * Off 32-bit ARM, guest code reads its thread pointer straight from the slot the patched reads load it from (x86/, arm64/)
 * -- ARM64's code its stack guard too, at TPIDR_EL0 + 0x28 in every function with an array on its stack -- and the slot is
 * empty on a thread that has not been given its guest block: a Java thread calling its first native method, a host
 * thread with a callback. So the block is made here, before such a thread's first call into guest code. (32-bit ARM's
 * code reads its guard from a global; its threads are as they were.)
 */
mg_addr mg_code_entry(mg_addr fn)
{
#if !MG_ARCH_ARM32 && MG_GUEST_CODE
    if (!mg_guest_tp_get()) mg_tls_thread_pointer();
#endif
    return fn;
}

/*
 * A rewritten PLT entry is one ARM instruction, `ldr pc, [pc, #-4]`, and the
 * address it loads. If the thread is interrupted exactly there, Windows
 * resumes it in Thumb state, where those bytes are an undefined instruction
 * (see arm/mg_patch.c). Finish the jump the instruction was about to make.
 */
bool mg_ld_plt_target(mg_addr pc, mg_addr *target)
{
    mg_lib *lib = mg_ld_lib_of((const void *)pc);
    int i;
    if (!MG_ARCH_ARM32 || !lib || (pc & 3)) return false;
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        const u8 *lo = lib->base + p->p_vaddr;
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if ((const u8 *)pc < lo || (const u8 *)pc + 8 > lo + p->p_filesz) continue;
        if (mg_rd32((const void *)pc) != 0xE51FF004u) return false;
        *target = mg_rd32((const void *)(pc + 4));
        return true;
    }
    return false;
}

int mg_ld_after_call(mg_addr lr)
{
    mg_lib *lib = mg_ld_lib_of((const void *)lr);
    mg_addr at = lr & ~(mg_addr)1;
    int i;
    /* (Its islands are the loader's code, which calls into the core: not known.) */
    if (!MG_ARCH_ARM32 || !lib || at < (mg_addr)lib->base + lib->vmin || at >= (mg_addr)lib->base + lib->vmax) return -1;
    for (i = 0; i < lib->phnum; i++) {
        const ElfW(Phdr) *p = &lib->phdr[i];
        mg_addr lo = (mg_addr)lib->base + p->p_vaddr;
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X) || at < lo + 4 || at > lo + p->p_filesz) continue;
        if (lr & 1) {
            /* Thumb: BL or BLX, 11110... then 11x1x... or 11x0x...; or the 16-bit BLX of a register, 010001111xxxx000. */
            u16 h1 = mg_rd16((const void *)(at - 4)), h2 = mg_rd16((const void *)(at - 2));
            return ((h1 & 0xF800) == 0xF000 && (h2 & 0xC000) == 0xC000) || (h2 & 0xFF87) == 0x4780;
        } else {
            /*
             * ARM: BL (cond 1011), BLX of an immediate (1111101x), BLX of a register (cond 0001 0010 1111 1111 1111 0011);
             * or any jump after `mov lr, pc`, ARMv4's call through a register.
             */
            u32 w = mg_rd32((const void *)(at - 4));
            if (at >= lo + 8 && mg_rd32((const void *)(at - 8)) == 0xE1A0E00Fu) return 1;
            if ((w >> 28) == 0xF) return (w & 0xFE000000u) == 0xFA000000u;
            return (w & 0x0F000000u) == 0x0B000000u || (w & 0x0FFFFFF0u) == 0x012FFF30u;
        }
    }
    return 0;
}
