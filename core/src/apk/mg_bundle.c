/*
 * Bundles of split APKs -- .apks (SAI's, bundletool's, APKPure's) and .xapk (APKPure's) -- unpacked into the base APK and
 * the splits this phone can use, as an installer picks what to install of what Google Play offers (meglet.h,
 * meglet_bundle_unpack).
 *
 * Which APK in a bundle is which is said by their manifests: the base names no split, a split names itself
 * (split="config.armeabi_v7a"). A split for a processor, a screen density or a language is named after what it is for,
 * by bundletool's convention, and the bundles' file names keep it: config.arm64_v8a.apk (APKPure), split_config.arm64_v8a.apk
 * (SAI, and Android's own name for an installed split), splits/base-arm64_v8a.apk (bundletool). So a split that is not
 * wanted is left out by its file's name, without being unpacked first -- BiP's three splits for other processors are
 * 130 MB -- and one that is wanted is unpacked and its manifest read: the name the manifest gives is the one that counts
 * (a split it turns out not to want is removed again), a split of another app is left out, and so is a second base.
 *
 * Everything in a bundle is untrusted: names are checked before they become paths, and nothing is written outside the
 * three places the caller and the layer name (the base's path, the folder of splits, the app's own storage).
 */
#include "mg_bundle.h"

#include <stdio.h>

#include "../../include/meglet.h"
#include "../host/mg_host.h"
#include "../run/mg_run.h"
#include "../zip/mg_zip.h"
#include "mg_apk.h"
#include "mg_json.h"

/* What a split is for, by its name. */
enum { K_BASE, K_ABI, K_DENSITY, K_LANGUAGE, K_CONFIG, K_PART, K_STANDALONE };

#define MAX_ITEMS 256
/* As many splits as are kept: the Java driver reads ten (core/src/drivers/mg_java.c, "2 to 11 the app's splits"). */
#define MAX_KEPT 10

typedef struct item {
    const mg_zip_entry *e;
    char name[96];             /* the split's name as its file gives it ("config.arm64_v8a"); "" when it gives none */
    int kind;
    char abi[24];              /* K_ABI: as Android names it ("arm64-v8a") */
    int density;               /* K_DENSITY: dots an inch */
    char lang[16];             /* K_LANGUAGE */
    bool keep, is_base;
    bool tried;                /* unpacked to see whether it is the base, and it was not */
    char why[160];             /* why it was left out */
    char file[120];            /* where it went in the folder of splits */
} item;

typedef struct ctx {
    mg_zip zip;
    item *items;
    int nitems;
    mg_apk_info *base, *part;
    char package[160];
    /* manifest.json, when there is one (an .xapk; some .apks) */
    char *mjson;
    size_t mjson_size;
    /* progress */
    void (*progress)(void *user, uint64_t done, uint64_t total);
    void *user;
    u64 done, total, piece_from;
    /* what was written, to take away again if it does not come to a whole */
    char *written[MAX_ITEMS + 16];
    int nwritten;
    char error[300];
    /* the rule for what is kept (policy) */
    char langs[16][16];
    int nlangs, above, below;
    bool v7;
    /* where the splits are written before they take the place of the folder's old ones */
    char *work;
} ctx;

/* ------------------------------------------------------------ names */

