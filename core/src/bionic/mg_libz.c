/*
 * libz.so: the vendored zlib, as it is. Its interface is integers, pointers
 * and a z_stream whose layout is the same in 32-bit Android and here, and the
 * allocation callbacks it may be given are called with plain integer
 * arguments, so nothing needs wrapping.
 *
 * Its gzip files (gzopen and the rest) are this layer's own, on the guest's
 * files: a path is the guest's, and so is a descriptor (gzdopen), and what is
 * read and written goes through the layer's descriptors. The vendored gz*.c
 * are not built: they open the host's files.
 */
#include "mg_bionic.h"
#include "mg_fd.h"

#include "zlib.h"

/*
 * A gzip file. It starts as zlib's gzFile_s, which zlib.h's gzgetc macro reads (have, next, pos): `have` stays 0 here,
 * so the macro calls gzgetc. (pos is a z_off64_t there, which is 4 bytes in 32-bit Android unless the app was built with
 * _LARGEFILE64_SOURCE: the low half of this one either way.) Reading takes a gzip stream (one member after another) or a
 * file as it is (zlib reads such a file transparently); writing makes a gzip stream at the level and strategy the mode
 * says. Zalo's libraries write their logs with gzopen, gzwrite and gzclose (libz3rdparty, libznetwork, libzmedia), and
 * libzjxl reads with gzgets; an import not provided is a stub that ends the app when called.
 */
typedef struct g_gz {
    u32 have;                  /* zlib's gzFile_s, as the gzgetc macro reads it */
    u8 *next;
    i64 pos;                   /* the uncompressed bytes read or written so far */
    int fd;
    bool writing, eof, transparent, started, inflating;
    int err;                   /* Z_OK, or what went wrong */
    i64 start;                 /* where reading began (gzrewind goes back there) */
    z_stream z;
    u8 in[16384];              /* compressed bytes read from the file, or made to be written to it */
    u8 out[16384];             /* bytes decompressed and not given out yet */
    u32 out_at, out_n;
} g_gz;

static bool write_all(int fd, const u8 *p, u32 n)
{
    while (n) {
        i32 w = mg_fd_write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (u32)w;
    }
    return true;
}

/* More compressed input: the bytes read, 0 at the file's end, -1 on an error. */
static int gz_read_in(g_gz *g)
{
    i32 n = mg_fd_read(g->fd, g->in, sizeof(g->in));
    if (n < 0) {
        g->err = Z_ERRNO;
        return -1;
    }
    g->z.next_in = g->in;
    g->z.avail_in = (uInt)n;
    return n;
}

/* More decompressed bytes into `out`: false at the end, or on an error. */
static bool gz_fill(g_gz *g)
{
    g->out_at = g->out_n = 0;
    while (!g->out_n && !g->eof && g->err == Z_OK) {
        if (!g->started) {
            g->started = true;
            if (gz_read_in(g) < 0) return false;
            if (g->z.avail_in >= 2 && g->in[0] == 0x1f && g->in[1] == 0x8b) {
                if (inflateInit2(&g->z, 15 + 16) != Z_OK) {
                    g->err = Z_MEM_ERROR;
                    return false;
                }
                g->inflating = true;
            } else {
                g->transparent = true;
            }
        }
        if (g->transparent) {
            u32 n;
            if (!g->z.avail_in && gz_read_in(g) <= 0) {
                g->eof = g->err == Z_OK;
                break;
            }
            n = g->z.avail_in < sizeof(g->out) ? g->z.avail_in : (u32)sizeof(g->out);
            memcpy(g->out, g->z.next_in, n);
            g->z.next_in += n;
            g->z.avail_in -= n;
            g->out_n = n;
            break;
        }
        if (!g->z.avail_in) {
            int n = gz_read_in(g);
            if (n < 0) break;
            if (n == 0) {
                g->err = Z_DATA_ERROR;   /* "unexpected end of file": a gzip stream that stops short */
                break;
            }
        }
        g->z.next_out = g->out;
        g->z.avail_out = sizeof(g->out);
        {
            int r = inflate(&g->z, Z_NO_FLUSH);
            g->out_n = (u32)(sizeof(g->out) - g->z.avail_out);
            if (r == Z_STREAM_END) {
                /* Another member may follow, as a gzip file may have several; anything else after it is not read. */
                if (!g->z.avail_in && gz_read_in(g) <= 0) g->eof = g->err == Z_OK;
                else if (g->z.avail_in >= 2 && g->z.next_in[0] == 0x1f && g->z.next_in[1] == 0x8b) inflateReset(&g->z);
                else g->eof = true;
            } else if (r != Z_OK && r != Z_BUF_ERROR) {
                g->err = r == Z_MEM_ERROR ? Z_MEM_ERROR : Z_DATA_ERROR;
            }
        }
    }
    return g->out_n > 0;
}

