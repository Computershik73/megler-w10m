/*
 * The manifest of the package of one Android app, made from Meglet's own (mg_appx.h, docs/PER-APP-PACKAGES.md).
 *
 * Everything Meglet's manifest says stays, its capabilities, dependencies, entry point and the publisher cache folder
 * among them, so that an app's package runs Meglet as Meglet runs; what makes it a package of its own is changed:
 *   - Identity's Name: "MegletApp." and the Android package, which has only letters, digits, '.' and '_' (Android's rule),
 *     '_' made '-' (an identity name has no '_'), always 50 characters long, the most a name may have: one longer is cut
 *     and given a hash of the whole at its end, one shorter is filled with a dot and a hash of the package (its
 *     resources.pri is made from one for a placeholder as long, mg_appx_pri.c);
 *   - PhoneIdentity's PhoneProductId: a GUID made from the identity name, so the same for the same package each time,
 *     and a package made again for the same app is an update of it;
 *   - Properties' DisplayName, and VisualElements' DisplayName and the tile's ShortName: the app's label (the short name
 *     cut to 40 characters, as the schema has it);
 *   - the tile's and the splash screen's BackgroundColor, when the caller gives them (the app's icon is drawn on them).
 * The version is Meglet's: a package made by a newer Meglet updates one made by an older.
 */
#include "mg_appx.h"

#include <stdio.h>

static u64 fnv64(const char *s, u64 h)
{
    for (; *s; s++) {
        h ^= (u8)*s;
        h *= 0x100000001b3ull;
    }
    return h;
}

bool mg_appx_app_identity(const char *android_package, char *out, size_t outlen)
{
    char name[64];
    size_t n = 0;
    const char *s;
    if (!android_package[0] || outlen < 51) return false;
    memcpy(name, "MegletApp.", 10);
    n = 10;
    for (s = android_package; *s && n < sizeof(name) - 1; s++) {
        char c = *s;
        if (c == '_') c = '-';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        name[n++] = c;
    }
    name[n] = 0;
    if (*s || n > 50) {
        /* Cut to 41 and a hash of the whole: "MegletApp.com.example.a-very-long-nam.1a2b3c4d" */
        if (name[40] == '.') name[40] = '-';
        snprintf(name + 41, sizeof(name) - 41, ".%08x", (unsigned)(fnv64(android_package, 0xcbf29ce484222325ull) >> 32));
    } else if (n < 50) {
        /* Filled to 50 with a dot and a hash of the package: every app package's name is as long as the placeholder its
           resources.pri was made for (mg_appx_pri.c). One of 49 gets a hash digit alone, a name not ending in a dot. */
        char hash[48];
        u64 h1 = fnv64(android_package, 0xcbf29ce484222325ull), h2 = fnv64(android_package, 0x84222325cbf29ce4ull),
            h3 = fnv64(android_package, 0x9e3779b97f4a7c15ull);
        snprintf(hash, sizeof(hash), "%016llx%016llx%016llx", (unsigned long long)h1, (unsigned long long)h2, (unsigned long long)h3);
        if (n < 49) name[n++] = '.';
        memcpy(name + n, hash, 50 - n);
        name[50] = 0;
    }
    snprintf(out, outlen, "%s", name);
    return true;
}

/* Replaces the value of attribute `attr` in the first element that starts with `element` (from `<`), in place in a new
   string; NULL if not there. `xml` is freed when a new string is made. */
static char *set_attr(char *xml, const char *element, const char *attr, const char *value)
{
    char *e = strstr(xml, element), *end, *a, *v, *q, *out;
    size_t alen = strlen(attr), pre, vlen;
    if (!e) return NULL;
    end = strchr(e, '>');
    if (!end) return NULL;
    /* The attribute: its name after white space (attributes may be on lines of their own), then ="value". */
    for (a = e + strlen(element); a < end; a++)
        if ((a[-1] == ' ' || a[-1] == '\n' || a[-1] == '\r' || a[-1] == '\t') && !strncmp(a, attr, alen) && a[alen] == '='
            && a[alen + 1] == '"')
            break;
    if (a >= end) return NULL;
    v = a + alen + 2;
    q = strchr(v, '"');
    if (!q) return NULL;
    pre = (size_t)(v - xml);
    vlen = strlen(value);
    out = (char *)malloc(pre + vlen + strlen(q) + 1);
    if (!out) return NULL;
    memcpy(out, xml, pre);
    memcpy(out + pre, value, vlen);
    strcpy(out + pre + vlen, q);
    free(xml);
    return out;
}

