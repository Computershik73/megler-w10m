/*
 * <stdio.h> over guest file descriptors.
 *
 * A guest FILE is this layer's own. Its first bytes have the layout of
 * bionic's struct __sFILE, because code built against old NDK headers reaches
 * into it: stdin, stdout and stderr are &__sF[0..2], sizeof(FILE) apart (84
 * bytes in 32-bit Android, 152 in 64-bit), and getc/putc/feof/fileno may be
 * macros that read _r, _w, _flags and _file. The counters are kept at zero so
 * those macros always call the functions behind them (__srget, __swbuf).
 *
 * Formatting is the host's vsnprintf and vsscanf: a guest va_list is a host
 * va_list (bionic/mg_bionic.h).
 */
#include "mg_bionic.h"

#include <stdio.h>

#include "mg_fd.h"

/* struct __sFILE: _p, _r and _w, then _flags and _file, shorts in 32-bit Android and ints in 64-bit. */
#if MG_GUEST_LP64
#define SF_SIZE 152
#define SF_FLAGS 16
#define SF_FILE 20
#else
#define SF_SIZE 84
#define SF_FLAGS 12
#define SF_FILE 14
#endif
#define SF_EOF 0x0020
#define SF_ERR 0x0040

static void sf_put(u8 *fp, int at, int v)
{
#if MG_GUEST_LP64
    mg_wr32(fp + at, (u32)v);
#else
    mg_wr16(fp + at, (u16)v);
#endif
}

typedef struct fstate {
    int fd;
    bool readable, writable, eof, err, line_buffered, unbuffered;
    int unget;                     /* -1: none */
    u8 *rbuf;
    u32 rpos, rlen;
    u8 *wbuf;
    u32 wlen;
    /* funopen: the guest's own read/write/seek/close */
    void *cookie;
    mg_addr fn_read, fn_write, fn_seek, fn_close;
    bool has_cookie;
    /* open_memstream */
    char **mem_ptr;
    size_t *mem_size;
    char *mem;
    size_t mem_len, mem_cap;
    bool is_memstream;
    volatile i32 lock;             /* flockfile: owner thread id, recursion below */
    i32 lock_depth;
} fstate;

#define BUF_SIZE 4096

/* The three standard streams, where old headers expect them. */
static u8 g_sF[3 * SF_SIZE];
static fstate g_std[3];
static void *g_stdin_ptr = g_sF, *g_stdout_ptr = g_sF + SF_SIZE, *g_stderr_ptr = g_sF + 2 * SF_SIZE;
static bool g_std_inited;

static void std_init(void)
{
    int i;
    if (g_std_inited) return;
    g_std_inited = true;
    for (i = 0; i < 3; i++) {
        g_std[i].fd = i;
        g_std[i].unget = -1;
        g_std[i].readable = i == 0;
        g_std[i].writable = i != 0;
        g_std[i].unbuffered = i != 0;      /* the descriptor behind them assembles lines for the log */
        sf_put(g_sF + i * SF_SIZE, SF_FILE, i);
    }
}

static fstate *fs_of(void *fp)
{
    u8 *p = (u8 *)fp;
    std_init();
    if (p >= g_sF && p < g_sF + sizeof(g_sF)) return &g_std[(p - g_sF) / SF_SIZE];
    return (fstate *)(p + SF_SIZE);
}

static void sync_head(void *fp, fstate *f)
{
    sf_put((u8 *)fp, SF_FLAGS, (f->eof ? SF_EOF : 0) | (f->err ? SF_ERR : 0));
    sf_put((u8 *)fp, SF_FILE, f->fd);
}

static void *file_new(int fd, bool readable, bool writable)
{
    u8 *block = (u8 *)mg_alloc(SF_SIZE + sizeof(fstate));
    fstate *f = (fstate *)(block + SF_SIZE);
    f->fd = fd;
    f->readable = readable;
    f->writable = writable;
    f->unget = -1;
    sync_head(block, f);
    return block;
}

/* --------------------------------------------------------------- raw I/O */

static i32 raw_read(fstate *f, void *buf, u32 n)
{
    if (f->has_cookie) return f->fn_read ? MG_GFN(int (*)(void *, char *, int), f->fn_read)(f->cookie, (char *)buf, (int)n) : 0;
    if (f->is_memstream) return 0;
    return mg_fd_read(f->fd, buf, n);
}

