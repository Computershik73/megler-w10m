/* <time.h> and the sleeping functions. time_t is a long: 32 bits in 32-bit bionic, 64 in 64-bit (gtime). */
#include "mg_bionic.h"

#include <stdio.h>

typedef struct g_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    glong tm_gmtoff;
    const char *tm_zone;
} g_tm;

static const char g_utc_name[] = "UTC";
static char g_local_name[16] = "LOC";

/* ------------------------------------------------------------- the clocks */

static gtime g_time(gtime *out)
{
    gtime t = (gtime)(mg_time_realtime_ns() / 1000000000ull);
    if (out) *out = t;
    return t;
}

static int g_gettimeofday(g_timeval *tv, void *tz)
{
    u64 now = mg_time_realtime_ns();
    (void)tz;
    if (tv) {
        tv->tv_sec = (gtime)(now / 1000000000ull);
        tv->tv_usec = (glong)(now % 1000000000ull / 1000);
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
        *now = mg_process_cpu_ns();
    } else if (clock == 3) {
        *now = mg_thread_cpu_ns(NULL);
    } else if (clock < 0 && (clock & 4)) {
        i32 tid = (i32)~((u32)clock >> 3 | 0xE0000000u);
        mg_thread *h = mg_gthread_host_of_tid(tid);
        if (!h) return false;
        *now = h == mg_thread_current() ? mg_thread_cpu_ns(NULL) : mg_thread_cpu_ns(h);
    } else if (clock < 0) {
        *now = mg_process_cpu_ns();
    } else {
        return false;
    }
    return true;
}

static int g_clock_gettime(int clock, g_timespec *ts)
{
    u64 now;
    switch (clock) {
    case 0: case 5: now = mg_time_realtime_ns(); break;           /* CLOCK_REALTIME, _COARSE */
    case 1: case 4: case 6: case 7:                               /* the monotonic kinds */
        now = mg_time_monotonic_ns();
        break;
    default:
        if (!cpu_clock(clock, &now)) {
            mg_set_errno(GE_INVAL);
            return -1;
        }
        break;
    }
    ts->tv_sec = (gtime)(now / 1000000000ull);
    ts->tv_nsec = (glong)(now % 1000000000ull);
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
        mg_set_errno(GE_INVAL);
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
        mg_set_errno(GE_INVAL);
        return -1;
    }
    mg_gsleep((u64)req->tv_sec * 1000000000ull + (u64)req->tv_nsec);
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

static int g_clock_nanosleep(int clock, int flags, const g_timespec *req, g_timespec *rem)
{
    if (flags & 1) {                               /* TIMER_ABSTIME */
        u64 now = clock == 0 ? mg_time_realtime_ns() : mg_time_monotonic_ns();
        u64 at = (u64)(gulong)req->tv_sec * 1000000000ull + (u64)(gulong)req->tv_nsec;
        if (at > now) mg_gsleep(at - now);
        return 0;
    }
    return g_nanosleep(req, rem) == 0 ? 0 : GE_INVAL;
}

static int g_usleep(unsigned usec) { mg_gsleep((u64)usec * 1000ull); return 0; }
static unsigned g_sleep(unsigned sec) { mg_gsleep((u64)sec * 1000000000ull); return 0; }

static u64 g_process_start;
static long g_clock(void)
{
    /* CLOCKS_PER_SEC is 1000000. Wall time since start stands in for CPU time. */
    if (!g_process_start) g_process_start = mg_time_monotonic_ns();
    return (long)((mg_time_monotonic_ns() - g_process_start) / 1000ull);
}

typedef struct g_tms {
    long utime, stime, cutime, cstime;
} g_tms;