/* What the writer has, deflated with `flush` and written to the file: false on an error. */
static bool gz_deflate(g_gz *g, int flush)
{
    int r;
    for (;;) {
        u32 n;
        g->z.next_out = g->in;
        g->z.avail_out = sizeof(g->in);
        r = deflate(&g->z, flush);
        if (r == Z_STREAM_ERROR) {
            g->err = Z_STREAM_ERROR;
            return false;
        }
        n = (u32)(sizeof(g->in) - g->z.avail_out);
        if (n && !write_all(g->fd, g->in, n)) {
            g->err = Z_ERRNO;
            return false;
        }
        if (flush == Z_FINISH ? r == Z_STREAM_END : g->z.avail_out != 0) return true;
    }
}

/* A gzip file on a descriptor, read or written as `mode` says ("rb", "wb9", "ab", ...); NULL for a mode it is not. */
static g_gz *gz_on(int fd, const char *mode)
{
    g_gz *g;
    int level = Z_DEFAULT_COMPRESSION, strategy = Z_DEFAULT_STRATEGY;
    bool reading = false, writing = false;
    const char *m;
    if (fd < 0 || !mode) return NULL;
    for (m = mode; *m; m++) {
        if (*m == 'r') reading = true;
        else if (*m == 'w' || *m == 'a') writing = true;
        else if (*m >= '0' && *m <= '9') level = *m - '0';
        else if (*m == 'f') strategy = Z_FILTERED;
        else if (*m == 'h') strategy = Z_HUFFMAN_ONLY;
        else if (*m == 'R') strategy = Z_RLE;
        else if (*m == 'F') strategy = Z_FIXED;
    }
    if (reading == writing) return NULL;
    g = (g_gz *)mg_alloc(sizeof(*g));
    g->fd = fd;
    g->writing = writing;
    if (writing) {
        if (deflateInit2(&g->z, level, Z_DEFLATED, 15 + 16, 8, strategy) != Z_OK) {
            free(g);
            return NULL;
        }
    } else {
        g->start = mg_fd_seek(fd, 0, 1 /* SEEK_CUR */);
        if (g->start < 0) g->start = 0;
    }
    return g;
}

static g_gz *g_gzdopen(int fd, const char *mode) { return gz_on(fd, mode); }

static g_gz *g_gzopen(const char *path, const char *mode)
{
    typedef int (*open_fn)(const char *, int, unsigned);
    int flags = 0, fd;
    g_gz *g;
    const char *m;
    bool w = false, a = false, x = false;
    if (!path || !mode) return NULL;
    for (m = mode; *m; m++) {
        if (*m == 'w') w = true;
        else if (*m == 'a') a = true;
        else if (*m == 'x') x = true;
    }
    if (w || a) flags = 01 /* O_WRONLY */ | 0100 /* O_CREAT */ | (a ? 02000 /* O_APPEND */ : 01000 /* O_TRUNC */) | (x ? 0200 /* O_EXCL */ : 0);
    fd = ((open_fn)mg_bionic_find("open"))(path, flags | 02000000 /* O_CLOEXEC */, 0666);
    if (fd < 0) return NULL;
    g = gz_on(fd, mode);
    if (!g) mg_fd_close(fd);
    return g;
}

