/*
 * Packages made and signed here (mg_appx.h, docs/PER-APP-PACKAGES.md).
 *
 * The package is a ZIP64 file laid out as makeappx lays one out: each file under a local header of version 45 with a data
 * descriptor (crc and sizes after the data, 24 bytes), stored here rather than compressed; then AppxBlockMap.xml likewise;
 * then [Content_Types].xml and AppxSignature.p7x with their sizes in their local headers (version 20); then the central
 * directory, the ZIP64 end record and its locator, and an end record whose counts, size and offset are all ones.
 *
 * The block map says each file's size, its local header's size and the SHA-256 of each 64 KB block (stored blocks have no
 * Size). The content types give each file's extension a type, and AppxBlockMap.xml and AppxSignature.p7x theirs.
 *
 * The signature (AppxSignature.p7x: "PKCX" and a DER PKCS #7 SignedData) is Authenticode's, over a digest that is the
 * five letters "APPX" followed by tagged SHA-256 digests, checked against signtool's on a package of its own:
 *   AXPC  every byte before the signature's local header;
 *   AXCD  the central directory without the signature's entry, then a ZIP64 end record, locator and end record written as
 *         if the signature were not there (its entry not counted, the directory starting where its local header does);
 *   AXCT  [Content_Types].xml;
 *   AXBM  AppxBlockMap.xml.
 * That digest goes into an SpcIndirectDataContent with the APPX SIP's information, whose content (without its own header)
 * the signer's messageDigest attribute is the SHA-256 of; the RSA signature (PKCS #1 v1.5, SHA-256) is over the signer's
 * attributes as a DER SET.
 */
#include "mg_appx.h"

#include <stdio.h>

#ifdef _WIN32

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <time.h>

#include "zlib.h"

#include "../host/mg_host.h"

#define BLOCK 65536u

static void set_err(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || !errlen) return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ buffers */

typedef struct buf {
    u8 *p;
    size_t n, cap;
    bool bad;
} buf;

