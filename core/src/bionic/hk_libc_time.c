/* <time.h> and the sleeping functions. time_t is 32 bits in 32-bit bionic. */
#include "hk_bionic.h"

#include <stdio.h>

typedef struct g_timespec {
    i32 tv_sec, tv_nsec;
} g_timespec;

typedef struct g_timeval {
    i32 tv_sec, tv_usec;
} g_timeval;

typedef struct g_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    i32 tm_gmtoff;
    u32 tm_zone;
} g_tm;

static const char g_utc_name[] = "UTC";
static char g_local_name[16] = "LOC";

/* ------------------------------------------------------------- the clocks */

static i32 g_time(i32 *out)
{
    i32 t = (i32)(hk_time_realtime_ns() / 1000000000ull);
    if (out) *out = t;
    return t;
}

static int g_gettimeofday(g_timeval *tv, void *tz)
{
    u64 now = hk_time_realtime_ns();
    (void)tz;
    if (tv) {
        tv->tv_sec = (i32)(now / 1000000000ull);
        tv->tv_usec = (i32)(now % 1000000000ull / 1000);
    }
    return 0;
}

/*
 * A clock of processor time, numbered as the kernel numbers them: CLOCK_PROCESS_CPUTIME_ID (2), CLOCK_THREAD_CPUTIME_ID
 * (3), and a negative one made by pthread_getcpuclockid or clock_getcpuclockid -- the thread or process id negated in the
 * high bits, the per-thread bit (4) set for a thread. Its time now in *now; false for any other number, and for a thread
 * that is not alive.
 */
static bool cpu_clock(int clock, u64 *now)
{
    if (clock == 2) {
        *now = hk_process_cpu_ns();
    } else if (clock == 3) {
        *now = hk_thread_cpu_ns(NULL);
    } else if (clock < 0 && (clock & 4)) {
        i32 tid = (i32)~((u32)clock >> 3 | 0xE0000000u);
        hk_thread *h = hk_gthread_host_of_tid(tid);
        if (!h) return false;
        *now = h == hk_thread_current() ? hk_thread_cpu_ns(NULL) : hk_thread_cpu_ns(h);
    } else if (clock < 0) {
        *now = hk_process_cpu_ns();
    } else {
        return false;
    }
    return true;
}

static int g_clock_gettime(int clock, g_timespec *ts)
{
    u64 now;
    switch (clock) {
    case 0: case 5: now = hk_time_realtime_ns(); break;           /* CLOCK_REALTIME, _COARSE */
    case 1: case 4: case 6: case 7:                               /* the monotonic kinds */
        now = hk_time_monotonic_ns();
        break;
    default:
        if (!cpu_clock(clock, &now)) {
            hk_set_errno(GE_INVAL);
            return -1;
        }
        break;
    }
    ts->tv_sec = (i32)(now / 1000000000ull);
    ts->tv_nsec = (i32)(now % 1000000000ull);
    return 0;
}

/* clock_getcpuclockid: the clock of a process's processor time; this process's only (getpid() is 4242 here, or 0). */
static int g_clock_getcpuclockid(int pid, i32 *clock)
{
    if (pid != 0 && pid != 4242) return GE_SRCH;
    *clock = (i32)((~(u32)(pid ? pid : 4242)) << 3) | 2;
    return 0;
}

static int g_clock_getres(int clock, g_timespec *ts)
{
    u64 now;
    if (clock > 7 || (clock < 0 && !cpu_clock(clock, &now))) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1;
    }
    return 0;
}

static int g_nanosleep(const g_timespec *req, g_timespec *rem)
{
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
        hk_set_errno(GE_INVAL);
        return -1;
    }
    hk_gsleep((u64)req->tv_sec * 1000000000ull + (u64)req->tv_nsec);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

static int g_clock_nanosleep(int clock, int flags, const g_timespec *req, g_timespec *rem)
{
    if (flags & 1) {                               /* TIMER_ABSTIME */
        u64 now = clock == 0 ? hk_time_realtime_ns() : hk_time_monotonic_ns();
        u64 at = (u64)(u32)req->tv_sec * 1000000000ull + (u64)(u32)req->tv_nsec;
        if (at > now) hk_gsleep(at - now);
        return 0;
    }
    return g_nanosleep(req, rem) == 0 ? 0 : GE_INVAL;
}

