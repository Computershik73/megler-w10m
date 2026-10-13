/*
 * The guest dynamic linker.
 *
 * Takes an Android app's 32-bit ARM shared objects (armeabi-v7a, or armeabi)
 * out of its APK and makes them run inside this process: maps them, relocates
 * them, binds their imports to the bionic shims (bionic/) or to each other,
 * rewrites the few instructions that cannot run here as they are
 * (arm/hk_patch.c), and runs their constructors.
 *
 * It follows Android's linker where that decides whether a library works:
 *
 *   - an import is looked up first in the importing library's own scope --
 *     itself, then its DT_NEEDED closure breadth-first -- and only then in the
 *     system (the shims) and the other loaded libraries. Engines ship libraries
 *     that each carry a private C++ runtime, and binding one library's
 *     exceptions to another's would break both;
 *   - a library's dependencies are mapped, relocated and initialised before it;
 *   - initialisers receive (argc, argv, envp), as bionic passes them;
 *   - the app's libraries and Android's own are two sets that do not see each
 *     other, as the linker namespaces of Android 7 and later keep them: an app
 *     may carry a libcrypto.so or a libicuuc.so of its own, and Android's
 *     runtime has libraries of those very names (see "The runtime's
 *     libraries" below).
 *
 * Guest code is native code here: the phone's CPU runs it directly. See
 * docs/ARCHITECTURE.md for what differs between Android's ABI and the host's
 * and how each difference is bridged.
 */
#ifndef HK_LD_H
#define HK_LD_H

#include "../hk_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_lib hk_lib;
struct hk_zip;

/* Where libraries come from: the APKs' lib/armeabi-v7a/ (or lib/armeabi/) directories, in the order added. */
#define HK_LD_MAX_APKS 16
bool hk_ld_add_apk(const char *path);
/* The APKs added, for asset access; NULL past the last. */
const struct hk_zip *hk_ld_apk_at(int index);
/* A folder to look for libraries in by file name, after the APKs. */
void hk_ld_add_search_dir(const char *dir);
/* The ABI directory the app's libraries come from: "armeabi-v7a", "armeabi", or NULL when no APK has 32-bit ARM code. */
const char *hk_ld_abi(void);
bool hk_ld_has_lib(const char *name);
/* Whether an address is inside a loaded library. Takes no lock: for asking about a thread that has been stopped. */
bool hk_ld_is_guest_code(hk_addr pc);
int hk_ld_apk_libs(void (*cb)(const char *name, u64 size, void *user), void *user);

/* Bind every later import of `name` to `fn` instead of whatever defines it. Call before the libraries load. */
void hk_ld_interpose(const char *name, void *fn);

/*
 * Load a library by file name or soname, with everything it needs (an
 * already-loaded one is returned as it is). NULL, with the reason logged, on
 * failure. A system library ("libc.so") returns NULL without an error: the
 * system is the shims, not a library this linker owns.
 */
hk_lib *hk_ld_load(const char *name);
/* Run the constructors of a library and of anything it needs that has not run. */
bool hk_ld_init(hk_lib *lib);

hk_lib *hk_ld_find_lib(const char *name);
/* A symbol exported by `lib`, or (lib == NULL) by any loaded library in load order; then the shims. */
void *hk_ld_sym(hk_lib *lib, const char *name);
/* The library, then its dependencies, as bionic's dlsym(handle) searches. */
void *hk_ld_sym_deep(hk_lib *lib, const char *name, hk_lib **found_in);

hk_lib *hk_ld_lib_of(const void *addr);
void *hk_ld_lib_base(const hk_lib *lib);
const char *hk_ld_lib_name(const hk_lib *lib);
/* The nearest exported symbol at or below an address, for crash reports and dladdr. */
const char *hk_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr, const void **lib_base);

