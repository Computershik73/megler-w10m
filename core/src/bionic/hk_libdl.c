/* libdl: dlopen and friends over the guest linker, and the unwinder's way of finding a library's tables. */
#include "hk_bionic.h"

#include <stdio.h>

#include "../ld/hk_ld_internal.h"
#include "hk_fd.h"
#include "../ndk/hk_egl.h"

/* What dlopen returns for a system library: its symbols are the shims'. */
static int g_system_handle;
#define SYSTEM_HANDLE ((void *)&g_system_handle)

/* 32-bit bionic's RTLD_DEFAULT and RTLD_NEXT (on 64-bit they are 0 and -1). */
#define G_RTLD_DEFAULT ((void *)(hk_addr)0xFFFFFFFFu)
#define G_RTLD_NEXT ((void *)(hk_addr)0xFFFFFFFEu)

static HK_THREAD_LOCAL char g_dlerror[200];
static HK_THREAD_LOCAL bool g_dlerror_set;

static void set_error(const char *fmt, const char *what)
{
    snprintf(g_dlerror, sizeof(g_dlerror), fmt, what);
    g_dlerror_set = true;
}

/*
 * The Android root's libraries an app may open by name: Android 7 to 9 list ICU's two among the public libraries
 * (public.libraries.android.txt), whose functions carry ICU's version in their names ("ucnv_open_60"), and apps open them:
 * MX Player's player library converts and detects the character sets of subtitles with them and ends its start-up with an
 * UnsatisfiedLinkError when it cannot. They are the copies the runtime loaded at its start, where libcore runs; without the
 * root they are not found, as before.
 */
static hk_lib *root_public_lib(const char *base)
{
    if (strcmp(base, "libicuuc.so") && strcmp(base, "libicui18n.so")) return NULL;
    return hk_ld_find_runtime_lib(base);
}

static void *g_dlopen(const char *name, int flags)
{
    const char *base;
    hk_lib *lib;
    (void)flags;
    hk_trace("dlopen(%s)", name ? name : "(null)");
    if (!name) return G_RTLD_DEFAULT;
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (hk_bionic_is_system_lib(base)) {
        if (!strcmp(base, "libvulkan.so")) {
            set_error("dlopen failed: library \"%s\" not found", name);
            return NULL;
        }
        return SYSTEM_HANDLE;
    }
    lib = hk_ld_find_lib(base);
    if (!lib) {
        if (!hk_ld_has_lib(base)) {
            lib = root_public_lib(base);
            if (lib) return lib;
            set_error("dlopen failed: library \"%s\" not found", name);
            return NULL;
        }
        lib = hk_ld_load(base);
    }
    if (!lib) {
        set_error("dlopen failed: \"%s\" could not be loaded", name);
        return NULL;
    }
    hk_ld_init(lib);
    return lib;
}

/*
 * android_dlextinfo as Android 9's bionic has it for 32-bit ARM (android/dlext.h), and its flags. Laid out by hand: the
 * guest's pointers are 32 bits whatever the host compiling this is.
 */
typedef struct g_dlextinfo {
    u64 flags;
    u32 reserved_addr, reserved_size;
    i32 relro_fd, library_fd;
    u64 library_fd_offset;
    u32 library_namespace;
} g_dlextinfo;

enum {
    DLEXT_RESERVED_ADDRESS = 0x1, DLEXT_RESERVED_ADDRESS_HINT = 0x2, DLEXT_WRITE_RELRO = 0x4, DLEXT_USE_RELRO = 0x8,
    DLEXT_USE_LIBRARY_FD = 0x10, DLEXT_USE_LIBRARY_FD_OFFSET = 0x20, DLEXT_FORCE_LOAD = 0x40, DLEXT_FORCE_FIXED_VADDR = 0x80,
    DLEXT_LOAD_AT_FIXED_ADDRESS = 0x100, DLEXT_USE_NAMESPACE = 0x200, DLEXT_RESERVED_ADDRESS_RECURSIVE = 0x400, DLEXT_VALID = 0x7FF,
};