static void put(buf *b, const void *d, size_t n)
{
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        u8 *p;
        while (cap < b->n + n) cap *= 2;
        p = (u8 *)realloc(b->p, cap);
        if (!p) {
            b->bad = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put_str(buf *b, const char *s) { put(b, s, strlen(s)); }
static void put8(buf *b, u8 v) { put(b, &v, 1); }
static void put16(buf *b, u16 v) { u8 t[2] = { (u8)v, (u8)(v >> 8) }; put(b, t, 2); }
static void put32(buf *b, u32 v) { u8 t[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; put(b, t, 4); }
static void put64(buf *b, u64 v) { put32(b, (u32)v); put32(b, (u32)(v >> 32)); }
static void buf_free(buf *b) { free(b->p); memset(b, 0, sizeof(*b)); }

/* ------------------------------------------------------------------ DER */

static void der_head(buf *b, u8 tag, size_t n)
{
    put8(b, tag);
    if (n < 0x80) put8(b, (u8)n);
    else if (n < 0x100) { put8(b, 0x81); put8(b, (u8)n); }
    else if (n < 0x10000) { put8(b, 0x82); put8(b, (u8)(n >> 8)); put8(b, (u8)n); }
    else { put8(b, 0x83); put8(b, (u8)(n >> 16)); put8(b, (u8)(n >> 8)); put8(b, (u8)n); }
}
static void der_raw(buf *b, u8 tag, const void *p, size_t n) { der_head(b, tag, n); put(b, p, n); }
/* The content `c` under `tag`; `c` is freed. */
static void der_wrap(buf *b, u8 tag, buf *c)
{
    if (c->bad) b->bad = true;
    der_raw(b, tag, c->p, c->n);
    buf_free(c);
}
static void der_oid(buf *b, const char *dotted)
{
    u8 t[32];
    size_t n = 0;
    unsigned long arcs[16];
    int k = 0, i;
    while (*dotted && k < 16) {
        arcs[k++] = strtoul(dotted, (char **)&dotted, 10);
        if (*dotted == '.') dotted++;
    }
    t[n++] = (u8)(arcs[0] * 40 + arcs[1]);
    for (i = 2; i < k; i++) {
        u8 tmp[5];
        int m = 0;
        unsigned long v = arcs[i];
        do { tmp[m++] = (u8)(v & 0x7f); v >>= 7; } while (v);
        while (m--) t[n++] = (u8)(tmp[m] | (m ? 0x80 : 0));
    }
    der_raw(b, 0x06, t, n);
}
static void der_null(buf *b) { put8(b, 0x05); put8(b, 0x00); }
/* An unsigned big-endian number as an INTEGER. */
static void der_uint(buf *b, const u8 *p, size_t n)
{
    while (n > 1 && !p[0]) { p++; n--; }
    if (p[0] & 0x80) {
        der_head(b, 0x02, n + 1);
        put8(b, 0);
        put(b, p, n);
    } else {
        der_raw(b, 0x02, p, n);
    }
}
static void der_int(buf *b, u32 v)
{
    u8 t[4] = { (u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v };
    der_uint(b, t, 4);
}
/* AlgorithmIdentifier: the OID and NULL parameters. */
static void der_alg(buf *b, const char *oid)
{
    buf c = { 0 };
    der_oid(&c, oid);
    der_null(&c);
    der_wrap(b, 0x30, &c);
}

/* Reads one TLV at *p (before end): its tag, where its content starts and how long it is; *p moves past it. */
static bool der_next(const u8 **p, const u8 *end, u8 *tag, const u8 **content, size_t *len)
{
    const u8 *q = *p;
    size_t n;
    if (end - q < 2) return false;
    *tag = *q++;
    n = *q++;
    if (n & 0x80) {
        int k = (int)(n & 0x7f);
        if (k < 1 || k > 3 || end - q < k) return false;
        n = 0;
        while (k--) n = n << 8 | *q++;
    }
    if ((size_t)(end - q) < n) return false;
    *content = q;
    *len = n;
    *p = q + n;
    return true;
}

#define OID_SHA256 "2.16.840.1.101.3.4.2.1"
#define OID_RSA "1.2.840.113549.1.1.1"
#define OID_SHA256_RSA "1.2.840.113549.1.1.11"
#define OID_SIGNED_DATA "1.2.840.113549.1.7.2"
#define OID_CONTENT_TYPE "1.2.840.113549.1.9.3"
#define OID_MESSAGE_DIGEST "1.2.840.113549.1.9.4"
#define OID_SPC_INDIRECT_DATA "1.3.6.1.4.1.311.2.1.4"
#define OID_SPC_STATEMENT_TYPE "1.3.6.1.4.1.311.2.1.11"
#define OID_SPC_SP_OPUS_INFO "1.3.6.1.4.1.311.2.1.12"
#define OID_SPC_INDIVIDUAL "1.3.6.1.4.1.311.2.1.21"
#define OID_SPC_SIPINFO "1.3.6.1.4.1.311.2.1.30"

/* The APPX subject interface package (osslsigncode's and signtool's SpcSipInfo for a package, not a bundle). */
static const u8 APPX_SIP[16] = { 0x4B, 0xDF, 0xC5, 0x0A, 0x07, 0xCE, 0xE2, 0x4D, 0xB7, 0x6E, 0x23, 0xC8, 0x39, 0xA0, 0x9F, 0xD1 };

/* ------------------------------------------------------------------ CNG */

static BCRYPT_ALG_HANDLE g_sha, g_rsa;

static bool cng(void)
{
    if (!g_sha && BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) g_sha = NULL;
    if (!g_rsa && BCryptOpenAlgorithmProvider(&g_rsa, BCRYPT_RSA_ALGORITHM, NULL, 0) != 0) g_rsa = NULL;
    return g_sha && g_rsa;
}

typedef struct sha {
    BCRYPT_HASH_HANDLE h;
} sha;

static bool sha_begin(sha *s) { return BCryptCreateHash(g_sha, &s->h, NULL, 0, NULL, 0, 0) == 0; }
static void sha_add(sha *s, const void *p, size_t n)
{
    const u8 *q = (const u8 *)p;
    while (n) {
        ULONG k = n > 0x40000000u ? 0x40000000u : (ULONG)n;
        BCryptHashData(s->h, (PUCHAR)q, k, 0);
        q += k;
        n -= k;
    }
}
static void sha_end(sha *s, u8 out[32])
{
    BCryptFinishHash(s->h, out, 32, 0);
    BCryptDestroyHash(s->h);
    s->h = NULL;
}
static bool sha256(const void *p, size_t n, u8 out[32])
{
    sha s;
    if (!sha_begin(&s)) return false;
    sha_add(&s, p, n);
    sha_end(&s, out);
    return true;
}

static bool rsa_sign(BCRYPT_KEY_HANDLE key, const void *data, size_t n, buf *out)
{
    u8 h[32], sig[512];
    ULONG got = 0;
    BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
    if (!sha256(data, n, h)) return false;
    if (BCryptSignHash(key, &pad, h, 32, sig, sizeof(sig), &got, BCRYPT_PAD_PKCS1) != 0) return false;
    put(out, sig, got);
    return !out->bad;
}

/* ------------------------------------------------------------------ files */

static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    return w;
}

static FILE *open_file(const char *path, const wchar_t *mode)
{
    wchar_t *w = wide(path);
    FILE *f = w ? _wfopen(w, mode) : NULL;
    free(w);
    return f;
}

static bool read_all(const char *path, buf *out)
{
    FILE *f = open_file(path, L"rb");
    u8 t[8192];
    size_t n;
    if (!f) return false;
    while ((n = fread(t, 1, sizeof(t), f)) > 0) put(out, t, n);
    fclose(f);
    return !out->bad;
}

static bool write_all(const char *path, const void *p, size_t n)
{
    FILE *f = open_file(path, L"wb");
    bool ok;
    if (!f) return false;
    ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* ------------------------------------------------------------------ the key and certificate */

struct mg_appx_key {
    BCRYPT_KEY_HANDLE key;
    buf cert;               /* DER */
    const u8 *issuer;       /* the certificate's issuer Name and serial number, as they are in it */
    size_t issuer_len;
    const u8 *serial;
    size_t serial_len;
};

/* A Name of one RDN, CN=value, as Windows writes it (UTF8String). */
static void der_name(buf *b, const char *cn)
{
    buf atv = { 0 }, rdn = { 0 }, name = { 0 };
    der_oid(&atv, "2.5.4.3");
    der_raw(&atv, 0x0C, cn, strlen(cn));
    der_wrap(&rdn, 0x30, &atv);
    der_wrap(&name, 0x31, &rdn);
    der_wrap(b, 0x30, &name);
}

static void der_utc(buf *b, time_t t)
{
    struct tm tm;
    char s[16];
    gmtime_s(&tm, &t);
    snprintf(s, sizeof(s), "%02d%02d%02d%02d%02d%02dZ", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec);
    der_raw(b, 0x17, s, 13);
}

/* An extension: its OID, critical or not, and its value's DER in an OCTET STRING (`value` is freed). */
static void der_ext(buf *b, const char *oid, bool critical, buf *value)
{
    buf c = { 0 };
    der_oid(&c, oid);
    if (critical) { put8(&c, 0x01); put8(&c, 0x01); put8(&c, 0xFF); }
    der_wrap(&c, 0x04, value);
    der_wrap(b, 0x30, &c);
}

/* The certificate for `key`, subject and issuer CN=cn, valid from a day ago for twenty years. */
static bool make_cert(BCRYPT_KEY_HANDLE key, const char *cn, buf *out)
{
    u8 blob[1024], serial[16], ski[32];
    ULONG n = 0;
    BCRYPT_RSAKEY_BLOB *h = (BCRYPT_RSAKEY_BLOB *)blob;
    buf rsapub = { 0 }, seq = { 0 }, spki = { 0 }, bits = { 0 }, tbs = { 0 }, ver = { 0 }, validity = { 0 }, exts = { 0 },
        extseq = { 0 }, v = { 0 }, eku = { 0 }, sig = { 0 }, cert = { 0 };
    time_t now = time(NULL);
    if (BCryptExportKey(key, NULL, BCRYPT_RSAPUBLIC_BLOB, blob, sizeof(blob), &n, 0) != 0) return false;
    /* RSAPublicKey { modulus, publicExponent }: CNG's blob has the exponent, then the modulus, both big-endian. */
    der_uint(&seq, blob + sizeof(*h) + h->cbPublicExp, h->cbModulus);
    der_uint(&seq, blob + sizeof(*h), h->cbPublicExp);
    der_wrap(&rsapub, 0x30, &seq);
    der_alg(&spki, OID_RSA);
    put8(&bits, 0);
    put(&bits, rsapub.p, rsapub.n);
    der_wrap(&spki, 0x03, &bits);
    sha256(rsapub.p, rsapub.n, ski);
    buf_free(&rsapub);

    BCryptGenRandom(NULL, serial, sizeof(serial), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    serial[0] = (u8)((serial[0] & 0x7f) | 0x40);

    der_int(&ver, 2);
    der_wrap(&tbs, 0xA0, &ver);
    der_uint(&tbs, serial, sizeof(serial));
    der_alg(&tbs, OID_SHA256_RSA);
    der_name(&tbs, cn);
    der_utc(&validity, now - 86400);
    der_utc(&validity, now + (time_t)20 * 365 * 86400);
    der_wrap(&tbs, 0x30, &validity);
    der_name(&tbs, cn);
    der_wrap(&tbs, 0x30, &spki);
    /* keyUsage digitalSignature (critical), extKeyUsage codeSigning, basicConstraints end entity (critical), and a
       subject key identifier: as signtool's certificates for packages have them. */
    der_raw(&v, 0x03, "\x07\x80", 2);
    der_ext(&extseq, "2.5.29.15", true, &v);
    der_oid(&eku, "1.3.6.1.5.5.7.3.3");
    der_wrap(&v, 0x30, &eku);
    der_ext(&extseq, "2.5.29.37", false, &v);
    der_raw(&v, 0x30, NULL, 0);
    der_ext(&extseq, "2.5.29.19", true, &v);
    der_raw(&v, 0x04, ski, 20);
    der_ext(&extseq, "2.5.29.14", false, &v);
    der_wrap(&exts, 0x30, &extseq);
    der_wrap(&tbs, 0xA3, &exts);
    {
        buf t = { 0 };
        der_wrap(&t, 0x30, &tbs);
        tbs = t;
    }
    put8(&sig, 0);
    if (tbs.bad || !rsa_sign(key, tbs.p, tbs.n, &sig)) {
        buf_free(&tbs);
        buf_free(&sig);
        return false;
    }
    put(&cert, tbs.p, tbs.n);
    buf_free(&tbs);
    der_alg(&cert, OID_SHA256_RSA);
    der_wrap(&cert, 0x03, &sig);
    der_wrap(out, 0x30, &cert);
    return !out->bad;
}

/* Finds the issuer and serial number in the certificate: tbsCertificate's [0] version, serialNumber, signature, issuer. */
static bool cert_parts(mg_appx_key *k)
{
    const u8 *p = k->cert.p, *end = p + k->cert.n, *c, *tbs, *tbs_end;
    u8 tag;
    size_t n;
    if (!der_next(&p, end, &tag, &c, &n) || tag != 0x30) return false;
    p = c;
    end = c + n;
    if (!der_next(&p, end, &tag, &tbs, &n) || tag != 0x30) return false;
    tbs_end = tbs + n;
    p = tbs;
    if (!der_next(&p, tbs_end, &tag, &c, &n)) return false;
    if (tag == 0xA0 && !der_next(&p, tbs_end, &tag, &c, &n)) return false;
    if (tag != 0x02) return false;
    k->serial = c;
    k->serial_len = n;
    if (!der_next(&p, tbs_end, &tag, &c, &n)) return false;   /* signature algorithm */
    {
        const u8 *start = p;
        if (!der_next(&p, tbs_end, &tag, &c, &n) || tag != 0x30) return false;
        k->issuer = start;
        k->issuer_len = (size_t)(p - start);
    }
    return true;
}

mg_appx_key *mg_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen)
{
    mg_appx_key *k;
    buf blob = { 0 };
    const char *cn = publisher;
    if (!cng()) {
        set_err(err, errlen, "CNG's SHA-256 or RSA is not there");
        return NULL;
    }
    if (strncmp(cn, "CN=", 3) || strchr(cn + 3, ',')) {
        set_err(err, errlen, "the publisher is to be one CN=..., not \"%s\"", publisher);
        return NULL;
    }
    cn += 3;
    k = (mg_appx_key *)calloc(1, sizeof(*k));
    if (!k) return NULL;
    if (read_all(key_path, &blob) && read_all(cert_path, &k->cert)) {
        if (BCryptImportKeyPair(g_rsa, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, &k->key, blob.p, (ULONG)blob.n, 0) != 0) {
            set_err(err, errlen, "the key in %s cannot be read", key_path);
            goto fail;
        }
    } else {
        ULONG n = 0;
        buf_free(&blob);
        buf_free(&k->cert);
        if (BCryptGenerateKeyPair(g_rsa, &k->key, 2048, 0) != 0 || BCryptFinalizeKeyPair(k->key, 0) != 0) {
            set_err(err, errlen, "an RSA key cannot be made");
            goto fail;
        }
        if (BCryptExportKey(k->key, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, 0, &n, 0) != 0) goto fail;
        blob.p = (u8 *)malloc(n);
        blob.n = blob.cap = n;
        if (!blob.p || BCryptExportKey(k->key, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, blob.p, n, &n, 0) != 0) goto fail;
        if (!make_cert(k->key, cn, &k->cert)) {
            set_err(err, errlen, "the certificate cannot be made");
            goto fail;
        }
        if (!write_all(key_path, blob.p, blob.n) || !write_all(cert_path, k->cert.p, k->cert.n)) {
            set_err(err, errlen, "the key or the certificate cannot be written (%s, %s)", key_path, cert_path);
            goto fail;
        }
    }
    SecureZeroMemory(blob.p, blob.n);
    buf_free(&blob);
    if (!cert_parts(k)) {
        set_err(err, errlen, "the certificate in %s cannot be read", cert_path);
        mg_appx_key_close(k);
        return NULL;
    }
    return k;
fail:
    if (blob.p) SecureZeroMemory(blob.p, blob.n);
    buf_free(&blob);
    mg_appx_key_close(k);
    return NULL;
}

void mg_appx_key_close(mg_appx_key *k)
{
    if (!k) return;
    if (k->key) BCryptDestroyKey(k->key);
    buf_free(&k->cert);
    free(k);
}

const u8 *mg_appx_key_cert(const mg_appx_key *k, size_t *len)
{
    *len = k->cert.n;
    return k->cert.p;
}

/* ------------------------------------------------------------------ the package */

typedef struct entry {
    char *name;
    u32 crc;
    u64 size;
    u64 offset;
    u16 version;            /* 45 for a file with a data descriptor, 20 for one with its sizes in its local header */
    u16 flags;
} entry;

struct mg_appx {
    FILE *f;
    char *path;
    u64 pos;
    sha axpc;               /* every byte written, up to the signature (while hashing) */
    bool hashing;
    entry *e;
    size_t n, cap;
    buf blockmap;
    u16 time, date;
    bool bad;
    char err[200];
};

static void fail(mg_appx *a, const char *fmt, ...)
{
    va_list ap;
    if (a->bad) return;
    a->bad = true;
    va_start(ap, fmt);
    vsnprintf(a->err, sizeof(a->err), fmt, ap);
    va_end(ap);
}

static void out(mg_appx *a, const void *p, size_t n)
{
    if (a->bad) return;
    if (fwrite(p, 1, n, a->f) != n) {
        fail(a, "writing %s failed", a->path);
        return;
    }
    if (a->hashing) sha_add(&a->axpc, p, n);
    a->pos += n;
}

static void out_buf(mg_appx *a, buf *b)
{
    if (b->bad) fail(a, "out of memory");
    out(a, b->p, b->n);
    buf_free(b);
}

mg_appx *mg_appx_begin(const char *path, char *err, size_t errlen)
{
    mg_appx *a;
    time_t now = time(NULL);
    struct tm tm;
    if (!cng()) {
        set_err(err, errlen, "CNG's SHA-256 or RSA is not there");
        return NULL;
    }
    a = (mg_appx *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->path = _strdup(path);
    a->f = open_file(path, L"wb");
    if (!a->path || !a->f || !sha_begin(&a->axpc)) {
        set_err(err, errlen, "%s cannot be written", path);
        if (a->f) fclose(a->f);
        if (a->axpc.h) BCryptDestroyHash(a->axpc.h);
        free(a->path);
        free(a);
        return NULL;
    }
    a->hashing = true;
    localtime_s(&tm, &now);
    a->time = (u16)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    a->date = (u16)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    put_str(&a->blockmap, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\r\n<BlockMap "
                          "xmlns=\"http://schemas.microsoft.com/appx/2010/blockmap\" "
                          "HashMethod=\"http://www.w3.org/2001/04/xmlenc#sha256\">");
    return a;
}

static entry *new_entry(mg_appx *a, const char *name, u16 version, u16 flags)
{
    entry *e;
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 32;
        entry *p = (entry *)realloc(a->e, cap * sizeof(*p));
        if (!p) {
            fail(a, "out of memory");
            return NULL;
        }
        a->e = p;
        a->cap = cap;
    }
    e = &a->e[a->n++];
    memset(e, 0, sizeof(*e));
    e->name = _strdup(name);
    e->offset = a->pos;
    e->version = version;
    e->flags = flags;
    if (!e->name) fail(a, "out of memory");
    return e;
}

static void local_header(mg_appx *a, const entry *e, u32 crc, u32 size)
{
    buf h = { 0 };
    put32(&h, 0x04034b50);
    put16(&h, e->version);
    put16(&h, e->flags);
    put16(&h, 0);           /* stored */
    put16(&h, a->time);
    put16(&h, a->date);
    put32(&h, crc);
    put32(&h, size);
    put32(&h, size);
    put16(&h, (u16)strlen(e->name));
    put16(&h, 0);
    put_str(&h, e->name);
    out_buf(a, &h);
}

static void xml_escaped(buf *b, const char *s, bool backslashes)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': put_str(b, "&amp;"); break;
        case '<': put_str(b, "&lt;"); break;
        case '>': put_str(b, "&gt;"); break;
        case '"': put_str(b, "&quot;"); break;
        case '\'': put_str(b, "&apos;"); break;
        case '/': put8(b, backslashes ? '\\' : '/'); break;
        default: put8(b, (u8)*s); break;
        }
    }
}

static void base64(buf *b, const u8 *p, size_t n)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i + 2 < n; i += 3) {
        u32 v = (u32)p[i] << 16 | (u32)p[i + 1] << 8 | p[i + 2];
        char c[4] = { t[v >> 18], t[(v >> 12) & 63], t[(v >> 6) & 63], t[v & 63] };
        put(b, c, 4);
    }
    if (i < n) {
        u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0);
        char c[4] = { t[v >> 18], t[(v >> 12) & 63], i + 1 < n ? t[(v >> 6) & 63] : '=', '=' };
        put(b, c, 4);
    }
}

