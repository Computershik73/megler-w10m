/*
 * The guest dynamic linker.
 *
 * Takes an Android app's 32-bit ARM shared objects (armeabi-v7a, or armeabi)
 * out of its APK and makes them run inside this process: maps them, relocates
 * them, binds their imports to the bionic shims (bionic/) or to each other,
 * rewrites the few instructions that cannot run here as they are
 * (arm/mg_patch.c), and runs their constructors.
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
#ifndef MG_LD_H
#define MG_LD_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_lib mg_lib;
struct mg_zip;

/* Where libraries come from: the APKs' lib/armeabi-v7a/ (or lib/armeabi/) directories, in the order added. */
#define MG_LD_MAX_APKS 16
bool mg_ld_add_apk(const char *path);
/*
 * Service APK libraries and their dependencies are isolated from the client's; so are a module's, whose class loader has a
 * scope of its own (dvm/mg_dvm_art.c, child scopes: 16 on), from its APK.
 */
#define MG_LD_MAX_SCOPES 64
bool mg_ld_add_service_apk(const char *path, int scope);
int mg_ld_set_scope(int scope);
int mg_ld_scope(void);
/* The APKs added, for asset access; NULL past the last. */
const struct mg_zip *mg_ld_apk_at(int index);
/* A folder to look for libraries in by file name, after the APKs. */
void mg_ld_add_search_dir(const char *dir);
/* The ABI directory the app's libraries come from: "armeabi-v7a", "armeabi", or NULL when no APK has 32-bit ARM code. */
const char *mg_ld_abi(void);
bool mg_ld_has_lib(const char *name);
/* Whether an address is inside a loaded library. Takes no lock: for asking about a thread that has been stopped. */
bool mg_ld_is_guest_code(mg_addr pc);
int mg_ld_apk_libs(void (*cb)(const char *name, u64 size, void *user), void *user);

/* Bind every later import of `name` to `fn` instead of whatever defines it. Call before the libraries load. */
void mg_ld_interpose(const char *name, void *fn);

/*
 * Load a library by file name or soname, with everything it needs (an
 * already-loaded one is returned as it is). NULL, with the reason logged, on
 * failure. A system library ("libc.so") returns NULL without an error: the
 * system is the shims, not a library this linker owns.
 */
mg_lib *mg_ld_load(const char *name);
/* Run the constructors of a library and of anything it needs that has not run. */
bool mg_ld_init(mg_lib *lib);

mg_lib *mg_ld_find_lib(const char *name);
/* A symbol exported by `lib`, or (lib == NULL) by any loaded library in load order; then the shims. */
void *mg_ld_sym(mg_lib *lib, const char *name);
/* The library, then its dependencies, as bionic's dlsym(handle) searches. */
void *mg_ld_sym_deep(mg_lib *lib, const char *name, mg_lib **found_in);

mg_lib *mg_ld_lib_of(const void *addr);
/*
 * One of Go's (its build ID note): its runtime runs goroutines on stacks it cuts from its own heap, whose free pages it
 * gives back with madvise (bionic/mg_libc_mem.c), and reaches its g through the thread pointer's segment by a register
 * on x86_64 (x86/mg_patch_x86.c).
 */
bool mg_ld_lib_is_go(const mg_lib *lib);
void *mg_ld_lib_base(const mg_lib *lib);
const char *mg_ld_lib_name(const mg_lib *lib);
/* The address space a library holds: its image and the code the patcher adds beside it. */
size_t mg_ld_lib_extent(const mg_lib *lib);
/* The nearest exported symbol at or below an address, for crash reports and dladdr. */
const char *mg_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr, const void **lib_base);

/* dl_iterate_phdr: load bias, name, program headers and their count; a non-zero return stops the walk. */
typedef int (*mg_ld_phdr_cb)(mg_addr bias, const char *name, const void *phdr, unsigned phnum, void *user);
int mg_ld_iterate(mg_ld_phdr_cb cb, void *user);
/* __gnu_Unwind_Find_exidx / dl_unwind_find_exidx: the unwind index of the library holding `pc`. */
const void *mg_ld_find_exidx(mg_addr pc, int *count);