/* A library's PT_GNU_RELRO, page by page as bionic serializes it: [start, start + size). False when it has none. */
static bool relro_of(const hk_lib *lib, u8 **start, size_t *size)
{
    int i;
    for (i = 0; i < lib->phnum; i++) {
        const Elf32_Phdr *p = &lib->phdr[i];
        if (p->p_type != PT_GNU_RELRO) continue;
        *start = lib->base + HK_ALIGN_DOWN(p->p_vaddr, HK_PAGE);
        *size = (size_t)(HK_ALIGN_UP(p->p_vaddr + p->p_memsz, HK_PAGE) - HK_ALIGN_DOWN(p->p_vaddr, HK_PAGE));
        return true;
    }
    return false;
}

/*
 * android_dlopen_ext, for what apps ask of it. Chromium's linker (Brave, every Chromium-based browser, WebView's loader)
 * reserves address space and has its library loaded at the start of it (ANDROID_DLEXT_RESERVED_ADDRESS), then reads the
 * library's ELF header there; and it shares the library's RELRO between its processes.
 *  - RESERVED_ADDRESS: the library's image at the reserved range's start, or a failure that says it does not fit;
 *    RESERVED_ADDRESS_HINT: there if it fits, anywhere if not; RESERVED_ADDRESS_RECURSIVE: what it needs too
 *    (hk_ld_load_placed). A library already loaded is given as it is, as bionic gives it.
 *  - WRITE_RELRO: the RELRO, relocated, is written to relro_fd, as bionic serializes it. There is one process here, so
 *    nothing maps it back; the pages are the library's own and already hold those bytes.
 *  - USE_RELRO: accepted. Mapping another process's relocated RELRO over identical pages saves memory between
 *    processes, and there is one.
 * The others are noted once and the library is loaded by its name.
 */
static void *g_android_dlopen_ext(const char *name, int flags, const void *info)
{
    const g_dlextinfo *x = (const g_dlextinfo *)info;
    const char *base;
    char why[200];
    hk_lib *lib;
    if (!x || !name || !(x->flags & (DLEXT_RESERVED_ADDRESS | DLEXT_RESERVED_ADDRESS_HINT | DLEXT_WRITE_RELRO | DLEXT_USE_RELRO))) return g_dlopen(name, flags);
    hk_trace("android_dlopen_ext(%s, flags 0x%x, at %08x, %u bytes, relro fd %d)", name, (unsigned)x->flags, (unsigned)x->reserved_addr,
             (unsigned)x->reserved_size, (int)x->relro_fd);
    if (x->flags & ~(u64)DLEXT_VALID) {
        snprintf(g_dlerror, sizeof(g_dlerror), "invalid extended flags to android_dlopen_ext: 0x%llx", (unsigned long long)x->flags);
        g_dlerror_set = true;
        return NULL;
    }
    if (x->flags & (DLEXT_USE_LIBRARY_FD | DLEXT_FORCE_FIXED_VADDR | DLEXT_LOAD_AT_FIXED_ADDRESS | DLEXT_USE_NAMESPACE))
        hk_note_once("android_dlopen_ext: flags 0x%x besides the reserved address and RELRO are not done; the library is loaded by its name", (unsigned)x->flags);
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (hk_bionic_is_system_lib(base)) return g_dlopen(name, flags);
    lib = hk_ld_find_lib(base);
    if (!lib && (x->flags & (DLEXT_RESERVED_ADDRESS | DLEXT_RESERVED_ADDRESS_HINT)) && x->reserved_addr) {
        if (!hk_ld_has_lib(base)) {
            set_error("dlopen failed: library \"%s\" not found", name);
            return NULL;
        }
        lib = hk_ld_load_placed(base, (void *)(hk_addr)x->reserved_addr, x->reserved_size, !(x->flags & DLEXT_RESERVED_ADDRESS),
                                (x->flags & DLEXT_RESERVED_ADDRESS_RECURSIVE) != 0, why, sizeof(why));
        if (!lib) {
            set_error("%s", why[0] ? why : "dlopen failed: the library could not be loaded");
            return NULL;
        }
    } else if (!lib) {
        return g_dlopen(name, flags);
    }
    if ((x->flags & DLEXT_WRITE_RELRO) && x->relro_fd >= 0) {
        u8 *start;
        size_t size, done = 0;
        if (relro_of(lib, &start, &size)) {
            while (done < size) {
                i32 n = hk_fd_write(x->relro_fd, start + done, (u32)(size - done));
                if (n <= 0) break;
                done += (size_t)n;
            }
            if (done < size) {
                set_error("dlopen failed: the RELRO of \"%s\" could not be written", name);
                return NULL;
            }
        }
    }
    hk_ld_init(lib);
    return lib;
}