static i32 raw_write(fstate *f, const void *buf, u32 n)
{
    if (f->has_cookie) return f->fn_write ? MG_GFN(int (*)(void *, const char *, int), f->fn_write)(f->cookie, (const char *)buf, (int)n) : (i32)n;
    if (f->is_memstream) {
        if (f->mem_len + n + 1 > f->mem_cap) {
            f->mem_cap = (f->mem_len + n + 1) * 2;
            f->mem = (char *)mg_g_realloc(f->mem, f->mem_cap);
            if (!f->mem) return -1;
        }
        memcpy(f->mem + f->mem_len, buf, n);
        f->mem_len += n;
        f->mem[f->mem_len] = 0;
        *f->mem_ptr = f->mem;
        *f->mem_size = f->mem_len;
        return (i32)n;
    }
    return mg_fd_write(f->fd, buf, n);
}

static int flush_write(void *fp, fstate *f)
{
    u32 done = 0;
    while (done < f->wlen) {
        i32 r = raw_write(f, f->wbuf + done, f->wlen - done);
        if (r <= 0) {
            f->err = true;
            f->wlen = 0;
            sync_head(fp, f);
            return -1;
        }
        done += (u32)r;
    }
    f->wlen = 0;
    return 0;
}

/* Bytes buffered for reading are ahead of where the descriptor thinks it is: step back before writing or seeking. */
static void drop_read(fstate *f)
{
    i32 ahead = (i32)(f->rlen - f->rpos) + (f->unget >= 0 ? 1 : 0);
    if (ahead > 0 && !f->has_cookie && !f->is_memstream) mg_fd_seek(f->fd, -(i64)ahead, 1);
    else if (ahead > 0 && f->has_cookie && f->fn_seek) MG_GFN(goff (*)(void *, goff, int), f->fn_seek)(f->cookie, -ahead, 1);
    f->rpos = f->rlen = 0;
    f->unget = -1;
}

static size_t file_write(void *fp, const void *data, size_t n)
{
    fstate *f = fs_of(fp);
    const u8 *p = (const u8 *)data;
    size_t left = n;
    if (!f->writable) {
        f->err = true;
        sync_head(fp, f);
        mg_set_errno(GE_BADF);
        return 0;
    }
    if (f->rlen || f->unget >= 0) drop_read(f);
    if (f->unbuffered) {
        while (left) {
            i32 r = raw_write(f, p, (u32)left);
            if (r <= 0) {
                f->err = true;
                sync_head(fp, f);
                break;
            }
            p += r;
            left -= (size_t)r;
        }
        return n - left;
    }
    if (!f->wbuf) f->wbuf = (u8 *)mg_alloc(BUF_SIZE);
    while (left) {
        u32 room = BUF_SIZE - f->wlen, take = left < room ? (u32)left : room;
        memcpy(f->wbuf + f->wlen, p, take);
        f->wlen += take;
        p += take;
        left -= take;
        if (f->wlen == BUF_SIZE && flush_write(fp, f) != 0) return n - left - take;
    }
    if (f->line_buffered && memchr(data, '\n', n)) flush_write(fp, f);
    return n;
}

static int fill_read(void *fp, fstate *f)
{
    i32 r;
    if (f->wlen) flush_write(fp, f);
    if (!f->rbuf) f->rbuf = (u8 *)mg_alloc(BUF_SIZE);
    r = raw_read(f, f->rbuf, BUF_SIZE);
    f->rpos = 0;
    if (r <= 0) {
        f->rlen = 0;
        if (r < 0) f->err = true;
        else f->eof = true;
        sync_head(fp, f);
        return -1;
    }
    f->rlen = (u32)r;
    return 0;
}

static int file_getc(void *fp)
{
    fstate *f = fs_of(fp);
    if (f->unget >= 0) {
        int c = f->unget;
        f->unget = -1;
        return c;
    }
    if (f->rpos == f->rlen && (!f->readable || fill_read(fp, f) != 0)) return -1;
    return f->rbuf[f->rpos++];
}