/*
 * The runtime's libraries.
 *
 * The Java runtime's native half -- Android's own libjavacore.so,
 * libopenjdk.so and what they need, out of the Android root -- is loaded by
 * this linker like an app's library, and is not the app's. A library knows
 * which of the two it is. Everything that looks a library up by name looks
 * only among its own kind, and in its own places: the app's in the APKs and
 * the folders of mg_ld_add_search_dir, the runtime's in the folders of
 * mg_ld_add_runtime_dir. That holds for
 *
 *   - a DT_NEEDED entry: the library needed is found, or loaded, where the
 *     one that needs it came from, and nowhere else. An app's library that
 *     needs "libcrypto.so" gets the APK's, or fails to load as on Android;
 *     it never gets the runtime's, and the runtime's never get the app's;
 *   - an import that is not in the importing library's own scope nor the
 *     system's: looked for among the loaded libraries of the same kind only;
 *   - mg_ld_load, mg_ld_find_lib, mg_ld_has_lib and mg_ld_sym(NULL, ...),
 *     which is dlopen and dlsym(RTLD_DEFAULT): the app's. The runtime's have
 *     the calls below.
 *
 * So two libraries of one name can be loaded at once, one of each kind. The
 * system's own (libc.so, liblog.so: the shims) are one for both.
 */
void mg_ld_add_runtime_dir(const char *dir);
mg_lib *mg_ld_load_runtime(const char *name);
/*
 * One of the app's libraries loaded into a range the app has reserved (android_dlopen_ext with ANDROID_DLEXT_RESERVED_ADDRESS,
 * as Android 9's bionic does it): its image starts at `at`, and it must fit in `size` bytes. When it does not, NULL with
 * the reason in `why` -- unless `hint`, when it is loaded wherever it goes (ANDROID_DLEXT_RESERVED_ADDRESS_HINT).
 * `recursive`: the libraries it needs that are not loaded yet go into the rest of the range too
 * (ANDROID_DLEXT_RESERVED_ADDRESS_RECURSIVE). A library already loaded is given as it is, wherever it is.
 */
mg_lib *mg_ld_load_placed(const char *name, void *at, size_t size, bool hint, bool recursive, char *why, size_t why_size);
/*
 * The part of [p, p + size) that a library placed in the app's range occupies -- its image and the code the patcher added
 * right after it -- as [*lo, *hi); false when none does. munmap leaves it (bionic has nothing after the image, so an app
 * may unmap the rest of its range there, and Chromium's linker does).
 */
bool mg_ld_placed_overlap(const void *p, size_t size, u8 **lo, u8 **hi);
mg_lib *mg_ld_find_runtime_lib(const char *name);
bool mg_ld_lib_is_runtime(const mg_lib *lib);
/* A function exported by a loaded library of the one kind or the other, in the order they were loaded; never the shims'. */
void *mg_ld_sym_among(bool runtime, const char *name, mg_lib **found_in);
/*
 * An app's library that is a file of its own rather than part of an APK: one
 * the app unpacked or downloaded and names by its path (System.load). The
 * name it is known by is the file's; what it needs comes from the APKs.
 */
mg_lib *mg_ld_load_file(const char *host_path);

/* Imports that nothing provided, bound to a stub that logs when called. */
size_t mg_ld_unresolved_count(void);
/* Recognised direct Linux calls are intercepted by default. Diagnostic mode
 * makes them fatal UDFs instead; it applies only to subsequently loaded code. */
void mg_patch_set_trap_svc(bool on);
/* Exact registered trap lookup; returns the continuation with its ISA bit. */
bool mg_patch_svc_target(mg_addr pc, mg_addr *target);
/* A thread-pointer read left as the file has it, at a hardware breakpoint: the register it sets, and where to go on. */
bool mg_patch_tp_target(mg_addr pc, int *reg, mg_addr *target);
/*
 * ARM64: the cache's type that a read of CTR_EL0 gives an app's code (arm64/mg_patch_arm64.c), an ARMv8 core's: 64-byte
 * lines of the instruction cache (IminLine 4) and of the data cache (DminLine 4), PIPT (L1Ip 3), reservations and
 * write-backs of 64 bytes (ERG, CWG 4), neither cache coherent of itself (IDC, DIC 0), bit 31 set as the architecture
 * has it.
 */