static bool ends_with_ci(const char *s, const char *tail)
{
    size_t n = strlen(s), t = strlen(tail), i;
    if (n < t) return false;
    for (i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}

static const char *last_part(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/*
 * The split's name a file in a bundle stands for, by bundletool's convention: "config.arm64_v8a.apk",
 * "split_config.arm64_v8a.apk" and bundletool's own "splits/base-arm64_v8a.apk" are all "config.arm64_v8a";
 * "splits/feature-xxhdpi.apk" is "feature.config.xxhdpi" and "splits/feature-master.apk" is "feature". "" for a name
 * that says nothing (the base, mostly).
 */
static void split_name_of(const char *entry, char *out, size_t cap)
{
    char name[200];
    const char *dash;
    size_t n;
    out[0] = 0;
    mg_strlcpy(name, last_part(entry), sizeof(name));
    n = strlen(name);
    if (n > 4 && ends_with_ci(name, ".apk")) name[n - 4] = 0;
    if (!strncmp(entry, "splits/", 7) && (dash = strrchr(name, '-')) != NULL) {
        char module[100];
        size_t m = (size_t)(dash - name);
        if (m >= sizeof(module)) m = sizeof(module) - 1;
        memcpy(module, name, m);
        module[m] = 0;
        if (!strcmp(dash + 1, "master")) {
            if (strcmp(module, "base")) mg_strlcpy(out, module, cap);
        } else if (!strcmp(module, "base")) {
            snprintf(out, cap, "config.%s", dash + 1);
        } else {
            snprintf(out, cap, "%s.config.%s", module, dash + 1);
        }
        return;
    }
    if (!strncmp(name, "split_", 6)) memmove(name, name + 6, strlen(name + 6) + 1);
    if (!strcmp(name, "base")) return;
    mg_strlcpy(out, name, cap);
}

/* What a split of that name is for (its kind), and the processor, density or language it is for. */
static int kind_of(const char *split, item *it)
{
    static const struct { const char *token, *abi; } abis[] = {
        { "armeabi_v7a", "armeabi-v7a" }, { "arm64_v8a", "arm64-v8a" }, { "armeabi", "armeabi" }, { "x86_64", "x86_64" },
        { "x86", "x86" }, { "mips64", "mips64" }, { "mips", "mips" }, { "riscv64", "riscv64" },
    };
    static const struct { const char *token; int dpi; } dens[] = {
        { "ldpi", 120 }, { "mdpi", 160 }, { "tvdpi", 213 }, { "hdpi", 240 }, { "xhdpi", 320 }, { "xxhdpi", 480 }, { "xxxhdpi", 640 },
    };
    const char *config = NULL, *p;
    char q[64];
    size_t n, i;
    for (p = split; (p = strstr(p, "config.")) != NULL; p++) config = p;
    if (!split[0]) return K_BASE;
    if (!config) return K_PART;
    mg_strlcpy(q, config + 7, sizeof(q));
    /* aapt2 names the splits it makes with the qualifier's API level: config.xxhdpi_v4. */
    n = strlen(q);
    for (i = n; i > 0 && q[i - 1] >= '0' && q[i - 1] <= '9'; i--) { }
    if (i < n && i >= 2 && q[i - 1] == 'v' && q[i - 2] == '_') q[i - 2] = 0;
    for (i = 0; i < MG_COUNT(abis); i++)
        if (!strcmp(q, abis[i].token)) {
            mg_strlcpy(it->abi, abis[i].abi, sizeof(it->abi));
            return K_ABI;
        }
    for (i = 0; i < MG_COUNT(dens); i++)
        if (!strcmp(q, dens[i].token)) {
            it->density = dens[i].dpi;
            return K_DENSITY;
        }
    /* A language: two or three small letters, with a region after an underscore at most (config.zh_TW). */
    for (i = 0; q[i] >= 'a' && q[i] <= 'z'; i++) { }
    if ((i == 2 || i == 3) && (!q[i] || q[i] == '_')) {
        memcpy(it->lang, q, i);
        it->lang[i] = 0;
        return K_LANGUAGE;
    }
    return K_CONFIG;
}

/* Whether a name can be a file's in a folder of the host's, as it is: no separators, nothing hidden, nothing odd. */
static bool safe_name(const char *s)
{
    size_t n = strlen(s), i;
    if (!n || n > 100 || s[0] == '.') return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    }
    return true;
}

/* A path inside the app's own storage: parts of names, none of them empty, "." or "..", no backslash or colon. */
static bool safe_rest(const char *s)
{
    const char *part = s;
    if (!*s || *s == '/') return false;
    for (;;) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part), i;
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        for (i = 0; i < n; i++)
            if (part[i] == '\\' || part[i] == ':' || (unsigned char)part[i] < 32) return false;
        if (!end) return true;
        part = end + 1;
    }
}

/* ------------------------------------------------------------ writing */

static void piece_written(void *user, u64 written)
{
    ctx *c = (ctx *)user;
    if (c->progress) c->progress(c->user, c->piece_from + written, c->total);
}

/* An entry out to a file: progress told, the file remembered to take away again on failure. */
static bool extract(ctx *c, const mg_zip_entry *e, const char *path)
{
    char err[160];
    c->piece_from = c->done;
    if (!mg_zip_extract(&c->zip, e, path, piece_written, c, err, sizeof(err))) {
        snprintf(c->error, sizeof(c->error), "%s could not be unpacked: %s", e->name, err);
        return false;
    }
    c->done += e->usize;
    if (c->nwritten < (int)MG_COUNT(c->written)) c->written[c->nwritten++] = mg_strdup(path);
    return true;
}

/* A file written in this call is no longer there (renamed or removed): it is not to be taken away. */
static void forget_written(ctx *c, const char *path)
{
    int i;
    for (i = 0; i < c->nwritten; i++)
        if (!strcmp(c->written[i], path)) {
            free(c->written[i]);
            c->written[i] = c->written[--c->nwritten];
            return;
        }
}

