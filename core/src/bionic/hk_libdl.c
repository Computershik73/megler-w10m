/* libdl: dlopen and friends over the guest linker, and the unwinder's way of finding a library's tables. */
#include "hk_bionic.h"

#include <stdio.h>

#include "../ld/hk_ld_internal.h"
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

static void *g_android_dlopen_ext(const char *name, int flags, const void *info)
{
    (void)info;
    return g_dlopen(name, flags);
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
    HK_WRAP("android_get_device_api_level", g_target_sdk),
    HK_WRAP("android_get_exported_namespace", g_android_get_exported_namespace),
    HK_WRAP("android_update_LD_LIBRARY_PATH", g_android_update_ld_library_path),
    HK_END
};
