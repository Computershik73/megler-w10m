/*
 * Bundles of split APKs, unpacked as the shell unpacks them (megler_bundle_unpack), and the APK report of what comes out
 * (megler_apk_report_splits), on both hosts:
 *
 *   mg_tests --bundles <test apps folder> <scratch folder> [folder of real bundles]
 *
 * The test app `splits` (core/tests/app/splits, tools/build-split-app.py) comes as splits.apks in bundletool's layout and
 * splits.xapk in APKPure's, each with splits for three processors, three densities, three languages, a feature split and
 * an asset pack, the .xapk with an icon and an expansion file. Checked: which splits are kept and which are left out and
 * why, the files and the record they leave, the expansion file in the app's own storage, the icon found in a density
 * split, the report of the app as one; a second unpacking over the first, which leaves nothing of the first behind; a
 * damaged bundle and one with no base, which leave nothing at all. Bundles made here from the app's APKs (a stored ZIP
 * written by this file) stand for a bundle whose native code is for 64-bit ARM alone.
 *
 * The real bundles are the three the phone's owner has (copied from their Downloads; none is in the repository):
 * BiP 4.2.42 (.apks: its base, splits for four processors and seven densities), MangaDex 1.0.0.0 (.xapk: eighteen
 * languages, one density, an icon) and Magnit 8.107.1 (.apks with APKPure's manifest.json: native code for arm64-v8a
 * alone). Each is left out with a line when it is not in the folder. Everything unpacked is removed again.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../include/megler.h"
#include "../src/apk/mg_apk.h"
#include "../src/apk/mg_json.h"
#include "../src/mg_base.h"
#include "../src/host/mg_host.h"
#include "../src/zip/mg_zip.h"
#include "mg_tests.h"
#include "zlib.h"

const char *megler_data_dir(void);

static int g_checks, g_failed;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

#define CHECK(cond)                                                    \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            say("  FAIL %s:%d: %s", __FILE__, __LINE__, #cond);        \
        }                                                              \
    } while (0)

/* --------------------------------------------------------------- helpers */

static bool exists(const char *path)
{
    mg_stat st;
    return mg_path_stat(path, &st, NULL);
}

static u64 size_of(const char *path)
{
    mg_stat st;
    return mg_path_stat(path, &st, NULL) ? st.size : 0;
}

/* The "name"s of the objects in the array `key` of a JSON object, into names; how many. */
static int names_in(const char *json, const char *key, char names[][96], int max)
{
    const char *end = json + strlen(json), *list = mg_json_member(json, end, key), *at = NULL;
    int n = 0;
    while (list && n < max && mg_json_next(list, end, &at)) {
        const char *v = mg_json_member(at, end, "name");
        if (v && mg_json_string(v, end, names[n], sizeof(names[0]))) n++;
        else if (mg_json_string(at, end, names[n], sizeof(names[0]))) n++;
    }
    return n;
}

static bool has_name(char names[][96], int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(names[i], name)) return true;
    return false;
}

/* The names of the array as one text, "a,b,c", for the log and to compare whole. */
static void joined(char names[][96], int n, char *out, size_t cap)
{
    int i;
    out[0] = 0;
    for (i = 0; i < n; i++) snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? "," : "", names[i]);
}

/* The "why" a left-out split was given in the answer: the text after its name. */
static bool left_out_because(const char *json, const char *name, const char *why_part)
{
    const char *end = json + strlen(json), *list = mg_json_member(json, end, "leftOut"), *at = NULL;
    while (list && mg_json_next(list, end, &at)) {
        char text[200];
        const char *v = mg_json_member(at, end, "name"), *w = mg_json_member(at, end, "why");
        if (!v || !mg_json_string(v, end, text, sizeof(text)) || strcmp(text, name)) continue;
        return w && mg_json_string(w, end, text, sizeof(text)) && strstr(text, why_part) != NULL;
    }
    return false;
}

typedef struct seen_progress {
    int calls;
    u64 last_done, last_total;
    bool backwards;
} seen_progress;

static MEGLER_CB void progress(void *user, uint64_t done, uint64_t total)
{
    seen_progress *p = (seen_progress *)user;
    if (p->calls && done < p->last_done) p->backwards = true;
    p->calls++;
    p->last_done = done;
    p->last_total = total;
}

/* Every file of a folder and its subfolders, and the folder: what the tests unpacked, taken away again. */
static void remove_tree(const char *dir);

static MG_HOSTCB void remove_one(const char *name, bool is_dir, void *user)
{
    char *path = mg_sprintf("%s/%s", (const char *)user, name);
    if (is_dir) remove_tree(path);
    else mg_path_remove(path, NULL);
    free(path);
}

static void remove_tree(const char *dir)
{
    mg_path_list(dir, remove_one, (void *)dir, NULL);
    mg_path_rmdir(dir, NULL);
}

/* ------------------------------------------------- a ZIP archive, stored */

typedef struct zip_out {
    mg_file *f;
    u64 at;
    struct {
        char name[120];
        u32 crc, size, csize;
        u16 method;                    /* 0 stored; 8 deflated (zip_add_big) */
        u64 offset;
    } e[32];
    int n;
} zip_out;

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

static void zip_write(zip_out *z, const void *data, size_t n)
{
    int err = 0;
    mg_file_write(z->f, data, n, &err);
    z->at += n;
}