static bool move_into_place(ctx *c, const char *from, const char *to)
{
    int err = 0;
    if (!mg_path_rename(from, to, &err)) {
        snprintf(c->error, sizeof(c->error), "%s could not be put in place (error %d)", last_part(to), err);
        return false;
    }
    forget_written(c, from);
    if (c->nwritten < (int)MG_COUNT(c->written)) c->written[c->nwritten++] = mg_strdup(to);
    return true;
}

/* What a folder of splits held before: removed, so that a split of an older version is not read with a newer base. */
static void clear_one(const char *name, bool is_dir, void *user)
{
    const char *dir = (const char *)user;
    char *path;
    if (is_dir) return;
    if (!ends_with_ci(name, ".apk") && !ends_with_ci(name, ".part") && strcmp(name, MG_BUNDLE_RECORD) && strcmp(name, MG_BUNDLE_ICON)) return;
    path = mg_sprintf("%s/%s", dir, name);
    mg_path_remove(path, NULL);
    free(path);
}

/* ------------------------------------------------------------ manifest.json */

/* manifest.json's entry for a file of the bundle: its "id" ("base", "config.ru"), from "split_apks". */
static bool mjson_split_id(const ctx *c, const char *file, char *id, size_t cap)
{
    const char *end = c->mjson + c->mjson_size, *list, *at = NULL;
    if (!c->mjson || !(list = mg_json_member(c->mjson, end, "split_apks"))) return false;
    while (mg_json_next(list, end, &at)) {
        const char *f = mg_json_member(at, end, "file"), *i = mg_json_member(at, end, "id");
        char name[200];
        if (f && i && mg_json_string(f, end, name, sizeof(name)) && !strcmp(name, file)) return mg_json_string(i, end, id, cap);
    }
    return false;
}

/* --------------------------------------------------------------- the record */

bool mg_bundle_record_read(const char *dir, mg_bundle_record *out)
{
    char *path = mg_sprintf("%s/%s", dir, MG_BUNDLE_RECORD);
    size_t size = 0;
    char *json = (char *)mg_file_read_all(path, &size);
    const char *end, *list, *at = NULL;
    memset(out, 0, sizeof(*out));
    free(path);
    if (!json) return false;
    end = json + size;
    list = mg_json_member(json, end, "leftOut");
    while (list && mg_json_next(list, end, &at)) {
        const char *name = mg_json_member(at, end, "name"), *abi = mg_json_member(at, end, "abi");
        char text[96];
        int i;
        if (name && out->nleft_out < (int)MG_COUNT(out->left_out) && mg_json_string(name, end, text, sizeof(text)) && text[0])
            mg_strlcpy(out->left_out[out->nleft_out++], text, sizeof(out->left_out[0]));
        if (abi && mg_json_string(abi, end, text, sizeof(text)) && text[0]) {
            for (i = 0; i < out->nabis && strcmp(out->abis[i], text); i++) { }
            if (i == out->nabis && out->nabis < (int)MG_COUNT(out->abis)) mg_strlcpy(out->abis[out->nabis++], text, sizeof(out->abis[0]));
        }
    }
    free(json);
    return true;
}

/* ------------------------------------------------------------ choosing */

/* The languages to keep, "en,ru", as a list of their codes; English when none are named. */
static int languages_of(const char *list, char out[][16], int max)
{
    int n = 0;
    const char *p = list && list[0] ? list : "en";
    while (*p && n < max) {
        size_t k = 0;
        while (*p == ',' || *p == ' ') p++;
        while (*p && *p != ',' && *p != ' ' && *p != '-' && *p != '_' && k + 1 < 16) {
            char ch = *p++;
            out[n][k++] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        }
        while (*p && *p != ',') p++;
        out[n][k] = 0;
        if (k) n++;
    }
    return n;
}

/*
 * What is kept of the splits whose names say what they are for. A processor's: 32-bit ARM's alone (armeabi-v7a; armeabi,
 * the older ARMv5, only where there is no v7a), never another's. A density's: the nearest at or above the screen's and
 * the nearest below it, so that a picture is there at the density Android would choose and at the one it falls back to
 * (a bundle with one density keeps that one, whatever it is). A language's: those named. Anything else -- feature splits,
 * asset packs, a split for a texture format -- is kept. The rule is made once, from all the names (policy), and put to
 * each split (decide), again to one whose manifest names it otherwise than its file did.
 */