/* dl_iterate_phdr: load bias, name, program headers and their count; a non-zero return stops the walk. */
typedef int (*hk_ld_phdr_cb)(hk_addr bias, const char *name, const void *phdr, unsigned phnum, void *user);
int hk_ld_iterate(hk_ld_phdr_cb cb, void *user);
/* __gnu_Unwind_Find_exidx / dl_unwind_find_exidx: the unwind index of the library holding `pc`. */
const void *hk_ld_find_exidx(hk_addr pc, int *count);

/*
 * The runtime's libraries.
 *
 * The Java runtime's native half -- Android's own libjavacore.so,
 * libopenjdk.so and what they need, out of the Android root -- is loaded by
 * this linker like an app's library, and is not the app's. A library knows
 * which of the two it is. Everything that looks a library up by name looks
 * only among its own kind, and in its own places: the app's in the APKs and
 * the folders of hk_ld_add_search_dir, the runtime's in the folders of
 * hk_ld_add_runtime_dir. That holds for
 *
 *   - a DT_NEEDED entry: the library needed is found, or loaded, where the
 *     one that needs it came from, and nowhere else. An app's library that
 *     needs "libcrypto.so" gets the APK's, or fails to load as on Android;
 *     it never gets the runtime's, and the runtime's never get the app's;
 *   - an import that is not in the importing library's own scope nor the
 *     system's: looked for among the loaded libraries of the same kind only;
 *   - hk_ld_load, hk_ld_find_lib, hk_ld_has_lib and hk_ld_sym(NULL, ...),
 *     which is dlopen and dlsym(RTLD_DEFAULT): the app's. The runtime's have
 *     the calls below.
 *
 * So two libraries of one name can be loaded at once, one of each kind. The
 * system's own (libc.so, liblog.so: the shims) are one for both.
 */
void hk_ld_add_runtime_dir(const char *dir);
hk_lib *hk_ld_load_runtime(const char *name);
hk_lib *hk_ld_find_runtime_lib(const char *name);
bool hk_ld_lib_is_runtime(const hk_lib *lib);
/* A function exported by a loaded library of the one kind or the other, in the order they were loaded; never the shims'. */
void *hk_ld_sym_among(bool runtime, const char *name, hk_lib **found_in);
/*
 * An app's library that is a file of its own rather than part of an APK: one
 * the app unpacked or downloaded and names by its path (System.load). The
 * name it is known by is the file's; what it needs comes from the APKs.
 */
hk_lib *hk_ld_load_file(const char *host_path);

/* Imports that nothing provided, bound to a stub that logs when called. */
size_t hk_ld_unresolved_count(void);
/* 0 quiet, 1 one line per library, 2 every unresolved import as well. */
void hk_ld_set_verbosity(int v);

/* What was done to a library's code so it runs here, for reports and tests. */
typedef struct hk_ld_stats {
    u32 tp_reads;          /* thread-pointer reads redirected */
    u32 tp_reads_failed;   /* found but out of reach of an island */
    u32 plt_entries;       /* ARM-mode PLT entries rewritten */
    u32 arm_functions;     /* exported functions in ARM (not Thumb) mode */
    u32 thumb_functions;
    u32 unresolved;
    u32 svc_sites;         /* direct system calls seen (not handled) */
} hk_ld_stats;
void hk_ld_get_stats(const hk_lib *lib, hk_ld_stats *out);

/*
 * The address to call for a guest function pointer. Thumb code is called as it
 * is; this is where an ARM-mode function gets its stand-in once ARM code is
 * translated rather than run (docs/ARCHITECTURE.md, "ARM mode"). Every call
 * from the host into guest code goes through it.
 */
hk_addr hk_code_entry(hk_addr fn);
#define HK_GFN(type, addr) ((type)hk_code_entry((hk_addr)(addr)))

/*
 * The fault handler's part of the linker. When `pc` is a rewritten PLT entry
 * that a thread was left on in Thumb state, `*target` is where the entry jumps
 * (bit 0 says Thumb, as in any code pointer) and the result is true.
 */
bool hk_ld_plt_target(hk_addr pc, hk_addr *target);

#ifdef __cplusplus
}
#endif

#endif /* HK_LD_H */