static int g_usleep(unsigned usec) { hk_gsleep((u64)usec * 1000ull); return 0; }
static unsigned g_sleep(unsigned sec) { hk_gsleep((u64)sec * 1000000000ull); return 0; }

static u64 g_process_start;
static long g_clock(void)
{
    /* CLOCKS_PER_SEC is 1000000. Wall time since start stands in for CPU time. */
    if (!g_process_start) g_process_start = hk_time_monotonic_ns();
    return (long)((hk_time_monotonic_ns() - g_process_start) / 1000ull);
}

typedef struct g_tms {
    long utime, stime, cutime, cstime;
} g_tms;

static long g_times(g_tms *t)
{
    long ticks = (long)(hk_time_monotonic_ns() / 10000000ull);   /* 100 per second */
    if (t) {
        t->utime = ticks;
        t->stime = t->cutime = t->cstime = 0;
    }
    return ticks;
}

/* -------------------------------------------------------- calendar time */

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int g_mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static void civil(i64 t, g_tm *tm)
{
    i64 days = t / 86400, rem = t % 86400;
    int y = 1970, m;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);       /* 1 January 1970 was a Thursday */
    while (days < 0) {
        y--;
        days += leap(y) ? 366 : 365;
    }
    for (;;) {
        int len = leap(y) ? 366 : 365;
        if (days < len) break;
        days -= len;
        y++;
    }
    tm->tm_year = y - 1900;
    tm->tm_yday = (int)days;
    for (m = 0; m < 12; m++) {
        int len = g_mdays[m] + (m == 1 && leap(y) ? 1 : 0);
        if (days < len) break;
        days -= len;
    }
    tm->tm_mon = m;
    tm->tm_mday = (int)days + 1;
}

/* Seconds since the epoch of a broken-down UTC time, normalising out-of-range fields as mktime must. */
static i64 from_civil(g_tm *tm)
{
    i64 y = (i64)tm->tm_year + 1900 + tm->tm_mon / 12, days = 0;
    int m = tm->tm_mon % 12, i;
    i64 k;
    if (m < 0) {
        m += 12;
        y--;
    }
    for (k = 1970; k < y; k++) days += leap((int)k) ? 366 : 365;
    for (k = y; k < 1970; k++) days -= leap((int)k) ? 366 : 365;
    for (i = 0; i < m; i++) days += g_mdays[i] + (i == 1 && leap((int)y) ? 1 : 0);
    days += tm->tm_mday - 1;
    return days * 86400 + (i64)tm->tm_hour * 3600 + (i64)tm->tm_min * 60 + tm->tm_sec;
}

static g_tm *g_gmtime_r(const i32 *t, g_tm *tm)
{
    civil(*t, tm);
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = (u32)(hk_addr)g_utc_name;
    return tm;
}

