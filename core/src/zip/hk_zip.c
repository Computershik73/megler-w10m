#include "hk_zip.h"

#include <stdio.h>

#include "zlib.h"

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) hk_strlcpy(err, msg, errlen);
}

static u64 rd64(const u8 *p) { return (u64)hk_rd32(p) | ((u64)hk_rd32(p + 4) << 32); }

static bool in_file(const hk_zip *z, u64 off, u64 len)
{
    return off <= z->size && len <= z->size - off;
}

bool hk_zip_pread(const hk_zip *z, u64 offset, void *buf, size_t n)
{
    hk_zip *w = (hk_zip *)z;           /* the lock and the file's position are not the archive's contents */
    i64 got;
    int err = 0;
    if (!z->file || !in_file(z, offset, n)) return false;
    hk_lock_acquire(&w->lock);
    got = hk_file_pread(z->file, buf, n, offset, &err);
    hk_lock_release(&w->lock);
    return got == (i64)n;
}

/* The end-of-central-directory record: the last 22 bytes, or earlier when the archive has a comment. `tail` is the end of the file. */
static bool find_end(const u8 *tail, size_t n, size_t *at)
{
    size_t i;
    if (n < 22) return false;
    for (i = n - 22;; i--) {
        if (hk_rd32(tail + i) == 0x06054b50u) {
            *at = i;
            return true;
        }
        if (i == 0) break;
    }
    return false;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const hk_zip_entry *)a)->name, ((const hk_zip_entry *)b)->name);
}

bool hk_zip_open(hk_zip *z, const char *path, char *err, size_t errlen)
{
    u8 *tail = NULL, *cd = NULL;
    const u8 *d;
    hk_stat st;
    size_t tail_size, end_at = 0;
    u64 tail_off, eocd, cd_off, cd_size, total, off;
    u32 i;
    int ferr = 0;
    memset(z, 0, sizeof(*z));
    hk_lock_init(&z->lock);
    z->file = hk_file_open(path, HK_O_READ, &ferr);
    if (!z->file || !hk_file_stat(z->file, &st)) {
        fail(err, errlen, "the file cannot be opened");
        goto bad;
    }
    z->size = st.size;
    tail_size = (size_t)(z->size > 22 + 65535 + 20 ? 22 + 65535 + 20 : z->size);
    tail_off = z->size - tail_size;
    tail = (u8 *)hk_alloc(tail_size ? tail_size : 1);
    if (!hk_zip_pread(z, tail_off, tail, tail_size) || !find_end(tail, tail_size, &end_at)) {
        fail(err, errlen, "not a ZIP archive (no end record)");
        goto bad;
    }
    eocd = tail_off + end_at;
    total = hk_rd16(tail + end_at + 10);
    cd_size = hk_rd32(tail + end_at + 12);
    cd_off = hk_rd32(tail + end_at + 16);
    if (total == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) {
        /* ZIP64: a locator 20 bytes before the end record points at the real one. */
        u8 rec64[56];
        u64 rec;
        if (end_at < 20 || hk_rd32(tail + end_at - 20) != 0x07064b50u) {
            fail(err, errlen, "ZIP64 locator missing");
            goto bad;
        }
        rec = rd64(tail + end_at - 20 + 8);
        if (!hk_zip_pread(z, rec, rec64, sizeof(rec64)) || hk_rd32(rec64) != 0x06064b50u) {
            fail(err, errlen, "ZIP64 end record damaged");
            goto bad;
        }
        total = rd64(rec64 + 32);
        cd_size = rd64(rec64 + 40);
        cd_off = rd64(rec64 + 48);
    }
    (void)eocd;
    if (!in_file(z, cd_off, cd_size) || total > 1000000 || cd_size > (256u << 20)) {
        fail(err, errlen, "central directory outside the file");
        goto bad;
    }
    cd = (u8 *)hk_alloc((size_t)cd_size ? (size_t)cd_size : 1);
    if (!hk_zip_pread(z, cd_off, cd, (size_t)cd_size)) {
        fail(err, errlen, "central directory cannot be read");
        goto bad;
    }
    /* From here on offsets are into the directory as read. */
    d = cd;
    z->entries = (hk_zip_entry *)hk_alloc(sizeof(hk_zip_entry) * (size_t)(total ? total : 1));
    off = 0;
    for (i = 0; i < (u32)total; i++) {
        const u8 *h;
        u16 nlen, elen, clen;
        hk_zip_entry *e = &z->entries[z->count];
        if (off > cd_size || cd_size - off < 46 || hk_rd32(d + off) != 0x02014b50u) {
            fail(err, errlen, "central directory entry damaged");
            goto bad;
        }
        h = d + off;
        nlen = hk_rd16(h + 28);
        elen = hk_rd16(h + 30);
        clen = hk_rd16(h + 32);
        if (cd_size - off - 46 < (u64)nlen + elen + clen) {
            fail(err, errlen, "central directory entry runs past the file");
            goto bad;
        }
        e->flags = hk_rd16(h + 8);
        e->method = hk_rd16(h + 10);
        e->crc = hk_rd32(h + 16);
        e->csize = hk_rd32(h + 20);
        e->usize = hk_rd32(h + 24);
        e->local_offset = hk_rd32(h + 42);
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->local_offset == 0xFFFFFFFFu) {
            /* The ZIP64 extra field carries, in this order, only the values that were 0xFFFFFFFF. */
            const u8 *x = h + 46 + nlen, *xe = x + elen;
            while (x + 4 <= xe) {
                u16 id = hk_rd16(x), len = hk_rd16(x + 2);
                const u8 *v = x + 4;
                if (v + len > xe) break;
                if (id == 1) {
                    if (e->usize == 0xFFFFFFFFu && len >= 8) { e->usize = rd64(v); v += 8; len -= 8; }
                    if (e->csize == 0xFFFFFFFFu && len >= 8) { e->csize = rd64(v); v += 8; len -= 8; }
                    if (e->local_offset == 0xFFFFFFFFu && len >= 8) { e->local_offset = rd64(v); }
                    break;
                }
                x = v + len;
            }
        }
        e->name = (char *)hk_alloc((size_t)nlen + 1);
        memcpy(e->name, h + 46, nlen);
        z->count++;
        off += 46 + (u64)nlen + elen + clen;
    }
    qsort(z->entries, z->count, sizeof(hk_zip_entry), by_name);
    z->path = hk_strdup(path);
    free(tail);
    free(cd);
    return true;