static void policy(ctx *c, int density_dpi, const char *languages)
{
    int i;
    c->nlangs = languages_of(languages, c->langs, 16);
    c->v7 = false;
    c->above = c->below = 0;
    if (density_dpi <= 0) density_dpi = 160;
    for (i = 0; i < c->nitems; i++) {
        const item *it = &c->items[i];
        if (it->kind == K_ABI && !strcmp(it->abi, "armeabi-v7a")) c->v7 = true;
        if (it->kind == K_DENSITY) {
            if (it->density >= density_dpi && (!c->above || it->density < c->above)) c->above = it->density;
            if (it->density < density_dpi && it->density > c->below) c->below = it->density;
        }
    }
}

static void decide(const ctx *c, item *it)
{
    int k;
    it->why[0] = 0;
    it->keep = true;
    if (it->kind == K_STANDALONE) {
        it->keep = false;
        mg_strlcpy(it->why, "a standalone APK, for Android versions without splits", sizeof(it->why));
    } else if (it->kind == K_ABI) {
        it->keep = !strcmp(it->abi, "armeabi-v7a") || (!c->v7 && !strcmp(it->abi, "armeabi"));
        if (!it->keep)
            snprintf(it->why, sizeof(it->why), strcmp(it->abi, "armeabi") ? "native code for %s, which Windows 10 Mobile cannot run" : "native code for %s, which the armeabi-v7a split stands in for", it->abi);
    } else if (it->kind == K_DENSITY) {
        it->keep = it->density == c->above || it->density == c->below;
        if (!it->keep) snprintf(it->why, sizeof(it->why), "pictures for another screen density (%d dpi)", it->density);
    } else if (it->kind == K_LANGUAGE) {
        it->keep = false;
        for (k = 0; k < c->nlangs && !it->keep; k++) it->keep = !strcmp(it->lang, c->langs[k]);
        if (!it->keep) snprintf(it->why, sizeof(it->why), "the strings of another language (%s)", it->lang);
    }
}

/* ------------------------------------------------------------ the base */

/* Whether an entry may be the base, by its name, before it is unpacked: 2 surely, 1 perhaps, 0 not. */
static int base_by_name(const ctx *c, const item *it)
{
    char id[100];
    const char *name = last_part(it->e->name);
    size_t plen = strlen(c->package);
    if (it->kind == K_STANDALONE) return 0;
    if (mjson_split_id(c, it->e->name, id, sizeof(id))) return !strcmp(id, "base") ? 2 : 0;
    if (!strcmp(it->e->name, "base.apk") || !strcmp(it->e->name, "splits/base-master.apk")) return 2;
    if (plen && strlen(name) == plen + 4 && !strncmp(name, c->package, plen)) return 2;
    return it->kind == K_BASE || it->kind == K_PART ? 1 : 0;
}

/* A standalone APK as the whole app, when there is no base: the one for 32-bit ARM first, then a universal one. */
static int standalone_score(const item *it)
{
    if (it->kind != K_STANDALONE) return 0;
    return 1 + (strstr(it->e->name, "armeabi_v7a") || strstr(it->e->name, "armeabi-v7a") ? 2 : 0) + (strstr(it->e->name, "universal") ? 1 : 0);
}

/*
 * The base: unpacked to the base's path (under .part until all is done) and its manifest read. Those that are surely it by
 * their names are tried first, then the biggest of those that may be; a standalone APK (bundletool's standalones/, a
 * universal.apk) is the whole app in a bundle that has no base.
 */
static item *find_base(ctx *c, const char *part_path)
{
    item *found = NULL;
    int pass, i;
    for (pass = 0; pass < 3 && !found; pass++) {
        for (;;) {
            item *best = NULL;
            for (i = 0; i < c->nitems; i++) {
                item *it = &c->items[i];
                if (it->tried) continue;
                if (pass < 2 ? base_by_name(c, it) != 2 - pass : !standalone_score(it)) continue;
                if (!best || (pass < 2 ? it->e->usize > best->e->usize : standalone_score(it) > standalone_score(best))) best = it;
            }
            if (!best) break;
            best->tried = true;
            if (!extract(c, best->e, part_path)) return NULL;
            if (mg_apk_read_info(part_path, c->base) && !c->base->split[0] && c->base->package[0]) {
                best->is_base = true;
                found = best;
                break;
            }
            /* A split after all, or no APK: it is read again with the others. */
            c->done -= best->e->usize;
            mg_path_remove(part_path, NULL);
            forget_written(c, part_path);
        }
    }
    if (!found && !c->error[0])
        mg_strlcpy(c->error, "there is no base APK in the bundle: every APK in it is a part of an app, and the app itself is not there", sizeof(c->error));
    return found;
}

/* ------------------------------------------------------------ the expansion files */

/*
 * Where an expansion file of the bundle goes: its place under the shared storage, as manifest.json gives it
 * ("install_path": "Android/obb/<package>/main.7.<package>.obb") or the bundle has it, must be the app's own
 * Android/obb or Android/data, which are the app's own folders here (mg_run_own_storage). NULL for anywhere else.
 */
