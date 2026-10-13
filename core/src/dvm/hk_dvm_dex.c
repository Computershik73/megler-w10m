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
#include "hk_dvm_internal.h"

#include <stdio.h>

#include "hk_dvm_ops.h"

u32 hk_dvm_uleb(const u8 **pp)
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

i32 hk_dvm_sleb(const u8 **pp)
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

hk_dvm_dex *hk_dvm_dex_open(const u8 *b, size_t size, const char *label)
{
    hk_dvm_dex *d;
    u32 file_size;
    if (size < 0x70 || memcmp(b, "dex\n", 4) != 0 || b[7] != 0) {
        hk_warn("dvm: %s is not a DEX file", label);
        return NULL;
    }
    if (b[4] != '0' || b[5] != '3' || b[6] < '5' || b[6] > '9') {
        hk_warn("dvm: %s is DEX version %c%c%c, which this runtime does not read", label, b[4], b[5], b[6]);
        return NULL;
    }
    if (((hk_addr)b & 3) != 0) {
        hk_warn("dvm: %s is not aligned to four bytes in memory", label);
        return NULL;
    }
    file_size = hk_dvm_rd32(b + 32);
    if (file_size > size || file_size < 0x70 || hk_dvm_rd32(b + 36) != 0x70 || hk_dvm_rd32(b + 40) != 0x12345678u) {
        hk_warn("dvm: %s: the header does not fit the file (%u bytes given, %u in the header)", label, (unsigned)size, (unsigned)file_size);
        return NULL;
    }
    if (adler32(b + 12, file_size - 12) != hk_dvm_rd32(b + 8)) {
        hk_warn("dvm: %s: the checksum is wrong, the file is damaged", label);
        return NULL;
    }
    d = (hk_dvm_dex *)hk_dvm_meta_alloc(sizeof(*d));
    d->b = b;
    d->size = file_size;
    d->nstr = hk_dvm_rd32(b + 0x38);
    d->str_off = hk_dvm_rd32(b + 0x3c);
    d->ntype = hk_dvm_rd32(b + 0x40);
    d->type_off = hk_dvm_rd32(b + 0x44);
    d->nproto = hk_dvm_rd32(b + 0x48);
    d->proto_off = hk_dvm_rd32(b + 0x4c);
    d->nfield = hk_dvm_rd32(b + 0x50);
    d->field_off = hk_dvm_rd32(b + 0x54);
    d->nmeth = hk_dvm_rd32(b + 0x58);
    d->meth_off = hk_dvm_rd32(b + 0x5c);
    d->ncls = hk_dvm_rd32(b + 0x60);
    d->cls_off = hk_dvm_rd32(b + 0x64);
    if (!table_fits(d->size, d->str_off, d->nstr, 4) || !table_fits(d->size, d->type_off, d->ntype, 4) || !table_fits(d->size, d->proto_off, d->nproto, 12)
        || !table_fits(d->size, d->field_off, d->nfield, 8) || !table_fits(d->size, d->meth_off, d->nmeth, 8) || !table_fits(d->size, d->cls_off, d->ncls, 32)
        || d->ntype > 0x10000 || d->nproto > 0x10000) {
        hk_warn("dvm: %s: a table lies outside the file", label);
        free(d);
        return NULL;
    }
    d->tcache = (hk_dvm_class *volatile *)hk_dvm_meta_alloc(sizeof(void *) * (d->ntype + 1));
    d->mcache = (hk_dvm_method *volatile *)hk_dvm_meta_alloc(sizeof(void *) * (d->nmeth + 1));
    d->fcache = (hk_dvm_field *volatile *)hk_dvm_meta_alloc(sizeof(void *) * (d->nfield + 1));
    d->scache = (hk_jobj *volatile *)hk_dvm_meta_alloc(sizeof(void *) * (d->nstr + 1));
    d->pcache = (const char *volatile *)hk_dvm_meta_alloc(sizeof(void *) * (d->nproto + 1));
    hk_strlcpy(d->name, label, sizeof(d->name));
    return d;
}

const char *hk_dvm_dex_string(const hk_dvm_dex *d, u32 idx, u32 *len16)
{
    const u8 *p;
    u32 off, n;
    if (len16) *len16 = 0;
    if (idx >= d->nstr) return "";
    off = hk_dvm_rd32(d->b + d->str_off + 4 * idx);
    if (off >= d->size) return "";
    p = d->b + off;
    n = hk_dvm_uleb(&p);
    if (len16) *len16 = n;
    return (const char *)p;
}

const char *hk_dvm_dex_type(const hk_dvm_dex *d, u32 idx)
{
    return idx < d->ntype ? hk_dvm_dex_string(d, hk_dvm_rd32(d->b + d->type_off + 4 * idx), NULL) : "";
}

const char *hk_dvm_dex_proto_shorty(const hk_dvm_dex *d, u32 proto)
{
    return proto < d->nproto ? hk_dvm_dex_string(d, hk_dvm_rd32(d->b + d->proto_off + 12 * proto), NULL) : "V";
}