bad:
    free(tail);
    free(cd);
    hk_zip_close(z);
    return false;
}

void hk_zip_close(hk_zip *z)
{
    u32 i;
    for (i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    free(z->path);
    if (z->file) hk_file_close(z->file);
    memset(z, 0, sizeof(*z));
}

const hk_zip_entry *hk_zip_find(const hk_zip *z, const char *name)
{
    u32 lo = 0, hi = z->count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = strcmp(z->entries[mid].name, name);
        if (!c) return &z->entries[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Where the entry's data starts: past the local header, whose name and extra lengths are its own. */
static bool entry_data(const hk_zip *z, const hk_zip_entry *e, u64 *start)
{
    u8 h[30];
    if (!hk_zip_pread(z, e->local_offset, h, sizeof(h)) || hk_rd32(h) != 0x04034b50u) return false;
    *start = e->local_offset + 30 + hk_rd16(h + 26) + hk_rd16(h + 28);
    return in_file(z, *start, e->csize);
}

bool hk_zip_stored_at(const hk_zip *z, const hk_zip_entry *e, u64 *offset)
{
    if (e->method != 0 || e->csize != e->usize) return false;
    return entry_data(z, e, offset);
}

bool hk_zip_read_into(const hk_zip *z, const hk_zip_entry *e, u8 *out, size_t size)
{
    u64 src = 0;
    if (!entry_data(z, e, &src) || size != e->usize) return false;
    if (e->method == 0) {
        if (e->csize != e->usize) return false;
        return hk_zip_pread(z, src, out, size);
    }
    if (e->method == 8) {
        /* Inflated a piece of the file at a time: the compressed bytes are never all in memory. */
        enum { PIECE = 256 * 1024 };
        z_stream s;
        u8 *in = (u8 *)malloc(PIECE);
        u64 left = e->csize;
        int r = Z_OK;
        if (!in) return false;
        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) {
            free(in);
            return false;
        }
        s.next_out = out;
        s.avail_out = (uInt)size;
        while (r == Z_OK) {
            size_t n = (size_t)(left > PIECE ? PIECE : left);
            if (!n || !hk_zip_pread(z, src, in, n)) break;
            src += n;
            left -= n;
            s.next_in = in;
            s.avail_in = (uInt)n;
            r = inflate(&s, left ? Z_NO_FLUSH : Z_FINISH);
            if (r == Z_BUF_ERROR && left && !s.avail_in) r = Z_OK;     /* wants more input, and there is more */
        }
        inflateEnd(&s);
        free(in);
        return r == Z_STREAM_END && s.total_out == size;
    }
    return false;
}

u8 *hk_zip_read(const hk_zip *z, const hk_zip_entry *e, size_t limit, size_t *size)
{
    u8 *out;
    if (e->usize > limit || e->usize > 0x7FFFFFF0u || e->csize > 0x7FFFFFF0u) return NULL;
    out = (u8 *)malloc((size_t)e->usize + 1);
    if (!out) return NULL;
    if (!hk_zip_read_into(z, e, out, (size_t)e->usize)) {
        free(out);
        return NULL;
    }
    out[e->usize] = 0;
    if (size) *size = (size_t)e->usize;
    return out;
}

bool hk_zip_extract(const hk_zip *z, const hk_zip_entry *e, const char *path, void (*progress)(void *user, u64 written), void *user,
                    char *err, size_t errlen)
{
    enum { IN_PIECE = 256 * 1024, OUT_PIECE = 1024 * 1024 };
    hk_file *out;
    u8 *in = NULL, *buf = NULL;
    u64 src = 0, left, written = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    z_stream s;
    bool inflating = false, ok = false;
    int ferr = 0, r = Z_OK;
    const char *why = "the entry is damaged";
    if (!entry_data(z, e, &src) || (e->method != 0 && e->method != 8) || (e->method == 0 && e->csize != e->usize)) {
        fail(err, errlen, e->method != 0 && e->method != 8 ? "the entry is compressed in a way this reader does not know" : why);
        return false;
    }
    memset(&s, 0, sizeof(s));
    out = hk_file_open(path, HK_O_WRITE | HK_O_CREATE | HK_O_TRUNC, &ferr);
    if (!out) {
        fail(err, errlen, "the file cannot be made");
        return false;
    }
    in = (u8 *)malloc(IN_PIECE);
    buf = (u8 *)malloc(OUT_PIECE);
    if (!in || !buf) {
        why = "no memory to unpack with";
        goto done;
    }
    left = e->csize;
    if (e->method == 8) {
        if (inflateInit2(&s, -MAX_WBITS) != Z_OK) goto done;
        inflating = true;
    }
    for (;;) {
        size_t n = (size_t)(left > IN_PIECE ? IN_PIECE : left), produced;
        if (e->method == 0) {
            /* Stored: the bytes as they are, a megabyte at a time. */
            n = (size_t)(left > OUT_PIECE ? OUT_PIECE : left);
            if (!n) break;
            if (!hk_zip_pread(z, src, buf, n)) goto done;
            src += n;
            left -= n;
            produced = n;
        } else {
            if (!s.avail_in) {
                if (!n) goto done;     /* the stream wants more than the entry has */
                if (!hk_zip_pread(z, src, in, n)) goto done;
                src += n;
                left -= n;
                s.next_in = in;
                s.avail_in = (uInt)n;
            }
            s.next_out = buf;
            s.avail_out = OUT_PIECE;
            r = inflate(&s, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END && !(r == Z_BUF_ERROR && left)) goto done;
            produced = OUT_PIECE - s.avail_out;
        }
        if (produced) {
            if (written + produced > e->usize) goto done;
            if (hk_file_write(out, buf, produced, &ferr) != (i64)produced) {
                why = "the file cannot be written (is the storage full?)";
                goto done;
            }
            crc = crc32(crc, buf, (uInt)produced);
            written += produced;
            if (progress) progress(user, written);
        }
        if (e->method == 8 && r == Z_STREAM_END) break;
    }
    ok = written == e->usize && (u32)crc == e->crc;
    if (!ok) why = "the entry is damaged (its size or its CRC is not what the archive says)";
done:
    if (inflating) inflateEnd(&s);
    free(in);
    free(buf);
    hk_file_close(out);
    if (!ok) {
        hk_path_remove(path, NULL);
        fail(err, errlen, why);
    }
    return ok;
}

void hk_zip_each(const hk_zip *z, const char *prefix, void (*cb)(const hk_zip_entry *e, void *user), void *user)
{
    size_t n = strlen(prefix);
    u32 lo = 0, hi = z->count, i;
    /* Sorted by name, so the matches are one run starting at the first name not below the prefix. */
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (strncmp(z->entries[mid].name, prefix, n) < 0) lo = mid + 1;
        else hi = mid;
    }
    for (i = lo; i < z->count && !strncmp(z->entries[i].name, prefix, n); i++) cb(&z->entries[i], user);
}
