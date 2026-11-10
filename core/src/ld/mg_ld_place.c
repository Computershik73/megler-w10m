/*
 * The app's native libraries as files of their own, where Android's installer extracts them: ApplicationInfo.nativeLibraryDir,
 * <data>/lib here (drivers/mg_java.c, place_libraries). The class loader looks there (BaseDexClassLoader.findLibrary opens the
 * file to see that it is there), and so do apps: loaders that read a library's ELF header to load what it needs first
 * (Facebook's SoLoader), loaders that sum the files against the APK's entries (ByteDance's librarian), code that lists the
 * folder. The library that runs is still the APK's, taken from there by the linker.
 *
 * Android's installer makes the folder anew at each install and update, so it holds this version's libraries and nothing
 * else. Up to 0.1.0.685 a file was written only when it was not there or not of the entry's size: an update whose library
 * kept its size (page-aligned builds often do) left the old one, and a library the update no longer had stayed for good.
 * TikTok Lite 47.7.4 moved libnpth.so out of its APK into an archive it unpacks itself (Zoin's tt_lib) and loads the file it
 * unpacked with System.load; the older version's libnpth.so still in the folder was found by its name and loaded instead, and
 * its natives were not the new Java's: the app ended at its start with an UnsatisfiedLinkError.
 */
#include "mg_ld.h"

#include <stdio.h>

#include "../host/mg_host.h"
#include "../zip/mg_zip.h"

/* One of the APKs' libraries: its name (in the entry's, past lib/<abi>/) and the entry of the first APK that has it. */
typedef struct lib_of {
    const char *name;
    const mg_zip *zip;
    const mg_zip_entry *e;
    bool placed;                       /* its file holds the entry's bytes: the index may say so */
} lib_of;

typedef struct libs_of {
    lib_of *v;
    int n, cap;
    size_t skip;
    const mg_zip *zip;
} libs_of;

static void lib_seen(const mg_zip_entry *e, void *user)
{
    libs_of *l = (libs_of *)user;
    const char *name = e->name + l->skip;
    size_t n = strlen(e->name);
    int i;
    if (n <= l->skip + 3 || strcmp(e->name + n - 3, ".so") || strchr(name, '/')) return;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->v[i].name, name)) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = (lib_of *)mg_realloc(l->v, sizeof(lib_of) * (size_t)l->cap);
    }
    l->v[l->n].name = name;
    l->v[l->n].zip = l->zip;
    l->v[l->n].e = e;
    l->v[l->n].placed = false;
    l->n++;
}

/* ------------------------------------------------------------------ index */

/* What the last placing wrote: a line of this, then one a library, "<its CRC-32 in hex> <its size> <its name>". */
#define INDEX_HEAD "megler placed libraries 1\n"

typedef struct index_line {
    char *name;
    u32 crc;
    u64 size;
} index_line;

static int read_index(const char *path, index_line **out)
{
    size_t n = 0, head = strlen(INDEX_HEAD);
    char *text = path ? (char *)mg_file_read_all(path, &n) : NULL, *p;
    int count = 0, cap = 0;
    *out = NULL;
    if (!text) return 0;
    if (n < head || strncmp(text, INDEX_HEAD, head)) {
        free(text);
        return 0;
    }
    for (p = text + head; *p;) {
        char *end = strchr(p, '\n');
        unsigned long crc = 0;
        unsigned long long size = 0;
        int used = 0;
        if (end) *end = 0;
        if (sscanf(p, "%8lx %llu %n", &crc, &size, &used) == 2 && used > 0 && p[used]) {
            if (count == cap) {
                cap = cap ? cap * 2 : 32;
                *out = (index_line *)mg_realloc(*out, sizeof(index_line) * (size_t)cap);
            }
            (*out)[count].name = mg_strdup(p + used);
            (*out)[count].crc = (u32)crc;
            (*out)[count].size = (u64)size;
            count++;
        }
        if (!end) break;
        p = end + 1;
    }
    free(text);
    return count;
}

static void write_index(const char *path, const libs_of *libs)
{
    size_t cap = 64 + (size_t)libs->n * 300, n;
    char *text = (char *)mg_alloc(cap), *dir = mg_strdup(path), *slash = strrchr(dir, '/'), *back = strrchr(dir, '\\'), *part;
    int i, err = 0;
    if (back && (!slash || back > slash)) slash = back;
    if (slash) {
        *slash = 0;
        mg_path_mkdirs(dir);
    }
    n = (size_t)snprintf(text, cap, "%s", INDEX_HEAD);
    for (i = 0; i < libs->n && n < cap; i++)
        if (libs->v[i].placed)
            n += (size_t)snprintf(text + n, cap - n, "%08x %llu %s\n", (unsigned)libs->v[i].e->crc, (unsigned long long)libs->v[i].e->usize,
                                  libs->v[i].name);
    /* Whole or not at all: a new file put over the old one. */
    part = mg_sprintf("%s.part", path);
    if (n >= cap || !mg_file_write_all(part, text, n) || !mg_path_rename(part, path, &err)) {
        mg_warn("ld: the index of the app's placed libraries could not be written (%s)", path);
        mg_path_remove(part, NULL);
    }
    free(part);
    free(dir);
    free(text);
}