#define MG_ARM64_CTR_EL0 0x8444C004u
/* ARM64: an instruction Linux lets user code run and Windows refuses (a read of CTR_EL0, cache maintenance by address), done to `regs`. */
bool mg_patch_sys_emulate(u32 w, mg_addr *regs);
/* The same for the instruction at `pc`, where it faulted: the thread then goes on after it. */
bool mg_patch_sys_fault(mg_addr pc, mg_addr *regs);
/*
 * 0 quiet, 1 one line per library and one naming its imports nothing provides (the first ones, as many as the line has
 * room for), 2 every unresolved import as it is met as well.
 */
void mg_ld_set_verbosity(int v);

/* What was done to a library's code so it runs here, for reports and tests. */
typedef struct mg_ld_stats {
    u32 tp_reads;          /* thread-pointer reads redirected */
    u32 tp_reads_failed;   /* found but out of reach of an island */
    u32 plt_entries;       /* ARM-mode PLT entries rewritten */
    u32 arm_functions;     /* exported functions in ARM (not Thumb) mode */
    u32 thumb_functions;
    u32 unresolved;        /* imports nothing provides, each name once */
    u32 svc_sites;         /* direct system calls intercepted */
    u32 tp_led_out;        /* x86_64: accesses of the thread pointer's segment through a register, led out to the island */
    u32 svc_led_out;       /* 32-bit ARM: of svc_sites, ARM ones of a library of Go's led out to the core (arm/mg_patch.c), not traps; ARM64: every one led out */
    u32 x18_writes;        /* ARM64: instructions of its code that write x18, Windows' TEB (arm64/mg_patch_arm64.c), less words its mapping symbols mark as data */
    u32 ctr_reads;         /* ARM64: reads of CTR_EL0 made MG_ARM64_CTR_EL0's (Windows refuses them in user mode) */
} mg_ld_stats;
void mg_ld_get_stats(const mg_lib *lib, mg_ld_stats *out);
/* The committed memory of the libraries the loader mapped: their segments, as read from their files. */
u64 mg_ld_image_bytes(void);
/* The bytes of the libraries' reservations reserved and not committed (the gaps between and after their segments). */
u64 mg_ld_reserved_bytes(void);
/* Whether a thread's stack for system calls (mg_tls.c, MG_TCB_SYSCALL) lies in [base, base + size): the census of the addresses asks. */
bool mg_tls_syscall_stack_in(const void *base, size_t size);

/*
 * The address to call for a guest function pointer. Thumb code is called as it
 * is; this is where an ARM-mode function gets its stand-in once ARM code is
 * translated rather than run (docs/ARCHITECTURE.md, "ARM mode"). Every call
 * from the host into guest code goes through it.
 */
mg_addr mg_code_entry(mg_addr fn);
#define MG_GFN(type, addr) ((type)mg_code_entry((mg_addr)(addr)))

/*
 * The fault handler's part of the linker. When `pc` is a rewritten PLT entry
 * that a thread was left on in Thumb state, `*target` is where the entry jumps
 * (bit 0 says Thumb, as in any code pointer) and the result is true.
 */
bool mg_ld_plt_target(mg_addr pc, mg_addr *target);

/*
 * The crash report's part (32-bit ARM): whether `lr` is where a call returns to, the instruction before it a BL or BLX in
 * an executable segment of the guest library lr is in -- 1 if so, 0 if lr is in such a library and is not (in its data, or
 * after any other instruction), -1 where that is not known (lr in no guest library, or another processor).
 */
int mg_ld_after_call(mg_addr lr);

#ifdef __cplusplus
}
#endif

#endif /* MG_LD_H */
