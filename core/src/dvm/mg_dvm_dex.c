/*
 * Reading a DEX file: its header and tables, strings, types, prototypes, line
 * numbers, and how long each instruction is.
 *
 * The file is used where it lies; nothing here copies it. Its header, its
 * checksum and the bounds of every table are checked when it is added, and
 * every index taken from bytecode is checked against its table before use.
 * What is not checked is the bytecode itself: there is no verifier, so a
 * method whose instructions name registers it does not have is not caught.
 * An app's DEX is as trusted as its native code, which is not checked either.
 */
#include "mg_dvm_internal.h"

#include <stdio.h>

#include "mg_dvm_ops.h"

u32 mg_dvm_uleb(const u8 **pp)
{
    const u8 *p = *pp;
    u32 r = 0;
    int shift = 0;
    for (;;) {
        u8 b = *p++;
        r |= (u32)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 28) break;
    }
    *pp = p;
    return r;
}

i32 mg_dvm_sleb(const u8 **pp)
{
    const u8 *p = *pp;
    u32 r = 0;
    int shift = 0;
    u8 b;
    do {
        b = *p++;
        r |= (u32)(b & 0x7f) << shift;
        shift += 7;
    } while ((b & 0x80) && shift < 35);
    if (shift < 32 && (b & 0x40)) r |= ~(u32)0 << shift;
    *pp = p;
    return (i32)r;
}