static int g_gzread(g_gz *g, void *buf, unsigned len)
{
    unsigned got = 0;
    if (!g || g->writing || (g->err != Z_OK && g->err != Z_BUF_ERROR)) return -1;
    while (got < len) {
        u32 n;
        if (g->out_at == g->out_n && !gz_fill(g)) break;
        n = g->out_n - g->out_at;
        if (n > len - got) n = len - got;
        memcpy((u8 *)buf + got, g->out + g->out_at, n);
        g->out_at += n;
        got += n;
    }
    g->pos += got;
    return got || g->err == Z_OK ? (int)got : -1;
}

static int g_gzgetc(g_gz *g)
{
    u8 c;
    return g_gzread(g, &c, 1) == 1 ? c : -1;
}

/* A line, its '\n' kept, NUL-terminated in at most len bytes; NULL when nothing is left or on an error. */
static char *g_gzgets(g_gz *g, char *buf, int len)
{
    int got = 0;
    if (!g || g->writing || !buf || len < 1) return NULL;
    while (got < len - 1) {
        u8 *nl;
        u32 n;
        if (g->out_at == g->out_n && !gz_fill(g)) break;
        n = g->out_n - g->out_at;
        if (n > (u32)(len - 1 - got)) n = (u32)(len - 1 - got);
        nl = (u8 *)memchr(g->out + g->out_at, '\n', n);
        if (nl) n = (u32)(nl - (g->out + g->out_at)) + 1;
        memcpy(buf + got, g->out + g->out_at, n);
        g->out_at += n;
        got += (int)n;
        if (nl) break;
    }
    buf[got] = 0;
    g->pos += got;
    return got ? buf : NULL;
}

static int g_gzwrite(g_gz *g, const void *buf, unsigned len)
{
    if (!g || !g->writing || g->err != Z_OK) return 0;
    if (!len) return 0;
    g->z.next_in = (Bytef *)buf;
    g->z.avail_in = len;
    if (!gz_deflate(g, Z_NO_FLUSH)) return 0;
    g->pos += len;
    return (int)len;
}

static int g_gzputs(g_gz *g, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!g || !s) return -1;
    return n == 0 ? 0 : g_gzwrite(g, s, (unsigned)n) == (int)n ? (int)n : -1;
}

static int g_gzputc(g_gz *g, int c)
{
    u8 b = (u8)c;
    return g_gzwrite(g, &b, 1) == 1 ? b : -1;
}

static int g_gzflush(g_gz *g, int flush)
{
    if (!g || !g->writing) return Z_STREAM_ERROR;
    if (g->err != Z_OK) return g->err;
    g->z.avail_in = 0;
    return gz_deflate(g, flush) ? Z_OK : g->err;
}

static int g_gzeof(g_gz *g) { return g && !g->writing && g->eof && g->out_at == g->out_n; }

static int g_gzrewind(g_gz *g)
{
    if (!g || g->writing) return -1;
    if (mg_fd_seek(g->fd, g->start, 0 /* SEEK_SET */) < 0) return -1;
    if (g->inflating) inflateEnd(&g->z);
    memset(&g->z, 0, sizeof(g->z));
    g->inflating = g->started = g->transparent = g->eof = false;
    g->err = Z_OK;
    g->out_at = g->out_n = 0;
    g->pos = 0;
    return 0;
}

static int g_gzclose(g_gz *g)
{
    int r = Z_OK;
    if (!g) return Z_STREAM_ERROR;
    if (g->writing) {
        g->z.avail_in = 0;
        if (g->err == Z_OK && !gz_deflate(g, Z_FINISH)) r = g->err;
        deflateEnd(&g->z);
    } else if (g->inflating) {
        inflateEnd(&g->z);
    }
    if (mg_fd_close(g->fd) != 0 && r == Z_OK) r = Z_ERRNO;
    free(g);
    return r;
}

static const char *g_gzerror(g_gz *g, int *errnum)
{
    int e = g ? g->err : Z_STREAM_ERROR;
    if (errnum) *errnum = e;
    switch (e) {
    case Z_OK: return "";
    case Z_ERRNO: return "error in the file";
    case Z_DATA_ERROR: return g && g->eof ? "data error" : "unexpected end of file";
    case Z_MEM_ERROR: return "out of memory";
    default: return "stream error";
    }
}