static char *expansion_target(const ctx *c, const char *install_path)
{
    static const char *const parts[] = { "Android/obb/", "Android/data/" };
    size_t i, plen = strlen(c->package);
    char *own, *out;
    while (*install_path == '/') install_path++;
    for (i = 0; i < MG_COUNT(parts); i++) {
        size_t n = strlen(parts[i]);
        const char *rest = install_path + n;
        if (strncmp(install_path, parts[i], n) || strncmp(rest, c->package, plen) || rest[plen] != '/' || !safe_rest(rest)) continue;
        own = mg_run_own_storage(meglet_storage_dir(), c->package);
        out = mg_sprintf("%s/%s", own, install_path);
        free(own);
        return out;
    }
    return NULL;
}

/* The folder a path is in, made. */
static bool make_parent(const char *path)
{
    char *dir = mg_strdup(path), *slash = strrchr(dir, '/');
    bool ok = true;
    if (slash) {
        *slash = 0;
        ok = mg_path_mkdirs(dir);
    }
    free(dir);
    return ok;
}

#define MAX_EXPANSIONS 64

typedef struct expansion {
    const mg_zip_entry *e;
    char install[300];             /* where under the shared storage it is to go */
} expansion;

typedef struct expansions {
    expansion x[MAX_EXPANSIONS];
    int n;
} expansions;

static void expansion_seen(const mg_zip_entry *e, void *user)
{
    expansions *l = (expansions *)user;
    size_t len = strlen(e->name);
    if (l->n >= MAX_EXPANSIONS || !len || e->name[len - 1] == '/') return;
    l->x[l->n].e = e;
    mg_strlcpy(l->x[l->n].install, e->name, sizeof(l->x[0].install));
    l->n++;
}

/* manifest.json's "expansions": [{"file": <in the bundle>, "install_path": <under the shared storage>}, ...]. */
static void expansions_of_manifest(ctx *c, expansions *l)
{
    const char *end = c->mjson + c->mjson_size, *list = c->mjson ? mg_json_member(c->mjson, end, "expansions") : NULL, *at = NULL;
    while (list && mg_json_next(list, end, &at)) {
        const char *f = mg_json_member(at, end, "file"), *p = mg_json_member(at, end, "install_path");
        char file[300], place[300];
        int k;
        if (!f || !p || !mg_json_string(f, end, file, sizeof(file)) || !mg_json_string(p, end, place, sizeof(place))) continue;
        for (k = 0; k < l->n && strcmp(l->x[k].e->name, file); k++) { }
        if (k == l->n) {
            const mg_zip_entry *e = mg_zip_find(&c->zip, file);
            if (!e || l->n >= MAX_EXPANSIONS) continue;
            l->x[k].e = e;
            l->n++;
        }
        mg_strlcpy(l->x[k].install, place, sizeof(l->x[0].install));
    }
}

/* ------------------------------------------------------------ the whole */

static int by_entry_name(const void *a, const void *b) { return strcmp(((const item *)a)->e->name, ((const item *)b)->e->name); }

static void add_entry(const mg_zip_entry *e, void *user)
{
    ctx *c = (ctx *)user;
    item *it;
    if (!ends_with_ci(e->name, ".apk") || c->nitems >= MAX_ITEMS || !strncmp(e->name, "__MACOSX/", 9)) return;
    it = &c->items[c->nitems++];
    memset(it, 0, sizeof(*it));
    it->e = e;
    if (!strncmp(e->name, "standalones/", 12) || !strcmp(last_part(e->name), "universal.apk")) {
        it->kind = K_STANDALONE;
        return;
    }
    split_name_of(e->name, it->name, sizeof(it->name));
    it->kind = kind_of(it->name, it);
}

/* Every file of a folder that meglet_bundle_unpack writes, and the folder itself. */
static void remove_folder(const char *dir)
{
    mg_path_list(dir, clear_one, (void *)dir, NULL);
    mg_path_rmdir(dir, NULL);
}