static size_t file_read(void *fp, void *out, size_t n)
{
    fstate *f = fs_of(fp);
    u8 *p = (u8 *)out;
    size_t got = 0;
    if (!f->readable) return 0;
    if (n && f->unget >= 0) {
        p[got++] = (u8)f->unget;
        f->unget = -1;
    }
    while (got < n) {
        u32 have = f->rlen - f->rpos;
        if (have) {
            u32 take = n - got < have ? (u32)(n - got) : have;
            memcpy(p + got, f->rbuf + f->rpos, take);
            f->rpos += take;
            got += take;
            continue;
        }
        if (n - got >= BUF_SIZE) {
            /* A large read goes straight to the descriptor. */
            i32 r;
            if (f->wlen) flush_write(fp, f);
            r = raw_read(f, p + got, (u32)(n - got));
            if (r <= 0) {
                if (r < 0) f->err = true;
                else f->eof = true;
                sync_head(fp, f);
                break;
            }
            got += (size_t)r;
            continue;
        }
        if (fill_read(fp, f) != 0) break;
    }
    return got;
}

/* ------------------------------------------------------------ open, close */

static bool parse_mode(const char *mode, int *flags, bool *r, bool *w)
{
    bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': *flags = plus ? 2 : 0; *r = true; *w = plus; break;
    case 'w': *flags = (plus ? 2 : 1) | 0100 | 01000; *r = plus; *w = true; break;
    case 'a': *flags = (plus ? 2 : 1) | 0100 | 02000; *r = plus; *w = true; break;
    default: return false;
    }
    if (strchr(mode, 'x')) *flags |= 0200;
    if (strchr(mode, 'e')) *flags |= 02000000;
    return true;
}

typedef int (*open_fn)(const char *, int, int);

static void *g_fopen(const char *path, const char *mode)
{
    int flags = 0, fd;
    bool r = false, w = false;
    mg_trace("fopen(%s, %s)", path ? path : "(null)", mode ? mode : "(null)");
    if (!path || !mode || !parse_mode(mode, &flags, &r, &w)) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    fd = ((open_fn)mg_bionic_find("open"))(path, flags, 0666);
    if (fd < 0) return NULL;
    return file_new(fd, r, w);
}

static void *g_fdopen(int fd, const char *mode)
{
    int flags = 0;
    bool r = false, w = false;
    if (!mode || !parse_mode(mode, &flags, &r, &w)) {
        mg_set_errno(GE_INVAL);
        return NULL;
    }
    return file_new(fd, r, w);
}

static int g_fflush(void *fp)
{
    fstate *f;
    if (!fp) return 0;
    f = fs_of(fp);
    return f->wlen ? flush_write(fp, f) : 0;
}

static int g_fclose(void *fp)
{
    fstate *f;
    int r = 0;
    if (!fp) return -1;
    f = fs_of(fp);
    if (f->wlen) r = flush_write(fp, f);
    if (f->has_cookie) {
        if (f->fn_close) MG_GFN(int (*)(void *), f->fn_close)(f->cookie);
    } else if (!f->is_memstream) {
        mg_fd_close(f->fd);
    }
    free(f->rbuf);
    free(f->wbuf);
    if (f < g_std || f >= g_std + 3) free(fp);
    return r;
}

static void *g_freopen(const char *path, const char *mode, void *fp)
{
    fstate *f = fs_of(fp);
    int flags = 0, fd;
    bool r = false, w = false;
    if (!path || !parse_mode(mode, &flags, &r, &w)) return NULL;
    if (f->wlen) flush_write(fp, f);
    fd = ((open_fn)mg_bionic_find("open"))(path, flags, 0666);
    if (fd < 0) return NULL;
    if (f->fd > 2) mg_fd_close(f->fd);
    f->fd = fd;
    f->readable = r;
    f->writable = w;
    f->eof = f->err = false;
    f->unbuffered = false;
    f->rpos = f->rlen = 0;
    f->unget = -1;
    sync_head(fp, f);
    return fp;
}

static void *g_funopen(void *cookie, mg_addr readfn, mg_addr writefn, mg_addr seekfn, mg_addr closefn)
{
    void *fp = file_new(-1, readfn != 0, writefn != 0);
    fstate *f = fs_of(fp);
    f->has_cookie = true;
    f->cookie = cookie;
    f->fn_read = readfn;
    f->fn_write = writefn;
    f->fn_seek = seekfn;
    f->fn_close = closefn;
    return fp;
}

static void *g_open_memstream(char **ptr, size_t *size)
{
    void *fp = file_new(-1, false, true);
    fstate *f = fs_of(fp);
    f->is_memstream = true;
    f->unbuffered = true;
    f->mem_ptr = ptr;
    f->mem_size = size;
    f->mem_cap = 64;
    f->mem = (char *)mg_g_malloc(f->mem_cap);
    f->mem[0] = 0;
    *ptr = f->mem;
    *size = 0;
    return fp;
}