static bool reserved_name(const char *name)
{
    return !_stricmp(name, "AppxBlockMap.xml") || !_stricmp(name, "[Content_Types].xml") || !_stricmp(name, "AppxSignature.p7x")
        || !_strnicmp(name, "AppxMetadata/", 13);
}

/* Names are kept to what a part name needs no escaping for (letters, digits, '.', '-', '_', and '/' between folders):
   anything else would have to be percent-encoded in the ZIP and the content types, and the packages made here have no
   need of it. */
static bool plain_name(const char *name)
{
    const char *s;
    if (!name[0] || name[0] == '/' || name[strlen(name) - 1] == '/' || strstr(name, "//")) return false;
    for (s = name; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'
              || *s == '_' || *s == '/'))
            return false;
    return true;
}

/* One file of the package, from a reader of its bytes: a local header, the bytes block by block, a data descriptor, and
   its line in the block map. */
typedef size_t (*reader)(void *src, u8 *to, size_t n);

static bool add(mg_appx *a, const char *name, reader read, void *src)
{
    entry *e;
    u8 *block;
    u32 crc = 0;
    u64 size = 0;
    size_t n;
    char line[64];
    if (a->bad) return false;
    if (!plain_name(name) || reserved_name(name)) {
        fail(a, "\"%s\" cannot be a file of a package", name);
        return false;
    }
    e = new_entry(a, name, 45, 0x0008);
    block = (u8 *)malloc(BLOCK);
    if (!e || !block) {
        free(block);
        fail(a, "out of memory");
        return false;
    }
    local_header(a, e, 0, 0);
    put_str(&a->blockmap, "<File Name=\"");
    xml_escaped(&a->blockmap, name, true);
    /* (The size goes in once it is known: the attribute is written after the blocks, at the end of the element's tag.) */
    {
        buf blocks = { 0 };
        while (!a->bad && (n = read(src, block, BLOCK)) > 0) {
            u8 h[32];
            crc = (u32)crc32(crc, block, (uInt)n);
            size += n;
            sha256(block, n, h);
            put_str(&blocks, "<Block Hash=\"");
            base64(&blocks, h, 32);
            put_str(&blocks, "\"/>");
            out(a, block, n);
        }
        snprintf(line, sizeof(line), "\" Size=\"%llu\" LfhSize=\"%u\">", (unsigned long long)size, (unsigned)(30 + strlen(name)));
        put_str(&a->blockmap, line);
        put(&a->blockmap, blocks.p, blocks.n);
        if (blocks.bad) a->blockmap.bad = true;
        buf_free(&blocks);
        put_str(&a->blockmap, "</File>");
    }
    free(block);
    if (size > 0xFFFFFFFFull) fail(a, "%s is larger than 4 GB", name);
    e->crc = crc;
    e->size = size;
    {
        buf d = { 0 };
        put32(&d, 0x08074b50);
        put32(&d, crc);
        put64(&d, size);
        put64(&d, size);
        out_buf(a, &d);
    }
    return !a->bad;
}

