/*
 * Bundles of split APKs, unpacked as the shell unpacks them (meglet_bundle_unpack), and the APK report of what comes out
 * (meglet_apk_report_splits), on both hosts:
 *
 *   hk_tests --bundles <test apps folder> <scratch folder> [folder of real bundles]
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

#include "../include/meglet.h"
#include "../src/apk/hk_json.h"
#include "../src/hk_base.h"
#include "../src/host/hk_host.h"
#include "../src/zip/hk_zip.h"
#include "hk_tests.h"
#include "zlib.h"

const char *meglet_data_dir(void);

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
    hk_stat st;
    return hk_path_stat(path, &st, NULL);
}

static u64 size_of(const char *path)
{
    hk_stat st;
    return hk_path_stat(path, &st, NULL) ? st.size : 0;
}

/* The "name"s of the objects in the array `key` of a JSON object, into names; how many. */
static int names_in(const char *json, const char *key, char names[][96], int max)
{
    const char *end = json + strlen(json), *list = hk_json_member(json, end, key), *at = NULL;
    int n = 0;
    while (list && n < max && hk_json_next(list, end, &at)) {
        const char *v = hk_json_member(at, end, "name");
        if (v && hk_json_string(v, end, names[n], sizeof(names[0]))) n++;
        else if (hk_json_string(at, end, names[n], sizeof(names[0]))) n++;
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
    const char *end = json + strlen(json), *list = hk_json_member(json, end, "leftOut"), *at = NULL;
    while (list && hk_json_next(list, end, &at)) {
        char text[200];
        const char *v = hk_json_member(at, end, "name"), *w = hk_json_member(at, end, "why");
        if (!v || !hk_json_string(v, end, text, sizeof(text)) || strcmp(text, name)) continue;
        return w && hk_json_string(w, end, text, sizeof(text)) && strstr(text, why_part) != NULL;
    }
    return false;
}

typedef struct seen_progress {
    int calls;
    u64 last_done, last_total;
    bool backwards;
} seen_progress;

static void progress(void *user, uint64_t done, uint64_t total)
{
    seen_progress *p = (seen_progress *)user;
    if (p->calls && done < p->last_done) p->backwards = true;
    p->calls++;
    p->last_done = done;
    p->last_total = total;
}

/* Every file of a folder and its subfolders, and the folder: what the tests unpacked, taken away again. */
static void remove_tree(const char *dir);

static void remove_one(const char *name, bool is_dir, void *user)
{
    char *path = hk_sprintf("%s/%s", (const char *)user, name);
    if (is_dir) remove_tree(path);
    else hk_path_remove(path, NULL);
    free(path);
}

static void remove_tree(const char *dir)
{
    hk_path_list(dir, remove_one, (void *)dir, NULL);
    hk_path_rmdir(dir, NULL);
}

/* ------------------------------------------------- a ZIP archive, stored */

typedef struct zip_out {
    hk_file *f;
    u64 at;
    struct {
        char name[120];
        u32 crc, size;
        u64 offset;
    } e[32];
    int n;
} zip_out;

static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

static void zip_write(zip_out *z, const void *data, size_t n)
{
    int err = 0;
    hk_file_write(z->f, data, n, &err);
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
    hk_strlcpy(z->e[z->n].name, name, sizeof(z->e[0].name));
    z->e[z->n].crc = crc;
    z->e[z->n].size = size;
    z->e[z->n].offset = z->at;
    z->n++;
    zip_write(z, h, sizeof(h));
    zip_write(z, name, nlen);
    zip_write(z, data, size);
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
        put16(h + 14, 0x21);
        put32(h + 16, z->e[i].crc);
        put32(h + 20, z->e[i].size);
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
    hk_file_close(z->f);
}

/* A bundle of some of the APKs of another (their names there, and the names they get in this one). */
static bool make_bundle(const char *from, const char *to, const char *const *names, const char *const *as, int n)
{
    hk_zip src;
    zip_out z;
    char err[160];
    int i, ferr = 0;
    if (!hk_zip_open(&src, from, err, sizeof(err))) return false;
    memset(&z, 0, sizeof(z));
    z.f = hk_file_open(to, HK_O_WRITE | HK_O_CREATE | HK_O_TRUNC, &ferr);
    if (!z.f) {
        hk_zip_close(&src);
        return false;
    }
    for (i = 0; i < n; i++) {
        const hk_zip_entry *e = hk_zip_find(&src, names[i]);
        size_t size = 0;
        u8 *data = e ? hk_zip_read(&src, e, 64u << 20, &size) : NULL;
        if (data) zip_add(&z, as[i], data, (u32)size);
        free(data);
    }
    zip_end(&z);
    hk_zip_close(&src);
    return true;
}

/* ------------------------------------------------------- the test app */

/* The splits the test app's bundles keep at 320 dpi with English and Russian, by the names their manifests give. */
static const char *const k_kept[] = { "config.armeabi_v7a", "config.en", "config.mdpi", "config.ru", "config.xxhdpi", "extra", "pack" };

static char *unpack(const char *bundle, const char *apk, const char *splits, int dpi, const char *languages, seen_progress *p)
{
    char *json;
    u64 started = hk_time_monotonic_ns();
    json = meglet_bundle_unpack(bundle, apk, splits, dpi, languages, p ? progress : NULL, p);
    say("  %s: %s (%.0f ms)", bundle, json ? json : "(nothing)", (double)(hk_time_monotonic_ns() - started) / 1e6);
    return json;
}

static void test_app_bundle(const char *java_dir, const char *scratch, const char *data_dir, bool xapk)
{
    char *bundle = hk_sprintf("%s/splits.%s", java_dir, xapk ? "xapk" : "apks"), *dir = hk_sprintf("%s/%s", scratch, xapk ? "x" : "a");
    char *apk = hk_sprintf("%s/splits.apk", dir), *splits = hk_sprintf("%s/splits.apk.splits", dir), *json, *report, *path;
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
    hk_path_mkdirs(dir);
    memset(&p, 0, sizeof(p));
    json = unpack(bundle, apk, splits, 320, "en,ru", &p);
    CHECK(json && strstr(json, "\"ok\":true") && strstr(json, "\"package\":\"meglet.test.splits\"") && strstr(json, "\"versionCode\":7"));
    n = names_in(json ? json : "{}", "splits", names, 32);
    joined(names, n, text, sizeof(text));
    say("  kept: %s", text);
    CHECK(n == (int)HK_COUNT(k_kept));
    for (i = 0; i < (int)HK_COUNT(k_kept); i++) {
        CHECK(has_name(names, n, k_kept[i]));
        path = hk_sprintf("%s/split_%s.apk", splits, k_kept[i]);
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
    path = hk_sprintf("%s/splits.apk.splits/config.arm64_v8a.apk", dir);
    CHECK(!exists(path));
    free(path);
    CHECK(exists(apk) && size_of(apk) > 1000);
    /* Nothing of the work is left: no part files, no work folder. */
    path = hk_sprintf("%s.part", apk);
    CHECK(!exists(path));
    free(path);
    path = hk_sprintf("%s.part", splits);
    CHECK(!exists(path));
    free(path);
    /* The record beside the splits is the answer. */
    path = hk_sprintf("%s/bundle.json", splits);
    {
        size_t size = 0;
        char *record = (char *)hk_file_read_all(path, &size);
        CHECK(record && json && size == strlen(json) && !memcmp(record, json, size));
        free(record);
    }
    free(path);
    CHECK(p.calls > 0 && !p.backwards && p.last_done == p.last_total && p.last_total > 0);
    say("  progress: %d calls, %llu of %llu bytes at the end", p.calls, (unsigned long long)p.last_done, (unsigned long long)p.last_total);

    /* The expansion file of the .xapk, in the app's own Android/obb, where it finds it when it runs. */
    path = hk_sprintf("%s/apps/meglet.test.splits/sdcard/Android/obb/meglet.test.splits/main.7.meglet.test.splits.obb", data_dir);
    if (xapk) {
        size_t size = 0;
        char *obb = (char *)hk_file_read_all(path, &size);
        CHECK(obb && size == 41 && !memcmp(obb, "the expansion file of meglet.test.splits\n", 41));
        CHECK(json && strstr(json, "\"to\":\"/storage/emulated/0/Android/obb/meglet.test.splits/main.7.meglet.test.splits.obb\""));
        free(obb);
    } else {
        CHECK(!exists(path));
    }
    free(path);
    path = hk_sprintf("%s/icon.png", splits);
    CHECK(exists(path) == xapk);
    free(path);

    /* The report of the app as one. */
    report = meglet_apk_report_splits(apk, splits);
    say("  report: %s", report ? report : "(none)");
    CHECK(report && strstr(report, "\"ok\":true") && strstr(report, "\"package\":\"meglet.test.splits\"") && strstr(report, "\"versionName\":\"1.7\""));
    CHECK(report && strstr(report, "\"abi\":\"armeabi-v7a\"") && strstr(report, "\"engine\":\"java\"") && strstr(report, "\"runnable\":true"));
    CHECK(report && strstr(report, "\"abis\":[\"armeabi-v7a\",\"arm64-v8a\",\"x86\"]"));
    CHECK(report && strstr(report, "\"name\":\"libsplitlib.so\"") && strstr(report, "\"hasIcon\":true"));
    n = names_in(report ? report : "{}", "splits", names, 32);
    CHECK(n == (int)HK_COUNT(k_kept));
    n = names_in(report ? report : "{}", "splitsLeftOut", names, 32);
    CHECK(n == (xapk ? 4 : 5) && has_name(names, n, "config.arm64_v8a"));
    meglet_free(report);
    /* The icon is in the density split alone (the base has no picture of it). */
    CHECK(meglet_apk_icon_splits(apk, splits, &icon, &icon_size) && icon_size > 8 && !memcmp(icon, "\x89PNG", 4));
    meglet_free(icon);
    icon = NULL;
    CHECK(!meglet_apk_icon(apk, &icon, &icon_size));
    meglet_free(icon);
    meglet_free(json);

    /* Again over the first: a split the folder had that the bundle has not is gone, the rest as before. */
    path = hk_sprintf("%s/split_config.fr.apk", splits);
    hk_file_write_all(path, "stale", 5);
    json = unpack(bundle, apk, splits, 320, "en", NULL);
    CHECK(json && strstr(json, "\"ok\":true") && !exists(path));
    n = names_in(json ? json : "{}", "splits", names, 32);
    CHECK(n == (int)HK_COUNT(k_kept) - 1 && !has_name(names, n, "config.ru") && has_name(names, n, "config.en"));
    free(path);
    path = hk_sprintf("%s/split_config.ru.apk", splits);
    CHECK(!exists(path));
    free(path);
    meglet_free(json);
    remove_tree(dir);
    free(bundle);
    free(dir);
    free(apk);
    free(splits);
}

/* The base alone, which says it needs its splits; a damaged bundle; one with no base; one whose native code is for arm64 alone. */
static void test_odd_bundles(const char *java_dir, const char *scratch)
{
    char *base = hk_sprintf("%s/splits-base.apk", java_dir), *xapk = hk_sprintf("%s/splits.xapk", java_dir), *dir = hk_sprintf("%s/odd", scratch);
    char *apk = hk_sprintf("%s/odd.apk", dir), *splits = hk_sprintf("%s/odd.apk.splits", dir), *bundle, *json, *report;
    say("bundles that are not whole, and a base alone");
    if (!exists(base) || !exists(xapk)) {
        say("  (no %s or %s: tools/build-test-app.sh makes them; these checks are left out)", base, xapk);
        goto done;
    }
    hk_path_mkdirs(dir);
    report = meglet_apk_report(base);
    say("  the base alone: %s", report ? report : "(none)");
    CHECK(report && strstr(report, "\"runnable\":true") && strstr(report, "only the base of an app that comes in several APKs") && strstr(report, "\"hasIcon\":false"));
    CHECK(report && !strstr(report, "\"splits\""));
    meglet_free(report);

    /* Damaged: the first half of the .xapk, which has no central directory. */
    bundle = hk_sprintf("%s/damaged.xapk", dir);
    {
        size_t size = 0;
        u8 *bytes = hk_file_read_all(xapk, &size);
        if (bytes) hk_file_write_all(bundle, bytes, size / 2);
        free(bytes);
    }
    json = unpack(bundle, apk, splits, 320, "en", NULL);
    CHECK(json && strstr(json, "\"ok\":false") && strstr(json, "cannot be read"));
    CHECK(!exists(apk) && !exists(splits));
    meglet_free(json);
    free(bundle);

    /* No base: splits alone. */
    {
        static const char *const names[] = { "config.en.apk", "config.armeabi_v7a.apk" };
        bundle = hk_sprintf("%s/nobase.apks", dir);
        CHECK(make_bundle(xapk, bundle, names, names, 2));
        json = unpack(bundle, apk, splits, 320, "en", NULL);
        CHECK(json && strstr(json, "\"ok\":false") && strstr(json, "no base APK"));
        CHECK(!exists(apk) && !exists(splits));
        meglet_free(json);
        free(bundle);
    }

    /* SAI's names, and native code for 64-bit ARM alone: unpacked, and the report says it cannot run, and why. */
    {
        static const char *const names[] = { "meglet.test.splits.apk", "config.arm64_v8a.apk", "config.en.apk", "config.xxhdpi.apk" };
        static const char *const as[] = { "base.apk", "split_config.arm64_v8a.apk", "split_config.en.apk", "split_config.xxhdpi.apk" };
        bundle = hk_sprintf("%s/arm64.apks", dir);
        CHECK(make_bundle(xapk, bundle, names, as, 4));
        json = unpack(bundle, apk, splits, 560, "en,ru", NULL);
        CHECK(json && strstr(json, "\"ok\":true") && strstr(json, "\"base\":\"base.apk\""));
        CHECK(json && left_out_because(json, "config.arm64_v8a", "arm64-v8a"));
        meglet_free(json);
        report = meglet_apk_report_splits(apk, splits);
        say("  native code for arm64 alone: %s", report ? report : "(none)");
        CHECK(report && strstr(report, "\"runnable\":false") && strstr(report, "no 32-bit ARM code") && strstr(report, "arm64-v8a only"));
        CHECK(report && strstr(report, "\"abis\":[\"arm64-v8a\"]") && !strstr(report, "\"abi\":"));
        meglet_free(report);
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
    char *bundle = hk_sprintf("%s/%s", real_dir, b->file), *dir = hk_sprintf("%s/real", scratch);
    char *apk = hk_sprintf("%s/app.apk", dir), *splits = hk_sprintf("%s/app.apk.splits", dir), *json, *report, want[200];
    char names[64][96], text[1200];
    int n, i;
    say("a real bundle: %s, unpacked for this phone (560 dpi, English and Russian)", b->file);
    if (!exists(bundle)) {
        say("  (not in %s: left out)", real_dir);
        goto done;
    }
    hk_path_mkdirs(dir);
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
    meglet_free(json);
    report = meglet_apk_report_splits(apk, splits);
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
    meglet_free(report);
    {
        uint8_t *icon = NULL;
        size_t size = 0;
        bool found = meglet_apk_icon_splits(apk, splits, &icon, &size);
        say("  icon: %s, %u bytes", !found ? "none" : size > 4 && !memcmp(icon, "\x89PNG", 4) ? "a PNG" : size > 12 && !memcmp(icon + 8, "WEBP", 4) ? "a WebP" : "something else",
            (unsigned)size);
        CHECK(found);
        meglet_free(icon);
    }
    remove_tree(dir);
done:
    free(bundle);
    free(dir);
    free(apk);
    free(splits);
}

int hk_bundle_tests_run(const char *java_dir, const char *scratch, const char *real_dir)
{
    static const char *const bip_kept[] = { "config.armeabi_v7a", "config.xxhdpi", "config.xxxhdpi" };
    static const char *const mangadex_kept[] = { "config.en", "config.ru", "config.xxhdpi" };
    static const char *const magnit_kept[] = { "config.en", "config.xhdpi" };
    static const real_bundle real[] = {
        { "BiP_v.4.2.42-HEAD(4450)(7.0-15.0)(arm7a,arm64-8a,x86,x86-64).apks", "com.turkcell.bip", "4.2.42-HEAD", "armeabi-v7a", true, bip_kept, 3, "config.arm64_v8a", NULL },
        { "MangaDex_1.0.0.0_APKPure.xapk", "org.mangadex.mangadex", "1.0.0.0", NULL, true, mangadex_kept, 3, "config.ja", NULL },
        { "\xd0\x9c\xd0\xb0\xd0\xb3\xd0\xbd\xd0\xb8\xd1\x82_8.107.1.apks", "ru.tander.magnit", "8.107.1", NULL, false, magnit_kept, 2, "config.arm64_v8a",
          "no 32-bit ARM code: the bundle it came in has native code for arm64-v8a only" },
    };
    int i;
    g_checks = g_failed = 0;
    hk_path_mkdirs(scratch);
    test_app_bundle(java_dir, scratch, meglet_data_dir(), false);
    test_app_bundle(java_dir, scratch, meglet_data_dir(), true);
    test_odd_bundles(java_dir, scratch);
    if (real_dir)
        for (i = 0; i < (int)HK_COUNT(real); i++) test_real(real_dir, scratch, &real[i]);
    else
        say("(no folder of real bundles was named: BiP, MangaDex and Magnit are left out)");
    say("%d checks, %d failed", g_checks, g_failed);
    return g_failed;
}
