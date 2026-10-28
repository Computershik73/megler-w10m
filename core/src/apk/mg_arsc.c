#include "mg_arsc.h"

#include <stdlib.h>
#include <string.h>

/* The chunks and values of ResourceTypes.h this reads. */
#define CHUNK_POOL 0x0001
#define CHUNK_TABLE 0x0002
#define CHUNK_PACKAGE 0x0200
#define CHUNK_TYPE 0x0201
#define TYPE_STRING 0x03
#define POOL_UTF8 0x100
#define SPAN_END 0xFFFFFFFFu
#define TYPE_SPARSE 0x01
#define TYPE_OFFSET16 0x02
#define ENTRY_COMPLEX 0x0001
#define ENTRY_COMPACT 0x0008

static u32 r16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 r32(const u8 *p) { return r16(p) | r16(p + 2) << 16; }
static void w32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ------------------------------------------------------------ the pool */

typedef struct pool {
    const u8 *at;                      /* the chunk */
    u32 header, size, count, styles, flags;
    u32 strings, style_data;           /* where the strings' and the styles' data start in the chunk */
} pool;

static bool pool_read(pool *p, const u8 *at, size_t avail)
{
    if (avail < 28 || r16(at) != CHUNK_POOL) return false;
    p->at = at;
    p->header = r16(at + 2);
    p->size = r32(at + 4);
    p->count = r32(at + 8);
    p->styles = r32(at + 12);
    p->flags = r32(at + 16);
    p->strings = r32(at + 20);
    p->style_data = r32(at + 24);
    if (p->header < 28 || p->size > avail || p->size < p->header || p->styles > p->count) return false;
    if ((u64)p->header + 4ull * ((u64)p->count + p->styles) > p->size) return false;
    if (p->count && (p->strings < p->header || p->strings > p->size)) return false;
    if (p->styles && (p->style_data < p->strings || p->style_data > p->size)) return false;
    return true;
}

/* String i's entry in the pool: its length prefix, its bytes and its terminator. */
static bool pool_string(const pool *p, u32 i, const u8 **s, size_t *len)
{
    u32 end = p->styles ? p->style_data : p->size, at;
    const u8 *q;
    size_t room, n;
    if (i >= p->count) return false;
    at = p->strings + r32(p->at + p->header + 4 * i);
    if (at < p->strings || at >= end) return false;
    q = p->at + at;
    room = end - at;
    if (p->flags & POOL_UTF8) {
        /* Its length in UTF-16 units, then in bytes, each in one byte or two. */
        size_t h = (q[0] & 0x80) ? 2 : 1;
        u32 bytes;
        if (room < h + 1) return false;
        bytes = q[h];
        if (bytes & 0x80) {
            if (room < h + 2) return false;
            bytes = ((bytes & 0x7F) << 8) | q[h + 1];
            h += 2;
        } else {
            h += 1;
        }
        n = h + bytes + 1;
    } else {
        u32 units;
        size_t h = 2;
        if (room < 2) return false;
        units = r16(q);
        if (units & 0x8000) {
            if (room < 4) return false;
            units = ((units & 0x7FFF) << 16) | r16(q + 2);
            h = 4;
        }
        n = h + 2 * (size_t)units + 2;
    }
    if (n > room) return false;
    *s = q;
    *len = n;
    return true;
}

/* Style i: its spans (a name, the first and the last character), and the end. */
static bool pool_style(const pool *p, u32 i, const u8 **s, size_t *len)
{
    u32 at, k;
    if (i >= p->styles) return false;
    at = p->style_data + r32(p->at + p->header + 4 * p->count + 4 * i);
    for (k = at;; k += 12) {
        if (k < at || (u64)k + 4 > p->size) return false;
        if (r32(p->at + k) == SPAN_END) break;
        if ((u64)k + 12 > p->size) return false;
    }
    *s = p->at + at;
    *len = k + 4 - at;
    return true;
}

/* ------------------------------------------------------------ languages */

/* A type chunk's language, as two or three letters; "" for none. */
static void chunk_language(const u8 *c, u32 header, char out[4])
{
    const u8 *config = c + 20;
    u8 a, b;
    out[0] = 0;
    if (header < 20 + 12 || r32(config) < 12) return;
    a = config[8];
    b = config[9];
    if (!a) return;
    if (a & 0x80) {
        /* Three letters packed into the two bytes, five bits each (ResTable_config::unpackLanguageOrRegion). */
        out[0] = (char)('a' + (b & 0x1F));
        out[1] = (char)('a' + (((b & 0xE0) >> 5) | ((a & 0x03) << 3)));
        out[2] = (char)('a' + ((a & 0x7C) >> 2));
        out[3] = 0;
    } else {
        out[0] = (char)a;
        out[1] = (char)b;
        out[2] = 0;
    }
}