static void *g_tmpfile(void)
{
    static volatile i32 serial;
    char path[64];
    snprintf(path, sizeof(path), "/data/local/tmp/tmp%d.%d", (int)mg_time_monotonic_ns(), (int)mg_atomic_add(&serial, 1));
    return g_fopen(path, "w+");
}

static void *g_popen(const char *cmd, const char *mode)
{
    (void)cmd; (void)mode;
    mg_set_errno(GE_NOSYS);
    return NULL;
}
static int g_pclose(void *fp) { (void)fp; return -1; }

/* ---------------------------------------------------------- reading, writing */

static size_t g_fread(void *buf, size_t size, size_t count, void *fp)
{
    u64 total = (u64)size * count;
    if (!total || total > 0x7FFFFFFFu) return 0;
    return file_read(fp, buf, (size_t)total) / size;
}

static size_t g_fwrite(const void *buf, size_t size, size_t count, void *fp)
{
    u64 total = (u64)size * count;
    if (!total || total > 0x7FFFFFFFu) return 0;
    return file_write(fp, buf, (size_t)total) / size;
}

static int g_fgetc(void *fp) { return file_getc(fp); }
static int g_getchar(void) { return file_getc(g_stdin_ptr); }

static int g_ungetc(int c, void *fp)
{
    fstate *f = fs_of(fp);
    if (c < 0 || f->unget >= 0) return -1;
    f->unget = (u8)c;
    f->eof = false;
    sync_head(fp, f);
    return c;
}

static int g_fputc(int c, void *fp)
{
    u8 b = (u8)c;
    return file_write(fp, &b, 1) == 1 ? b : -1;
}

/*
 * fputwc, fgetwc and ungetwc: a wide character on a stream as UTF-8, bionic's only multibyte encoding. WEOF is
 * 0xFFFFFFFF; a byte sequence that is not UTF-8 is EILSEQ. ungetwc puts back a character just read, or any one byte.
 */