static void *g_dlsym(void *handle, const char *name)
{
    void *p;
    if (!name) return NULL;
    if (!handle || handle == G_RTLD_DEFAULT || handle == G_RTLD_NEXT) p = hk_ld_sym(NULL, name);
    else if (handle == SYSTEM_HANDLE) p = hk_bionic_find(name) ? hk_bionic_find(name) : hk_egl_resolve(name);
    else {
        p = hk_ld_sym_deep((hk_lib *)handle, name, NULL);
        if (!p) p = hk_bionic_find(name);
    }
    if (!p) set_error("undefined symbol: %s", name);
    return p;
}

static void *g_dlvsym(void *handle, const char *name, const char *version)
{
    (void)version;
    return g_dlsym(handle, name);
}

static int g_dlclose(void *handle) { (void)handle; return 0; }

static const char *g_dlerror_fn(void)
{
    if (!g_dlerror_set) return NULL;
    g_dlerror_set = false;
    return g_dlerror;
}

typedef struct g_dl_info {
    u32 dli_fname, dli_fbase, dli_sname, dli_saddr;
} g_dl_info;

static int g_dladdr(const void *addr, g_dl_info *info)
{
    const char *lib = NULL, *sym;
    const void *sym_addr = NULL, *base = NULL;
    sym = hk_ld_symbol_at(addr, &lib, &sym_addr, &base);
    if (!lib) return 0;
    info->dli_fname = (u32)(hk_addr)lib;
    info->dli_fbase = (u32)(hk_addr)base;
    info->dli_sname = (u32)(hk_addr)sym;
    info->dli_saddr = (u32)(hk_addr)sym_addr;
    return 1;
}

typedef struct g_dl_phdr_info {
    u32 dlpi_addr, dlpi_name, dlpi_phdr;
    u16 dlpi_phnum;
    u64 dlpi_adds, dlpi_subs;
    u32 dlpi_tls_modid, dlpi_tls_data;
} g_dl_phdr_info;

typedef struct iterate_ctx {
    hk_addr cb;
    void *data;
} iterate_ctx;

static int iterate_one(hk_addr bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    iterate_ctx *c = (iterate_ctx *)user;
    g_dl_phdr_info info;
    memset(&info, 0, sizeof(info));
    info.dlpi_addr = (u32)bias;
    info.dlpi_name = (u32)(hk_addr)name;
    info.dlpi_phdr = (u32)(hk_addr)phdr;
    info.dlpi_phnum = (u16)phnum;
    info.dlpi_adds = 1;
    return HK_GFN(int (*)(g_dl_phdr_info *, size_t, void *), c->cb)(&info, sizeof(info), c->data);
}

static int g_dl_iterate_phdr(hk_addr cb, void *data)
{
    iterate_ctx c;
    c.cb = cb;
    c.data = data;
    return hk_ld_iterate(iterate_one, &c);
}

/* The ARM unwinder asks the C library which exception index covers a return address. */
static const void *g_find_exidx(hk_addr pc, int *count) { return hk_ld_find_exidx(pc, count); }