static void zip_add(zip_out *z, const char *name, const u8 *data, u32 size)
{
    u8 h[30];
    u32 crc = (u32)crc32(crc32(0L, Z_NULL, 0), data, size);
    size_t nlen = strlen(name);
    if (z->n >= 32) return;
    memset(h, 0, sizeof(h));
    put32(h, 0x04034b50u);
    put16(h + 4, 20);
    put16(h + 12, 0x21);
    put32(h + 14, crc);
    put32(h + 18, size);
    put32(h + 22, size);
    put16(h + 26, (u32)nlen);
    mg_strlcpy(z->e[z->n].name, name, sizeof(z->e[0].name));
    z->e[z->n].crc = crc;
    z->e[z->n].size = z->e[z->n].csize = size;
    z->e[z->n].method = 0;
    z->e[z->n].offset = z->at;
    z->n++;
    zip_write(z, h, sizeof(h));
    zip_write(z, name, nlen);
    zip_write(z, data, size);
}

/*
 * A large entry that is mostly nothing: `head` at its start, `tail` at `tail_at`, zeros between and to `total`. Stored, the
 * zeros are not written but skipped (a hole, where the filesystem keeps one); deflated, they are a few kilobytes. False when
 * it could not be written.
 */
static bool zip_add_big(zip_out *z, const char *name, bool deflated, const u8 *head, u32 head_n, const u8 *tail, u32 tail_n, u32 tail_at, u32 total)
{
    enum { PIECE = 1 << 20 };
    u8 h[30], *zeros = (u8 *)calloc(1, PIECE), *packed = NULL;
    size_t nlen = strlen(name), packed_n = 0, packed_cap = 0;
    u32 crc = (u32)crc32(0L, Z_NULL, 0), at;
    z_stream s;
    int err = 0;
    bool ok = zeros && z->n < 32 && head_n <= tail_at && tail_at + tail_n <= total;
    memset(&s, 0, sizeof(s));
    if (ok && deflated) ok = deflateInit2(&s, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    /* The bytes in order: the CRC over all of them, and deflated as they come. */
    for (at = 0; ok && at <= total;) {
        const u8 *p;
        u32 n;
        if (at < head_n) {
            p = head + at;
            n = head_n - at;
        } else if (at >= tail_at && at < tail_at + tail_n) {
            p = tail + (at - tail_at);
            n = tail_at + tail_n - at;
        } else {
            u32 next = at < tail_at ? tail_at : total;
            p = zeros;
            n = next - at > PIECE ? PIECE : next - at;
        }
        crc = (u32)crc32(crc, p, n);
        if (deflated) {
            int flush = at + n == total ? Z_FINISH : Z_NO_FLUSH, r;
            s.next_in = (Bytef *)p;
            s.avail_in = n;
            do {
                if (packed_cap - packed_n < 65536) {
                    u8 *grown = (u8 *)realloc(packed, packed_cap + (1 << 20));
                    if (!grown) {
                        ok = false;
                        break;
                    }
                    packed = grown;
                    packed_cap += 1 << 20;
                }
                s.next_out = packed + packed_n;
                s.avail_out = (uInt)(packed_cap - packed_n);
                r = deflate(&s, flush);
                packed_n = packed_cap - s.avail_out;
            } while (ok && (s.avail_in || (flush == Z_FINISH && r != Z_STREAM_END)));
        }
        at += n;
        if (at == total) break;
    }
    if (deflated) deflateEnd(&s);
    if (ok) {
        memset(h, 0, sizeof(h));
        put32(h, 0x04034b50u);
        put16(h + 4, 20);
        put16(h + 8, deflated ? 8 : 0);
        put16(h + 12, 0x21);
        put32(h + 14, crc);
        put32(h + 18, deflated ? (u32)packed_n : total);
        put32(h + 22, total);
        put16(h + 26, (u32)nlen);
        mg_strlcpy(z->e[z->n].name, name, sizeof(z->e[0].name));
        z->e[z->n].crc = crc;
        z->e[z->n].size = total;
        z->e[z->n].csize = deflated ? (u32)packed_n : total;
        z->e[z->n].method = deflated ? 8 : 0;
        z->e[z->n].offset = z->at;
        z->n++;
        zip_write(z, h, sizeof(h));
        zip_write(z, name, nlen);
        if (deflated) {
            zip_write(z, packed, packed_n);
        } else {
            /* The head, the tail where it goes, and the file's position past the end: nothing written between. */
            ok = mg_file_pwrite(z->f, head, head_n, z->at, &err) == (i64)head_n && mg_file_pwrite(z->f, tail, tail_n, z->at + tail_at, &err) == (i64)tail_n;
            if (tail_at + tail_n < total) ok = ok && mg_file_pwrite(z->f, zeros, 1, z->at + total - 1, &err) == 1;
            z->at += total;
            ok = ok && mg_file_seek(z->f, (i64)z->at, 0, &err) == (i64)z->at;
        }
    }
    free(zeros);
    free(packed);
    return ok;
}

static void zip_end(zip_out *z)
{
    u64 start = z->at;
    u8 h[46], end[22];
    int i;
    for (i = 0; i < z->n; i++) {
        size_t nlen = strlen(z->e[i].name);
        memset(h, 0, sizeof(h));
        put32(h, 0x02014b50u);
        put16(h + 4, 20);
        put16(h + 6, 20);
        put16(h + 10, z->e[i].method);
        put16(h + 14, 0x21);
        put32(h + 16, z->e[i].crc);
        put32(h + 20, z->e[i].csize);
        put32(h + 24, z->e[i].size);
        put16(h + 28, (u32)nlen);
        put32(h + 42, (u32)z->e[i].offset);
        zip_write(z, h, sizeof(h));
        zip_write(z, z->e[i].name, nlen);
    }
    memset(end, 0, sizeof(end));
    put32(end, 0x06054b50u);
    put16(end + 8, (u32)z->n);
    put16(end + 10, (u32)z->n);
    put32(end + 12, (u32)(z->at - start));
    put32(end + 16, (u32)start);
    zip_write(z, end, sizeof(end));
    mg_file_close(z->f);
}

/* A bundle of some of the APKs of another (their names there, and the names they get in this one). */
static bool make_bundle(const char *from, const char *to, const char *const *names, const char *const *as, int n)
{
    mg_zip src;
    zip_out z;
    char err[160];
    int i, ferr = 0;
    if (!mg_zip_open(&src, from, err, sizeof(err))) return false;
    memset(&z, 0, sizeof(z));
    z.f = mg_file_open(to, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &ferr);
    if (!z.f) {
        mg_zip_close(&src);
        return false;
    }
    for (i = 0; i < n; i++) {
        const mg_zip_entry *e = mg_zip_find(&src, names[i]);
        size_t size = 0;
        u8 *data = e ? mg_zip_read(&src, e, 64u << 20, &size) : NULL;
        if (data) zip_add(&z, as[i], data, (u32)size);
        free(data);
    }
    zip_end(&z);
    mg_zip_close(&src);
    return true;
}

/* ------------------------------------------------------- the test app */

/* The splits the test app's bundles keep at 320 dpi with English and Russian, by the names their manifests give. */
static const char *const k_kept[] = { "config.armeabi_v7a", "config.en", "config.mdpi", "config.ru", "config.xxhdpi", "extra", "pack" };

static char *unpack(const char *bundle, const char *apk, const char *splits, int dpi, const char *languages, seen_progress *p)
{
    char *json;
    u64 started = mg_time_monotonic_ns();
    json = megler_bundle_unpack(bundle, apk, splits, dpi, languages, p ? progress : NULL, p);
    say("  %s: %s (%.0f ms)", bundle, json ? json : "(nothing)", (double)(mg_time_monotonic_ns() - started) / 1e6);
    return json;
}

static void test_app_bundle(const char *java_dir, const char *scratch, const char *data_dir, bool xapk)
{
    char *bundle = mg_sprintf("%s/splits.%s", java_dir, xapk ? "xapk" : "apks"), *dir = mg_sprintf("%s/%s", scratch, xapk ? "x" : "a");
    char *apk = mg_sprintf("%s/splits.apk", dir), *splits = mg_sprintf("%s/splits.apk.splits", dir), *json, *report, *path;
    char names[32][96], text[600];
    seen_progress p;
    uint8_t *icon = NULL;
    size_t icon_size = 0;
    int n, i;
    say("the test app's %s, unpacked for a screen of 320 dpi, in English and Russian", xapk ? ".xapk (APKPure's layout)" : ".apks (bundletool's layout)");
    if (!exists(bundle)) {
        say("  (no %s: tools/build-test-app.sh makes it; these checks are left out)", bundle);
        free(bundle);
        free(dir);
        free(apk);
        free(splits);
        return;
    }
    mg_path_mkdirs(dir);
    memset(&p, 0, sizeof(p));
    json = unpack(bundle, apk, splits, 320, "en,ru", &p);
    CHECK(json && strstr(json, "\"ok\":true") && strstr(json, "\"package\":\"megler.test.splits\"") && strstr(json, "\"versionCode\":7"));
    n = names_in(json ? json : "{}", "splits", names, 32);
    joined(names, n, text, sizeof(text));
    say("  kept: %s", text);
    CHECK(n == (int)MG_COUNT(k_kept));
    for (i = 0; i < (int)MG_COUNT(k_kept); i++) {
        CHECK(has_name(names, n, k_kept[i]));
        path = mg_sprintf("%s/split_%s.apk", splits, k_kept[i]);
        CHECK(exists(path));
        free(path);
    }
    /* Left out by their names, without being unpacked: the other processors, the third density, the third language. */
    n = names_in(json ? json : "{}", "leftOut", names, 32);
    joined(names, n, text, sizeof(text));
    say("  left out: %s", text);
    CHECK(n == (xapk ? 4 : 5));
    CHECK(left_out_because(json, "config.arm64_v8a", "arm64-v8a") && left_out_because(json, "config.x86", "x86"));
    CHECK(left_out_because(json, "config.ldpi", "density (120 dpi)") && left_out_because(json, "config.de", "language (de)"));
    if (!xapk) CHECK(left_out_because(json, "standalone-armeabi_v7a.apk", "standalone"));
    CHECK(json && strstr(json, "\"abi\":\"arm64-v8a\"") && strstr(json, "\"abi\":\"x86\""));
    path = mg_sprintf("%s/splits.apk.splits/config.arm64_v8a.apk", dir);
    CHECK(!exists(path));
    free(path);
    CHECK(exists(apk) && size_of(apk) > 1000);
    /* Nothing of the work is left: no part files, no work folder. */
    path = mg_sprintf("%s.part", apk);
    CHECK(!exists(path));
    free(path);
    path = mg_sprintf("%s.part", splits);
    CHECK(!exists(path));
    free(path);
    /* The record beside the splits is the answer. */
    path = mg_sprintf("%s/bundle.json", splits);
    {
        size_t size = 0;
        char *record = (char *)mg_file_read_all(path, &size);
        CHECK(record && json && size == strlen(json) && !memcmp(record, json, size));
        free(record);
    }
    free(path);
    CHECK(p.calls > 0 && !p.backwards && p.last_done == p.last_total && p.last_total > 0);
    say("  progress: %d calls, %llu of %llu bytes at the end", p.calls, (unsigned long long)p.last_done, (unsigned long long)p.last_total);

    /* The expansion file of the .xapk, in the app's own Android/obb, where it finds it when it runs. */
    path = mg_sprintf("%s/apps/megler.test.splits/sdcard/Android/obb/megler.test.splits/main.7.megler.test.splits.obb", data_dir);
    if (xapk) {
        size_t size = 0;
        char *obb = (char *)mg_file_read_all(path, &size);
        CHECK(obb && size == 41 && !memcmp(obb, "the expansion file of megler.test.splits\n", 41));
        CHECK(json && strstr(json, "\"to\":\"/storage/emulated/0/Android/obb/megler.test.splits/main.7.megler.test.splits.obb\""));
        free(obb);
    } else {
        CHECK(!exists(path));
    }
    free(path);
    path = mg_sprintf("%s/icon.png", splits);
    CHECK(exists(path) == xapk);
    free(path);

    /* The report of the app as one. */
    report = megler_apk_report_splits(apk, splits);
    say("  report: %s", report ? report : "(none)");
    CHECK(report && strstr(report, "\"ok\":true") && strstr(report, "\"package\":\"megler.test.splits\"") && strstr(report, "\"versionName\":\"1.7\""));
    CHECK(report && strstr(report, "\"abi\":\"armeabi-v7a\"") && strstr(report, "\"engine\":\"java\"") && strstr(report, "\"runnable\":true"));
    CHECK(report && strstr(report, "\"abis\":[\"armeabi-v7a\",\"arm64-v8a\",\"x86\"]"));
    CHECK(report && strstr(report, "\"name\":\"libsplitlib.so\"") && strstr(report, "\"hasIcon\":true"));
    n = names_in(report ? report : "{}", "splits", names, 32);
    CHECK(n == (int)MG_COUNT(k_kept));
    n = names_in(report ? report : "{}", "splitsLeftOut", names, 32);
    CHECK(n == (xapk ? 4 : 5) && has_name(names, n, "config.arm64_v8a"));
    megler_free(report);
    /* The icon is in the density split alone (the base has no picture of it). */
    CHECK(megler_apk_icon_splits(apk, splits, &icon, &icon_size) && icon_size > 8 && !memcmp(icon, "\x89PNG", 4));
    megler_free(icon);
    icon = NULL;
    CHECK(!megler_apk_icon(apk, &icon, &icon_size));
    megler_free(icon);
    megler_free(json);

    /* Again over the first: a split the folder had that the bundle has not is gone, the rest as before. */
    path = mg_sprintf("%s/split_config.fr.apk", splits);
    mg_file_write_all(path, "stale", 5);
    json = unpack(bundle, apk, splits, 320, "en", NULL);
    CHECK(json && strstr(json, "\"ok\":true") && !exists(path));
    n = names_in(json ? json : "{}", "splits", names, 32);
    CHECK(n == (int)MG_COUNT(k_kept) - 1 && !has_name(names, n, "config.ru") && has_name(names, n, "config.en"));
    free(path);
    path = mg_sprintf("%s/split_config.ru.apk", splits);
    CHECK(!exists(path));
    free(path);
    megler_free(json);
    remove_tree(dir);
    free(bundle);
    free(dir);
    free(apk);
    free(splits);
}

/* The base alone, which says it needs its splits; a damaged bundle; one with no base; one whose native code is for arm64 alone. */
/* The names of an archive's entries, for copying it whole. */
typedef struct entry_names {
    char names[32][96];
    int n;
} entry_names;

static void name_seen(const mg_zip_entry *e, void *user)
{
    entry_names *w = (entry_names *)user;
    if (w->n < 32) mg_strlcpy(w->names[w->n++], e->name, sizeof(w->names[0]));
}

/*
 * A Flutter app: the natives test app with Flutter's engine and an app's Dart code beside its libraries (libflutter.so,
 * libapp.so; their bytes do not matter to the report). It is a Java app that the Java driver runs, and the report says it
 * is built with Flutter. It was taken for an engine of its own, with no driver: "there is no driver for that engine yet".
 */
/*
 * A Java app (the natives test app) with libraries of an engine's added, each a few bytes of an ELF header: its report
 * says "java", runnable, and carries the engine's note, and `note2` too when it is not NULL.
 */
static void test_engine_app(const char *java_dir, const char *scratch, const char *what, const char *const *libs, int nlibs, const char *note,
                            const char *note2)
{
    char *from = mg_sprintf("%s/natives.apk", java_dir), *dir = mg_sprintf("%s/%s", scratch, what), *apk = mg_sprintf("%s/%s.apk", dir, what), *report;
    static entry_names names;
    static const u8 elf[] = { 0x7F, 'E', 'L', 'F', 1, 1, 1, 0 };
    mg_zip src;
    zip_out z;
    char err[160];
    int i, ferr = 0;
    say("a %s app", what);
    if (!exists(from) || !mg_zip_open(&src, from, err, sizeof(err))) {
        say("  (no %s: tools/build-test-app.sh makes it; these checks are left out)", from);
        goto done;
    }
    mg_path_mkdirs(dir);
    memset(&names, 0, sizeof(names));
    mg_zip_each(&src, "", name_seen, &names);
    memset(&z, 0, sizeof(z));
    z.f = mg_file_open(apk, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &ferr);
    if (!z.f) {
        mg_zip_close(&src);
        CHECK(false);
        goto done;
    }
    for (i = 0; i < names.n; i++) {
        const mg_zip_entry *e = mg_zip_find(&src, names.names[i]);
        size_t size = 0;
        u8 *data = e ? mg_zip_read(&src, e, 64u << 20, &size) : NULL;
        if (data) zip_add(&z, names.names[i], data, (u32)size);
        free(data);
    }
    for (i = 0; i < nlibs; i++) {
        char *entry = mg_sprintf("lib/armeabi-v7a/%s", libs[i]);
        zip_add(&z, entry, elf, sizeof(elf));
        free(entry);
    }
    zip_end(&z);
    mg_zip_close(&src);
    report = megler_apk_report(apk);
    say("  %s", report ? report : "(no report)");
    CHECK(report && strstr(report, "\"engine\":\"java\"") && strstr(report, "\"runnable\":true") && strstr(report, note) && (!note2 || strstr(report, note2)));
    megler_free(report);
done:
    free(apk);
    free(dir);
    free(from);
}

/* ------------------------------------- a large library's NativeActivity entry */

/* The parts of the 32-bit ARM ELF test_big_library makes: its first 4 KB with the headers and the symbol tables, and 4 KB at 40 MB with its dynamic section. */
enum { BIG_HEAD = 0x1000, BIG_TAIL = 0x1000, BIG_DATA = 40u << 20, BIG_DATA_VADDR = BIG_DATA + 0x10000 };

static u32 gnu_hash_of(const char *s)
{
    u32 h = 5381;
    while (*s) h = h * 33 + (u8)*s++;
    return h;
}

static u32 sysv_hash_of(const char *s)
{
    u32 h = 0, g;
    while (*s) {
        h = (h << 4) + (u8)*s++;
        g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/*
 * The head and the tail of a library of 40 MB + 4 KB whose dynamic section is at 40 MB, past the first 32 MB, where its
 * second segment begins (as a large library's data comes after all its code); its symbol tables at its start. Its dynamic
 * symbols: JNI_OnLoad, and ANativeActivity_onCreate -- defined (`entry`) or an import. Found through a GNU hash table, a
 * SysV one, or both.
 */
static void big_library(u8 *head, u8 *tail, bool entry, bool gnu, bool sysv)
{
    static const char strtab[] = "\0JNI_OnLoad\0ANativeActivity_onCreate";
    const u32 symtab_at = 0x100, strtab_at = 0x200, hash_at = 0x300, gnu_at = 0x400, onload = 1, create = 12;
    /* The symbols in the order a linker gives them: the imports first, those GNU's table does not hash. */
    u32 names[3], defined[3], symoff, i, d = 0, nsyms = 3;
    u8 *p;
    memset(head, 0, BIG_HEAD);
    memset(tail, 0, BIG_TAIL);
    names[0] = 0;
    defined[0] = 0;
    if (entry) {
        names[1] = onload, defined[1] = 1, names[2] = create, defined[2] = 1;
        symoff = 1;
    } else {
        names[1] = create, defined[1] = 0, names[2] = onload, defined[2] = 1;
        symoff = 2;
    }
    /* The ELF header: 32-bit, little-endian, a shared object for ARM, three program headers right after it. */
    memcpy(head, "\177ELF\1\1\1", 7);
    put16(head + 16, 3);
    put16(head + 18, 40);
    put32(head + 20, 1);
    put32(head + 28, 52);
    put32(head + 36, 0x05000000u);
    put16(head + 40, 52);
    put16(head + 42, 32);
    put16(head + 44, 3);
    put16(head + 46, 40);
    /* PT_LOAD of the code (the head and the hole), PT_LOAD of the data at 40 MB (at another address), PT_DYNAMIC in it. */
    p = head + 52;
    put32(p, 1), put32(p + 4, 0), put32(p + 8, 0), put32(p + 12, 0), put32(p + 16, BIG_DATA), put32(p + 20, BIG_DATA), put32(p + 24, 5), put32(p + 28, 0x1000);
    p += 32;
    put32(p, 1), put32(p + 4, BIG_DATA), put32(p + 8, BIG_DATA_VADDR), put32(p + 12, BIG_DATA_VADDR), put32(p + 16, BIG_TAIL), put32(p + 20, BIG_TAIL),
        put32(p + 24, 6), put32(p + 28, 0x1000);
    p += 32;
    put32(p, 2), put32(p + 4, BIG_DATA), put32(p + 8, BIG_DATA_VADDR), put32(p + 12, BIG_DATA_VADDR), put32(p + 16, 8 * 8), put32(p + 20, 8 * 8), put32(p + 24, 6),
        put32(p + 28, 4);
    /* .dynsym and .dynstr */
    for (i = 1; i < nsyms; i++) {
        p = head + symtab_at + i * 16;
        put32(p, names[i]);
        put32(p + 4, defined[i] ? 0x2000 + i * 16 : 0);
        p[12] = 0x12;               /* global, a function */
        put16(p + 14, defined[i] ? 7 : 0);
    }
    memcpy(head + strtab_at, strtab, sizeof(strtab));
    /* .hash: two buckets, the chains through all three symbols */
    if (sysv) {
        u32 bucket[2] = { 0, 0 }, chain[3] = { 0, 0, 0 };
        for (i = 1; i < nsyms; i++) {
            u32 b = sysv_hash_of(strtab + names[i]) % 2;
            chain[i] = bucket[b];
            bucket[b] = i;
        }
        put32(head + hash_at, 2);
        put32(head + hash_at + 4, nsyms);
        for (i = 0; i < 2; i++) put32(head + hash_at + 8 + i * 4, bucket[i]);
        for (i = 0; i < nsyms; i++) put32(head + hash_at + 16 + i * 4, chain[i]);
    }
    /* .gnu.hash: one bucket, a bloom word that lets everything through, the defined symbols' chain */
    if (gnu) {
        put32(head + gnu_at, 1);
        put32(head + gnu_at + 4, symoff);
        put32(head + gnu_at + 8, 1);
        put32(head + gnu_at + 12, 5);
        put32(head + gnu_at + 16, 0xFFFFFFFFu);
        put32(head + gnu_at + 20, symoff);
        for (i = symoff; i < nsyms; i++) put32(head + gnu_at + 24 + (i - symoff) * 4, (gnu_hash_of(strtab + names[i]) & ~1u) | (i == nsyms - 1 ? 1u : 0u));
    }
    /* .dynamic */
    p = tail;
    if (sysv) put32(p + d * 8, 4), put32(p + d * 8 + 4, hash_at), d++;
    if (gnu) put32(p + d * 8, 0x6ffffef5u), put32(p + d * 8 + 4, gnu_at), d++;
    put32(p + d * 8, 5), put32(p + d * 8 + 4, strtab_at), d++;
    put32(p + d * 8, 6), put32(p + d * 8 + 4, symtab_at), d++;
    put32(p + d * 8, 10), put32(p + d * 8 + 4, (u32)sizeof(strtab)), d++;
    put32(p + d * 8, 11), put32(p + d * 8 + 4, 16), d++;
}

/*
 * Whether an app whose activity names its library with android.app.lib_name is a NativeActivity: its library is looked at
 * for ANativeActivity_onCreate (core/src/apk/mg_apk.c, native_entry). 2GIS (Qt: its QtActivity, libv4android.so of 277 MB in
 * the split config.armeabi_v7a) was taken for a NativeActivity, and its start ended in "has no ANativeActivity_onCreate"
 * (package 681): the look read the library's first 32 MB, and its dynamic section was past them. Here the natives test app
 * (lib_name "natives", an activity of its own) with its libnatives.so replaced by a library of 40 MB whose dynamic section is
 * at 40 MB, stored (the file has a hole where the filesystem allows) or deflated (a few kilobytes), its symbols hashed by a
 * GNU table, a SysV one or both: a Java app where the entry is an import, a NativeActivity where it is defined. And the
 * cursor a deflated library is read through: inflated once as far as the dynamic section, its tables then read from what it
 * kept of its head, and inflated again from the start for a read past what is kept.
 */
static void test_big_library(const char *java_dir, const char *scratch)
{
    static const struct {
        bool deflated, entry, gnu, sysv;
        const char *engine;
    } variants[] = {
        { false, false, true, true, "java" },
        { true, true, true, false, "native-activity" },
        { true, false, false, true, "java" },
        { false, true, false, true, "native-activity" },
    };
    char *from = mg_sprintf("%s/natives.apk", java_dir), *dir = mg_sprintf("%s/big-library", scratch);
    static entry_names names;
    static mg_apk_info info;
    u8 *head = (u8 *)malloc(BIG_HEAD), *tail = (u8 *)malloc(BIG_TAIL);
    mg_zip src;
    char err[160];
    int v, i;
    say("an app whose library's dynamic section is past its first 32 MB");
#if !MG_APK_ABI_ARM
    say("  (this build looks at its own processor's libraries, not 32-bit ARM's: these checks are left out)");
    goto done;
#endif
    if (!head || !tail || !exists(from) || !mg_zip_open(&src, from, err, sizeof(err))) {
        say("  (no %s: tools/build-test-app.sh makes it; these checks are left out)", from);
        goto done;
    }
    mg_path_mkdirs(dir);
    memset(&names, 0, sizeof(names));
    mg_zip_each(&src, "", name_seen, &names);
    for (v = 0; v < (int)MG_COUNT(variants); v++) {
        char *apk = mg_sprintf("%s/big-%d.apk", dir, v);
        zip_out z;
        int ferr = 0;
        bool made = true, read;
        u64 size;
        memset(&z, 0, sizeof(z));
        z.f = mg_file_open(apk, MG_O_WRITE | MG_O_CREATE | MG_O_TRUNC, &ferr);
        CHECK(z.f != NULL);
        if (!z.f) {
            free(apk);
            continue;
        }
        for (i = 0; i < names.n; i++) {
            const mg_zip_entry *e = mg_zip_find(&src, names.names[i]);
            size_t n = 0;
            u8 *data;
            if (!strcmp(names.names[i], "lib/armeabi-v7a/libnatives.so")) continue;
            data = e ? mg_zip_read(&src, e, 64u << 20, &n) : NULL;
            if (data) zip_add(&z, names.names[i], data, (u32)n);
            free(data);
        }
        big_library(head, tail, variants[v].entry, variants[v].gnu, variants[v].sysv);
        made = zip_add_big(&z, "lib/armeabi-v7a/libnatives.so", variants[v].deflated, head, BIG_HEAD, tail, BIG_TAIL, BIG_DATA, BIG_DATA + BIG_TAIL);
        zip_end(&z);
        CHECK(made);
        size = size_of(apk);
        memset(&info, 0, sizeof(info));
        read = mg_apk_read_info(apk, &info);
        say("  %s, its symbols hashed by %s, ANativeActivity_onCreate %s (the APK %.1f MB): %s, %s", variants[v].deflated ? "deflated" : "stored",
            variants[v].gnu && variants[v].sysv ? "GNU's table and SysV's" : variants[v].gnu ? "GNU's table" : "SysV's table",
            variants[v].entry ? "defined" : "an import", (double)size / 1048576.0, read ? info.engine : "not read", read ? (info.runnable ? "runnable" : info.reason) : info.error);
        CHECK(read && !strcmp(info.engine, variants[v].engine) && info.runnable);
        if (read) mg_apk_info_free(&info);
        /* The deflated library read through its cursor, as the look reads it. */
        if (v == 1 && made) {
            mg_zip big;
            if (mg_zip_open(&big, apk, err, sizeof(err))) {
                const mg_zip_entry *e = mg_zip_find(&big, "lib/armeabi-v7a/libnatives.so");
                mg_zip_cursor *c = e ? mg_zip_cursor_open(&big, e, 32u << 20) : NULL;
                u8 got[64];
                u64 once = 0, twice = 0;
                bool ok = c != NULL;
                ok = ok && mg_zip_cursor_read(c, 0, got, 52) && !memcmp(got, head, 52);
                ok = ok && mg_zip_cursor_read(c, BIG_DATA, got, 16) && !memcmp(got, tail, 16);
                once = mg_zip_cursor_inflated(c);
                ok = ok && mg_zip_cursor_read(c, 0x100, got, 48) && !memcmp(got, head + 0x100, 48) && mg_zip_cursor_inflated(c) == once;
                ok = ok && mg_zip_cursor_read(c, BIG_DATA - 16, got, 32) && !memcmp(got + 16, tail, 16) && got[0] == 0 && got[15] == 0;
                twice = mg_zip_cursor_inflated(c);
                say("  the deflated one through its cursor: %.1f MB inflated to its dynamic section, its tables then from what it kept, "
                    "%.1f MB more for a read before that past the first 32 MB", (double)once / 1048576.0, (double)(twice - once) / 1048576.0);
                CHECK(ok && e && once >= BIG_DATA + 16 && once < BIG_DATA + (1u << 20) && twice > once + (32u << 20) && twice < 2 * once + (1u << 20));
                CHECK(!mg_zip_cursor_read(c, BIG_DATA + BIG_TAIL - 8, got, 16));
                mg_zip_cursor_close(c);
                mg_zip_close(&big);
            } else {
                CHECK(false);
            }
        }
        mg_path_remove(apk, NULL);
        free(apk);
    }
    mg_zip_close(&src);
    mg_path_rmdir(dir, NULL);
done:
    free(head);
    free(tail);
    free(dir);
    free(from);
}

static void test_odd_bundles(const char *java_dir, const char *scratch)
{
    char *base = mg_sprintf("%s/splits-base.apk", java_dir), *xapk = mg_sprintf("%s/splits.xapk", java_dir), *dir = mg_sprintf("%s/odd", scratch);
    char *apk = mg_sprintf("%s/odd.apk", dir), *splits = mg_sprintf("%s/odd.apk.splits", dir), *bundle, *json, *report;
    say("bundles that are not whole, and a base alone");
    if (!exists(base) || !exists(xapk)) {
        say("  (no %s or %s: tools/build-test-app.sh makes them; these checks are left out)", base, xapk);
        goto done;
    }
    mg_path_mkdirs(dir);
    report = megler_apk_report(base);
    say("  the base alone: %s", report ? report : "(none)");
    CHECK(report && strstr(report, "\"runnable\":true") && strstr(report, "only the base of an app that comes in several APKs") && strstr(report, "\"hasIcon\":false"));
    CHECK(report && !strstr(report, "\"splits\""));
    megler_free(report);

    /* Damaged: the first half of the .xapk, which has no central directory. */
    bundle = mg_sprintf("%s/damaged.xapk", dir);
    {
        size_t size = 0;
        u8 *bytes = mg_file_read_all(xapk, &size);
        if (bytes) mg_file_write_all(bundle, bytes, size / 2);
        free(bytes);
    }
    json = unpack(bundle, apk, splits, 320, "en", NULL);
    CHECK(json && strstr(json, "\"ok\":false") && strstr(json, "cannot be read"));
    CHECK(!exists(apk) && !exists(splits));
    megler_free(json);
    free(bundle);

    /* No base: splits alone. */
    {
        static const char *const names[] = { "config.en.apk", "config.armeabi_v7a.apk" };
        bundle = mg_sprintf("%s/nobase.apks", dir);
        CHECK(make_bundle(xapk, bundle, names, names, 2));
        json = unpack(bundle, apk, splits, 320, "en", NULL);
        CHECK(json && strstr(json, "\"ok\":false") && strstr(json, "no base APK"));
        CHECK(!exists(apk) && !exists(splits));
        megler_free(json);
        free(bundle);
    }

    /* SAI's names, and native code for 64-bit ARM alone: unpacked, and the report says it cannot run, and why. */
    {
        static const char *const names[] = { "megler.test.splits.apk", "config.arm64_v8a.apk", "config.en.apk", "config.xxhdpi.apk" };
        static const char *const as[] = { "base.apk", "split_config.arm64_v8a.apk", "split_config.en.apk", "split_config.xxhdpi.apk" };
        bundle = mg_sprintf("%s/arm64.apks", dir);
        CHECK(make_bundle(xapk, bundle, names, as, 4));
        json = unpack(bundle, apk, splits, 560, "en,ru", NULL);
        CHECK(json && strstr(json, "\"ok\":true") && strstr(json, "\"base\":\"base.apk\""));
        CHECK(json && left_out_because(json, "config.arm64_v8a", "arm64-v8a"));
        megler_free(json);
        report = megler_apk_report_splits(apk, splits);
        say("  native code for arm64 alone: %s", report ? report : "(none)");
        CHECK(report && strstr(report, "\"runnable\":false") && strstr(report, "no 32-bit ARM code") && strstr(report, "arm64-v8a only"));
        CHECK(report && strstr(report, "\"abis\":[\"arm64-v8a\"]") && !strstr(report, "\"abi\":"));
        megler_free(report);
        free(bundle);
    }
    remove_tree(dir);
done:
    free(base);
    free(xapk);
    free(dir);
    free(apk);
    free(splits);
}

/* ------------------------------------------------------- the real ones */

typedef struct real_bundle {
    const char *file, *package, *version, *abi;   /* abi NULL: none usable */
    bool runnable;
    const char *const *kept;
    int nkept;
    const char *left_out;                         /* one of those left out */
    const char *reason;                           /* a part of the reason, for one that is not runnable */
} real_bundle;

static void test_real(const char *real_dir, const char *scratch, const real_bundle *b)
{
    char *bundle = mg_sprintf("%s/%s", real_dir, b->file), *dir = mg_sprintf("%s/real", scratch);
    char *apk = mg_sprintf("%s/app.apk", dir), *splits = mg_sprintf("%s/app.apk.splits", dir), *json, *report, want[200];
    char names[64][96], text[1200];
    int n, i;
    say("a real bundle: %s, unpacked for this phone (560 dpi, English and Russian)", b->file);
    if (!exists(bundle)) {
        say("  (not in %s: left out)", real_dir);
        goto done;
    }
    mg_path_mkdirs(dir);
    json = unpack(bundle, apk, splits, 560, "en,ru", NULL);
    snprintf(want, sizeof(want), "\"package\":\"%s\"", b->package);
    CHECK(json && strstr(json, "\"ok\":true") && strstr(json, want));
    n = names_in(json ? json : "{}", "splits", names, 64);
    joined(names, n, text, sizeof(text));
    say("  kept: %s", text);
    CHECK(n == b->nkept);
    for (i = 0; i < b->nkept; i++) CHECK(has_name(names, n, b->kept[i]));
    n = names_in(json ? json : "{}", "leftOut", names, 64);
    joined(names, n, text, sizeof(text));
    say("  left out: %s", text);
    CHECK(has_name(names, n, b->left_out));
    megler_free(json);
    report = megler_apk_report_splits(apk, splits);
    say("  report: %s", report ? report : "(none)");
    CHECK(report && strstr(report, "\"ok\":true") && strstr(report, want));
    snprintf(want, sizeof(want), "\"versionName\":\"%s\"", b->version);
    CHECK(report && strstr(report, want));
    if (b->abi) {
        snprintf(want, sizeof(want), "\"abi\":\"%s\"", b->abi);
        CHECK(report && strstr(report, want));
    } else {
        CHECK(report && !strstr(report, "\"abi\":"));
    }
    CHECK(report && strstr(report, b->runnable ? "\"runnable\":true" : "\"runnable\":false"));
    if (b->reason) CHECK(report && strstr(report, b->reason));
    megler_free(report);
    {
        uint8_t *icon = NULL;
        size_t size = 0;
        bool found = megler_apk_icon_splits(apk, splits, &icon, &size);
        say("  icon: %s, %u bytes", !found ? "none" : size > 4 && !memcmp(icon, "\x89PNG", 4) ? "a PNG" : size > 12 && !memcmp(icon + 8, "WEBP", 4) ? "a WebP" : "something else",
            (unsigned)size);
        CHECK(found);
        megler_free(icon);
    }
    remove_tree(dir);
done:
    free(bundle);
    free(dir);
    free(apk);
    free(splits);
}

int mg_bundle_tests_run(const char *java_dir, const char *scratch, const char *real_dir)
{
    static const char *const bip_kept[] = { "config.armeabi_v7a", "config.xxhdpi", "config.xxxhdpi" };
    static const char *const mangadex_kept[] = { "config.en", "config.ru", "config.xxhdpi" };
    static const char *const magnit_kept[] = { "config.en", "config.xhdpi" };
    static const char *const zalo_kept[] = { "config.armeabi_v7a", "config.xxhdpi", "config.xxxhdpi", "mediapipe_faceeffect", "tensorflowLite" };
    static const real_bundle real[] = {
        { "BiP_v.4.2.42-HEAD(4450)(7.0-15.0)(arm7a,arm64-8a,x86,x86-64).apks", "com.turkcell.bip", "4.2.42-HEAD", "armeabi-v7a", true, bip_kept, 3, "config.arm64_v8a", NULL },
        { "MangaDex_1.0.0.0_APKPure.xapk", "org.mangadex.mangadex", "1.0.0.0", NULL, true, mangadex_kept, 3, "config.ja", NULL },
        /* APKMirror's .apkm: base.apk and split_*.apk, its info.json and icon.png, its own signature in META-INF. Zalo carries
           libcocos2dcpp.so among its 44 libraries, and is the Java app it is. */
        { "com.zing.zalo_26.10.02-261002903_APKMirror.apkm", "com.zing.zalo", "26.10.02", "armeabi-v7a", true, zalo_kept, 5, "config.arm64_v8a", NULL },
        { "\xd0\x9c\xd0\xb0\xd0\xb3\xd0\xbd\xd0\xb8\xd1\x82_8.107.1.apks", "ru.tander.magnit", "8.107.1", NULL, false, magnit_kept, 2, "config.arm64_v8a",
          "no 32-bit ARM code: the bundle it came in has native code for arm64-v8a only" },
    };
    int i;
    g_checks = g_failed = 0;
    mg_path_mkdirs(scratch);
    test_app_bundle(java_dir, scratch, megler_data_dir(), false);
    test_app_bundle(java_dir, scratch, megler_data_dir(), true);
    test_odd_bundles(java_dir, scratch);
    {
        static const char *const flutter[] = { "libflutter.so", "libapp.so" }, *const gdx[] = { "libgdx.so", "libgdx-freetype.so", "libdexprotector.so" };
        static const char *const cocos[] = { "libcocos2dcpp.so" }, *const sdl[] = { "libSDL2.so", "libmain.so" };
        test_engine_app(java_dir, scratch, "Flutter", flutter, (int)MG_COUNT(flutter), "Built with Flutter", NULL);
        test_engine_app(java_dir, scratch, "libGDX", gdx, (int)MG_COUNT(gdx), "Built with libGDX", "Protected by DexProtector");
        /* (Zalo's, a messenger's, as its mini-games' engine: it was refused as an app built on cocos2d-x.) */
        test_engine_app(java_dir, scratch, "cocos2d-x", cocos, (int)MG_COUNT(cocos), "With cocos2d-x", NULL);
        /* (Minecraft: Story Mode's: it was refused as an app built on sdl.) */
        test_engine_app(java_dir, scratch, "SDL", sdl, (int)MG_COUNT(sdl), "Built with SDL", NULL);
        /* (Google Play's PairIP, said: its checks fork a process that traces the app, as Earn to Die Rogue's do.) */
        {
            static const char *const pairip[] = { "libgdx.so", "libpairipcore.so" };
            test_engine_app(java_dir, scratch, "PairIP", pairip, (int)MG_COUNT(pairip), "Built with libGDX", "Protected by Google Play's PairIP");
        }
    }
    test_big_library(java_dir, scratch);
    if (real_dir)
        for (i = 0; i < (int)MG_COUNT(real); i++) test_real(real_dir, scratch, &real[i]);
    else
        say("(no folder of real bundles was named: BiP, MangaDex and Magnit are left out)");
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