const char *hk_dvm_dex_proto_sig(hk_dvm_dex *d, u32 proto)
{
    const u8 *p;
    const char *sig, *ret;
    u32 params, count = 0, i;
    size_t len, k = 0;
    char *text;
    if (proto >= d->nproto) return hk_dvm_intern("()V");
    sig = d->pcache[proto];
    if (sig) return sig;
    p = d->b + d->proto_off + 12 * proto;
    ret = hk_dvm_dex_type(d, hk_dvm_rd32(p + 4));
    params = hk_dvm_rd32(p + 8);
    if (params && (u64)params + 4 <= d->size) {
        count = hk_dvm_rd32(d->b + params);
        if ((u64)params + 4 + (u64)count * 2 > d->size) count = 0;
    }
    /* A Kotlin constructor's signature runs to thousands of characters: measure, then build. */
    len = 2 + strlen(ret);
    for (i = 0; i < count; i++) len += strlen(hk_dvm_dex_type(d, hk_dvm_rd16(d->b + params + 4 + 2 * i)));
    text = (char *)hk_dvm_meta_alloc(len + 1);
    text[k++] = '(';
    for (i = 0; i < count; i++) {
        const char *t = hk_dvm_dex_type(d, hk_dvm_rd16(d->b + params + 4 + 2 * i));
        size_t l = strlen(t);
        memcpy(text + k, t, l);
        k += l;
    }
    text[k++] = ')';
    memcpy(text + k, ret, strlen(ret) + 1);
    sig = hk_dvm_intern(text);
    free(text);
    d->pcache[proto] = sig;
    return sig;
}

bool hk_dvm_descriptor_name(const char *desc, char *out, size_t n)
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

int hk_dvm_dex_line(const hk_dvm_method *m, u32 pc)
{
    const hk_dvm_dex *d;
    const u8 *p, *end;
    u32 debug, nparams, i, addr = 0;
    i32 line, best;
    if (!m || !m->code || !m->cls->dex) return -1;
    d = m->cls->dex;
    debug = hk_dvm_rd32(m->code + 8);
    if (!debug || debug >= d->size) return -1;
    p = d->b + debug;
    end = d->b + d->size;
    line = (i32)hk_dvm_uleb(&p);
    nparams = hk_dvm_uleb(&p);
    for (i = 0; i < nparams && p < end; i++) hk_dvm_uleb(&p);
    best = line;
    /* The line table is a little program: each special opcode moves the address and the line and marks a position. */
    while (p < end) {
        u8 op = *p++;
        if (op == 0x00) break;                                   /* end of the sequence */
        switch (op) {
        case 0x01: addr += hk_dvm_uleb(&p); break;               /* advance pc */
        case 0x02: line += hk_dvm_sleb(&p); break;               /* advance line */
        case 0x03: hk_dvm_uleb(&p); hk_dvm_uleb(&p); hk_dvm_uleb(&p); break;                      /* start local */
        case 0x04: hk_dvm_uleb(&p); hk_dvm_uleb(&p); hk_dvm_uleb(&p); hk_dvm_uleb(&p); break;     /* start local, with a signature */
        case 0x05: case 0x06: hk_dvm_uleb(&p); break;            /* end local, restart local */
        case 0x07: case 0x08: break;                             /* prologue end, epilogue begin */
        case 0x09: hk_dvm_uleb(&p); break;                       /* set file */
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

void *hk_dvm_meta_alloc(size_t size)
{
    hk_atomic_add(&g_dvm.meta_units, (i32)((size + 15) >> 4));
    return hk_alloc(size);
}

bool hk_dvm_method_annotated(const hk_dvm_method *m, const char *descriptor)
{
    const hk_dvm_dex *d = m->cls->dex;
    const u8 *dir;
    u32 off, nfields, nmethods, i, k;
    if (!d) return false;
    /* class_def -> its annotations directory -> the entry for this method -> a set of annotations -> each one's type. */
    off = hk_dvm_rd32(d->b + d->cls_off + 32 * m->cls->def + 20);
    if (!off || (u64)off + 16 > d->size) return false;
    dir = d->b + off;
    nfields = hk_dvm_rd32(dir + 4);
    nmethods = hk_dvm_rd32(dir + 8);
    if ((u64)off + 16 + ((u64)nfields + nmethods) * 8 > d->size) return false;
    for (i = 0; i < nmethods; i++) {
        const u8 *entry = dir + 16 + 8 * (size_t)nfields + 8 * (size_t)i;
        u32 set, count;
        if (hk_dvm_rd32(entry) != m->idx) continue;
        set = hk_dvm_rd32(entry + 4);
        if (!set || (u64)set + 4 > d->size) return false;
        count = hk_dvm_rd32(d->b + set);
        if ((u64)set + 4 + (u64)count * 4 > d->size) return false;
        for (k = 0; k < count; k++) {
            u32 item = hk_dvm_rd32(d->b + set + 4 + 4 * k);
            const u8 *p;
            if (!item || (u64)item + 2 > d->size) continue;
            p = d->b + item + 1;                                 /* past the visibility */
            if (!strcmp(hk_dvm_dex_type(d, hk_dvm_uleb(&p)), descriptor)) return true;
        }
        return false;
    }
    return false;
}

const char *hk_dvm_dex_source(const hk_dvm_class *c)
{
    u32 idx;
    if (!c || !c->dex) return NULL;
    idx = hk_dvm_rd32(c->dex->b + c->dex->cls_off + 32 * c->def + 16);
    return idx == 0xFFFFFFFFu ? NULL : hk_dvm_dex_string(c->dex, idx, NULL);
}

u32 hk_dvm_insn_units(const u16 *insns, u32 ninsns, u32 pc)
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

const char *hk_dvm_insn_name(u16 unit)
{
    if (unit == 0x0100) return "packed-switch-data";
    if (unit == 0x0200) return "sparse-switch-data";
    if (unit == 0x0300) return "array-data";
    return k_dvm_ops[unit & 0xFF].name ? k_dvm_ops[unit & 0xFF].name : "(unused)";
}