/* A split, unpacked into the work folder and its manifest read; false only when it could not be written. */
static bool take_split(ctx *c, item *it, int index)
{
    char *tmp = mg_sprintf("%s/%d.part", c->work, index), *final;
    bool ok;
    if (!extract(c, it->e, tmp)) {
        free(tmp);
        return false;
    }
    if (!mg_apk_read_info(tmp, c->part) || strcmp(c->part->package, c->package) || !c->part->split[0]) {
        it->keep = false;
        if (!c->part->package[0]) mg_strlcpy(it->why, "not an APK that can be read", sizeof(it->why));
        else if (strcmp(c->part->package, c->package)) snprintf(it->why, sizeof(it->why), "a part of another app (%s)", c->part->package);
        else mg_strlcpy(it->why, "a second base APK of the same app", sizeof(it->why));
    } else {
        /* The name its manifest gives is the one that counts: chosen again if it says otherwise than the file's name did. */
        int named = it->kind, k;
        mg_strlcpy(it->name, c->part->split, sizeof(it->name));
        it->kind = kind_of(it->name, it);
        if (it->kind != named) decide(c, it);
        /* The same split twice in one bundle (under two names): the first is kept. */
        for (k = 0; k < c->nitems && it->keep; k++)
            if (&c->items[k] != it && c->items[k].keep && c->items[k].file[0] && !strcmp(c->items[k].name, it->name)) {
                it->keep = false;
                snprintf(it->why, sizeof(it->why), "the same split as %s", c->items[k].e->name);
            }
    }
    if (!it->keep) {
        mg_path_remove(tmp, NULL);
        forget_written(c, tmp);
        free(tmp);
        return true;
    }
    snprintf(it->file, sizeof(it->file), "split_%s.apk", it->name);
    if (!safe_name(it->file)) snprintf(it->file, sizeof(it->file), "split_%d.apk", index);
    final = mg_sprintf("%s/%s", c->work, it->file);
    ok = move_into_place(c, tmp, final);
    free(tmp);
    free(final);
    return ok;
}

/* The icon the bundle came with: manifest.json's "icon", else an icon.png of its own; NULL for none, or one too big for an icon. */
static const mg_zip_entry *bundle_icon(const ctx *c)
{
    char name[200];
    const char *v = c->mjson ? mg_json_member(c->mjson, c->mjson + c->mjson_size, "icon") : NULL;
    const mg_zip_entry *e;
    if (!v || !mg_json_string(v, c->mjson + c->mjson_size, name, sizeof(name))) mg_strlcpy(name, MG_BUNDLE_ICON, sizeof(name));
    e = mg_zip_find(&c->zip, name);
    return e && e->usize > 0 && e->usize <= (8u << 20) ? e : NULL;
}

static char *answer(ctx *c, const char *bundle_path, const item *base, const char *expanded, bool icon, int kept, int nexp)
{
    mg_jbuf r;
    bool first;
    int i;
    memset(&r, 0, sizeof(r));
    mg_jraw(&r, "{");
    mg_jkey(&r, "ok", true);
    mg_jraw(&r, "true");
    mg_jkey(&r, "bundle", false);
    mg_jstr(&r, last_part(bundle_path));
    mg_jkey(&r, "package", false);
    mg_jstr(&r, c->package);
    mg_jkey(&r, "versionCode", false);
    mg_jint(&r, c->base->version_code);
    mg_jkey(&r, "versionName", false);
    mg_jstr(&r, c->base->version_name);
    mg_jkey(&r, "base", false);
    mg_jstr(&r, base->e->name);
    mg_jkey(&r, "splits", false);
    mg_jraw(&r, "[");
    for (i = 0, first = true; i < c->nitems; i++) {
        const item *it = &c->items[i];
        if (it->is_base || !it->keep) continue;
        if (!first) mg_jraw(&r, ",");
        first = false;
        mg_jraw(&r, "{");
        mg_jkey(&r, "name", true);
        mg_jstr(&r, it->name);
        mg_jkey(&r, "from", false);
        mg_jstr(&r, it->e->name);
        mg_jkey(&r, "file", false);
        mg_jstr(&r, it->file);
        mg_jraw(&r, "}");
    }
    mg_jraw(&r, "]");
    mg_jkey(&r, "leftOut", false);
    mg_jraw(&r, "[");
    for (i = 0, first = true; i < c->nitems; i++) {
        const item *it = &c->items[i];
        if (it->is_base || it->keep) continue;
        if (!first) mg_jraw(&r, ",");
        first = false;
        mg_jraw(&r, "{");
        mg_jkey(&r, "name", true);
        mg_jstr(&r, it->name[0] ? it->name : last_part(it->e->name));
        mg_jkey(&r, "from", false);
        mg_jstr(&r, it->e->name);
        if (it->kind == K_ABI) {
            mg_jkey(&r, "abi", false);
            mg_jstr(&r, it->abi);
        }
        mg_jkey(&r, "why", false);
        mg_jstr(&r, it->why[0] ? it->why : "not wanted");
        mg_jraw(&r, "}");
    }
    mg_jraw(&r, "]");
    mg_jkey(&r, "expansions", false);
    mg_jraw(&r, expanded);
    mg_jkey(&r, "icon", false);
    mg_jraw(&r, icon ? "true" : "false");
    mg_jraw(&r, "}");
    mg_info("bundle: %s is %s %s (%d): its base and %d split(s) kept, %d expansion file(s)", last_part(bundle_path), c->package, c->base->version_name,
            c->base->version_code, kept, nexp);
    return r.s;
}

