/*
 * The few things of mg_base.c's that the host layer itself calls (mg_alloc, mg_strdup, mg_log...), for Megler.exe on
 * desktop Windows, where mg_base.c is part of the core's ELF and the host layer is not (docs/DESKTOP.md). The same
 * allocator: the core's malloc is the UCRT's (mg_crt_bridge.c), so a block either side allocates the other may free. The
 * log's lines are formatted here with Windows' C library and go to the core's log once it is loaded (mg_log_line), to the
 * debugger and standard error before.
 *
 * Compiled only into the desktop builds (MG_ELF_HOST).
 */
#if defined(MG_ELF_HOST)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdio.h>

#include "mg_core_loader.h"

static void (*volatile g_core_log)(int level, const char *line);

void *mg_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) mg_fatal("out of memory (%u bytes)", (unsigned)n);
    return p;
}

void *mg_realloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) mg_fatal("out of memory (%u bytes)", (unsigned)n);
    return q;
}

char *mg_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)mg_alloc(n);
    memcpy(d, s, n);
    return d;
}

char *mg_sprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    char *s;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    s = (char *)mg_alloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

size_t mg_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

/* The core's log, once it is loaded (mg_core_load calls this). */
void mg_host_base_use_core_log(void)
{
    g_core_log = (void (*)(int, const char *))mg_core_sym("mg_log_line");
}

void mg_logv(int level, const char *fmt, va_list ap)
{
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, ap);
    if (g_core_log) {
        g_core_log(level, line);
        return;
    }
    if (level < MG_LOG_INFO) return;
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
    fputs(line, stderr);
    fputc('\n', stderr);
}

void mg_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_logv(level, fmt, ap);
    va_end(ap);
}

void mg_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_logv(MG_LOG_ERROR, fmt, ap);
    va_end(ap);
    abort();
}

#endif /* MG_ELF_HOST */