static bool same_language(const char *a, const char *b)
{
    static const char *const pairs[][2] = { { "he", "iw" }, { "id", "in" }, { "yi", "ji" }, { "tl", "fil" } };
    size_t i;
    if (!strcmp(a, b)) return true;
    for (i = 0; i < MG_COUNT(pairs); i++)
        if ((!strcmp(a, pairs[i][0]) && !strcmp(b, pairs[i][1])) || (!strcmp(a, pairs[i][1]) && !strcmp(b, pairs[i][0]))) return true;
    return false;
}

static bool kept(const u8 *c, u32 header, const char *const *languages, int n)
{
    char lang[4];
    int i;
    if (n < 0) return true;
    chunk_language(c, header, lang);
    if (!lang[0]) return true;
    for (i = 0; i < n; i++)
        if (same_language(lang, languages[i])) return true;
    return false;
}

/* ------------------------------------------------------------ the values */

/* Each TYPE_STRING value of a type chunk: `fn` with where its data is in the chunk, and the data (a string's index). */
static bool type_strings(const u8 *c, u32 size, void (*fn)(u32 at, u32 data, void *user), void *user)
{
    u32 header = r16(c + 2), flags = c[9], count = r32(c + 12), start = r32(c + 16), i;
    if (header < 20 || header > size || start > size) return false;
    for (i = 0; i < count; i++) {
        u32 off, e, esize, eflags;
        if (flags & TYPE_SPARSE) {
            /* An index and an offset in words, two bytes each. */
            if ((u64)header + 4ull * (i + 1) > size) return false;
            off = r16(c + header + 4 * i + 2) * 4u;
        } else if (flags & TYPE_OFFSET16) {
            u32 o;
            if ((u64)header + 2ull * (i + 1) > size) return false;
            o = r16(c + header + 2 * i);
            if (o == 0xFFFF) continue;
            off = o * 4u;
        } else {
            if ((u64)header + 4ull * (i + 1) > size) return false;
            off = r32(c + header + 4 * i);
            if (off == 0xFFFFFFFFu) continue;
        }
        e = start + off;
        if (e < start || (u64)e + 8 > size) return false;
        esize = r16(c + e);
        eflags = r16(c + e + 2);
        if (eflags & ENTRY_COMPACT) {
            /* A key, flags with the value's type in their high byte, and the data. */
            if ((eflags >> 8) == TYPE_STRING) fn(e + 4, r32(c + e + 4), user);
        } else if (eflags & ENTRY_COMPLEX) {
            /* A map: its parent and count, then the count's (name, Res_value) pairs. */
            u32 k, m, maps;
            if ((u64)e + 16 > size || esize < 16) return false;
            maps = r32(c + e + 12);
            m = e + esize;
            for (k = 0; k < maps; k++, m += 12) {
                if (m < e || (u64)m + 12 > size) return false;
                if (c[m + 7] == TYPE_STRING) fn(m + 8, r32(c + m + 8), user);
            }
        } else {
            u32 v = e + esize;
            if (v < e || (u64)v + 8 > size) return false;
            if (c[v + 3] == TYPE_STRING) fn(v + 4, r32(c + v + 4), user);
        }
    }
    return true;
}

/* ------------------------------------------------------------ the table */

typedef struct table {
    const u8 *at;
    u32 size, header;
    pool strings;
    u32 strings_at;                    /* the global pool's offset in the table */
} table;

static bool table_read(table *t, const u8 *at, size_t size)
{
    u32 off;
    if (size < 12 || r16(at) != CHUNK_TABLE) return false;
    t->at = at;
    t->header = r16(at + 2);
    t->size = r32(at + 4);
    if (t->header < 12 || t->size > size || t->size < t->header) return false;
    for (off = t->header; (u64)off + 8 <= t->size; off += r32(at + off + 4)) {
        u32 csize = r32(at + off + 4);
        if (csize < 8 || (u64)off + csize > t->size) return false;
        if (r16(at + off) == CHUNK_POOL) {
            t->strings_at = off;
            return pool_read(&t->strings, at + off, csize);
        }
    }
    return false;
}