static u32 adler32(const u8 *p, size_t n)
{
    u32 a = 1, b = 0;
    while (n) {
        /* 5552 bytes is the most the two sums can take in before they must be reduced. */
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return b << 16 | a;
}

static bool table_fits(size_t size, u32 off, u32 count, u32 each)
{
    return count == 0 || (off >= 0x70 && (u64)off + (u64)count * each <= (u64)size);
}

/* The header and the tables' bounds; the checksum when `check` says so. NULL, with the reason in the log, when wrong. */
static mg_dvm_dex *dex_header(const u8 *b, size_t size, const char *label, bool check)
{
    mg_dvm_dex *d;
    u32 file_size;
    if (size < 0x70 || memcmp(b, "dex\n", 4) != 0 || b[7] != 0) {
        mg_warn("dvm: %s is not a DEX file", label);
        return NULL;
    }
    if (b[4] != '0' || b[5] != '3' || b[6] < '5' || b[6] > '9') {
        mg_warn("dvm: %s is DEX version %c%c%c, which this runtime does not read", label, b[4], b[5], b[6]);
        return NULL;
    }
    if (((mg_addr)b & 3) != 0) {
        mg_warn("dvm: %s is not aligned to four bytes in memory", label);
        return NULL;
    }
    file_size = mg_dvm_rd32(b + 32);
    if (file_size > size || file_size < 0x70 || mg_dvm_rd32(b + 36) != 0x70 || mg_dvm_rd32(b + 40) != 0x12345678u) {
        mg_warn("dvm: %s: the header does not fit the file (%u bytes given, %u in the header)", label, (unsigned)size, (unsigned)file_size);
        return NULL;
    }
    if (check && adler32(b + 12, file_size - 12) != mg_dvm_rd32(b + 8)) {
        mg_warn("dvm: %s: the checksum is wrong, the file is damaged", label);
        return NULL;
    }
    d = (mg_dvm_dex *)mg_dvm_meta_alloc(sizeof(*d));
    d->b = b;
    d->size = file_size;
    d->nstr = mg_dvm_rd32(b + 0x38);
    d->str_off = mg_dvm_rd32(b + 0x3c);
    d->ntype = mg_dvm_rd32(b + 0x40);
    d->type_off = mg_dvm_rd32(b + 0x44);
    d->nproto = mg_dvm_rd32(b + 0x48);
    d->proto_off = mg_dvm_rd32(b + 0x4c);
    d->nfield = mg_dvm_rd32(b + 0x50);
    d->field_off = mg_dvm_rd32(b + 0x54);
    d->nmeth = mg_dvm_rd32(b + 0x58);
    d->meth_off = mg_dvm_rd32(b + 0x5c);
    d->ncls = mg_dvm_rd32(b + 0x60);
    d->cls_off = mg_dvm_rd32(b + 0x64);
    if (!table_fits(d->size, d->str_off, d->nstr, 4) || !table_fits(d->size, d->type_off, d->ntype, 4) || !table_fits(d->size, d->proto_off, d->nproto, 12)
        || !table_fits(d->size, d->field_off, d->nfield, 8) || !table_fits(d->size, d->meth_off, d->nmeth, 8) || !table_fits(d->size, d->cls_off, d->ncls, 32)
        || d->ntype > 0x10000 || d->nproto > 0x10000) {
        mg_warn("dvm: %s: a table lies outside the file", label);
        free(d);
        return NULL;
    }
    d->checksum = mg_dvm_rd32(b + 8);
    mg_strlcpy(d->name, label, sizeof(d->name));
    return d;
}

/* What each index resolved to, filled as code first uses it. */
static void dex_caches(mg_dvm_dex *d)
{
    d->tcache = (mg_dvm_class *volatile *)mg_dvm_meta_alloc(sizeof(void *) * (d->ntype + 1));
    d->mcache = (mg_dvm_method *volatile *)mg_dvm_meta_alloc(sizeof(void *) * (d->nmeth + 1));
    d->fcache = (mg_dvm_field *volatile *)mg_dvm_meta_alloc(sizeof(void *) * (d->nfield + 1));
    d->scache = (mg_jobj *volatile *)mg_dvm_meta_alloc(sizeof(void *) * (d->nstr + 1));
    d->pcache = (const char *volatile *)mg_dvm_meta_alloc(sizeof(void *) * (d->nproto + 1));
}

mg_dvm_dex *mg_dvm_dex_open(const u8 *b, size_t size, const char *label)
{
    mg_dvm_dex *d = dex_header(b, size, label, true);
    if (d) dex_caches(d);
    return d;
}

/*
 * Every DEX file of an app was viewed whole from the start, its checksum read over all of it, and its caches made: Yandex
 * Maps' sixteen and Google Play services' fifteen in it were 245 MB of a 32-bit process's 2 GB of addresses, read from the
 * flash at every start, whether a class of them was used or not (package 494). A file is now registered by its classes'
 * names (mg_dvm_classes_add_dex copies them), and viewed when the first of its classes is linked.
 */
mg_dvm_dex *mg_dvm_dex_open_lazy(const u8 *b, size_t size, const char *label, const char *map_path, u64 map_at)
{
    mg_dvm_dex *d;
    if (!map_path) return NULL;
    d = dex_header(b, size, label, false);
    if (!d) return NULL;
    d->b = b;
    d->lazy = true;
    d->map_path = mg_strdup(map_path);
    d->map_at = map_at;
    d->map_size = size;
    return d;
}

/* Another of the runtime's DEX files that is the same bytes of the same file: viewed already, when `viewed` says so. */
static mg_dvm_dex *same_file(const char *map_path, u64 map_at, size_t size, const mg_dvm_dex *not_this, bool viewed)
{
    mg_dvm_dex *o;
    u32 i;
    for (i = 0; (o = mg_dvm_dex_at(i)) != NULL; i++)
        if (o != not_this && o->map_path && o->map_at == map_at && o->map_size == size && !strcmp(o->map_path, map_path) && (!viewed || (!o->lazy && o->b)))
            return o;
    return NULL;
}

bool mg_dvm_dex_known(const char *map_path, u64 map_at, size_t size) { return map_path && same_file(map_path, map_at, size, NULL, false) != NULL; }

bool mg_dvm_dex_ready(mg_dvm_dex *d)
{
    mg_file *f;
    void *base = NULL;
    size_t mapped = 0;
    const u8 *b = NULL;
    mg_dvm_dex *other;
    int err = 0;
    if (!d->lazy) return true;
    if (d->lazy_failed) return false;
    /*
     * The same file viewed for another scope (a service's or a process's over the app's own APK): its view serves, as the
     * bytes read for the app's own served such a scope before the files were viewed when needed.
     */
    other = same_file(d->map_path, d->map_at, d->map_size, d, true);
    if (other && other->checksum == d->checksum && other->size == d->size) {
        dex_caches(d);
        d->b = other->b;
        d->lazy = false;
        return true;
    }
    f = mg_file_open(d->map_path, MG_O_READ, &err);
    if (f) {
        b = (const u8 *)mg_file_map(f, d->map_at, d->map_size, &base, &mapped);
        mg_file_close(f);
    }
    /* The same file as when its classes were registered, and whole. */
    if (!b || ((mg_addr)b & 3) || mg_dvm_rd32(b + 32) != d->size || mg_dvm_rd32(b + 8) != d->checksum || adler32(b + 12, d->size - 12) != d->checksum) {
        mg_warn("dvm: %s: %s, and none of its classes can be had", d->name, b ? "it is not the file its classes were found in, or it is damaged" : "it cannot be viewed again");
        if (base) mg_section_unmap(base, mapped);
        d->lazy_failed = true;
        return false;
    }
    dex_caches(d);
    d->b = b;
    d->lazy = false;
    mg_dvm_dex_viewed(mapped);
    mg_dbg("dvm: %s is viewed now, for the first of its classes", d->name);
    return true;
}

const char *mg_dvm_dex_string(const mg_dvm_dex *d, u32 idx, u32 *len16)
{
    const u8 *p;
    u32 off, n;
    if (len16) *len16 = 0;
    if (idx >= d->nstr) return "";
    off = mg_dvm_rd32(d->b + d->str_off + 4 * idx);
    if (off >= d->size) return "";
    p = d->b + off;
    n = mg_dvm_uleb(&p);
    if (len16) *len16 = n;
    return (const char *)p;
}

const char *mg_dvm_dex_type(const mg_dvm_dex *d, u32 idx)
{
    return idx < d->ntype ? mg_dvm_dex_string(d, mg_dvm_rd32(d->b + d->type_off + 4 * idx), NULL) : "";
}

const char *mg_dvm_dex_proto_shorty(const mg_dvm_dex *d, u32 proto)
{
    return proto < d->nproto ? mg_dvm_dex_string(d, mg_dvm_rd32(d->b + d->proto_off + 12 * proto), NULL) : "V";
}

const char *mg_dvm_dex_proto_sig(mg_dvm_dex *d, u32 proto)
{
    const u8 *p;
    const char *sig, *ret;
    u32 params, count = 0, i;
    size_t len, k = 0;
    char *text;
    if (proto >= d->nproto) return mg_dvm_intern("()V");
    sig = d->pcache[proto];
    if (sig) return sig;
    p = d->b + d->proto_off + 12 * proto;
    ret = mg_dvm_dex_type(d, mg_dvm_rd32(p + 4));
    params = mg_dvm_rd32(p + 8);
    if (params && (u64)params + 4 <= d->size) {
        count = mg_dvm_rd32(d->b + params);
        if ((u64)params + 4 + (u64)count * 2 > d->size) count = 0;
    }
    /* A Kotlin constructor's signature runs to thousands of characters: measure, then build. */
    len = 2 + strlen(ret);
    for (i = 0; i < count; i++) len += strlen(mg_dvm_dex_type(d, mg_dvm_rd16(d->b + params + 4 + 2 * i)));
    text = (char *)mg_dvm_meta_alloc(len + 1);
    text[k++] = '(';
    for (i = 0; i < count; i++) {
        const char *t = mg_dvm_dex_type(d, mg_dvm_rd16(d->b + params + 4 + 2 * i));
        size_t l = strlen(t);
        memcpy(text + k, t, l);
        k += l;
    }
    text[k++] = ')';
    memcpy(text + k, ret, strlen(ret) + 1);
    sig = mg_dvm_intern(text);
    free(text);
    d->pcache[proto] = sig;
    return sig;
}

bool mg_dvm_descriptor_name(const char *desc, char *out, size_t n)
{
    size_t l = strlen(desc);
    if (desc[0] == 'L' && l >= 2 && desc[l - 1] == ';') {
        if (l - 2 >= n) return false;
        memcpy(out, desc + 1, l - 2);
        out[l - 2] = 0;
        return true;
    }
    if (l >= n) return false;
    memcpy(out, desc, l + 1);
    return true;
}

int mg_dvm_dex_line(const mg_dvm_method *m, u32 pc)
{
    const mg_dvm_dex *d;
    const u8 *p, *end;
    u32 debug, nparams, i, addr = 0;
    i32 line, best;
    if (!m || !m->code || !m->cls->dex) return -1;
    d = m->cls->dex;
    debug = mg_dvm_rd32(m->code + 8);
    if (!debug || debug >= d->size) return -1;
    p = d->b + debug;
    end = d->b + d->size;
    line = (i32)mg_dvm_uleb(&p);
    nparams = mg_dvm_uleb(&p);
    for (i = 0; i < nparams && p < end; i++) mg_dvm_uleb(&p);
    best = line;
    /* The line table is a little program: each special opcode moves the address and the line and marks a position. */
    while (p < end) {
        u8 op = *p++;
        if (op == 0x00) break;                                   /* end of the sequence */
        switch (op) {
        case 0x01: addr += mg_dvm_uleb(&p); break;               /* advance pc */
        case 0x02: line += mg_dvm_sleb(&p); break;               /* advance line */
        case 0x03: mg_dvm_uleb(&p); mg_dvm_uleb(&p); mg_dvm_uleb(&p); break;                      /* start local */
        case 0x04: mg_dvm_uleb(&p); mg_dvm_uleb(&p); mg_dvm_uleb(&p); mg_dvm_uleb(&p); break;     /* start local, with a signature */
        case 0x05: case 0x06: mg_dvm_uleb(&p); break;            /* end local, restart local */
        case 0x07: case 0x08: break;                             /* prologue end, epilogue begin */
        case 0x09: mg_dvm_uleb(&p); break;                       /* set file */
        default: {
            int adjusted = op - 0x0a;
            addr += (u32)(adjusted / 15);
            line += -4 + adjusted % 15;
            if (addr > pc) return best;
            best = line;
        }
        }
    }
    return best;
}

void *mg_dvm_meta_alloc(size_t size)
{
    mg_atomic_add(&g_dvm.meta_units, (i32)((size + 15) >> 4));
    return mg_alloc(size);
}

bool mg_dvm_method_annotated(const mg_dvm_method *m, const char *descriptor)
{
    const mg_dvm_dex *d = m->cls->dex;
    const u8 *dir;
    u32 off, nfields, nmethods, i, k;
    if (!d) return false;
    /* class_def -> its annotations directory -> the entry for this method -> a set of annotations -> each one's type. */
    off = mg_dvm_rd32(d->b + d->cls_off + 32 * m->cls->def + 20);
    if (!off || (u64)off + 16 > d->size) return false;
    dir = d->b + off;
    nfields = mg_dvm_rd32(dir + 4);
    nmethods = mg_dvm_rd32(dir + 8);
    if ((u64)off + 16 + ((u64)nfields + nmethods) * 8 > d->size) return false;
    for (i = 0; i < nmethods; i++) {
        const u8 *entry = dir + 16 + 8 * (size_t)nfields + 8 * (size_t)i;
        u32 set, count;
        if (mg_dvm_rd32(entry) != m->idx) continue;
        set = mg_dvm_rd32(entry + 4);
        if (!set || (u64)set + 4 > d->size) return false;
        count = mg_dvm_rd32(d->b + set);
        if ((u64)set + 4 + (u64)count * 4 > d->size) return false;
        for (k = 0; k < count; k++) {
            u32 item = mg_dvm_rd32(d->b + set + 4 + 4 * k);
            const u8 *p;
            if (!item || (u64)item + 2 > d->size) continue;
            p = d->b + item + 1;                                 /* past the visibility */
            if (!strcmp(mg_dvm_dex_type(d, mg_dvm_uleb(&p)), descriptor)) return true;
        }
        return false;
    }
    return false;
}

const char *mg_dvm_dex_source(const mg_dvm_class *c)
{
    u32 idx;
    if (!c || !c->dex) return NULL;
    idx = mg_dvm_rd32(c->dex->b + c->dex->cls_off + 32 * c->def + 16);
    return idx == 0xFFFFFFFFu ? NULL : mg_dvm_dex_string(c->dex, idx, NULL);
}

u32 mg_dvm_insn_units(const u16 *insns, u32 ninsns, u32 pc)
{
    u16 w = insns[pc];
    u64 n;
    if ((w & 0xFF) == 0 && w != 0) {
        /* A switch's or an array's data, which sits among the instructions and says how long it is. */
        if (pc + 2 > ninsns) return 0;
        if (w == 0x0100) n = 4 + (u64)insns[pc + 1] * 2;
        else if (w == 0x0200) n = 2 + (u64)insns[pc + 1] * 4;
        else if (w == 0x0300) {
            if (pc + 4 > ninsns) return 0;
            n = 4 + ((u64)insns[pc + 1] * ((u32)insns[pc + 2] | (u32)insns[pc + 3] << 16) + 1) / 2;
        } else return 0;
    } else {
        n = k_dvm_ops[w & 0xFF].units;
    }
    if (n == 0 || (u64)pc + n > ninsns) return 0;
    return (u32)n;
}

const char *mg_dvm_insn_name(u16 unit)
{
    if (unit == 0x0100) return "packed-switch-data";
    if (unit == 0x0200) return "sparse-switch-data";
    if (unit == 0x0300) return "array-data";
    return k_dvm_ops[unit & 0xFF].name ? k_dvm_ops[unit & 0xFF].name : "(unused)";
}