#define G_WEOF 0xFFFFFFFFu
static u32 utf8_encode(u32 c, u8 *out)
{
    if (c < 0x80) { out[0] = (u8)c; return 1; }
    if (c < 0x800) { out[0] = (u8)(0xC0 | c >> 6); out[1] = (u8)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { out[0] = (u8)(0xE0 | c >> 12); out[1] = (u8)(0x80 | (c >> 6 & 0x3F)); out[2] = (u8)(0x80 | (c & 0x3F)); return 3; }
    if (c < 0x110000) {
        out[0] = (u8)(0xF0 | c >> 18); out[1] = (u8)(0x80 | (c >> 12 & 0x3F)); out[2] = (u8)(0x80 | (c >> 6 & 0x3F)); out[3] = (u8)(0x80 | (c & 0x3F));
        return 4;
    }
    return 0;
}

static u32 g_fputwc(u32 c, void *fp)
{
    u8 b[4];
    u32 n = utf8_encode(c, b);
    if (!n) {
        mg_set_errno(GE_ILSEQ);
        return G_WEOF;
    }
    return file_write(fp, b, n) == n ? c : G_WEOF;
}

static u32 g_fgetwc(void *fp)
{
    int c = file_getc(fp), more, i;
    u32 w;
    if (c < 0) return G_WEOF;
    if (c < 0x80) return (u32)c;
    if ((c & 0xE0) == 0xC0) { more = 1; w = (u32)c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { more = 2; w = (u32)c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { more = 3; w = (u32)c & 0x07; }
    else { mg_set_errno(GE_ILSEQ); return G_WEOF; }
    for (i = 0; i < more; i++) {
        c = file_getc(fp);
        if (c < 0 || (c & 0xC0) != 0x80) {
            mg_set_errno(GE_ILSEQ);
            return G_WEOF;
        }
        w = w << 6 | ((u32)c & 0x3F);
    }
    return w;
}

static u32 g_ungetwc(u32 c, void *fp)
{
    fstate *f = fs_of(fp);
    u8 b[4];
    u32 n;
    if (c == G_WEOF) return G_WEOF;
    if (c < 0x80) return g_ungetc((int)c, fp) < 0 ? G_WEOF : c;
    n = utf8_encode(c, b);
    /* Back over the bytes just read, when they are still in the buffer as they were. */
    if (!n || f->unget >= 0 || f->rpos < n || memcmp(f->rbuf + f->rpos - n, b, n)) return G_WEOF;
    f->rpos -= n;
    f->eof = false;
    sync_head(fp, f);
    return c;
}
static int g_putchar(int c) { return g_fputc(c, g_stdout_ptr); }

static char *g_fgets(char *buf, int n, void *fp)
{
    int i = 0, c = 0;
    if (n <= 0) return NULL;
    while (i < n - 1 && (c = file_getc(fp)) >= 0) {
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (!i) return NULL;
    buf[i] = 0;
    return buf;
}
static char *g_fgets_chk(char *buf, int n, void *fp, size_t cap) { (void)cap; return g_fgets(buf, n, fp); }

static int g_fputs(const char *s, void *fp)
{
    size_t n = strlen(s);
    return file_write(fp, s, n) == n ? 0 : -1;
}

static int g_puts(const char *s)
{
    if (g_fputs(s, g_stdout_ptr) != 0) return -1;
    return g_fputc('\n', g_stdout_ptr) < 0 ? -1 : 0;
}

static gssize g_getdelim(char **line, size_t *cap, int delim, void *fp)
{
    size_t n = 0;
    int c;
    if (!line || !cap) {
        mg_set_errno(GE_INVAL);
        return -1;
    }
    if (!*line || !*cap) {
        *cap = 128;
        *line = (char *)mg_g_realloc(*line, *cap);
    }
    while ((c = file_getc(fp)) >= 0) {
        if (n + 2 > *cap) {
            *cap *= 2;
            *line = (char *)mg_g_realloc(*line, *cap);
        }
        (*line)[n++] = (char)c;
        if (c == delim) break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (gssize)n;
}
static gssize g_getline(char **line, size_t *cap, void *fp) { return g_getdelim(line, cap, '\n', fp); }

/* What old headers' getc and putc macros fall into. */
static int g_srget(void *fp) { return file_getc(fp); }
static int g_swbuf(int c, void *fp) { return g_fputc(c, fp); }

/* ----------------------------------------------------------------- position */

static i64 file_tell(void *fp)
{
    fstate *f = fs_of(fp);
    i64 at;
    /* (funopen's seek is fpos_t's: an off_t, a long.) */
    if (f->has_cookie) at = f->fn_seek ? MG_GFN(goff (*)(void *, goff, int), f->fn_seek)(f->cookie, 0, 1) : -1;
    else if (f->is_memstream) return (i64)f->mem_len;
    else at = mg_fd_seek(f->fd, 0, 1);
    if (at < 0) return -1;
    return at - (i64)(f->rlen - f->rpos) - (f->unget >= 0 ? 1 : 0) + (i64)f->wlen;
}

static int file_seek(void *fp, i64 offset, int whence)
{
    fstate *f = fs_of(fp);
    i64 r;
    if (f->wlen) flush_write(fp, f);
    if (whence == 1) {
        /* Relative to where the reader is, which is behind the descriptor by what is buffered. */
        offset -= (i64)(f->rlen - f->rpos) + (f->unget >= 0 ? 1 : 0);
    }
    f->rpos = f->rlen = 0;
    f->unget = -1;
    if (f->has_cookie) r = f->fn_seek ? MG_GFN(goff (*)(void *, goff, int), f->fn_seek)(f->cookie, (goff)offset, whence) : -1;
    else if (f->is_memstream) r = -1;
    else r = mg_fd_seek(f->fd, offset, whence);
    if (r < 0) return -1;
    f->eof = false;
    sync_head(fp, f);
    return 0;
}

static int g_fseek(void *fp, long offset, int whence) { return file_seek(fp, offset, whence); }
static int g_fseeko64(void *fp, i64 offset, int whence) { return file_seek(fp, offset, whence); }
static long g_ftell(void *fp)
{
    i64 at = file_tell(fp);
    if (at > 0x7FFFFFFF) {
        mg_set_errno(GE_OVERFLOW);
        return -1;
    }
    return (long)at;
}
static i64 g_ftello64(void *fp) { return file_tell(fp); }
static void g_rewind(void *fp)
{
    fstate *f = fs_of(fp);
    file_seek(fp, 0, 0);
    f->err = false;
    sync_head(fp, f);
}
/* fpos_t is an off_t in bionic, a long (32 bits in 32-bit Android); fpos64_t 64 bits. */
static int g_fgetpos64(void *fp, i64 *pos)
{
    i64 at = file_tell(fp);
    if (at < 0) return -1;
    *pos = at;
    return 0;
}
static int g_fsetpos64(void *fp, const i64 *pos) { return file_seek(fp, *pos, 0); }
static int g_fgetpos(void *fp, goff *pos)
{
    i64 at = file_tell(fp);
    if (at < 0) return -1;
    if (sizeof(goff) < sizeof(i64) && at > 0x7FFFFFFF) {
        mg_set_errno(GE_OVERFLOW);
        return -1;
    }
    *pos = (goff)at;
    return 0;
}
static int g_fsetpos(void *fp, const goff *pos) { return file_seek(fp, *pos, 0); }

static int g_feof(void *fp) { return fs_of(fp)->eof; }
static int g_ferror(void *fp) { return fs_of(fp)->err; }
static int g_fileno(void *fp) { return fs_of(fp)->fd; }
static void g_clearerr(void *fp)
{
    fstate *f = fs_of(fp);
    f->eof = f->err = false;
    sync_head(fp, f);
}

static int g_setvbuf(void *fp, char *buf, int mode, size_t size)
{
    fstate *f = fs_of(fp);
    (void)buf; (void)size;
    /* _IOFBF 0, _IOLBF 1, _IONBF 2 */
    f->line_buffered = mode == 1;
    if (f->fd > 2 || f->has_cookie) f->unbuffered = mode == 2;
    return 0;
}
static void g_setbuf(void *fp, char *buf) { g_setvbuf(fp, buf, buf ? 0 : 2, BUF_SIZE); }
static void g_setbuffer(void *fp, char *buf, int size) { g_setvbuf(fp, buf, buf ? 0 : 2, (size_t)size); }
static int g_setlinebuf(void *fp) { return g_setvbuf(fp, NULL, 1, 0); }
static int g_fpurge(void *fp)
{
    fstate *f = fs_of(fp);
    f->wlen = 0;
    f->rpos = f->rlen = 0;
    f->unget = -1;
    return 0;
}

static void g_flockfile(void *fp)
{
    fstate *f = fs_of(fp);
    i32 me = mg_gthread_self()->tid;
    if (f->lock == me) {
        f->lock_depth++;
        return;
    }
    while (mg_atomic_cas(&f->lock, 0, me) != 0) mg_thread_yield();
    f->lock_depth = 1;
}
static void g_funlockfile(void *fp)
{
    fstate *f = fs_of(fp);
    if (--f->lock_depth == 0) mg_atomic_xchg(&f->lock, 0);
}
static int g_ftrylockfile(void *fp)
{
    fstate *f = fs_of(fp);
    i32 me = mg_gthread_self()->tid;
    if (f->lock == me) {
        f->lock_depth++;
        return 0;
    }
    if (mg_atomic_cas(&f->lock, 0, me) != 0) return -1;
    f->lock_depth = 1;
    return 0;
}

/* ----------------------------------------------------------------- printf */

static int g_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return vsnprintf(buf, n, fmt, ap); }
static int g_vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7FFFFFFF, fmt, ap); }

static int g_snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static int g_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

static int g_vsnprintf_chk(char *buf, size_t n, int flags, size_t cap, const char *fmt, va_list ap)
{
    (void)flags; (void)cap;
    return vsnprintf(buf, n, fmt, ap);
}
static int g_vsprintf_chk(char *buf, int flags, size_t cap, const char *fmt, va_list ap)
{
    (void)flags;
    return vsnprintf(buf, cap ? cap : 0x7FFFFFFF, fmt, ap);
}
static int g_snprintf_chk(char *buf, size_t n, int flags, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)flags; (void)cap;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static int g_sprintf_chk(char *buf, int flags, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)flags;
    va_start(ap, fmt);
    r = vsnprintf(buf, cap ? cap : 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return r;
}

/* Formatted text of any length, malloc'd by the host's allocator; -1 on a bad format. */
static int format_alloc(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    char small[256];
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n < 0) {
        va_end(ap2);
        return -1;
    }
    *out = (char *)mg_alloc((size_t)n + 1);
    if ((size_t)n < sizeof(small)) memcpy(*out, small, (size_t)n + 1);
    else vsnprintf(*out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return n;
}

static int g_vfprintf(void *fp, const char *fmt, va_list ap)
{
    char *text;
    int n = format_alloc(&text, fmt, ap);
    if (n < 0) return -1;
    if (file_write(fp, text, (size_t)n) != (size_t)n) n = -1;
    free(text);
    return n;
}

static int g_fprintf(void *fp, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}

static int g_vprintf(const char *fmt, va_list ap) { return g_vfprintf(g_stdout_ptr, fmt, ap); }

static int g_printf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vfprintf(g_stdout_ptr, fmt, ap);
    va_end(ap);
    return r;
}

static int g_fprintf_chk(void *fp, int flags, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)flags;
    va_start(ap, fmt);
    r = g_vfprintf(fp, fmt, ap);
    va_end(ap);
    return r;
}
static int g_vfprintf_chk(void *fp, int flags, const char *fmt, va_list ap) { (void)flags; return g_vfprintf(fp, fmt, ap); }
static int g_printf_chk(int flags, const char *fmt, ...)
{
    va_list ap;
    int r;
    (void)flags;
    va_start(ap, fmt);
    r = g_vfprintf(g_stdout_ptr, fmt, ap);
    va_end(ap);
    return r;
}