static long g_times(g_tms *t)
{
    long ticks = (long)(mg_time_monotonic_ns() / 10000000ull);   /* 100 per second */
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

static g_tm *g_gmtime_r(const gtime *t, g_tm *tm)
{
    civil(*t, tm);
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = g_utc_name;
    return tm;
}

static g_tm *g_localtime_r(const gtime *t, g_tm *tm)
{
    bool dst = false;
    i32 off = mg_time_utc_offset(*t, &dst);
    civil((i64)*t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = g_local_name;
    return tm;
}

static g_tm *g_gmtime(const gtime *t)
{
    static MG_THREAD_LOCAL g_tm tm;
    return g_gmtime_r(t, &tm);
}

static g_tm *g_localtime(const gtime *t)
{
    static MG_THREAD_LOCAL g_tm tm;
    return g_localtime_r(t, &tm);
}

static gtime g_timegm(g_tm *tm)
{
    i64 t = from_civil(tm);
    civil(t, tm);
    tm->tm_isdst = 0;
    return (gtime)t;
}

static gtime g_mktime(g_tm *tm)
{
    /* Local time to UTC: the offset at the moment itself, found in two steps (it differs across a DST change). */
    i64 local = from_civil(tm), t;
    bool dst = false;
    i32 off = mg_time_utc_offset(local, &dst);
    t = local - off;
    off = mg_time_utc_offset(t, &dst);
    t = local - off;
    civil(t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = g_local_name;
    return (gtime)t;
}

/*
 * bionic's time64.h, which 32-bit code calls for times past 2038 (Chromium's base::Time does): the same with a 64-bit time.
 */
static g_tm *g_gmtime64_r(const i64 *t, g_tm *tm)
{
    civil(*t, tm);
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = g_utc_name;
    return tm;
}

static g_tm *g_localtime64_r(const i64 *t, g_tm *tm)
{
    bool dst = false;
    i32 off = mg_time_utc_offset(*t, &dst);
    civil(*t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = g_local_name;
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
    i32 off = mg_time_utc_offset(local, &dst);
    t = local - off;
    off = mg_time_utc_offset(t, &dst);
    t = local - off;
    civil(t + off, tm);
    tm->tm_isdst = dst ? 1 : 0;
    tm->tm_gmtoff = off;
    tm->tm_zone = g_local_name;
    return t;
}

static gdouble g_difftime(gtime a, gtime b) { return mg_gdb((double)a - (double)b); }

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
            case 'Z': piece = tm->tm_zone ? tm->tm_zone : ""; break;
            case 'z': {
                int off = (int)tm->tm_gmtoff;
                snprintf(tmp, sizeof(tmp), "%c%02d%02d", off < 0 ? '-' : '+', abs(off) / 3600, abs(off) % 3600 / 60);
                break;
            }
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
                snprintf(tmp, sizeof(tmp), "%lld", (long long)(from_civil(&copy) - tm->tm_gmtoff));
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
    static MG_THREAD_LOCAL char buf[32];
    return g_asctime_r(tm, buf);
}
static char *g_ctime_r(const gtime *t, char *buf)
{
    g_tm tm;
    return g_asctime_r(g_localtime_r(t, &tm), buf);
}
static char *g_ctime(const gtime *t)
{
    static MG_THREAD_LOCAL char buf[32];
    return g_ctime_r(t, buf);
}

static glong g_timezone;
static int g_daylight;
static char *g_tzname[2];

static void g_tzset(void)
{
    bool dst = false;
    i32 off = mg_time_utc_offset((i64)(mg_time_realtime_ns() / 1000000000ull), &dst);
    g_timezone = -off;
    g_daylight = dst ? 1 : 0;
    snprintf(g_local_name, sizeof(g_local_name), "GMT%c%d", off < 0 ? '-' : '+', abs(off) / 3600);
    g_tzname[0] = g_tzname[1] = g_local_name;
}

static int g_settimeofday(const void *tv, const void *tz)
{
    (void)tv; (void)tz;
    mg_set_errno(GE_PERM);
    return -1;
}
/* No interval timer is ever armed (no SIGALRM is sent): getitimer says so, and setitimer's old one was none. Each
   struct itimerval is two timevals. */
static int g_getitimer(int which, g_timeval *cur)
{
    (void)which;
    if (cur) memset(cur, 0, 2 * sizeof(g_timeval));
    return 0;
}
static int g_setitimer(int which, const g_timeval *set, g_timeval *old)
{
    (void)set;
    return g_getitimer(which, old);
}
static unsigned g_alarm(unsigned s) { (void)s; return 0; }
static int g_timer_fail(void)
{
    mg_set_errno(GE_NOSYS);
    return -1;
}

void mg_time_init(void)
{
    g_process_start = mg_time_monotonic_ns();
    g_tzset();
}

const mg_sym mg_tab_time[] = {
    MG_WRAP("time", g_time), MG_WRAP("gettimeofday", g_gettimeofday), MG_WRAP("clock_gettime", g_clock_gettime), MG_WRAP("clock_getcpuclockid", g_clock_getcpuclockid),
    MG_WRAP("clock_getres", g_clock_getres), MG_WRAP("nanosleep", g_nanosleep), MG_WRAP("clock_nanosleep", g_clock_nanosleep),
    MG_WRAP("usleep", g_usleep), MG_WRAP("sleep", g_sleep), MG_WRAP("clock", g_clock), MG_WRAP("times", g_times),
    MG_WRAP("gmtime", g_gmtime), MG_WRAP("gmtime_r", g_gmtime_r), MG_WRAP("localtime", g_localtime), MG_WRAP("localtime_r", g_localtime_r),
    MG_WRAP("mktime", g_mktime), MG_WRAP("timegm", g_timegm), MG_WRAP("timelocal", g_mktime), MG_WRAP("difftime", g_difftime),
    MG_WRAP("gmtime64_r", g_gmtime64_r), MG_WRAP("localtime64_r", g_localtime64_r), MG_WRAP("timegm64", g_timegm64), MG_WRAP("mktime64", g_mktime64),
    MG_WRAP("strftime", g_strftime), MG_WRAP("strftime_l", g_strftime_l), MG_WRAP("asctime", g_asctime), MG_WRAP("asctime_r", g_asctime_r),
    MG_WRAP("ctime", g_ctime), MG_WRAP("ctime_r", g_ctime_r), MG_WRAP("tzset", g_tzset), MG_WRAP("settimeofday", g_settimeofday),
    MG_WRAP("getitimer", g_getitimer), MG_WRAP("setitimer", g_setitimer), MG_WRAP("alarm", g_alarm),
    MG_WRAP("timer_create", g_timer_fail), MG_WRAP("timer_settime", g_timer_fail), MG_WRAP("timer_delete", g_timer_fail),
    MG_DATA("timezone", &g_timezone), MG_DATA("daylight", &g_daylight), MG_DATA("tzname", g_tzname),
    MG_END
};