/* A file read through the host's files (mg_host.h), so that one the core reaches by handle is read as any other. */
typedef struct file_src {
    mg_file *f;
    bool failed;
} file_src;

static size_t read_file(void *src, u8 *to, size_t n)
{
    file_src *s = (file_src *)src;
    int err = 0;
    i64 got = mg_file_read(s->f, to, n, &err);
    if (got < 0) {
        s->failed = true;
        return 0;
    }
    return (size_t)got;
}

typedef struct mem_src {
    const u8 *p;
    size_t left;
} mem_src;

static size_t read_mem(void *src, u8 *to, size_t n)
{
    mem_src *m = (mem_src *)src;
    if (n > m->left) n = m->left;
    memcpy(to, m->p, n);
    m->p += n;
    m->left -= n;
    return n;
}

bool mg_appx_add_file(mg_appx *a, const char *name, const char *src_path)
{
    file_src s;
    int err = 0;
    bool ok;
    if (a->bad) return false;
    s.f = mg_file_open(src_path, MG_O_READ, &err);
    s.failed = false;
    if (!s.f) {
        fail(a, "%s cannot be read", src_path);
        return false;
    }
    ok = add(a, name, read_file, &s);
    if (s.failed) fail(a, "reading %s failed", src_path);
    mg_file_close(s.f);
    return ok && !a->bad;
}