static int g_vasprintf(char **out, const char *fmt, va_list ap)
{
    char *text;
    int n = format_alloc(&text, fmt, ap);
    if (n < 0) {
        *out = NULL;
        return -1;
    }
    /* The guest will free() it. */
    *out = mg_g_strdup(text);
    free(text);
    return *out ? n : -1;
}

static int g_asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vasprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

static int g_vdprintf(int fd, const char *fmt, va_list ap)
{
    char *text;
    int n = format_alloc(&text, fmt, ap);
    if (n < 0) return -1;
    mg_fd_write(fd, text, (u32)n);
    free(text);
    return n;
}

static int g_dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

static void g_perror(const char *s)
{
    typedef char *(*strerror_fn)(int);
    const char *text = ((strerror_fn)mg_bionic_find("strerror"))(*mg_errno_ptr());
    if (s && *s) mg_log(MG_LOG_WARN, "%s: %s", s, text);
    else mg_log(MG_LOG_WARN, "%s", text);
}

/* ------------------------------------------------------------------ scanf */

static int g_vsscanf(const char *s, const char *fmt, va_list ap) { return vsscanf(s, fmt, ap); }

static int g_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

/* How many arguments a scanf format stores through. */
static int scanf_arg_count(const char *fmt)
{
    int n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') continue;
        fmt++;
        if (*fmt == '%') continue;
        if (*fmt == '*') {
            /* suppressed: skip to its conversion letter */
            while (*fmt && !strchr("diouxXeEfFgGaAcspn[", *fmt)) fmt++;
        } else {
            while (*fmt && !strchr("diouxXeEfFgGaAcspn[", *fmt)) fmt++;
            n++;
        }
        if (*fmt == '[') {
            fmt++;
            if (*fmt == '^') fmt++;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
        }
        if (!*fmt) break;
    }
    return n;
}

