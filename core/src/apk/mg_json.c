#include "mg_json.h"

#include <stdio.h>

/* ------------------------------------------------------------- writing */

void mg_jraw(mg_jbuf *j, const char *text)
{
    size_t len = strlen(text);
    if (j->n + len + 1 > j->cap) {
        j->cap = (j->n + len + 1) * 2;
        j->s = (char *)mg_realloc(j->s, j->cap);
    }
    memcpy(j->s + j->n, text, len + 1);
    j->n += len;
}

void mg_jstr(mg_jbuf *j, const char *text)
{
    char tmp[8];
    mg_jraw(j, "\"");
    for (; *text; text++) {
        u8 c = (u8)*text;
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            tmp[2] = 0;
        } else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        } else {
            tmp[0] = (char)c;
            tmp[1] = 0;
        }
        mg_jraw(j, tmp);
    }
    mg_jraw(j, "\"");
}

void mg_jkey(mg_jbuf *j, const char *key, bool first)
{
    if (!first) mg_jraw(j, ",");
    mg_jstr(j, key);
    mg_jraw(j, ":");
}

void mg_jint(mg_jbuf *j, long long v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lld", v);
    mg_jraw(j, tmp);
}

/* ------------------------------------------------------------- reading */

#define MAX_DEPTH 32

static const char *ws(const char *p, const char *end)
{
    while (p && p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Past a string that starts at p (its opening quote); NULL when it does not end. */
static const char *skip_string(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') {
            if (++p >= end) return NULL;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Past the value that starts at p; NULL when it is not one, or is not whole before `end`. */
static const char *skip_value(const char *p, const char *end, int depth)
{
    p = ws(p, end);
    if (!p || p >= end || depth > MAX_DEPTH) return NULL;
    if (*p == '"') return skip_string(p, end);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        bool object = *p == '{';
        p = ws(p + 1, end);
        if (p < end && *p == close) return p + 1;
        for (;;) {
            if (object) {
                if (p >= end || *p != '"') return NULL;
                p = ws(skip_string(p, end), end);
                if (!p || p >= end || *p != ':') return NULL;
                p++;
            }
            p = ws(skip_value(p, end, depth + 1), end);
            if (!p || p >= end) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p = ws(p + 1, end);
        }
    }
    /* A number, true, false or null: what follows up to the next delimiter. */
    {
        const char *start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        return p > start ? p : NULL;
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(const char *p, const char *end, u32 *out)
{
    int i;
    u32 v = 0;
    if (end - p < 4) return false;
    for (i = 0; i < 4; i++) {
        int d = hex_digit(p[i]);
        if (d < 0) return false;
        v = v << 4 | (u32)d;
    }
    *out = v;
    return true;
}

bool mg_json_string(const char *v, const char *end, char *out, size_t cap)
{
    const char *p = ws(v, end);
    size_t n = 0;
    if (cap) out[0] = 0;
    if (!p || p >= end || *p != '"' || !cap) return false;
    for (p++; p < end && *p != '"'; p++) {
        char utf8[4];
        size_t len = 1, k;
        u32 c = (u8)*p;
        if (c == '\\') {
            if (++p >= end) return false;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u':
                if (!hex4(p + 1, end, &c)) return false;
                p += 4;
                /* A surrogate pair is one character. */
                if (c >= 0xD800 && c < 0xDC00 && end - p > 6 && p[1] == '\\' && p[2] == 'u') {
                    u32 lo;
                    if (hex4(p + 3, end, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                break;
            default: c = (u8)*p; break;
            }
            if (c < 0x80) utf8[0] = (char)c;
            else if (c < 0x800) {
                utf8[0] = (char)(0xC0 | (c >> 6));
                utf8[1] = (char)(0x80 | (c & 0x3F));
                len = 2;
            } else if (c < 0x10000) {
                utf8[0] = (char)(0xE0 | (c >> 12));
                utf8[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[2] = (char)(0x80 | (c & 0x3F));
                len = 3;
            } else {
                utf8[0] = (char)(0xF0 | (c >> 18));
                utf8[1] = (char)(0x80 | ((c >> 12) & 0x3F));
                utf8[2] = (char)(0x80 | ((c >> 6) & 0x3F));
                utf8[3] = (char)(0x80 | (c & 0x3F));
                len = 4;
            }
        } else {
            utf8[0] = (char)c;
        }
        if (n + len >= cap) continue;          /* cut to fit; the rest is passed over */
        for (k = 0; k < len; k++) out[n++] = utf8[k];
    }
    out[n] = 0;
    return p < end;
}

bool mg_json_number(const char *v, const char *end, long long *out)
{
    const char *p = ws(v, end);
    bool quoted, negative = false, any = false;
    long long n = 0;
    if (!p || p >= end) return false;
    quoted = *p == '"';
    if (quoted) p++;
    if (p < end && *p == '-') {
        negative = true;
        p++;
    }
    for (; p < end && *p >= '0' && *p <= '9'; p++) {
        if (n < 100000000000000000LL) n = n * 10 + (*p - '0');
        any = true;
    }
    if (!any || (quoted && (p >= end || *p != '"'))) return false;
    *out = negative ? -n : n;
    return true;
}

const char *mg_json_member(const char *obj, const char *end, const char *key)
{
    const char *p = ws(obj, end);
    size_t klen = strlen(key);
    if (!p || p >= end || *p != '{') return NULL;
    p = ws(p + 1, end);
    while (p && p < end && *p == '"') {
        const char *name = p + 1, *after = skip_string(p, end);
        if (!after) return NULL;
        p = ws(after, end);
        if (!p || p >= end || *p != ':') return NULL;
        p = ws(p + 1, end);
        /* (A name with an escape in it is no name looked for here.) */
        if ((size_t)(after - 1 - name) == klen && !memcmp(name, key, klen)) return p;
        p = ws(skip_value(p, end, 1), end);
        if (!p || p >= end || *p != ',') return NULL;
        p = ws(p + 1, end);
    }
    return NULL;
}

bool mg_json_next(const char *v, const char *end, const char **at)
{
    const char *p;
    if (!*at) {
        p = ws(v, end);
        if (!p || p >= end || *p != '[') return false;
        p = ws(p + 1, end);
        if (!p || p >= end || *p == ']') return false;
        *at = p;
        return true;
    }
    p = ws(skip_value(*at, end, 1), end);
    if (!p || p >= end || *p != ',') return false;
    p = ws(p + 1, end);
    if (!p || p >= end) return false;
    *at = p;
    return true;
}