static void g_gzclearerr(g_gz *g)
{
    if (!g) return;
    g->err = Z_OK;
    g->eof = false;
}

static int g_gzbuffer(g_gz *g, unsigned size) { (void)size; return g ? 0 : -1; }
static int g_gzdirect(g_gz *g) { return g && !g->writing && g->transparent; }
static long g_gztell(g_gz *g) { return g ? (long)g->pos : -1; }
static i64 g_gztell64(g_gz *g) { return g ? g->pos : -1; }

/*
 * zlib's *_combine take a z_off_t, which Android's zlib.h has as a long (32 bits in 32-bit Android); the vendored zlib's is
 * 64 bits wide whatever the processor, which on 32-bit ARM read a register the caller had not set.
 */
static uLong g_adler32_combine(uLong a, uLong b, glong len) { return adler32_combine(a, b, (z_off_t)len); }
static uLong g_crc32_combine(uLong a, uLong b, glong len) { return crc32_combine(a, b, (z_off_t)len); }

static int g_gzsetparams(g_gz *g, int level, int strategy)
{
    if (!g || !g->writing) return Z_STREAM_ERROR;
    return deflateParams(&g->z, level, strategy);
}

const mg_sym mg_tab_zlib[] = {
    MG_DIRECT(zlibVersion), MG_DIRECT(deflate), MG_DIRECT(deflateEnd), MG_DIRECT(inflate), MG_DIRECT(inflateEnd),
    MG_DIRECT(deflateSetDictionary), MG_DIRECT(deflateCopy), MG_DIRECT(deflateReset), MG_DIRECT(deflateParams),
    MG_DIRECT(deflateTune), MG_DIRECT(deflateBound), MG_DIRECT(deflatePrime), MG_DIRECT(deflateSetHeader),
    MG_DIRECT(inflateSetDictionary), MG_DIRECT(inflateSync), MG_DIRECT(inflateCopy), MG_DIRECT(inflateReset),
    MG_DIRECT(inflateReset2), MG_DIRECT(inflatePrime), MG_DIRECT(inflateMark), MG_DIRECT(inflateGetHeader),
    MG_DIRECT(inflateBack), MG_DIRECT(inflateBackEnd), MG_DIRECT(zlibCompileFlags), MG_DIRECT(compress), MG_DIRECT(compress2),
    MG_DIRECT(compressBound), MG_DIRECT(uncompress), MG_DIRECT(adler32), MG_DIRECT(crc32),
    MG_WRAP("adler32_combine", g_adler32_combine), MG_WRAP("crc32_combine", g_crc32_combine), MG_DIRECT(deflateInit_),
    MG_DIRECT(inflateInit_), MG_DIRECT(deflateInit2_), MG_DIRECT(inflateInit2_), MG_DIRECT(inflateBackInit_), MG_DIRECT(zError), MG_DIRECT(inflateSyncPoint), MG_DIRECT(get_crc_table),
    MG_WRAP("gzopen", g_gzopen), MG_WRAP("gzopen64", g_gzopen), MG_WRAP("gzdopen", g_gzdopen), MG_WRAP("gzread", g_gzread),
    MG_WRAP("gzwrite", g_gzwrite), MG_WRAP("gzputs", g_gzputs), MG_WRAP("gzputc", g_gzputc), MG_WRAP("gzgets", g_gzgets),
    MG_WRAP("gzgetc", g_gzgetc), MG_WRAP("gzgetc_", g_gzgetc), MG_WRAP("gzflush", g_gzflush), MG_WRAP("gzeof", g_gzeof),
    MG_WRAP("gzrewind", g_gzrewind), MG_WRAP("gzclose", g_gzclose), MG_WRAP("gzclose_r", g_gzclose), MG_WRAP("gzclose_w", g_gzclose),
    MG_WRAP("gzerror", g_gzerror), MG_WRAP("gzclearerr", g_gzclearerr), MG_WRAP("gzbuffer", g_gzbuffer), MG_WRAP("gzdirect", g_gzdirect),
    MG_WRAP("gztell", g_gztell), MG_WRAP("gztell64", g_gztell64), MG_WRAP("gzsetparams", g_gzsetparams),
    MG_END
};