/* Each type chunk of each package: `fn` with the chunk, its size and its offset in the table. */
static bool each_type(const table *t, bool (*fn)(const u8 *c, u32 size, u32 at, void *user), void *user)
{
    u32 off;
    for (off = t->header; (u64)off + 8 <= t->size; off += r32(t->at + off + 4)) {
        u32 csize = r32(t->at + off + 4), sub;
        if (csize < 8 || (u64)off + csize > t->size) return false;
        if (r16(t->at + off) != CHUNK_PACKAGE) continue;
        for (sub = off + r16(t->at + off + 2); (u64)sub + 8 <= (u64)off + csize; sub += r32(t->at + sub + 4)) {
            u32 ssize = r32(t->at + sub + 4);
            if (ssize < 8 || (u64)sub + ssize > (u64)off + csize) return false;
            if (r16(t->at + sub) == CHUNK_TYPE && !fn(t->at + sub, ssize, sub, user)) return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ cutting down */

typedef struct cut {
    const table *t;
    const char *const *languages;
    int n;
    u8 *used;                          /* a bit for each string of the global pool that stays */
    u32 *index;                        /* each string's index in the new pool; SPAN_END for one that goes */
    u32 dropped;                       /* type chunks that go */
    u32 first_dropped;                 /* the offset of the first, in the table */
    u8 *out;                           /* when writing: the chunk being written */
} cut;

static void mark(u32 at, u32 data, void *user)
{
    cut *c = (cut *)user;
    (void)at;
    if (data < c->t->strings.count) c->used[data >> 3] |= (u8)(1u << (data & 7));
}

static bool is_used(const cut *c, u32 i) { return (c->used[i >> 3] >> (i & 7)) & 1; }

static bool mark_type(const u8 *chunk, u32 size, u32 at, void *user)
{
    cut *c = (cut *)user;
    if (!kept(chunk, r16(chunk + 2), c->languages, c->n)) {
        if (!c->dropped) c->first_dropped = at;
        c->dropped++;
        return true;
    }
    return type_strings(chunk, size, mark, c);
}

static void renumber(u32 at, u32 data, void *user)
{
    cut *c = (cut *)user;
    if (data < c->t->strings.count && c->index[data] != SPAN_END) w32(c->out + at, c->index[data]);
}

u8 *mg_arsc_keep_languages(const u8 *data, size_t size, const char *const *languages, int n, size_t *out_size)
{
    table t;
    cut c;
    const pool *g = &t.strings;
    u32 i, count = 0, styles = 0, off;
    size_t text = 0, spans = 0, pool_size, total, at;
    bool more;
    u8 *out = NULL, *p;
    if (!table_read(&t, data, size) || n < 0) return NULL;
    memset(&c, 0, sizeof(c));
    c.t = &t;
    c.languages = languages;
    c.n = n;
    c.used = (u8 *)calloc(g->count / 8 + 1, 1);
    c.index = (u32 *)malloc(sizeof(u32) * ((size_t)g->count + 1));
    if (!c.used || !c.index || !each_type(&t, mark_type, &c) || !c.dropped) goto fail;

    /* A styled string that stays keeps its spans, whose names are strings too, which may be styled themselves. */
    do {
        more = false;
        for (i = 0; i < g->styles; i++) {
            const u8 *s;
            size_t len, k;
            if (!is_used(&c, i)) continue;
            if (!pool_style(g, i, &s, &len)) goto fail;
            for (k = 0; k + 12 <= len; k += 12) {
                u32 name = r32(s + k);
                if (name < g->count && !is_used(&c, name)) {
                    c.used[name >> 3] |= (u8)(1u << (name & 7));
                    more = true;
                }
            }
        }
    } while (more);

    /* The new pool's numbering, in the old order: the styled strings stay first, as a pool must have them. */
    for (i = 0; i < g->count; i++) {
        const u8 *s;
        size_t len;
        if (!is_used(&c, i)) {
            c.index[i] = SPAN_END;
            continue;
        }
        if (!pool_string(g, i, &s, &len)) goto fail;
        c.index[i] = count++;
        text += len;
        if (i < g->styles) {
            if (!pool_style(g, i, &s, &len)) goto fail;
            styles++;
            spans += len;
        }
    }
    text = (text + 3) & ~(size_t)3;
    /* After the last style, the rest of a whole span of ends (ResStringPool checks for one). */
    if (styles) spans += 8;
    pool_size = g->header + 4 * ((size_t)count + styles) + text + spans;

    /* The type and key pools come before any type chunk; a table that has one that goes before them is left whole. */
    for (off = t.header; (u64)off + 8 <= t.size; off += r32(data + off + 4))
        if (r16(data + off) == CHUNK_PACKAGE && (u64)off + 280 <= t.size && r16(data + off + 2) >= 280) {
            u32 types = off + r32(data + off + 268), keys = off + r32(data + off + 276);
            if (c.first_dropped && (c.first_dropped < types || c.first_dropped < keys)) goto fail;
        }

    total = t.size - g->size + pool_size;
    out = (u8 *)malloc(total);
    if (!out) goto fail;

    /* The table's header, then everything in its order: the new pool for the old, each package without the chunks that go. */
    memcpy(out, data, t.header);
    at = t.header;
    for (off = t.header; (u64)off + 8 <= t.size; off += r32(data + off + 4)) {
        u32 csize = r32(data + off + 4);
        if (off == t.strings_at) {
            /* The pool: its header, the strings' offsets, the styles' offsets, the strings, the styles. */
            const size_t strings_start = g->header + 4 * ((size_t)count + styles), styles_start = strings_start + text;
            size_t sat = strings_start, yat = styles_start, k;
            u32 j = 0, y = 0;
            p = out + at;
            memcpy(p, g->at, g->header);
            w32(p + 4, (u32)pool_size);
            w32(p + 8, count);
            w32(p + 12, styles);
            w32(p + 20, (u32)strings_start);
            w32(p + 24, styles ? (u32)styles_start : 0);
            for (i = 0; i < g->count; i++) {
                const u8 *s;
                size_t len;
                if (c.index[i] == SPAN_END) continue;
                pool_string(g, i, &s, &len);
                w32(p + g->header + 4 * j++, (u32)(sat - strings_start));
                memcpy(p + sat, s, len);
                sat += len;
                if (i < g->styles) {
                    pool_style(g, i, &s, &len);
                    w32(p + g->header + 4 * ((size_t)count + y++), (u32)(yat - styles_start));
                    for (k = 0; k + 12 <= len; k += 12) {
                        u32 name = r32(s + k);
                        w32(p + yat + k, name < g->count ? c.index[name] : name);
                        memcpy(p + yat + k + 4, s + k + 4, 8);
                    }
                    w32(p + yat + len - 4, SPAN_END);
                    yat += len;
                }
            }
            while (sat < styles_start) p[sat++] = 0;
            if (styles) memset(p + yat, 0xFF, 8);
            at += pool_size;
        } else if (r16(data + off) == CHUNK_PACKAGE) {
            size_t start = at;
            u32 sub, header = r16(data + off + 2);
            memcpy(out + at, data + off, header);
            at += header;
            for (sub = off + header; (u64)sub + 8 <= (u64)off + csize; sub += r32(data + sub + 4)) {
                u32 ssize = r32(data + sub + 4);
                const u8 *s = data + sub;
                if (r16(s) == CHUNK_TYPE && !kept(s, r16(s + 2), languages, n)) continue;
                memcpy(out + at, s, ssize);
                if (r16(s) == CHUNK_TYPE) {
                    c.out = out + at;
                    type_strings(s, ssize, renumber, &c);
                }
                at += ssize;
            }
            w32(out + start + 4, (u32)(at - start));
        } else {
            memcpy(out + at, data + off, csize);
            at += csize;
        }
    }
    w32(out + 4, (u32)at);
    free(c.used);
    free(c.index);
    *out_size = at;
    return out;
fail:
    free(out);
    free(c.used);
    free(c.index);
    return NULL;
}

/* ------------------------------------------------------------ for the tests */

typedef struct each {
    const table *t;
    const char *const *languages;
    int n;
    void (*fn)(const u8 *s, size_t len, void *user);
    void *user;
    bool bad;
} each;

static void each_value(u32 at, u32 data, void *user)
{
    each *e = (each *)user;
    const u8 *s;
    size_t len;
    (void)at;
    if (pool_string(&e->t->strings, data, &s, &len)) e->fn(s, len, e->user);
    else e->bad = true;
}

static bool each_type_strings(const u8 *chunk, u32 size, u32 at, void *user)
{
    each *e = (each *)user;
    (void)at;
    if (!kept(chunk, r16(chunk + 2), e->languages, e->n)) return true;
    return type_strings(chunk, size, each_value, e);
}

bool mg_arsc_each_string(const u8 *data, size_t size, const char *const *languages, int n, void (*fn)(const u8 *s, size_t len, void *user),
                         void *user)
{
    table t;
    each e;
    if (!table_read(&t, data, size)) return false;
    e.t = &t;
    e.languages = languages;
    e.n = n;
    e.fn = fn;
    e.user = user;
    e.bad = false;
    return each_type(&t, each_type_strings, &e) && !e.bad;
}
