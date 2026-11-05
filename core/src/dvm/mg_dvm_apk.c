/* DEX files out of an APK or a jar. Apart from the rest of the runtime because this alone needs the zip reader, and with it zlib. */
#include "mg_dvm_internal.h"

#include <stdio.h>

#include "../zip/mg_zip.h"

/*
 * DEX files already read, by what their zip entries say of them (CRC-32 and size): the same file read again for another
 * scope -- an app's own APK for a process of its own (megler.Processes: Play Store's :background), a module's APK for a
 * second loader -- is given the bytes read before, which the runtime only reads (mg_dvm_dex_open takes them const) and
 * keeps for as long as the process lives. Play Store 53's :background process had a copy of its own of Play Store's eight
 * DEX files, tens of megabytes, in an address space that Google Play services 26 and the WebView fill too.
 */
typedef struct read_dex {
    u32 crc;
    u64 size;
    u8 *data;
    size_t len;
} read_dex;
static read_dex g_read[128];
static int g_nread;
static mg_lock g_read_lock = MG_LOCK_INIT;
/* The DEX files' bytes: read into memory of the process's, and views of APKs (view_of, below). */
static u64 g_bytes_read, g_bytes_viewed;

void mg_dvm_dex_viewed(u64 bytes)
{
    mg_lock_acquire(&g_read_lock);
    g_bytes_viewed += bytes;
    mg_lock_release(&g_read_lock);
}

void mg_dvm_dex_memory(u64 *read, u64 *viewed)
{
    mg_lock_acquire(&g_read_lock);
    *read = g_bytes_read;
    *viewed = g_bytes_viewed;
    mg_lock_release(&g_read_lock);
}

static u8 *read_before(const mg_zip_entry *e, size_t *len)
{
    u8 *found = NULL;
    int i;
    mg_lock_acquire(&g_read_lock);
    for (i = 0; i < g_nread && !found; i++)
        if (g_read[i].crc == e->crc && g_read[i].size == e->usize) {
            found = g_read[i].data;
            *len = g_read[i].len;
        }
    mg_lock_release(&g_read_lock);
    return found;
}

static void keep_read(const mg_zip_entry *e, u8 *data, size_t len)
{
    mg_lock_acquire(&g_read_lock);
    if (g_nread < (int)MG_COUNT(g_read)) {
        g_read[g_nread].crc = e->crc;
        g_read[g_nread].size = e->usize;
        g_read[g_nread].data = data;
        g_read[g_nread].len = len;
        g_nread++;
    }
    mg_lock_release(&g_read_lock);
}

/*
 * A DEX file the APK stores as it is (not deflated), on a 4-byte boundary as zipalign puts it: a read-only view of those
 * bytes of the APK, which are the file's pages and none of the process's memory (Windows counts the memory an app has
 * committed against its limit; a view of a file it does not). Google Play services 26 stores its fifteen, 109 MB; they
 * were read into memory of the process's in every app that has it. NULL where the host cannot make one; then it is read.
 */
/* A view made here, and how to make it again: the file and where in it the DEX file starts; what mg_section_unmap takes. */
typedef struct dex_view {
    char *path;
    u64 at;
    void *base;
    size_t mapped;
} dex_view;

static u8 *view_of(const mg_zip *z, const mg_zip_entry *e, const char *apk, size_t *size, dex_view *v)
{
    u64 at = 0;
    u8 *p;
    if (e->method != 0 || !e->usize || e->usize > ((u64)256 << 20) || !mg_zip_stored_at(z, e, &at) || (at & 3)) return NULL;
    p = (u8 *)mg_file_map(z->file, at, (size_t)e->usize, &v->base, &v->mapped);
    if (p) {
        *size = (size_t)e->usize;
        v->path = mg_strdup(apk);
        v->at = at;
    }
    return p;
}

const char *megler_data_dir(void);

/* The other files of the folder for the same APK's same entry (`apk`-...-`entry`-...), but `keep` and one being made: an
   older version's, or one named the old way with a hash of the APK's path, removed. (One still viewed is not removed: Windows
   refuses, and it goes another time.) */