bool mg_appx_add_data(mg_appx *a, const char *name, const void *data, size_t size)
{
    mem_src m = { (const u8 *)data, size };
    return add(a, name, read_mem, &m);
}

/* A file whose bytes are all known (the content types, the signature): its sizes in its local header, no descriptor. */
static void add_known(mg_appx *a, const char *name, const buf *b)
{
    entry *e = new_entry(a, name, 20, 0);
    if (!e) return;
    e->crc = (u32)crc32(0, b->p, (uInt)b->n);
    e->size = b->n;
    local_header(a, e, e->crc, (u32)b->n);
    out(a, b->p, b->n);
}

static const char *type_of(const char *ext)
{
    static const struct { const char *ext, *type; } types[] = {
        { "exe", "application/x-msdownload" }, { "dll", "application/x-msdownload" }, { "png", "image/png" },
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "txt", "text/plain" },
        { "apk", "application/vnd.android.package-archive" },
    };
    size_t i;
    for (i = 0; i < MG_COUNT(types); i++)
        if (!_stricmp(ext, types[i].ext)) return types[i].type;
    return "application/octet-stream";
}

/* [Content_Types].xml: a type for each extension (AppxManifest.xml's for xml when it is the only XML file, as makeappx
   writes it), one for each file without an extension, and those of the block map and the signature. */