static g_tm *g_localtime_r(const i32 *t, g_tm *tm)
{
    bool dst = false;
    i32 off = hk_time_utc_offset(*t, &dst);
    civil((i64)*t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = (u32)(hk_addr)g_local_name;
    return tm;
}

static g_tm *g_gmtime(const i32 *t)
{
    static HK_THREAD_LOCAL g_tm tm;
    return g_gmtime_r(t, &tm);
}

static g_tm *g_localtime(const i32 *t)
{
    static HK_THREAD_LOCAL g_tm tm;
    return g_localtime_r(t, &tm);
}

static i32 g_timegm(g_tm *tm)
{
    i64 t = from_civil(tm);
    civil(t, tm);
    tm->tm_isdst = 0;
    return (i32)t;
}

static i32 g_mktime(g_tm *tm)
{
    /* Local time to UTC: the offset at the moment itself, found in two steps (it differs across a DST change). */
    i64 local = from_civil(tm), t;
    bool dst = false;
    i32 off = hk_time_utc_offset(local, &dst);
    t = local - off;
    off = hk_time_utc_offset(t, &dst);
    t = local - off;
    civil(t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = (u32)(hk_addr)g_local_name;
    return (i32)t;
}

/*
 * bionic's time64.h, which 32-bit code calls for times past 2038 (Chromium's base::Time does): the same with a 64-bit time.
 */
static g_tm *g_gmtime64_r(const i64 *t, g_tm *tm)
{
    civil(*t, tm);
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = (u32)(hk_addr)g_utc_name;
    return tm;
}

static g_tm *g_localtime64_r(const i64 *t, g_tm *tm)
{
    bool dst = false;
    i32 off = hk_time_utc_offset(*t, &dst);
    civil(*t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = (u32)(hk_addr)g_local_name;
    return tm;
}

static i64 g_timegm64(g_tm *tm)
{
    i64 t = from_civil(tm);
    civil(t, tm);
    tm->tm_isdst = 0;
    return t;
}

static i64 g_mktime64(g_tm *tm)
{
    i64 local = from_civil(tm), t;
    bool dst = false;
    i32 off = hk_time_utc_offset(local, &dst);
    t = local - off;
    off = hk_time_utc_offset(t, &dst);
    t = local - off;
    civil(t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = (u32)(hk_addr)g_local_name;
    return t;
}

static u64 g_difftime(i32 a, i32 b) { return hk_db((double)a - (double)b); }

static const char *const g_wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const g_mn[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static size_t g_strftime(char *out, size_t cap, const char *fmt, const g_tm *tm)
{
    size_t n = 0;
    char tmp[64];
    if (!cap) return 0;
    for (; *fmt; fmt++) {
        const char *piece = tmp;
        size_t len;
        if (*fmt != '%') {
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            fmt++;
            if (*fmt == 'E' || *fmt == 'O' || *fmt == '-' || *fmt == '_' || *fmt == '0') fmt++;
            switch (*fmt) {
            case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
            case 'C': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) / 100); break;
            case 'y': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) % 100); break;
            case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
            case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
            case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
            case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
            case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
            case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
            case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
            case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
            case 'p': piece = tm->tm_hour < 12 ? "AM" : "PM"; break;
            case 'a': snprintf(tmp, sizeof(tmp), "%.3s", g_wd[(unsigned)tm->tm_wday % 7]); break;
            case 'A': piece = g_wd[(unsigned)tm->tm_wday % 7]; break;
            case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", g_mn[(unsigned)tm->tm_mon % 12]); break;
            case 'B': piece = g_mn[(unsigned)tm->tm_mon % 12]; break;
            case 'u': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
            case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
            case 'Z': piece = tm->tm_zone ? (const char *)(hk_addr)tm->tm_zone : ""; break;
            case 'z': snprintf(tmp, sizeof(tmp), "%c%02d%02d", tm->tm_gmtoff < 0 ? '-' : '+', abs(tm->tm_gmtoff) / 3600, abs(tm->tm_gmtoff) % 3600 / 60); break;
            case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
            case 'D': case 'x': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
            case 'T': case 'X': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
            case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
            case 'r': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min, tm->tm_sec, tm->tm_hour < 12 ? "AM" : "PM"); break;
            case 'c':
                snprintf(tmp, sizeof(tmp), "%.3s %.3s %2d %02d:%02d:%02d %d", g_wd[(unsigned)tm->tm_wday % 7], g_mn[(unsigned)tm->tm_mon % 12],
                         tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
                break;
            case 's': {
                g_tm copy = *tm;
                snprintf(tmp, sizeof(tmp), "%d", (int)(from_civil(&copy) - tm->tm_gmtoff));
                break;
            }
            case 'n': piece = "\n"; break;
            case 't': piece = "\t"; break;
            case '%': piece = "%"; break;
            case 0: fmt--; piece = ""; break;
            default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
            }
        }
        len = strlen(piece);
        if (n + len >= cap) return 0;
        memcpy(out + n, piece, len);
        n += len;
    }
    out[n] = 0;
    return n;
}

static size_t g_strftime_l(char *out, size_t cap, const char *fmt, const g_tm *tm, void *loc) { (void)loc; return g_strftime(out, cap, fmt, tm); }

static char *g_asctime_r(const g_tm *tm, char *buf)
{
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", g_wd[(unsigned)tm->tm_wday % 7], g_mn[(unsigned)tm->tm_mon % 12], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}
static char *g_asctime(const g_tm *tm)
{
    static HK_THREAD_LOCAL char buf[32];
    return g_asctime_r(tm, buf);
}
static char *g_ctime_r(const i32 *t, char *buf)
{
    g_tm tm;
    return g_asctime_r(g_localtime_r(t, &tm), buf);
}
static char *g_ctime(const i32 *t)
{
    static HK_THREAD_LOCAL char buf[32];
    return g_ctime_r(t, buf);
}

static i32 g_timezone;
static int g_daylight;
static u32 g_tzname[2];

static void g_tzset(void)
{
    bool dst = false;
    i32 off = hk_time_utc_offset((i64)(hk_time_realtime_ns() / 1000000000ull), &dst);
    g_timezone = -off;
    g_daylight = dst ? 1 : 0;
    snprintf(g_local_name, sizeof(g_local_name), "GMT%c%d", off < 0 ? '-' : '+', abs(off) / 3600);
    g_tzname[0] = g_tzname[1] = (u32)(hk_addr)g_local_name;
}

static int g_settimeofday(const void *tv, const void *tz)
{
    (void)tv; (void)tz;
    hk_set_errno(GE_PERM);
    return -1;
}
static int g_itimer(int which, const void *a, void *b) { (void)which; (void)a; (void)b; return 0; }
static unsigned g_alarm(unsigned s) { (void)s; return 0; }
static int g_timer_fail(void)
{
    hk_set_errno(GE_NOSYS);
    return -1;
}

void hk_time_init(void)
{
    g_process_start = hk_time_monotonic_ns();
    g_tzset();
}

const hk_sym hk_tab_time[] = {
    HK_WRAP("time", g_time), HK_WRAP("gettimeofday", g_gettimeofday), HK_WRAP("clock_gettime", g_clock_gettime), HK_WRAP("clock_getcpuclockid", g_clock_getcpuclockid),
    HK_WRAP("clock_getres", g_clock_getres), HK_WRAP("nanosleep", g_nanosleep), HK_WRAP("clock_nanosleep", g_clock_nanosleep),
    HK_WRAP("usleep", g_usleep), HK_WRAP("sleep", g_sleep), HK_WRAP("clock", g_clock), HK_WRAP("times", g_times),
    HK_WRAP("gmtime", g_gmtime), HK_WRAP("gmtime_r", g_gmtime_r), HK_WRAP("localtime", g_localtime), HK_WRAP("localtime_r", g_localtime_r),
    HK_WRAP("mktime", g_mktime), HK_WRAP("timegm", g_timegm), HK_WRAP("timelocal", g_mktime), HK_WRAP("difftime", g_difftime),
    HK_WRAP("gmtime64_r", g_gmtime64_r), HK_WRAP("localtime64_r", g_localtime64_r), HK_WRAP("timegm64", g_timegm64), HK_WRAP("mktime64", g_mktime64),
    HK_WRAP("strftime", g_strftime), HK_WRAP("strftime_l", g_strftime_l), HK_WRAP("asctime", g_asctime), HK_WRAP("asctime_r", g_asctime_r),
    HK_WRAP("ctime", g_ctime), HK_WRAP("ctime_r", g_ctime_r), HK_WRAP("tzset", g_tzset), HK_WRAP("settimeofday", g_settimeofday),
    HK_WRAP("getitimer", g_itimer), HK_WRAP("setitimer", g_itimer), HK_WRAP("alarm", g_alarm),
    HK_WRAP("timer_create", g_timer_fail), HK_WRAP("timer_settime", g_timer_fail), HK_WRAP("timer_delete", g_timer_fail),
    HK_DATA("timezone", &g_timezone), HK_DATA("daylight", &g_daylight), HK_DATA("tzname", g_tzname),
    HK_END
};