typedef struct older_files {
    const char *dir, *apk, *entry, *keep;
} older_files;

static MG_HOSTCB void remove_older(const char *name, bool is_dir, void *user)
{
    const older_files *o = (const older_files *)user;
    size_t n = strlen(o->apk);
    char mid[80], *path;
    int err = 0;
    snprintf(mid, sizeof(mid), "-%s-", o->entry);
    if (is_dir || strncmp(name, o->apk, n) || name[n] != '-' || !strstr(name + n, mid) || !strcmp(name, o->keep) || strstr(name, ".part")) return;
    path = mg_sprintf("%s/%s", o->dir, name);
    mg_path_remove(path, &err);
    free(path);
}

/*
 * A DEX file the APK deflates: inflated once into a file of its own, <data_dir>/dex/<APK>-<entry>-<its CRC-32>-<its size>,
 * and viewed from there, as one the APK stores as it is (view_of): the file's pages, not the process's memory. Android keeps
 * an app's DEX files outside its APK so too (ART's vdex). Play Store 53 deflates its eight, 58 MB. Made under another name
 * and renamed when it is whole; the other files of the same APK and entry (an older version's) are removed. The name has
 * no hash of the APK's path, which has Megler's version in it for the APKs Megler brings: one was named with it, and every
 * update of Megler made another copy (four of Facebook's, of Play Store's 58 MB each). NULL where there is no data folder or
 * the file cannot be made or viewed: then it is read into memory, as before. `*made` says whether it was made now.
 */
static u8 *cached_view_of(const mg_zip *z, const mg_zip_entry *e, const char *apk, const char *apk_base, const char *entry, size_t *size,
                          bool *made, dex_view *v)
{
    const char *root = megler_data_dir();
    char why[160];
    char *dir, *name, *path, *tmp;
    older_files o;
    mg_stat st;
    mg_file *f;
    u8 *p = NULL;
    int err = 0;
    (void)apk;
    *made = false;
    why[0] = 0;
    if (!root || !root[0] || e->method == 0 || !e->usize || e->usize > ((u64)256 << 20)) return NULL;
    dir = mg_sprintf("%s/dex", root);
    name = mg_sprintf("%.100s-%s-%08x-%llu", apk_base, entry, (unsigned)e->crc, (unsigned long long)e->usize);
    path = mg_sprintf("%s/%s", dir, name);
    o.dir = dir;
    o.apk = apk_base;
    o.entry = entry;
    o.keep = name;
    if (!mg_path_stat(path, &st, &err) || st.size != e->usize) {
        tmp = mg_sprintf("%s.%u.part", path, (unsigned)mg_thread_self_id());
        mg_path_mkdirs(dir);
        if (mg_zip_extract(z, e, tmp, NULL, NULL, why, sizeof(why)) && mg_path_rename(tmp, path, &err)) {
            *made = true;
        } else {
            mg_warn("dvm: %s!%s could not be kept in %s (%s): read into memory", apk_base, entry, dir, why[0] ? why : "renaming it failed");
            mg_path_remove(tmp, &err);
        }
        free(tmp);
    }
    mg_path_list(dir, remove_older, &o, &err);
    f = mg_file_open(path, MG_O_READ, &err);
    if (f) {
        if (mg_file_stat(f, &st) && st.size == e->usize) p = (u8 *)mg_file_map(f, 0, (size_t)e->usize, &v->base, &v->mapped);
        mg_file_close(f);
    }
    if (p) {
        *size = (size_t)e->usize;
        v->path = path;
        v->at = 0;
        path = NULL;
    }
    free(path);
    free(name);
    free(dir);
    return p;
}

/*
 * The names of a DEX file's classes kept in the data folder's dex-names, as <APK>-<entry>-<its checksum>-<its size>: the pool
 * its registration keeps (mg_dvm_dex_names), read in one go at the next start rather than a page of the file's tables here and
 * one of its strings there. Registering Yandex Maps' sixteen took 0.75 s of the main thread at every start, Google Play
 * services' fifteen 0.64 s more when the app first asked for them (package 531). The file of an older version of the same APK
 * and entry is removed when the new one is made; one that does not answer the DEX file's header is not taken.
 */