/*
 * scanf from a stream: the host can only scan a string, so a stretch of the
 * stream is read ahead, scanned with "%n" added to learn how much was used,
 * and the stream is put back to just after that.
 */
static int g_vfscanf(void *fp, const char *fmt, va_list ap)
{
    void *args[34];
    char *text, *fmt2;
    int nargs = scanf_arg_count(fmt), used = 0, r, i;
    i64 start = file_tell(fp);
    size_t got;
    if (nargs > 32) {
        mg_note_once("fscanf: more than 32 conversions in \"%s\"", fmt);
        return -1;
    }
    for (i = 0; i < nargs; i++) args[i] = va_arg(ap, void *);
    args[nargs] = &used;
    for (i = nargs + 1; i < 34; i++) args[i] = NULL;
    text = (char *)mg_alloc(BUF_SIZE + 1);
    got = file_read(fp, text, BUF_SIZE);
    text[got] = 0;
    fmt2 = mg_sprintf("%s%%n", fmt);
    r = sscanf(text, fmt2, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11],
               args[12], args[13], args[14], args[15], args[16], args[17], args[18], args[19], args[20], args[21], args[22], args[23],
               args[24], args[25], args[26], args[27], args[28], args[29], args[30], args[31], args[32], args[33]);
    free(fmt2);
    /* "%n" is only reached when everything before it matched; otherwise how far it got is not known, and a line is taken. */
    if (!used) {
        const char *nl = strchr(text, '\n');
        used = nl ? (int)(nl - text) + 1 : (int)got;
    }
    if (start >= 0) {
        fstate *f = fs_of(fp);
        file_seek(fp, start + used, 0);
        if (!got) {
            f->eof = true;
            sync_head(fp, f);
        }
    }
    free(text);
    if (!got && r <= 0) return -1;
    return r;
}

