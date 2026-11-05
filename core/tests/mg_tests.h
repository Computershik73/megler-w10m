#ifndef MG_TESTS_H
#define MG_TESTS_H

#include "../include/megler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A line of the tests' own output; NULL sends it to stdout. */
typedef void (MEGLER_CB *mg_test_sink)(const char *line, void *user);

/*
 * Runs the core's tests. `libdir` holds the guest test libraries (NULL: skip
 * the tests that load them); `scratch` is a directory the tests may fill.
 * Returns the number of failed checks.
 */
int mg_tests_run(const char *libdir, const char *scratch, mg_test_sink sink, void *user);

/*
 * The other process of the SQLite tests (mg_tests --sqlite-hold, started by the tests on Windows): the database `db`
 * opened, a write begun in it and held, with its journal, the file `flag` made, and the write rolled back once `flag` is
 * gone (a minute at most). 0 when all of it went as it should.
 */
int mg_tests_sqlite_hold(const char *db, const char *flag);

/*
 * The Dalvik runtime's part of the tests (mg_dvm_tests.c), which mg_tests_run
 * runs with the rest. `fixtures` is the folder of its DEX files: `dvm` beside
 * the guest test libraries, unless mg_tests_set_dvm names another (the PC's
 * build has no guest libraries to be beside). `apk` is a real APK whose DEX
 * is read and partly run, or NULL.
 */
typedef struct mg_dvm_test_config {
    const char *fixtures;
    const char *apk;
    const char *guest_dir;             /* the guest test libraries, where guest code can run; else NULL */
    int trace;                         /* mg_dvm_set_trace while the tests run */
    /*
     * A folder laid out like Android's /system, with libcore in it: the
     * runtime is then started on that and everything is run on it. NULL: on
     * the built-in java.lang. mg_tests_run looks for the folder `android`
     * beside the folder of the guest test libraries (where the app's package
     * has it), unless mg_tests_set_dvm_android names another or says none
     * ("").
     */
    const char *android_root;
    const char *scratch;               /* a folder the tests may write in */
} mg_dvm_test_config;
void mg_tests_set_dvm(const char *fixtures, const char *apk, int trace);
void mg_tests_set_dvm_android(const char *root);
int mg_dvm_tests_run(const mg_dvm_test_config *config, mg_test_sink sink, void *user, int *checks);
/* How fast the interpreter is on this machine, printed. */
void mg_dvm_tests_bench(const char *fixtures, const char *android_root, int ms);

/*
 * Runs an APK for `milliseconds` with a stand-in for OpenGL ES, touches it and
 * presses Back, printing what happened. Only where guest code can run (an
 * ARM32 host); megler_init first. Returns 0 if the app started and ended cleanly.
 */
int mg_tests_smoke_run(const char *apk, int milliseconds);

/*
 * Starts one of the Java test apps (`which`: "hello" or "demos") through
 * megler_run on an Android root, with a stand-in for the shell's drawing, and
 * checks what it logs and draws (mg_java_tests.c). megler_init first, with
 * mg_java_tests_log as the log's sink. Returns the number of failed checks.
 */
int mg_tests_java_run(const char *which, const char *apk, const char *android_root, const char *picture_dir, int verbose);
/* The screen the app is given, before mg_tests_java_run: 480 x 800 at 160 dpi unless this says otherwise (a density that is a multiple of 160). */
void mg_tests_java_set_screen(int width, int height, int dpi);
/* For `app`: what to do to it once it is still ("tap:X,Y", "drag:X,Y,X2,Y2", "wait:MS", "back", "key:CODE"); the strings are kept, not copied. */
void mg_tests_java_set_script(int count, char **steps);
MEGLER_CB void mg_java_tests_log(int level, const char *line, void *user);

/*
 * Bundles of split APKs (mg_bundle_tests.c): the test app's .apks and .xapk out of `java_dir` (build/guest/java), unpacked
 * under `scratch`, and the real bundles in `real_dir` where it is not NULL. megler_init first: the expansion files go to its
 * data folder. Returns the number of failed checks.
 */
int mg_bundle_tests_run(const char *java_dir, const char *scratch, const char *real_dir);

#ifdef __cplusplus
}
#endif

#endif