#define NAMES_MAGIC 0x4E44474Du /* "MGDN" */
typedef struct names_head {
    u32 magic, version, checksum, size, ncls, pool;
} names_head;

static char *names_path(const char *apk_base, const char *entry, const u8 *dex, char **dir)
{
    const char *root = megler_data_dir();
    *dir = NULL;
    if (!root || !root[0]) return NULL;
    *dir = mg_sprintf("%s/dex-names", root);
    return mg_sprintf("%s/%.100s-%s-%08x-%u", *dir, apk_base, entry, (unsigned)mg_dvm_rd32(dex + 8), (unsigned)mg_dvm_rd32(dex + 32));
}

static char *names_kept(const char *path, const u8 *dex, size_t *pool_size)
{
    size_t n = 0, i;
    u32 ends = 0;
    u8 *f;
    names_head h;
    char *pool;
    *pool_size = 0;
    if (!path || !(f = mg_file_read_all(path, &n))) return NULL;
    memset(&h, 0, sizeof(h));
    if (n > sizeof(h)) memcpy(&h, f, sizeof(h));
    if (h.magic != NAMES_MAGIC || h.version != 1 || h.checksum != mg_dvm_rd32(dex + 8) || h.size != mg_dvm_rd32(dex + 32) || h.ncls != mg_dvm_rd32(dex + 0x60)
        || (size_t)h.pool != n - sizeof(h) || f[n - 1] != 0) {
        free(f);
        return NULL;
    }
    for (i = sizeof(h); i < n; i++) ends += f[i] == 0;
    if (ends != (h.ncls ? h.ncls : 1)) {
        free(f);
        return NULL;
    }
    pool = (char *)mg_dvm_meta_alloc_kind(h.pool, DVM_META_POOL);
    memcpy(pool, f + sizeof(h), h.pool);
    free(f);
    *pool_size = h.pool;
    return pool;
}

static bool names_keep(const char *dir, const char *path, const char *apk_base, const char *entry, const u8 *dex, const char *pool, size_t pool_size)
{
    names_head h;
    older_files o;
    const char *slash = strrchr(path, '/');
    char *tmp = mg_sprintf("%s.%u.part", path, (unsigned)mg_thread_self_id());
    u8 *buf = (u8 *)malloc(sizeof(h) + pool_size);
    bool kept = false;
    int err = 0;
    h.magic = NAMES_MAGIC;
    h.version = 1;
    h.checksum = mg_dvm_rd32(dex + 8);
    h.size = mg_dvm_rd32(dex + 32);
    h.ncls = mg_dvm_rd32(dex + 0x60);
    h.pool = (u32)pool_size;
    if (buf) {
        memcpy(buf, &h, sizeof(h));
        memcpy(buf + sizeof(h), pool, pool_size);
        mg_path_mkdirs(dir);
        kept = mg_file_write_all(tmp, buf, sizeof(h) + pool_size) && mg_path_rename(tmp, path, &err);
        if (!kept) mg_path_remove(tmp, &err);
    }
    free(buf);
    free(tmp);
    if (kept) {
        o.dir = dir;
        o.apk = apk_base;
        o.entry = entry;
        o.keep = slash ? slash + 1 : path;
        mg_path_list(dir, remove_older, &o, &err);
    }
    return kept;
}