static int g_fscanf(void *fp, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = g_vfscanf(fp, fmt, ap);
    va_end(ap);
    return r;
}

static int g_scanf(const char *fmt, ...) { (void)fmt; return -1; }

#define ST(n, f) MG_WRAP(n, f)

const mg_sym mg_tab_stdio[] = {
    ST("fopen", g_fopen), ST("fopen64", g_fopen), ST("fdopen", g_fdopen), ST("freopen", g_freopen), ST("freopen64", g_freopen),
    ST("fclose", g_fclose), ST("fflush", g_fflush), ST("funopen", g_funopen), ST("open_memstream", g_open_memstream),
    ST("tmpfile", g_tmpfile), ST("tmpfile64", g_tmpfile), ST("popen", g_popen), ST("pclose", g_pclose),
    ST("fread", g_fread), ST("fwrite", g_fwrite), ST("fread_unlocked", g_fread), ST("fwrite_unlocked", g_fwrite),
    ST("__fread_chk", g_fread), ST("__fwrite_chk", g_fwrite),
    ST("fgetc", g_fgetc), ST("getc", g_fgetc), ST("getc_unlocked", g_fgetc), ST("getchar", g_getchar), ST("ungetc", g_ungetc),
    ST("fputc", g_fputc), ST("putc", g_fputc), ST("putc_unlocked", g_fputc), ST("putchar", g_putchar),
    ST("fputwc", g_fputwc), ST("putwc", g_fputwc), ST("fgetwc", g_fgetwc), ST("getwc", g_fgetwc), ST("ungetwc", g_ungetwc),
    ST("fgets", g_fgets), ST("__fgets_chk", g_fgets_chk), ST("fputs", g_fputs), ST("puts", g_puts),
    ST("getline", g_getline), ST("getdelim", g_getdelim), ST("__srget", g_srget), ST("__swbuf", g_swbuf),
    ST("fseek", g_fseek), ST("fseeko", g_fseek), ST("fseeko64", g_fseeko64), ST("ftell", g_ftell), ST("ftello", g_ftell),
    ST("ftello64", g_ftello64), ST("rewind", g_rewind), ST("fgetpos", g_fgetpos), ST("fsetpos", g_fsetpos),
    ST("fgetpos64", g_fgetpos64), ST("fsetpos64", g_fsetpos64),
    ST("feof", g_feof), ST("ferror", g_ferror), ST("fileno", g_fileno), ST("clearerr", g_clearerr),
    ST("feof_unlocked", g_feof), ST("ferror_unlocked", g_ferror), ST("fileno_unlocked", g_fileno),
    ST("setvbuf", g_setvbuf), ST("setbuf", g_setbuf), ST("setbuffer", g_setbuffer), ST("setlinebuf", g_setlinebuf),
    ST("fpurge", g_fpurge), ST("__fpurge", g_fpurge),
    ST("flockfile", g_flockfile), ST("funlockfile", g_funlockfile), ST("ftrylockfile", g_ftrylockfile),
    ST("vsnprintf", g_vsnprintf), ST("vsprintf", g_vsprintf), ST("snprintf", g_snprintf), ST("sprintf", g_sprintf),
    ST("__vsnprintf_chk", g_vsnprintf_chk), ST("__vsprintf_chk", g_vsprintf_chk), ST("__snprintf_chk", g_snprintf_chk),
    ST("__sprintf_chk", g_sprintf_chk),
    ST("vfprintf", g_vfprintf), ST("fprintf", g_fprintf), ST("vprintf", g_vprintf), ST("printf", g_printf),
    ST("__fprintf_chk", g_fprintf_chk), ST("__vfprintf_chk", g_vfprintf_chk), ST("__printf_chk", g_printf_chk),
    ST("vasprintf", g_vasprintf), ST("asprintf", g_asprintf), ST("vdprintf", g_vdprintf), ST("dprintf", g_dprintf),
    ST("perror", g_perror),
    ST("vsscanf", g_vsscanf), ST("sscanf", g_sscanf), ST("vfscanf", g_vfscanf), ST("fscanf", g_fscanf), ST("scanf", g_scanf),
    MG_DATA("__sF", g_sF), MG_DATA("stdin", &g_stdin_ptr), MG_DATA("stdout", &g_stdout_ptr), MG_DATA("stderr", &g_stderr_ptr),
    MG_END
};

/* For the rest of the layer: the guest's stdout as a FILE. */
void *mg_stdio_stdout(void) { std_init(); return g_stdout_ptr; }