/* Replaces the text between <tag> and </tag> (the first such element). */
static char *set_text(char *xml, const char *tag, const char *value)
{
    char open[64], close[64], *a, *b, *out;
    size_t pre, vlen;
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    a = strstr(xml, open);
    if (!a) return NULL;
    a += strlen(open);
    b = strstr(a, close);
    if (!b) return NULL;
    pre = (size_t)(a - xml);
    vlen = strlen(value);
    out = (char *)malloc(pre + vlen + strlen(b) + 1);
    if (!out) return NULL;
    memcpy(out, xml, pre);
    memcpy(out + pre, value, vlen);
    strcpy(out + pre + vlen, b);
    free(xml);
    return out;
}

/* The label escaped for an attribute or element text, cut to `max` characters; a UTF-8 sequence is copied whole or not at
   all, and a byte that begins none (or a control character) is a space. */
static void escaped(char *out, size_t outlen, const char *s, size_t max)
{
    size_t n = 0, chars = 0;
    while (*s && chars < max) {
        unsigned char c = (unsigned char)*s;
        size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, k, i;
        const char *r = NULL;
        char tmp[5];
        if (len == 1) {
            r = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : NULL;
            if (!r) {
                tmp[0] = c < 0x20 || c >= 0x80 ? ' ' : (char)c;
                tmp[1] = 0;
                r = tmp;
            }
        } else {
            for (i = 0; i < len && s[i]; i++) tmp[i] = s[i];
            if (i < len) break;
            tmp[len] = 0;
            r = tmp;
        }
        k = strlen(r);
        if (n + k + 1 > outlen) break;
        memcpy(out + n, r, k);
        n += k;
        s += len;
        chars++;
    }
    out[n] = 0;
}

char *mg_appx_app_manifest(const char *meglet, const char *android_package, const char *label, const char *tile_color,
                           const char *splash_color, char *err, size_t errlen)
{
    char name[64], guid[40], shown[1600], shortname[260];
    u64 h1, h2;
    char *xml;
    if (!mg_appx_app_identity(android_package, name, sizeof(name))) {
        if (err && errlen) snprintf(err, errlen, "\"%s\" is not an Android package name", android_package);
        return NULL;
    }
    h1 = fnv64(name, 0xcbf29ce484222325ull);
    h2 = fnv64(name, 0x84222325cbf29ce4ull);
    /* A version 5 (name-based) layout: version nibble 5, variant 10. */
    snprintf(guid, sizeof(guid), "%08x-%04x-5%03x-%04x-%012llx", (unsigned)(h1 >> 32), (unsigned)(h1 >> 16) & 0xffff,
             (unsigned)h1 & 0xfff, (unsigned)((h2 >> 48) & 0x3fff) | 0x8000, (unsigned long long)(h2 & 0xffffffffffffull));
    escaped(shown, sizeof(shown), label && label[0] ? label : android_package, 256);
    escaped(shortname, sizeof(shortname), label && label[0] ? label : android_package, 40);
    xml = (char *)malloc(strlen(meglet) + 1);
    if (!xml) return NULL;
    strcpy(xml, meglet);
#define STEP(call, what)                                                                                                 \
    do {                                                                                                                 \
        char *next = (call);                                                                                             \
        if (!next) {                                                                                                     \
            if (err && errlen) snprintf(err, errlen, "Meglet's manifest has no %s", what);                                \
            free(xml);                                                                                                   \
            return NULL;                                                                                                 \
        }                                                                                                                \
        xml = next;                                                                                                      \
    } while (0)
    STEP(set_attr(xml, "<Identity", "Name", name), "Identity Name");
    STEP(set_attr(xml, "<mp:PhoneIdentity", "PhoneProductId", guid), "PhoneProductId");
    STEP(set_text(xml, "DisplayName", shown), "DisplayName");
    STEP(set_attr(xml, "<uap:VisualElements", "DisplayName", shown), "VisualElements DisplayName");
    STEP(set_attr(xml, "<uap:DefaultTile", "ShortName", shortname), "the tile's ShortName");
    if (tile_color) STEP(set_attr(xml, "<uap:VisualElements", "BackgroundColor", tile_color), "VisualElements BackgroundColor");
    if (splash_color) STEP(set_attr(xml, "<uap:SplashScreen", "BackgroundColor", splash_color), "the splash screen's BackgroundColor");
#undef STEP
    return xml;
}