static void content_types(mg_appx *a, buf *b)
{
    buf defaults = { 0 }, overrides = { 0 };
    char seen[64][16];
    int nseen = 0;
    size_t i;
    int other_xml = 0;
    for (i = 0; i < a->n; i++) {
        const char *dot = strrchr(a->e[i].name, '.'), *slash = strrchr(a->e[i].name, '/');
        if (dot && (!slash || dot > slash) && !_stricmp(dot + 1, "xml") && _stricmp(a->e[i].name, "AppxManifest.xml") && _stricmp(a->e[i].name, "AppxBlockMap.xml"))
            other_xml = 1;
    }
    put_str(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
               "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">");
    for (i = 0; i < a->n; i++) {
        const char *name = a->e[i].name, *dot = strrchr(name, '.'), *slash = strrchr(name, '/');
        int k;
        if (!_stricmp(name, "AppxBlockMap.xml")) continue;
        if (!dot || (slash && dot < slash) || !dot[1] || strlen(dot + 1) >= sizeof(seen[0])) {
            put_str(&overrides, "<Override PartName=\"/");
            xml_escaped(&overrides, name, false);
            put_str(&overrides, "\" ContentType=\"application/octet-stream\"/>");
            continue;
        }
        for (k = 0; k < nseen && _stricmp(seen[k], dot + 1); k++) {}
        if (k < nseen || nseen == (int)MG_COUNT(seen)) continue;
        strcpy(seen[nseen++], dot + 1);
        put_str(&defaults, "<Default Extension=\"");
        xml_escaped(&defaults, dot + 1, false);
        put_str(&defaults, "\" ContentType=\"");
        put_str(&defaults, !_stricmp(dot + 1, "xml") ? (other_xml ? "application/xml" : "application/vnd.ms-appx.manifest+xml") : type_of(dot + 1));
        put_str(&defaults, "\"/>");
    }
    if (other_xml) put_str(&overrides, "<Override PartName=\"/AppxManifest.xml\" ContentType=\"application/vnd.ms-appx.manifest+xml\"/>");
    put_str(&overrides, "<Override PartName=\"/AppxBlockMap.xml\" ContentType=\"application/vnd.ms-appx.blockmap+xml\"/>"
                        "<Override PartName=\"/AppxSignature.p7x\" ContentType=\"application/vnd.ms-appx.signature\"/>");
    put(b, defaults.p, defaults.n);
    put(b, overrides.p, overrides.n);
    if (defaults.bad || overrides.bad) b->bad = true;
    buf_free(&defaults);
    buf_free(&overrides);
    put_str(b, "</Types>");
}

static void central_entry(buf *b, const entry *e, u16 time, u16 date)
{
    put32(b, 0x02014b50);
    put16(b, 0x2d);         /* made by: MS-DOS, version 4.5 */
    put16(b, e->version);
    put16(b, e->flags);
    put16(b, 0);            /* stored */
    put16(b, time);
    put16(b, date);
    put32(b, e->crc);
    put32(b, (u32)e->size);
    put32(b, (u32)e->size);
    put16(b, (u16)strlen(e->name));
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0);
    put32(b, 0);
    put32(b, (u32)e->offset);
    put_str(b, e->name);
}

