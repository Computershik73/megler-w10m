/* liblog: Android's log, into ours. */
#include "mg_bionic.h"

#include <stdio.h>

static int level_of(int prio)
{
    /* ANDROID_LOG_VERBOSE 2, DEBUG 3, INFO 4, WARN 5, ERROR 6, FATAL 7 */
    if (prio <= 3) return MG_LOG_DEBUG;
    if (prio == 4) return MG_LOG_INFO;
    if (prio == 5) return MG_LOG_WARN;
    return MG_LOG_ERROR;
}

static char letter_of(int prio)
{
    static const char l[] = "??VDIWEF";
    return prio >= 0 && prio < 8 ? l[prio] : '?';
}

static int g_log_write(int prio, const char *tag, const char *text)
{
    mg_log(level_of(prio), "%c/%s: %s", letter_of(prio), tag ? tag : "", text ? text : "");
    return 1;
}

static int g_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, ap);
    return g_log_write(prio, tag, line);
}

static int g_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

static int g_log_buf_write(int buf, int prio, const char *tag, const char *text)
{
    (void)buf;
    return g_log_write(prio, tag, text);
}

static int g_log_buf_print(int buf, int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)buf;
    va_start(ap, fmt);
    r = g_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

static void g_log_assert(const char *cond, const char *tag, const char *fmt, ...)
{
    char line[1024];
    line[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
    }
    mg_fatal("F/%s: assertion failed: %s %s", tag ? tag : "", cond ? cond : "", line);
}

static int g_log_is_loggable(int prio, const char *tag, int def) { (void)prio; (void)tag; (void)def; return 1; }
static void g_set_abort_message(const char *msg) { mg_err("abort message: %s", msg ? msg : ""); }

const mg_sym mg_tab_log[] = {
    MG_WRAP("__android_log_write", g_log_write),
    MG_WRAP("__android_log_print", g_log_print),
    MG_WRAP("__android_log_vprint", g_log_vprint),
    MG_WRAP("__android_log_buf_write", g_log_buf_write),
    MG_WRAP("__android_log_buf_print", g_log_buf_print),
    MG_WRAP("__android_log_assert", g_log_assert),
    MG_WRAP("__android_log_is_loggable", g_log_is_loggable),
    MG_WRAP("android_set_abort_message", g_set_abort_message),
    MG_END
};