int mg_dvm_add_apk_scope(const char *path, int scope)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\'), *base;
    mg_zip z;
    char err[160];
    int added = 0, shared = 0, viewed = 0, kept = 0, made_now = 0, names_read = 0, names_made = 0, i;
    if (back && (!slash || back > slash)) slash = back;
    base = slash ? slash + 1 : path;
    if (!mg_zip_open(&z, path, err, sizeof(err))) {
        mg_warn("dvm: %s: %s", path, err);
        return 0;
    }
    /* classes.dex, classes2.dex, classes3.dex... with no gap, as Android reads them. */
    for (i = 1; i < 100; i++) {
        const mg_zip_entry *e;
        char name[32], label[96];
        size_t size = 0;
        u8 *data;
        bool before, view;
        dex_view v;
        if (i == 1) snprintf(name, sizeof(name), "classes.dex");
        else snprintf(name, sizeof(name), "classes%d.dex", i);
        e = mg_zip_find(&z, name);
        if (!e) break;
        /*
         * A view of the APK where it stores the file as it is, else read whole into memory of its own: the classes are used
         * from there for as long as the process lives.
         */
        memset(&v, 0, sizeof(v));
        data = read_before(e, &size);
        before = data != NULL;
        view = !data && (data = view_of(&z, e, path, &size, &v)) != NULL;
        if (!data) {
            bool made = false;
            view = (data = cached_view_of(&z, e, path, base, name, &size, &made, &v)) != NULL;
            if (view) {
                kept++;
                if (made) made_now++;
            }
        }
        if (!data) data = mg_zip_read(&z, e, (size_t)256 << 20, &size);
        if (!data) {
            mg_warn("dvm: %s: %s cannot be read", path, name);
            continue;
        }
        snprintf(label, sizeof(label), "%.60s!%s", base, name);
        /*
         * A view: its classes registered by their names, and the view let go; the file is viewed again when the first of its
         * classes is linked (mg_dvm_dex_ready), and not at all when none is. A view the runtime would not take as such is read
         * as before.
         */
        if (view) {
            /* (The same file registered for another scope already: the two share one view, once either is needed, and its names.) */
            bool known = mg_dvm_dex_known(v.path, v.at, size), lazy;
            char *names = NULL, *names_file = NULL, *names_dir = NULL;
            size_t names_size = 0;
            if (!known && size >= 0x70) {
                names_file = names_path(base, name, data, &names_dir);
                names = names_kept(names_file, data, &names_size);
                if (names) {
                    names_read++;
                } else if ((names = mg_dvm_dex_names(data, size, &names_size)) != NULL && names_file
                           && names_keep(names_dir, names_file, base, name, data, names, names_size)) {
                    names_made++;
                }
            }
            free(names_file);
            free(names_dir);
            lazy = mg_dvm_add_dex_lazy(data, size, label, scope, v.path, v.at, names, names_size);
            if (!lazy && names) mg_dvm_meta_free_kind(names, names_size, DVM_META_POOL);
            mg_section_unmap(v.base, v.mapped);
            free(v.path);
            if (lazy) {
                added++;
                if (known) shared++;
                else viewed++;
                continue;
            }
            view = false;
            data = mg_zip_read(&z, e, (size_t)256 << 20, &size);
            if (!data) continue;
        }
        if (scope ? mg_dvm_add_dex_scope(data, size, label, scope) : mg_dvm_add_dex(data, size, label)) {
            added++;
            if (before) shared++;
            else keep_read(e, data, size);
            if (view) viewed++;
            if (!before) {
                mg_lock_acquire(&g_read_lock);
                if (view) g_bytes_viewed += size;
                else g_bytes_read += size;
                mg_lock_release(&g_read_lock);
            }
        } else if (!before && !view) {
            /* (A view the runtime would not take stays mapped: rare, and address space is all it costs.) */
            free(data);
        }
    }
    mg_zip_close(&z);
    if (shared) mg_info("dvm: %s: %d of its DEX files are the ones read before, shared", base, shared);
    if (viewed) mg_info("dvm: %s: %d of its DEX files are views of files, each viewed once a class of it is needed: %d of the APK, which stores "
                        "them as they are, %d inflated into the data folder's dex (%d of them now); the names of their classes: %d from the data "
                        "folder's dex-names, %d kept there now", base, viewed, viewed - kept, kept, made_now, names_read, names_made);
    return added;
}

int mg_dvm_add_apk(const char *path) { return mg_dvm_add_apk_scope(path, 0); }