typedef struct g_tls_index {
    u32 module, offset;
} g_tls_index;

static void *g_tls_get_addr(const g_tls_index *ti) { return hk_tls_get_addr(ti->module, ti->offset); }

static int g_target_sdk(void) { return 28; }
static void g_set_target_sdk(int v) { (void)v; }
static void *g_android_get_exported_namespace(const char *name) { (void)name; return NULL; }
static void g_android_update_ld_library_path(const char *path) { (void)path; }

/*
 * The linker's own entry points, which Android 8 and later's libdl.so calls (ld-android.so, bionic/linker/dlfcn.cpp): dlopen,
 * dlsym and the rest, with the caller's address added, by which Android picks the caller's linker namespace. Apps look them
 * up in libdl.so to open a library as another library would: Instagram's app state logger
 * (AppStateLoggerNative.registerSelfSigkillHandlers) threw "Could not find __loader_dlopen from libdl.so" and ended the app's
 * start. There are no namespaces here, so the caller changes nothing.
 */
static void *g_loader_dlopen(const char *name, int flags, const void *caller) { (void)caller; return g_dlopen(name, flags); }
static void *g_loader_android_dlopen_ext(const char *name, int flags, const void *info, const void *caller)
{
    (void)caller;
    return g_android_dlopen_ext(name, flags, info);
}
static void *g_loader_dlsym(void *handle, const char *name, const void *caller) { (void)caller; return g_dlsym(handle, name); }
static void *g_loader_dlvsym(void *handle, const char *name, const char *version, const void *caller)
{
    (void)caller;
    return g_dlvsym(handle, name, version);
}

const hk_sym hk_tab_dl[] = {
    HK_WRAP("dlopen", g_dlopen),
    HK_WRAP("android_dlopen_ext", g_android_dlopen_ext),
    HK_WRAP("dlsym", g_dlsym),
    HK_WRAP("dlvsym", g_dlvsym),
    HK_WRAP("dlclose", g_dlclose),
    HK_WRAP("dlerror", g_dlerror_fn),
    HK_WRAP("dladdr", g_dladdr),
    HK_WRAP("dl_iterate_phdr", g_dl_iterate_phdr),
    HK_WRAP("dl_unwind_find_exidx", g_find_exidx),
    HK_WRAP("__gnu_Unwind_Find_exidx", g_find_exidx),
    HK_WRAP("__tls_get_addr", g_tls_get_addr),
    HK_WRAP("android_get_application_target_sdk_version", g_target_sdk),
    HK_WRAP("android_set_application_target_sdk_version", g_set_target_sdk),
    HK_WRAP("android_get_device_api_level", hk_sdk_level),
    HK_WRAP("android_get_exported_namespace", g_android_get_exported_namespace),
    HK_WRAP("android_update_LD_LIBRARY_PATH", g_android_update_ld_library_path),
    HK_WRAP("__loader_dlopen", g_loader_dlopen),
    HK_WRAP("__loader_android_dlopen_ext", g_loader_android_dlopen_ext),
    HK_WRAP("__loader_dlsym", g_loader_dlsym),
    HK_WRAP("__loader_dlvsym", g_loader_dlvsym),
    HK_WRAP("__loader_dlclose", g_dlclose),
    HK_WRAP("__loader_dlerror", g_dlerror_fn),
    HK_WRAP("__loader_dladdr", g_dladdr),
    HK_WRAP("__loader_dl_iterate_phdr", g_dl_iterate_phdr),
    HK_WRAP("__loader_dl_unwind_find_exidx", g_find_exidx),
    HK_WRAP("__loader_android_get_application_target_sdk_version", g_target_sdk),
    HK_WRAP("__loader_android_set_application_target_sdk_version", g_set_target_sdk),
    HK_WRAP("__loader_android_get_exported_namespace", g_android_get_exported_namespace),
    HK_WRAP("__loader_android_update_LD_LIBRARY_PATH", g_android_update_ld_library_path),
    HK_END
};