/* The ZIP64 end record, its locator and the end record, for `count` entries in `size` bytes of directory at `offset`. */
static void end_records(buf *b, u64 count, u64 size, u64 offset)
{
    put32(b, 0x06064b50);
    put64(b, 44);
    put16(b, 0x2d);
    put16(b, 0x2d);
    put32(b, 0);
    put32(b, 0);
    put64(b, count);
    put64(b, count);
    put64(b, size);
    put64(b, offset);
    put32(b, 0x07064b50);
    put32(b, 0);
    put64(b, offset + size);
    put32(b, 1);
    put32(b, 0x06054b50);
    put16(b, 0);
    put16(b, 0);
    put16(b, 0xFFFF);
    put16(b, 0xFFFF);
    put32(b, 0xFFFFFFFF);
    put32(b, 0xFFFFFFFF);
    put16(b, 0);
}

static void directory(mg_appx *a, buf *b)
{
    size_t i;
    for (i = 0; i < a->n; i++) central_entry(b, &a->e[i], a->time, a->date);
}

/* The SpcIndirectDataContent for the package's digest `blob` (its DER, with its own header). */
static void indirect_data(buf *out, const buf *blob)
{
    buf sip = { 0 }, attr = { 0 }, digest = { 0 }, c = { 0 };
    int i;
    der_int(&sip, 0x01010000);
    der_raw(&sip, 0x04, APPX_SIP, sizeof(APPX_SIP));
    for (i = 0; i < 5; i++) der_int(&sip, 0);
    der_oid(&attr, OID_SPC_SIPINFO);
    der_wrap(&attr, 0x30, &sip);
    der_wrap(&c, 0x30, &attr);
    der_alg(&digest, OID_SHA256);
    der_raw(&digest, 0x04, blob->p, blob->n);
    der_wrap(&c, 0x30, &digest);
    der_wrap(out, 0x30, &c);
}

static int der_cmp(const buf *x, const buf *y)
{
    size_t n = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->p, y->p, n);
    if (c) return c;
    return x->n < y->n ? -1 : x->n > y->n;
}

/* The signer's attributes, sorted as a DER SET OF is, their content only (no SET header). */
static void signer_attributes(buf *out, const u8 digest[32])
{
    buf attr[4] = { { 0 } }, v = { 0 }, s = { 0 }, t = { 0 };
    int i, j;
    /* SpcSpOpusInfo, empty */
    der_oid(&attr[0], OID_SPC_SP_OPUS_INFO);
    der_raw(&v, 0x30, NULL, 0);
    der_wrap(&attr[0], 0x31, &v);
    /* contentType: SpcIndirectDataContent */
    der_oid(&attr[1], OID_CONTENT_TYPE);
    der_oid(&v, OID_SPC_INDIRECT_DATA);
    der_wrap(&attr[1], 0x31, &v);
    /* SpcStatementType: individual code signing */
    der_oid(&attr[2], OID_SPC_STATEMENT_TYPE);
    der_oid(&s, OID_SPC_INDIVIDUAL);
    der_wrap(&v, 0x30, &s);
    der_wrap(&attr[2], 0x31, &v);
    /* messageDigest */
    der_oid(&attr[3], OID_MESSAGE_DIGEST);
    der_raw(&v, 0x04, digest, 32);
    der_wrap(&attr[3], 0x31, &v);
    for (i = 0; i < 4; i++) {
        der_wrap(&t, 0x30, &attr[i]);
        attr[i] = t;
        memset(&t, 0, sizeof(t));
    }
    for (i = 1; i < 4; i++)
        for (j = i; j > 0 && der_cmp(&attr[j - 1], &attr[j]) > 0; j--) {
            buf x = attr[j];
            attr[j] = attr[j - 1];
            attr[j - 1] = x;
        }
    for (i = 0; i < 4; i++) {
        put(out, attr[i].p, attr[i].n);
        if (attr[i].bad) out->bad = true;
        buf_free(&attr[i]);
    }
}

/* AppxSignature.p7x for the package digest `blob`. */
static bool signature(const mg_appx_key *k, const buf *blob, buf *p7x)
{
    buf spc = { 0 }, attrs = { 0 }, set = { 0 }, sig = { 0 }, si = { 0 }, ias = { 0 }, sd = { 0 }, algs = { 0 }, ci = { 0 },
        certs = { 0 }, sis = { 0 }, sdseq = { 0 }, top = { 0 }, explicit_sd = { 0 };
    const u8 *p, *c;
    u8 tag, digest[32];
    size_t n;
    indirect_data(&spc, blob);
    if (spc.bad) return false;
    /* The message digest is of the SpcIndirectDataContent's content, without its SEQUENCE header. */
    p = spc.p;
    if (!der_next(&p, spc.p + spc.n, &tag, &c, &n)) return false;
    sha256(c, n, digest);
    signer_attributes(&attrs, digest);
    der_raw(&set, 0x31, attrs.p, attrs.n);
    if (set.bad || !rsa_sign(k->key, set.p, set.n, &sig)) {
        buf_free(&spc);
        buf_free(&attrs);
        buf_free(&set);
        buf_free(&sig);
        return false;
    }
    buf_free(&set);

    der_int(&si, 1);
    put(&ias, k->issuer, k->issuer_len);
    der_uint(&ias, k->serial, k->serial_len);
    der_wrap(&si, 0x30, &ias);
    der_alg(&si, OID_SHA256);
    der_wrap(&si, 0xA0, &attrs);
    der_alg(&si, OID_RSA);
    der_wrap(&si, 0x04, &sig);
    der_wrap(&sis, 0x30, &si);

    der_int(&sd, 1);
    der_alg(&algs, OID_SHA256);
    der_wrap(&sd, 0x31, &algs);
    der_oid(&ci, OID_SPC_INDIRECT_DATA);
    der_wrap(&ci, 0xA0, &spc);
    der_wrap(&sd, 0x30, &ci);
    put(&certs, k->cert.p, k->cert.n);
    der_wrap(&sd, 0xA0, &certs);
    der_wrap(&sd, 0x31, &sis);
    der_wrap(&sdseq, 0x30, &sd);
    der_oid(&top, OID_SIGNED_DATA);
    der_wrap(&explicit_sd, 0xA0, &sdseq);
    put(&top, explicit_sd.p, explicit_sd.n);
    buf_free(&explicit_sd);
    put_str(p7x, "PKCX");
    der_wrap(p7x, 0x30, &top);
    return !p7x->bad;
}