char *meglet_bundle_unpack(const char *bundle_path, const char *apk_path, const char *splits_dir, int density_dpi, const char *languages,
                         void (*progress)(void *user, uint64_t done, uint64_t total), void *user)
{
    ctx *c = (ctx *)mg_alloc(sizeof(ctx));
    expansions *obbs = (expansions *)mg_alloc(sizeof(expansions));
    int i, pass, kept = 0;
    char *base_part = NULL, *out = NULL, *path;
    item *base = NULL;
    const mg_zip_entry *icon_entry = NULL;
    mg_jbuf x;
    bool ok = false, icon = false;
    memset(&x, 0, sizeof(x));
    c->progress = progress;
    c->user = user;
    c->items = (item *)mg_alloc(sizeof(item) * MAX_ITEMS);
    c->base = (mg_apk_info *)mg_alloc(sizeof(mg_apk_info));
    c->part = (mg_apk_info *)mg_alloc(sizeof(mg_apk_info));
    if (!bundle_path || !apk_path || !splits_dir || !apk_path[0] || !splits_dir[0]) {
        mg_strlcpy(c->error, "no bundle was given, or nowhere to unpack it", sizeof(c->error));
        goto end;
    }
    if (!mg_zip_open(&c->zip, bundle_path, c->error, sizeof(c->error))) {
        char why[200];
        mg_strlcpy(why, c->error, sizeof(why));
        snprintf(c->error, sizeof(c->error), "the bundle cannot be read: %s", why);
        goto end;
    }
    {
        const mg_zip_entry *m = mg_zip_find(&c->zip, "manifest.json");
        if (m) c->mjson = (char *)mg_zip_read(&c->zip, m, 4u << 20, &c->mjson_size);
        if (c->mjson) {
            const char *v = mg_json_member(c->mjson, c->mjson + c->mjson_size, "package_name");
            if (v) mg_json_string(v, c->mjson + c->mjson_size, c->package, sizeof(c->package));
        }
    }
    mg_zip_each(&c->zip, "", add_entry, c);
    if (!c->nitems) {
        mg_strlcpy(c->error, mg_zip_find(&c->zip, "AndroidManifest.xml") ? "this is an APK, not a bundle of APKs: add it as an .apk" : "there is no APK in the bundle", sizeof(c->error));
        goto end;
    }
    qsort(c->items, (size_t)c->nitems, sizeof(item), by_entry_name);
    /* The splits go to a folder of their own first, and take the old folder's place only once all is there. */
    c->work = mg_sprintf("%s.part", splits_dir);
    remove_folder(c->work);
    if (!mg_path_mkdirs(c->work)) {
        mg_strlcpy(c->error, "the folder for the splits cannot be made", sizeof(c->error));
        goto end;
    }
    policy(c, density_dpi, languages);
    for (i = 0; i < c->nitems; i++) decide(c, &c->items[i]);
    mg_zip_each(&c->zip, "Android/obb/", expansion_seen, obbs);
    mg_zip_each(&c->zip, "Android/data/", expansion_seen, obbs);
    expansions_of_manifest(c, obbs);
    /* What is to be written, for the progress (the base reckoned as the biggest APK until it is found). */
    for (i = 0; i < c->nitems; i++)
        if (c->items[i].keep && c->items[i].e->usize > c->total) c->total = c->items[i].e->usize;

    base_part = mg_sprintf("%s.part", apk_path);
    if (!(base = find_base(c, base_part))) goto end;
    mg_strlcpy(c->package, c->base->package, sizeof(c->package));
    c->total = c->done;
    for (i = 0; i < c->nitems; i++)
        if (!c->items[i].is_base && c->items[i].keep) c->total += c->items[i].e->usize;
    for (i = 0; i < obbs->n; i++) c->total += obbs->x[i].e->usize;
    icon_entry = bundle_icon(c);
    if (icon_entry) c->total += icon_entry->usize;

    /*
     * The splits: those for this phone's processor, density and languages first, so that of more than ten what is left out
     * is a feature split or an asset pack, never the split with the native code.
     */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < c->nitems; i++) {
            item *it = &c->items[i];
            bool config = it->kind == K_ABI || it->kind == K_DENSITY || it->kind == K_LANGUAGE || it->kind == K_CONFIG;
            if (it->is_base) {
                it->keep = false;
                continue;
            }
            if (!it->keep || config != (pass == 0) || it->file[0]) continue;
            if (kept >= MAX_KEPT) {
                it->keep = false;
                snprintf(it->why, sizeof(it->why), "more than %d splits: an app is run with %d at most", MAX_KEPT, MAX_KEPT);
                continue;
            }
            if (!take_split(c, it, i)) goto end;
            if (it->keep) kept++;
        }
    }

    /* The expansion files, where the app finds them. */
    mg_jraw(&x, "[");
    for (i = 0; i < obbs->n; i++) {
        const expansion *e = &obbs->x[i];
        char *target = expansion_target(c, e->install), *tmp;
        if (i) mg_jraw(&x, ",");
        mg_jraw(&x, "{");
        mg_jkey(&x, "from", true);
        mg_jstr(&x, e->e->name);
        mg_jkey(&x, "size", false);
        mg_jint(&x, (long long)e->e->usize);
        if (!target) {
            mg_jkey(&x, "leftOut", false);
            mg_jstr(&x, "it is to go somewhere else than the app's own Android/obb or Android/data");
            mg_jraw(&x, "}");
            c->total -= e->e->usize;
            continue;
        }
        tmp = mg_sprintf("%s.part", target);
        ok = make_parent(target) && extract(c, e->e, tmp) && move_into_place(c, tmp, target);
        free(tmp);
        free(target);
        if (!ok) {
            if (!c->error[0]) snprintf(c->error, sizeof(c->error), "the folder for %s cannot be made", e->e->name);
            goto end;
        }
        ok = false;
        mg_jkey(&x, "to", false);
        path = mg_sprintf("/storage/emulated/0/%s", e->install[0] == '/' ? e->install + 1 : e->install);
        mg_jstr(&x, path);
        free(path);
        mg_jraw(&x, "}");
    }
    mg_jraw(&x, "]");

    /* The icon the bundle came with (an .xapk's icon.png), for an app whose APKs have none this layer finds. */
    if (icon_entry) {
        path = mg_sprintf("%s/%s", c->work, MG_BUNDLE_ICON);
        icon = extract(c, icon_entry, path);
        free(path);
        if (!icon) goto end;
    }
    out = answer(c, bundle_path, base, x.s, icon, kept, obbs->n);
    path = mg_sprintf("%s/%s", c->work, MG_BUNDLE_RECORD);
    if (!mg_file_write_all(path, out, strlen(out))) mg_warn("bundle: the record of what was unpacked could not be written in %s", c->work);
    free(path);
    /* All is there: the base and the folder of splits take the old ones' places. */
    remove_folder(splits_dir);
    {
        int err = 0;
        if (!mg_path_rename(c->work, splits_dir, &err)) {
            snprintf(c->error, sizeof(c->error), "the folder of splits could not be put in place (error %d)", err);
            free(out);
            out = NULL;
            goto end;
        }
    }
    /* (Its files are the folder's now, under their new names: nothing of it is to be taken away from here on.) */
    for (i = 0; i < c->nwritten; i++)
        if (!strncmp(c->written[i], c->work, strlen(c->work)) && c->written[i][strlen(c->work)] == '/') c->written[i][0] = 0;
    if (!move_into_place(c, base_part, apk_path)) {
        free(out);
        out = NULL;
        remove_folder(splits_dir);
        goto end;
    }
    ok = true;
end:
    if (!ok) {
        /* Nothing of a bundle that did not come out whole is left: no base, no splits, no part of an expansion file. */
        mg_jbuf f;
        for (i = 0; i < c->nwritten; i++)
            if (c->written[i][0]) mg_path_remove(c->written[i], NULL);
        if (c->work) remove_folder(c->work);
        if (!c->error[0]) mg_strlcpy(c->error, "the bundle could not be unpacked", sizeof(c->error));
        mg_warn("bundle: %s: %s", bundle_path ? last_part(bundle_path) : "?", c->error);
        memset(&f, 0, sizeof(f));
        mg_jraw(&f, "{");
        mg_jkey(&f, "ok", true);
        mg_jraw(&f, "false");
        mg_jkey(&f, "error", false);
        mg_jstr(&f, c->error);
        mg_jraw(&f, "}");
        free(out);
        out = f.s;
    }
    for (i = 0; i < c->nwritten; i++) free(c->written[i]);
    if (c->zip.file) mg_zip_close(&c->zip);
    free(base_part);
    free(x.s);
    free(c->work);
    free(c->mjson);
    free(c->items);
    free(c->base);
    free(c->part);
    free(c);
    free(obbs);
    return out;
}