/* ------------------------------------------------------------------ files */

typedef struct names_of {
    char **v;
    int n, cap;
} names_of;

static MG_HOSTCB void file_seen(const char *name, bool is_dir, void *user)
{
    names_of *f = (names_of *)user;
    size_t n = strlen(name);
    if (is_dir || n < 3 || strcmp(name + n - 3, ".so")) return;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 32;
        f->v = (char **)mg_realloc(f->v, sizeof(char *) * (size_t)f->cap);
    }
    f->v[f->n++] = mg_strdup(name);
}

/* Whether a file holds the entry's bytes, read back: its size and CRC-32. */
static bool holds_entry(const lib_of *l, const char *path)
{
    u32 crc = 0;
    u64 size = 0;
    return mg_zip_file_crc(path, &crc, &size) && crc == l->e->crc && size == l->e->usize;
}

/*
 * A library's file written from its entry, a piece at a time, and read back: the index will say it holds the entry's bytes,
 * and a Lumia 650 was seen to change a bit of what it copied (0.1.0.668). An empty file where it cannot be written.
 */
static bool write_lib(const lib_of *l, const char *path)
{
    char err[160] = "";
    int tries;
    for (tries = 0; tries < 2; tries++) {
        /* (The second time, a file in the way that may not be written -- made read-only -- is removed first.) */
        if (tries) mg_path_remove(path, NULL);
        if (!mg_zip_extract(l->zip, l->e, path, NULL, NULL, err, sizeof(err))) continue;
        if (holds_entry(l, path)) return true;
        snprintf(err, sizeof(err), "what was written does not read back as the APK's entry");
    }
    mg_warn("ld: %s could not be written to the app's library folder (%s)", l->name, err);
    /* An empty file still says "it is there" to the class loader; the library that runs is the APK's all the same. */
    mg_path_remove(path, NULL);
    mg_file_write_all(path, "", 0);
    return false;
}

bool mg_ld_place_libs(const struct mg_zip *const *apks, int napks, const char *abi, const char *dir, const char *index, mg_ld_placed *out)
{
    libs_of libs;
    names_of files;
    index_line *had = NULL;
    char prefix[48];
    int nhad, i, k, trusted = 0;
    bool changed = false;
    memset(out, 0, sizeof(*out));
    memset(&libs, 0, sizeof(libs));
    memset(&files, 0, sizeof(files));
    if (!dir || !dir[0] || !mg_path_mkdirs(dir)) return false;
    if (abi) {
        snprintf(prefix, sizeof(prefix), "lib/%s/", abi);
        libs.skip = strlen(prefix);
        for (i = 0; i < napks; i++) {
            if (!apks[i]) continue;
            libs.zip = apks[i];
            mg_zip_each(apks[i], prefix, lib_seen, &libs);
        }
    }
    out->libs = libs.n;
    /*
     * What no APK has any more goes first, before anything is written: a name that differs from an older file's in its case
     * alone is then made anew with its own, on Windows too, where the two are one file.
     */
    mg_path_list(dir, file_seen, &files, NULL);
    for (i = 0; i < files.n; i++) {
        char *path;
        for (k = 0; k < libs.n && strcmp(libs.v[k].name, files.v[i]); k++) { }
        if (k < libs.n) continue;
        path = mg_sprintf("%s/%s", dir, files.v[i]);
        if (mg_path_remove(path, NULL)) {
            /* Their names for the log, as many as there is room for, then "...". */
            size_t used = strlen(out->removed_names);
            if (!(used >= 3 && !strcmp(out->removed_names + used - 3, "..."))) {
                const char *say = used + strlen(files.v[i]) + 8 < sizeof(out->removed_names) ? files.v[i] : "...";
                snprintf(out->removed_names + used, sizeof(out->removed_names) - used, "%s%s", used ? ", " : "", say);
            }
            out->removed++;
        }
        free(path);
    }
    nhad = read_index(index, &had);
    for (i = 0; i < libs.n; i++) {
        lib_of *l = &libs.v[i];
        char *path = mg_sprintf("%s/%s", dir, l->name);
        mg_stat st;
        bool same = false;
        if (mg_path_stat(path, &st, NULL) && !st.is_dir && st.size == l->e->usize) {
            for (k = 0; k < nhad && strcmp(had[k].name, l->name); k++) { }
            if (k < nhad) {
                /* What the index says was written there: the entry's bytes when its CRC-32 and size are the entry's. */
                same = had[k].crc == l->e->crc && had[k].size == l->e->usize;
                trusted += same;
            } else {
                /* A file the index does not name (Megler's of before the index, or the index lost): summed, once. */
                out->summed++;
                same = holds_entry(l, path);
                changed = true;
            }
        }
        if (same) {
            l->placed = true;
        } else {
            changed = true;
            if (write_lib(l, path)) {
                l->placed = true;
                out->written++;
            } else {
                out->failed++;
            }
        }
        free(path);
    }
    /* Written again only when it would say something else: a start that changed nothing writes nothing. */
    if (index && (changed || trusted != nhad)) write_index(index, &libs);
    for (i = 0; i < nhad; i++) free(had[i].name);
    free(had);
    for (i = 0; i < files.n; i++) free(files.v[i]);
    free(files.v);
    free(libs.v);
    return true;
}