bool mg_appx_finish(mg_appx *a, const mg_appx_key *k, char *err, size_t errlen)
{
    buf types = { 0 }, cd = { 0 }, hashed = { 0 }, blob = { 0 }, p7x = { 0 };
    u8 h[32];
    size_t i;
    bool ok;
    put_str(&a->blockmap, "</BlockMap>");
    if (a->blockmap.bad) fail(a, "out of memory");
    if (!a->bad) {
        mem_src m = { a->blockmap.p, a->blockmap.n };
        /* The block map is a file of the package like the others, in the same form, but not in itself. */
        entry *e = new_entry(a, "AppxBlockMap.xml", 45, 0x0008);
        if (e) {
            u32 crc = 0;
            local_header(a, e, 0, 0);
            crc = (u32)crc32(0, m.p, (uInt)m.left);
            out(a, m.p, m.left);
            e->crc = crc;
            e->size = m.left;
            {
                buf d = { 0 };
                put32(&d, 0x08074b50);
                put32(&d, crc);
                put64(&d, e->size);
                put64(&d, e->size);
                out_buf(a, &d);
            }
        }
    }
    content_types(a, &types);
    if (types.bad) fail(a, "out of memory");
    add_known(a, "[Content_Types].xml", &types);

    /* The digests, the signature's file not being there yet: what is written so far, and the directory and end records
       of a package without it, whose directory would start where the signature's local header will. */
    put_str(&blob, "APPX");
    put_str(&blob, "AXPC");
    a->hashing = false;
    sha_end(&a->axpc, h);
    put(&blob, h, 32);
    directory(a, &hashed);
    end_records(&hashed, a->n, hashed.n, a->pos);
    put_str(&blob, "AXCD");
    sha256(hashed.p, hashed.n, h);
    put(&blob, h, 32);
    buf_free(&hashed);
    put_str(&blob, "AXCT");
    sha256(types.p, types.n, h);
    put(&blob, h, 32);
    put_str(&blob, "AXBM");
    sha256(a->blockmap.p, a->blockmap.n, h);
    put(&blob, h, 32);
    if (!a->bad && (blob.bad || !signature(k, &blob, &p7x))) fail(a, "the package cannot be signed");
    if (!a->bad) add_known(a, "AppxSignature.p7x", &p7x);
    {
        u64 at = a->pos;
        directory(a, &cd);
        end_records(&cd, a->n, cd.n, at);
        out_buf(a, &cd);
    }
    if (a->pos > 0xFFFFFFFFull) fail(a, "the package is larger than 4 GB");
    if (fclose(a->f) != 0) fail(a, "writing %s failed", a->path);
    ok = !a->bad;
    if (!ok) {
        wchar_t *w = wide(a->path);
        if (w) _wremove(w);
        free(w);
        set_err(err, errlen, "%s", a->err);
    }
    buf_free(&types);
    buf_free(&blob);
    buf_free(&p7x);
    buf_free(&a->blockmap);
    for (i = 0; i < a->n; i++) free(a->e[i].name);
    free(a->e);
    free(a->path);
    free(a);
    return ok;
}

#else

mg_appx_key *mg_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen)
{
    (void)key_path; (void)cert_path; (void)publisher;
    if (err && errlen) snprintf(err, errlen, "packages are made on Windows only");
    return NULL;
}
void mg_appx_key_close(mg_appx_key *k) { (void)k; }
const u8 *mg_appx_key_cert(const mg_appx_key *k, size_t *len) { (void)k; *len = 0; return NULL; }
mg_appx *mg_appx_begin(const char *path, char *err, size_t errlen)
{
    (void)path;
    if (err && errlen) snprintf(err, errlen, "packages are made on Windows only");
    return NULL;
}
bool mg_appx_add_file(mg_appx *a, const char *name, const char *src_path) { (void)a; (void)name; (void)src_path; return false; }
bool mg_appx_add_data(mg_appx *a, const char *name, const void *data, size_t size) { (void)a; (void)name; (void)data; (void)size; return false; }
bool mg_appx_finish(mg_appx *a, const mg_appx_key *k, char *err, size_t errlen)
{
    (void)a; (void)k;
    if (err && errlen) snprintf(err, errlen, "packages are made on Windows only");
    return false;
}

#endif
