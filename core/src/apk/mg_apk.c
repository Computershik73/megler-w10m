/*
 * Reading an APK: the binary AndroidManifest.xml, enough of resources.arsc to
 * turn "@string/app_name" into text and "@mipmap/ic_launcher" into a file, and
 * the native libraries' ELF headers. All of it is untrusted input; every
 * offset is checked against the buffer it came from.
 */
#include "mg_apk.h"

#include <stdio.h>

#include "../../include/megler.h"
#include "../bionic/mg_bionic.h"
#include "../drivers/mg_driver.h"
#include "../mg_core.h"
#include "../elf/mg_elf.h"
#include "../ndk/mg_egl.h"
#include "../zip/mg_zip.h"
#include "mg_bundle.h"
#include "mg_icon.h"
#include "mg_json.h"
#include "../webp/mg_webp.h"

/* -------------------------------------------------------- string pools */

typedef struct pool {
    const u8 *base;        /* the chunk */
    u32 size;
    u32 count, strings_start;
    bool utf8;
} pool;

static bool pool_open(pool *p, const u8 *chunk, u32 avail)
{
    memset(p, 0, sizeof(*p));
    if (avail < 28 || mg_rd16(chunk) != 0x0001) return false;
    p->base = chunk;
    p->size = mg_rd32(chunk + 4);
    if (p->size > avail || p->size < 28) return false;
    p->count = mg_rd32(chunk + 8);
    p->utf8 = (mg_rd32(chunk + 16) & 0x100) != 0;
    p->strings_start = mg_rd32(chunk + 20);
    if (p->count > (p->size - 28) / 4 || p->strings_start > p->size) return false;
    return true;
}

/* String `index` as UTF-8 into out (always terminated); false when there is no such string. */
static bool pool_get(const pool *p, u32 index, char *out, size_t cap)
{
    const u8 *s, *end;
    u32 off;
    size_t n = 0;
    if (cap) out[0] = 0;
    if (!p->base || index >= p->count || !cap) return false;
    off = mg_rd32(p->base + mg_rd16(p->base + 2) + index * 4);
    if (off >= p->size - p->strings_start) return false;
    s = p->base + p->strings_start + off;
    end = p->base + p->size;
    if (p->utf8) {
        u32 len;
        if (s + 2 > end) return false;
        if (*s++ & 0x80) s++;                       /* the UTF-16 length, one or two bytes: not needed */
        if (s >= end) return false;
        len = *s++;
        if (len & 0x80) {
            if (s >= end) return false;
            len = ((len & 0x7F) << 8) | *s++;
        }
        if (len > (u32)(end - s)) return false;
        if (len > cap - 1) len = (u32)cap - 1;
        memcpy(out, s, len);
        out[len] = 0;
        return true;
    } else {
        u32 len, i;
        if (s + 2 > end) return false;
        len = mg_rd16(s);
        s += 2;
        if (len & 0x8000) {
            if (s + 2 > end) return false;
            len = ((len & 0x7FFF) << 16) | mg_rd16(s);
            s += 2;
        }
        if (len > (u32)(end - s) / 2) return false;
        for (i = 0; i < len && n + 4 < cap; i++) {
            u32 c = mg_rd16(s + i * 2);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
                u32 lo = mg_rd16(s + i * 2 + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            if (c < 0x80) out[n++] = (char)c;
            else if (c < 0x800) {
                out[n++] = (char)(0xC0 | (c >> 6));
                out[n++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out[n++] = (char)(0xE0 | (c >> 12));
                out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            } else {
                out[n++] = (char)(0xF0 | (c >> 18));
                out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
                out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[n++] = (char)(0x80 | (c & 0x3F));
            }
        }
        out[n] = 0;
        return true;
    }
}

/* ---------------------------------------------------------- resources.arsc */

/*
 * The most of a resources.arsc read for an APK's label and icon (each read is freed when they are found). Google Play
 * services 26's is 88 MB, 60 MB of it its strings: at 64 MB it was not read, and the app had its package's name and no icon.
 */
#define ARSC_MAX (256u << 20)

typedef struct arsc {
    const u8 *data;
    u32 size;
    pool strings;
} arsc;

static bool arsc_open(arsc *a, const u8 *data, u32 size)
{
    memset(a, 0, sizeof(*a));
    if (!data || size < 12 || mg_rd16(data) != 0x0002) return false;
    a->data = data;
    a->size = mg_rd32(data + 4) < size ? mg_rd32(data + 4) : size;
    if (mg_rd16(data + 2) + 28u > a->size) return false;
    return pool_open(&a->strings, data + mg_rd16(data + 2), a->size - mg_rd16(data + 2));
}

/*
 * How well a configuration's language and region (ResTable_config's, at `cfg`, `size` bytes) fit the phone's
 * (mg_locale_parts): 3 both, 2 its language with no region named, 1 its language with another region, 0 none named, -1
 * another language. A language of three letters (packed) is taken for another.
 */
static int locale_fit(const u8 *cfg, u32 size)
{
    char language[8], region[8];
    if (size < 12 || (!cfg[8] && !cfg[9])) return 0;
    mg_locale_parts(language, sizeof(language), region, sizeof(region));
    if ((cfg[8] & 0x80) || cfg[8] != (u8)language[0] || cfg[9] != (u8)language[1] || language[2]) return -1;
    if (!cfg[10] && !cfg[11]) return 2;
    return !(cfg[10] & 0x80) && cfg[10] == (u8)region[0] && cfg[11] == (u8)region[1] && !region[2] ? 3 : 1;
}

static bool arsc_value(const arsc *a, u32 id, bool want_file, char *out, size_t cap, u32 *best, int depth);

/*
 * The value of resource `id` as text: a string resource's string, or a file
 * resource's path, through references to others of the same table (urent's
 * label is @string/app_name, which is @string/application_name). Among
 * configurations, `want_file` prefers the densest one whose value ends in
 * .png or .webp (an icon); otherwise the one of the phone's language and
 * region, as Android chooses (values-ru-rRU, then values-ru, then a ru of
 * another region), and failing that the default configuration. False when the
 * resource is not a string, or when nothing here is better than
 * `*best_score` already is: the score of what was found before, in another
 * table of the same app (a split's), which this raises to what it finds. NULL
 * starts from nothing.
 */
static bool arsc_string(const arsc *a, u32 id, bool want_file, char *out, size_t cap, u32 *best)
{
    return arsc_value(a, id, want_file, out, cap, best, 0);
}

static bool arsc_value(const arsc *a, u32 id, bool want_file, char *out, size_t cap, u32 *best, int depth)
{
    u32 pkg_id = id >> 24, type_id = (id >> 16) & 0xFF, entry = id & 0xFFFF;
    u32 off, best_score = best ? *best : 0;
    bool found = false;
    if (!a->data) return false;
    off = mg_rd16(a->data + 2) + a->strings.size;
    while (off + 8 <= a->size) {
        const u8 *pk = a->data + off;
        u32 psize = mg_rd32(pk + 4), phead = mg_rd16(pk + 2), at;
        if (psize < 8 || psize > a->size - off) break;
        if (mg_rd16(pk) == 0x0200 && phead >= 284 && mg_rd32(pk + 8) == pkg_id) {
            for (at = phead; at + 8 <= psize;) {
                const u8 *ch = pk + at;
                u32 csize = mg_rd32(ch + 4), chead = mg_rd16(ch + 2);
                if (csize < 8 || csize > psize - at) break;
                if (mg_rd16(ch) == 0x0201 && chead >= 20 && ch[8] == type_id) {
                    u32 count = mg_rd32(ch + 12), start = mg_rd32(ch + 16), eoff = 0xFFFFFFFFu, flags = ch[9];
                    u32 cfg_size = chead >= 24 ? mg_rd32(ch + 20) : 0, density = cfg_size >= 16 && chead >= 36 ? mg_rd16(ch + 20 + 14) : 0;
                    bool is_default = true;
                    u32 k;
                    /* The default configuration is all zeros after its own size field. */
                    for (k = 4; k < cfg_size && 20 + k < chead; k++)
                        if (ch[20 + k]) is_default = false;
                    if (flags & 1) {
                        /* Sparse: (index, offset / 4) pairs of 16 bits each. */
                        for (k = 0; k < count && chead + k * 4 + 4 <= csize; k++)
                            if (mg_rd16(ch + chead + k * 4) == entry) eoff = (u32)mg_rd16(ch + chead + k * 4 + 2) * 4;
                    } else if (flags & 2) {
                        if (entry < count && chead + entry * 2 + 2 <= csize) {
                            u32 v = mg_rd16(ch + chead + entry * 2);
                            eoff = v == 0xFFFF ? 0xFFFFFFFFu : v * 4;
                        }
                    } else if (entry < count && chead + entry * 4 + 4 <= csize) {
                        eoff = mg_rd32(ch + chead + entry * 4);
                    }
                    if (eoff != 0xFFFFFFFFu && start <= csize && eoff <= csize - start && csize - start - eoff >= 16) {
                        const u8 *e = ch + start + eoff;
                        u32 esize = mg_rd16(e), eflags = mg_rd16(e + 2);
                        u8 vtype = !(eflags & 1) && esize >= 8 && esize + 8 <= csize - start - eoff ? e[esize + 3] : 0;
                        if (vtype == 0x03 || vtype == 0x01) {
                            char text[200];
                            bool got;
                            /* A reference: the value it names, chosen among that one's configurations (four deep at most). */
                            if (vtype == 0x03) got = pool_get(&a->strings, mg_rd32(e + esize + 4), text, sizeof(text));
                            else got = depth < 4 && arsc_value(a, mg_rd32(e + esize + 4), want_file, text, sizeof(text), NULL, depth + 1);
                            if (got) {
                                size_t n = strlen(text);
                                bool png = n > 4 && !strcmp(text + n - 4, ".png"), webp = n > 5 && !strcmp(text + n - 5, ".webp");
                                u32 score;
                                int fit = want_file ? 0 : locale_fit(ch + 20, cfg_size < chead - 20 ? cfg_size : chead - 20);
                                /* Any PNG before any WebP (the phone has no WebP decoder), then the densest. */
                                if (want_file) score = png || webp ? (png ? 100000 : 1000) + (density == 0xFFFE || density == 0xFFFF ? 0 : density) : 1;
                                else score = fit > 0 ? 1500 + 500 * (u32)fit : is_default ? 1000 : 1;
                                if (score > best_score) {
                                    best_score = score;
                                    mg_strlcpy(out, text, cap);
                                    found = true;
                                }
                            }
                        }
                    }
                }
                at += csize;
            }
        }
        off += psize;
    }
    if (best) *best = best_score;
    return found;
}

/*
 * Resource `id`'s value as it is (Res_value's type and data; a string's text), for drawing an icon (mg_icon.c): of the
 * configurations that differ from the default in density or API level alone (not a language's, not night's), the one
 * Android 8 would take for a drawable -- anydpi, then nodpi, then the densest, then the default. A reference comes back
 * as one. False when none is better than `*best` (raised to what this finds), as arsc_value's is.
 */
static bool arsc_raw(const arsc *a, u32 id, u8 *type, u32 *data, char *text, size_t cap, u32 *best)
{
    u32 pkg_id = id >> 24, type_id = (id >> 16) & 0xFF, entry = id & 0xFFFF;
    u32 off, best_score = best ? *best : 0;
    bool found = false;
    if (!a->data) return false;
    off = mg_rd16(a->data + 2) + a->strings.size;
    while (off + 8 <= a->size) {
        const u8 *pk = a->data + off;
        u32 psize = mg_rd32(pk + 4), phead = mg_rd16(pk + 2), at;
        if (psize < 8 || psize > a->size - off) break;
        if (mg_rd16(pk) == 0x0200 && phead >= 284 && mg_rd32(pk + 8) == pkg_id) {
            for (at = phead; at + 8 <= psize;) {
                const u8 *ch = pk + at;
                u32 csize = mg_rd32(ch + 4), chead = mg_rd16(ch + 2);
                if (csize < 8 || csize > psize - at) break;
                if (mg_rd16(ch) == 0x0201 && chead >= 20 && ch[8] == type_id) {
                    u32 count = mg_rd32(ch + 12), start = mg_rd32(ch + 16), eoff = 0xFFFFFFFFu, flags = ch[9];
                    u32 cfg_size = chead >= 24 ? mg_rd32(ch + 20) : 0, density = cfg_size >= 16 && chead >= 36 ? mg_rd16(ch + 20 + 14) : 0;
                    bool plain = true;
                    u32 k, score;
                    /* Plain: nothing set but the density (bytes 14-15) and the API level (24-27). */
                    for (k = 4; k < cfg_size && 20 + k < chead; k++)
                        if (ch[20 + k] && !(k == 14 || k == 15 || (k >= 24 && k <= 27))) plain = false;
                    if (!plain) {
                        at += csize;
                        continue;
                    }
                    score = 10000 + (density == 0xFFFE ? 9000 : density == 0xFFFF ? 8000 : density);
                    if (flags & 1) {
                        for (k = 0; k < count && chead + k * 4 + 4 <= csize; k++)
                            if (mg_rd16(ch + chead + k * 4) == entry) eoff = (u32)mg_rd16(ch + chead + k * 4 + 2) * 4;
                    } else if (flags & 2) {
                        if (entry < count && chead + entry * 2 + 2 <= csize) {
                            u32 v = mg_rd16(ch + chead + entry * 2);
                            eoff = v == 0xFFFF ? 0xFFFFFFFFu : v * 4;
                        }
                    } else if (entry < count && chead + entry * 4 + 4 <= csize) {
                        eoff = mg_rd32(ch + chead + entry * 4);
                    }
                    if (score > best_score && eoff != 0xFFFFFFFFu && start <= csize && eoff <= csize - start && csize - start - eoff >= 16) {
                        const u8 *e = ch + start + eoff;
                        u32 esize = mg_rd16(e), eflags = mg_rd16(e + 2);
                        if (!(eflags & 1) && esize >= 8 && esize + 8 <= csize - start - eoff) {
                            *type = e[esize + 3];
                            *data = mg_rd32(e + esize + 4);
                            if (cap) text[0] = 0;
                            if (*type == 0x03) pool_get(&a->strings, *data, text, cap);
                            best_score = score;
                            found = true;
                        }
                    }
                }
                at += csize;
            }
        }
        off += psize;
    }
    if (best) *best = best_score;
    return found;
}

/* ----------------------------------------------------- AndroidManifest.xml */

/* The attributes this layer reads, by their resource ids (names can be stripped from the string pool; the ids cannot). */
#define ATTR_LABEL 0x01010001u
#define ATTR_ICON 0x01010002u
#define ATTR_NAME 0x01010003u
#define ATTR_SHARED_USER_ID 0x0101000bu
#define ATTR_HAS_CODE 0x0101000cu
#define ATTR_SCREEN_ORIENTATION 0x0101001eu
#define ATTR_VALUE 0x01010024u
#define ATTR_TARGET_ACTIVITY 0x01010202u
#define ATTR_ENABLED 0x0101000eu
#define ATTR_MIN_SDK 0x0101020cu
#define ATTR_VERSION_CODE 0x0101021bu
#define ATTR_VERSION_NAME 0x0101021cu
#define ATTR_TARGET_SDK 0x01010270u
#define ATTR_IS_FEATURE_SPLIT 0x0101055bu
#define ATTR_IS_SPLIT_REQUIRED 0x01010591u
#define ATTR_REQUIRED_SPLIT_TYPES 0x0101064eu

typedef struct attr {
    u32 id;                /* 0 when the attribute has no resource id */
    char name[48];
    u8 type;
    u32 data;
    char text[200];        /* a string value */
} attr;

typedef struct manifest_ctx {
    mg_apk_info *info;
    const arsc *res;
    /* where the parser is */
    bool in_application, in_activity, in_filter;
    char cur_activity[200], cur_lib[96], cur_func[96];
    bool cur_main, cur_launcher, found_launcher;
    bool cur_disabled;                 /* android:enabled="false": the launcher does not show it, so it is not how the app starts */
    char disabled_activity[200];       /* ...unless every launcher entry is: then the first of them is taken after all */
    int disabled_orientation;
    int cur_orientation;
    /* How each activity seen so far asks to be held, for an alias that comes after its target and has no say of its own. */
    struct {
        u32 name_hash;
        int orientation;
    } seen[96];
    int nseen;
    mg_apk_meta act_meta[16];
    int act_nmeta;
    u32 icon_ref, label_ref;
} manifest_ctx;

static void full_class_name(const mg_apk_info *info, const char *name, char *out, size_t cap)
{
    if (name[0] == '.') snprintf(out, cap, "%s%s", info->package, name);
    else if (!strchr(name, '.') && name[0]) snprintf(out, cap, "%s.%s", info->package, name);
    else mg_strlcpy(out, name, cap);
}

static u32 name_hash(const char *s)
{
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (u8)*s) * 16777619u;
    return h;
}

/*
 * android:screenOrientation, of ActivityInfo's sixteen values, as one of three: the four that are landscape whichever way
 * up (landscape, sensorLandscape, reverseLandscape, userLandscape), the four that are portrait, and everything else --
 * unspecified, user, behind, sensor, nosensor, the full ones, locked -- which leaves it to the device.
 */
static int orientation_of(i32 value)
{
    switch (value) {
    case 0: case 6: case 8: case 11: return MG_APK_LANDSCAPE;
    case 1: case 7: case 9: case 12: return MG_APK_PORTRAIT;
    default: return MG_APK_UNSPECIFIED;
    }
}

static void add_meta(mg_apk_meta *list, int *n, int cap, const char *name, const char *value)
{
    if (*n >= cap || !name[0]) return;
    mg_strlcpy(list[*n].name, name, sizeof(list[*n].name));
    mg_strlcpy(list[*n].value, value, sizeof(list[*n].value));
    ++*n;
}

static void start_element(manifest_ctx *c, const char *tag, const attr *attrs, int nattrs)
{
    mg_apk_info *info = c->info;
    const attr *name = NULL, *value = NULL;
    int i;
    for (i = 0; i < nattrs; i++) {
        if (attrs[i].id == ATTR_NAME || (!attrs[i].id && !strcmp(attrs[i].name, "name"))) name = &attrs[i];
        if (attrs[i].id == ATTR_VALUE || (!attrs[i].id && !strcmp(attrs[i].name, "value"))) value = &attrs[i];
    }
    if (!strcmp(tag, "manifest")) {
        for (i = 0; i < nattrs; i++) {
            if (!attrs[i].id && !strcmp(attrs[i].name, "package")) mg_strlcpy(info->package, attrs[i].text, sizeof(info->package));
            if (attrs[i].id == ATTR_VERSION_CODE) info->version_code = (int)attrs[i].data;
            if (attrs[i].id == ATTR_VERSION_NAME) mg_strlcpy(info->version_name, attrs[i].text, sizeof(info->version_name));
            if (attrs[i].id == ATTR_SHARED_USER_ID) mg_strlcpy(info->shared_user_id, attrs[i].text, sizeof(info->shared_user_id));
            /* A split's own name and the feature it is of, which have no resource ids; and what a base says of its splits. */
            if (!attrs[i].id && !strcmp(attrs[i].name, "split")) mg_strlcpy(info->split, attrs[i].text, sizeof(info->split));
            if (!attrs[i].id && !strcmp(attrs[i].name, "configForSplit")) mg_strlcpy(info->config_for_split, attrs[i].text, sizeof(info->config_for_split));
            if (attrs[i].id == ATTR_IS_FEATURE_SPLIT && attrs[i].type == 0x12) info->feature_split = attrs[i].data != 0;
            if (attrs[i].id == ATTR_IS_SPLIT_REQUIRED && attrs[i].type == 0x12 && attrs[i].data) info->split_required = true;
            if (attrs[i].id == ATTR_REQUIRED_SPLIT_TYPES && attrs[i].text[0]) {
                mg_strlcpy(info->required_split_types, attrs[i].text, sizeof(info->required_split_types));
                info->split_required = true;
            }
        }
    } else if (!strcmp(tag, "uses-sdk")) {
        for (i = 0; i < nattrs; i++) {
            if (attrs[i].id == ATTR_MIN_SDK) info->min_sdk = (int)attrs[i].data;
            if (attrs[i].id == ATTR_TARGET_SDK) info->target_sdk = (int)attrs[i].data;
        }
    } else if (!strcmp(tag, "application")) {
        c->in_application = true;
        for (i = 0; i < nattrs; i++) {
            if (attrs[i].id == ATTR_HAS_CODE) info->has_code = attrs[i].data != 0;
            if (attrs[i].id == ATTR_NAME) full_class_name(info, attrs[i].text, info->application, sizeof(info->application));
            if (attrs[i].id == ATTR_LABEL) {
                if (attrs[i].type == 0x03) mg_strlcpy(info->label, attrs[i].text, sizeof(info->label));
                else if (attrs[i].type == 0x01) c->label_ref = attrs[i].data;
            }
            if (attrs[i].id == ATTR_ICON && attrs[i].type == 0x01) c->icon_ref = attrs[i].data;
        }
    } else if (c->in_application && (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias"))) {
        c->in_activity = true;
        c->cur_main = c->cur_launcher = false;
        c->cur_lib[0] = c->cur_func[0] = 0;
        c->act_nmeta = 0;
        c->cur_activity[0] = 0;
        c->cur_orientation = MG_APK_UNSPECIFIED;
        c->cur_disabled = false;
        if (name) full_class_name(info, name->text, c->cur_activity, sizeof(c->cur_activity));
        for (i = 0; i < nattrs; i++) {
            if (attrs[i].id == ATTR_SCREEN_ORIENTATION) c->cur_orientation = orientation_of((i32)attrs[i].data);
            if (attrs[i].id == ATTR_ENABLED && attrs[i].type == 0x12) c->cur_disabled = attrs[i].data == 0;
            if (attrs[i].id == ATTR_TARGET_ACTIVITY && !strcmp(tag, "activity-alias")) {
                /*
                 * An alias is another name for an activity, with intent filters of its own: an app with a choice of
                 * launcher icons has one for each, all but one disabled. No class has the alias's name. What is started
                 * is the activity it stands for, held as that one is.
                 */
                char target[200];
                u32 h;
                int k;
                full_class_name(info, attrs[i].text, target, sizeof(target));
                h = name_hash(target);
                for (k = 0; k < c->nseen; k++)
                    if (c->seen[k].name_hash == h) c->cur_orientation = c->seen[k].orientation;
                mg_strlcpy(c->cur_activity, target, sizeof(c->cur_activity));
            }
        }
        if (!strcmp(tag, "activity") && c->nseen < (int)MG_COUNT(c->seen)) {
            c->seen[c->nseen].name_hash = name_hash(c->cur_activity);
            c->seen[c->nseen++].orientation = c->cur_orientation;
        }
    } else if (c->in_application && !c->in_activity && !strcmp(tag, "uses-library") && name) {
        if (info->nuses_libraries < (int)MG_COUNT(info->uses_libraries))
            mg_strlcpy(info->uses_libraries[info->nuses_libraries++], name->text, sizeof(info->uses_libraries[0]));
    } else if (!strcmp(tag, "intent-filter")) {
        c->in_filter = true;
    } else if (c->in_filter && c->in_activity && name) {
        if (!strcmp(tag, "action") && !strcmp(name->text, "android.intent.action.MAIN")) c->cur_main = true;
        if (!strcmp(tag, "category") && (!strcmp(name->text, "android.intent.category.LAUNCHER") || !strcmp(name->text, "android.intent.category.LEANBACK_LAUNCHER")))
            c->cur_launcher = true;
    } else if (!strcmp(tag, "meta-data") && name) {
        char text[200];
        if (value && value->type == 0x03) mg_strlcpy(text, value->text, sizeof(text));
        else if (value && value->type == 0x12) mg_strlcpy(text, value->data ? "true" : "false", sizeof(text));
        else if (value) snprintf(text, sizeof(text), "%d", (int)value->data);
        else text[0] = 0;
        if (c->in_activity) {
            if (!strcmp(name->text, "android.app.lib_name")) mg_strlcpy(c->cur_lib, text, sizeof(c->cur_lib));
            if (!strcmp(name->text, "android.app.func_name")) mg_strlcpy(c->cur_func, text, sizeof(c->cur_func));
            add_meta(c->act_meta, &c->act_nmeta, (int)MG_COUNT(c->act_meta), name->text, text);
        } else if (c->in_application) {
            add_meta(info->meta, &info->nmeta, (int)MG_COUNT(info->meta), name->text, text);
            /* What Google Play's own library checks at an app's start (its "missing splits" check): the base is not whole alone. */
            if (!strcmp(name->text, "com.android.vending.splits.required") && !strcmp(text, "true")) info->split_required = true;
        }
    }
}

static void end_element(manifest_ctx *c, const char *tag)
{
    mg_apk_info *info = c->info;
    int i;
    if (!strcmp(tag, "intent-filter")) c->in_filter = false;
    else if (!strcmp(tag, "application")) {
        c->in_application = false;
        if (!info->activity[0] && c->disabled_activity[0]) {
            mg_strlcpy(info->activity, c->disabled_activity, sizeof(info->activity));
            info->orientation = c->disabled_orientation;
        }
    } else if (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias")) {
        if (c->cur_main && c->cur_launcher && c->cur_disabled && !c->disabled_activity[0]) {
            mg_strlcpy(c->disabled_activity, c->cur_activity, sizeof(c->disabled_activity));
            c->disabled_orientation = c->cur_orientation;
        }
        /* The first activity that is both MAIN and LAUNCHER is the app; failing that, the first MAIN one. */
        if (c->cur_main && !c->cur_disabled && (!c->found_launcher) && (c->cur_launcher || !info->activity[0])) {
            mg_strlcpy(info->activity, c->cur_activity, sizeof(info->activity));
            info->orientation = c->cur_orientation;
            mg_strlcpy(info->native_library, c->cur_lib, sizeof(info->native_library));
            mg_strlcpy(info->func_name, c->cur_func, sizeof(info->func_name));
            for (i = 0; i < c->act_nmeta; i++) add_meta(info->meta, &info->nmeta, (int)MG_COUNT(info->meta), c->act_meta[i].name, c->act_meta[i].value);
            if (c->cur_launcher) c->found_launcher = true;
        }
        c->in_activity = false;
    }
}

/* The elements of a binary XML in their order: `start` with each one's tag and its attributes (the first 24), `end` with its tag. */
typedef struct xml_walker {
    void (*start)(void *user, const char *tag, const attr *attrs, int nattrs);
    void (*end)(void *user, const char *tag);
    void *user;
} xml_walker;

static bool xml_walk(const u8 *xml, u32 size, const xml_walker *w)
{
    pool strings;
    const u8 *resmap = NULL;
    u32 resmap_count = 0, off;
    memset(&strings, 0, sizeof(strings));
    if (size < 8 || mg_rd16(xml) != 0x0003) return false;
    off = mg_rd16(xml + 2);
    while (off + 8 <= size) {
        const u8 *ch = xml + off;
        u32 type = mg_rd16(ch), head = mg_rd16(ch + 2), csize = mg_rd32(ch + 4);
        if (csize < 8 || csize > size - off || head > csize) break;
        if (type == 0x0001) {
            pool_open(&strings, ch, csize);
        } else if (type == 0x0180) {
            resmap = ch + head;
            resmap_count = (csize - head) / 4;
        } else if (type == 0x0102 && csize >= 36) {
            char tag[64];
            attr attrs[24];
            u32 astart = mg_rd16(ch + 24), asize = mg_rd16(ch + 26), acount = mg_rd16(ch + 28), i;
            int n = 0;
            pool_get(&strings, mg_rd32(ch + 20), tag, sizeof(tag));
            for (i = 0; i < acount && n < (int)MG_COUNT(attrs) && asize >= 20; i++) {
                const u8 *a = ch + 16 + astart + i * asize;
                u32 name_index;
                attr *out = &attrs[n];
                if (a + 20 > ch + csize) break;
                name_index = mg_rd32(a + 4);
                memset(out, 0, sizeof(*out));
                pool_get(&strings, name_index, out->name, sizeof(out->name));
                if (name_index < resmap_count) out->id = mg_rd32(resmap + name_index * 4);
                out->type = a[15];
                out->data = mg_rd32(a + 16);
                if (out->type == 0x03) pool_get(&strings, out->data, out->text, sizeof(out->text));
                else if (mg_rd32(a + 8) != 0xFFFFFFFFu) pool_get(&strings, mg_rd32(a + 8), out->text, sizeof(out->text));
                n++;
            }
            w->start(w->user, tag, attrs, n);
        } else if (type == 0x0103 && csize >= 24) {
            char tag[64];
            pool_get(&strings, mg_rd32(ch + 20), tag, sizeof(tag));
            w->end(w->user, tag);
        }
        off += csize;
    }
    return true;
}

static void manifest_start(void *user, const char *tag, const attr *attrs, int nattrs) { start_element((manifest_ctx *)user, tag, attrs, nattrs); }
static void manifest_end(void *user, const char *tag) { end_element((manifest_ctx *)user, tag); }

static bool parse_manifest(mg_apk_info *info, const arsc *res, const u8 *xml, u32 size)
{
    manifest_ctx c;
    xml_walker w;
    memset(&c, 0, sizeof(c));
    c.info = info;
    c.res = res;
    info->has_code = true;
    w.start = manifest_start;
    w.end = manifest_end;
    w.user = &c;
    if (!xml_walk(xml, size, &w)) return false;
    if (!info->label[0] && c.label_ref) arsc_string(res, c.label_ref, false, info->label, sizeof(info->label), NULL);
    info->icon_id = c.icon_ref;
    info->label_id = c.label_ref;
    return info->package[0] != 0;
}

/* ------------------------------------------------------------- libraries */

static void note(mg_apk_info *info, const char *fmt, ...)
{
    va_list ap;
    if (info->nnotes >= (int)MG_COUNT(info->notes)) return;
    va_start(ap, fmt);
    vsnprintf(info->notes[info->nnotes++], sizeof(info->notes[0]), fmt, ap);
    va_end(ap);
}

typedef struct lib_walk {
    const mg_zip *zip;
    mg_apk_info *info;
    const char *prefix;
    bool has[16];
    char first_lib[96];
    int count;
} lib_walk;

enum { L_UNITY, L_UE4, L_COCOS, L_GODOT, L_SDL, L_MAIN, L_GDX, L_FLUTTER, L_MONO, L_IL2CPP, L_DEXPROTECTOR, L_VULKAN_ONLY };

static void lib_seen(const mg_zip_entry *e, void *user)
{
    lib_walk *w = (lib_walk *)user;
    const char *name = e->name + strlen(w->prefix);
    if (strchr(name, '/') || strlen(name) < 4 || strcmp(name + strlen(name) - 3, ".so")) return;
    w->count++;
    if (!w->first_lib[0]) mg_strlcpy(w->first_lib, name, sizeof(w->first_lib));
    if (!strcmp(name, "libunity.so")) w->has[L_UNITY] = true;
    if (!strcmp(name, "libil2cpp.so")) w->has[L_IL2CPP] = true;
    if (!strcmp(name, "libmono.so") || !strcmp(name, "libmonobdwgc-2.0.so") || !strcmp(name, "libmonosgen-2.0.so")) w->has[L_MONO] = true;
    if (!strcmp(name, "libUE4.so") || !strcmp(name, "libUnreal.so")) w->has[L_UE4] = true;
    if (!strncmp(name, "libcocos2d", 10) || !strcmp(name, "libgame.so") || !strcmp(name, "libMyGame.so")) w->has[L_COCOS] = true;
    if (!strcmp(name, "libgodot_android.so")) w->has[L_GODOT] = true;
    if (!strcmp(name, "libSDL2.so") || !strcmp(name, "libSDL3.so")) w->has[L_SDL] = true;
    if (!strcmp(name, "libmain.so") || !strcmp(name, "libnative-activity.so")) w->has[L_MAIN] = true;
    if (!strcmp(name, "libgdx.so")) w->has[L_GDX] = true;
    if (!strcmp(name, "libflutter.so")) w->has[L_FLUTTER] = true;
    if (!strcmp(name, "libdexprotector.so")) w->has[L_DEXPROTECTOR] = true;
}

static void abi_seen(const mg_zip_entry *e, void *user)
{
    mg_apk_info *info = (mg_apk_info *)user;
    const char *abi = e->name + 4, *slash = strchr(abi, '/');
    char name[24];
    int i;
    if (!slash || (size_t)(slash - abi) >= sizeof(name) || strlen(slash) < 5 || strcmp(slash + strlen(slash) - 3, ".so")) return;
    memcpy(name, abi, (size_t)(slash - abi));
    name[slash - abi] = 0;
    for (i = 0; i < info->nabis; i++)
        if (!strcmp(info->abis[i], name)) return;
    if (info->nabis < (int)MG_COUNT(info->abis)) mg_strlcpy(info->abis[info->nabis++], name, sizeof(info->abis[0]));
}

static void abi_add(mg_apk_info *info, const char *name)
{
    int i;
    for (i = 0; i < info->nabis; i++)
        if (!strcmp(info->abis[i], name)) return;
    if (info->nabis < (int)MG_COUNT(info->abis)) mg_strlcpy(info->abis[info->nabis++], name, sizeof(info->abis[0]));
}

/*
 * The ELF of the libraries an app's APKs are looked at for (MG_APK_ABI_ARM): 32-bit ARM's on the phone and in the builds
 * that run no guest code, the processor's own elsewhere -- ELF64 on x86_64 and ARM64.
 */
#if MG_APK_ABI_ARM
typedef Elf32_Ehdr A_Ehdr;
typedef Elf32_Phdr A_Phdr;
typedef Elf32_Dyn A_Dyn;
typedef Elf32_Sym A_Sym;
#define A_CLASS 1
#define A_MACHINE EM_ARM
#else
typedef ElfW(Ehdr) A_Ehdr;
typedef ElfW(Phdr) A_Phdr;
typedef ElfW(Dyn) A_Dyn;
typedef ElfW(Sym) A_Sym;
#define A_CLASS MG_ELFCLASS
#define A_MACHINE MG_EM
#endif
/* (A word of the GNU hash table's bloom filter is the class's: 8 bytes in ELF64.) */
#define A_BLOOM_WORD (A_CLASS == 2 ? 8u : 4u)

static const u8 *elf_at(const u8 *d, size_t n, u64 vaddr, u64 need);

/* How many dynamic symbols: the SysV hash table says outright; the GNU one by its last chain. 0 when neither says. */
static u32 elf_symbol_count(const u8 *d, size_t n, u32 hash, u32 gnu)
{
    u32 nsyms = 0;
    if (hash) {
        const u8 *h = elf_at(d, n, hash, 8);
        if (h) nsyms = mg_rd32(h + 4);
    }
    if (!nsyms && gnu) {
        const u8 *g = elf_at(d, n, gnu, 16);
        if (g) {
            u32 nb = mg_rd32(g), symoff = mg_rd32(g + 4), bloom = mg_rd32(g + 8), top = 0, b;
            const u8 *buckets = nb < 0x1000000 && bloom < 0x1000000 ? elf_at(d, n, gnu + 16 + bloom * A_BLOOM_WORD, nb * 4) : NULL;
            if (buckets) {
                for (b = 0; b < nb; b++)
                    if (mg_rd32(buckets + b * 4) > top) top = mg_rd32(buckets + b * 4);
                nsyms = symoff;
                if (top >= symoff) {
                    for (;; top++) {
                        const u8 *chain = elf_at(d, n, gnu + 16 + bloom * A_BLOOM_WORD + nb * 4 + (top - symoff) * 4, 4);
                        if (!chain) break;
                        if (mg_rd32(chain) & 1) {
                            nsyms = top + 1;
                            break;
                        }
                    }
                }
            }
        }
    }
    return nsyms;
}

/*
 * Whether the library android.app.lib_name names exports NativeActivity's entry -- android.app.func_name, else
 * ANativeActivity_onCreate --, looked for in the dynamic symbols of its first 32 MB, where an ELF file keeps them: 1 yes,
 * 0 no, -1 not known (no such library, not ARM's ELF, its tables past what was read).
 */
static int native_entry(mg_zip *const *zips, int nzips, const mg_apk_info *info)
{
    char path[160];
    const char *want = info->func_name[0] ? info->func_name : "ANativeActivity_onCreate";
    int k, found = -1;
    snprintf(path, sizeof(path), "lib/%s/lib%s.so", info->abi, info->native_library);
    for (k = 0; k < nzips && found < 0; k++) {
        const mg_zip_entry *e = mg_zip_find(zips[k], path);
        size_t n = 0;
        u8 *d = e ? mg_zip_read_head(zips[k], e, 32u << 20, &n) : NULL;
        const A_Ehdr *eh = (const A_Ehdr *)d;
        if (d && n > sizeof(A_Ehdr) && !memcmp(d, "\177ELF", 4) && d[4] == A_CLASS && eh->e_machine == A_MACHINE &&
            eh->e_phentsize == sizeof(A_Phdr) && eh->e_phoff <= n && (size_t)eh->e_phnum * sizeof(A_Phdr) <= n - eh->e_phoff) {
            const A_Phdr *ph = (const A_Phdr *)(d + eh->e_phoff);
            const A_Dyn *dyn = NULL;
            u32 ndyn = 0, symtab = 0, strtab = 0, strsz = 0, hash = 0, gnu = 0, nsyms, j;
            int i;
            for (i = 0; i < eh->e_phnum; i++)
                if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset <= n && ph[i].p_filesz <= n - ph[i].p_offset) {
                    dyn = (const A_Dyn *)(d + ph[i].p_offset);
                    ndyn = (u32)(ph[i].p_filesz / sizeof(A_Dyn));
                }
            for (j = 0; j < ndyn && dyn[j].d_tag != DT_NULL; j++) {
                if (dyn[j].d_tag == DT_SYMTAB) symtab = (u32)dyn[j].d_val;
                if (dyn[j].d_tag == DT_STRTAB) strtab = (u32)dyn[j].d_val;
                if (dyn[j].d_tag == DT_STRSZ) strsz = (u32)dyn[j].d_val;
                if (dyn[j].d_tag == DT_HASH) hash = (u32)dyn[j].d_val;
                if (dyn[j].d_tag == DT_GNU_HASH) gnu = (u32)dyn[j].d_val;
            }
            nsyms = strtab && strsz ? elf_symbol_count(d, n, hash, gnu) : 0;
            if (symtab && nsyms && nsyms < 4000000) {
                const char *str = (const char *)elf_at(d, n, strtab, strsz);
                const A_Sym *sym = (const A_Sym *)elf_at(d, n, symtab, nsyms * sizeof(A_Sym));
                if (str && sym && str[strsz - 1] == 0) {
                    found = 0;
                    for (j = 1; j < nsyms && !found; j++)
                        if (sym[j].st_name < strsz && sym[j].st_shndx != SHN_UNDEF && !strcmp(str + sym[j].st_name, want)) found = 1;
                }
            }
        }
        free(d);
    }
    return found;
}

/*
 * What the app is and whether it runs, from its manifest and the native libraries of all its APKs (zips[0] the base, the
 * rest its splits). `bundle_abis` are the ABIs of the splits that the unpacking of a bundle left out: they are the app's,
 * and say what it has besides what is here, but are never the one used.
 */
static void classify(mg_apk_info *info, mg_zip *const *zips, int nzips, char (*bundle_abis)[24], int nbundle_abis)
{
    lib_walk w;
    char prefix[40], others[120];
    int i, k;
    bool v7 = false, v5 = false, any64 = false, in_parts = info->nsplits > 0 || info->nleft_out > 0, abi_split_missing;
    for (k = 0; k < nzips; k++) mg_zip_each(zips[k], "lib/", abi_seen, info);
    /* (The processor's own ABI elsewhere than on 32-bit ARM: MG_APK_ABI_ARM.) */
    for (i = 0; i < info->nabis; i++) {
#if MG_APK_ABI_ARM
        if (!strcmp(info->abis[i], "armeabi-v7a")) v7 = true;
        else if (!strcmp(info->abis[i], "armeabi")) v5 = true;
#else
        if (!strcmp(info->abis[i], MG_GUEST_ABI)) v7 = true;
#endif
    }
#if MG_APK_ABI_ARM
    if (v7) strcpy(info->abi, "armeabi-v7a");
    else if (v5) strcpy(info->abi, "armeabi");
#else
    if (v7) strcpy(info->abi, MG_GUEST_ABI);
    (void)v5;
#endif
    for (i = 0; i < nbundle_abis; i++) abi_add(info, bundle_abis[i]);
    others[0] = 0;
    for (i = 0; i < info->nabis; i++) {
        if (!strcmp(info->abis[i], "arm64-v8a")) any64 = true;
#if MG_APK_ABI_ARM
        if (strcmp(info->abis[i], "armeabi-v7a") && strcmp(info->abis[i], "armeabi") && strlen(others) + strlen(info->abis[i]) + 3 < sizeof(others))
#else
        if (strcmp(info->abis[i], MG_GUEST_ABI) && strlen(others) + strlen(info->abis[i]) + 3 < sizeof(others))
#endif
            snprintf(others + strlen(others), sizeof(others) - strlen(others), "%s%s", others[0] ? ", " : "", info->abis[i]);
    }

    memset(&w, 0, sizeof(w));
    w.info = info;
    if (info->abi[0]) {
        snprintf(prefix, sizeof(prefix), "lib/%s/", info->abi);
        w.prefix = prefix;
        for (k = 0; k < nzips; k++) mg_zip_each(zips[k], prefix, lib_seen, &w);
    }

    if (w.has[L_UNITY]) strcpy(info->engine, "unity");
    else if (w.has[L_UE4]) strcpy(info->engine, "unreal");
    else if (w.has[L_GODOT]) strcpy(info->engine, "godot");
    /*
     * cocos2d-x in an app with bytecode and an activity of its own is a Java app's. cocos2d-x's Cocos2dxActivity is an
     * Activity with a GLSurfaceView that loads the engine (libcocos2dcpp.so) through JNI and draws with OpenGL ES, as
     * libGDX's does, and an app may carry the engine for a part of it: Zalo (com.zing.zalo), a messenger with 44 libraries,
     * libcocos2dcpp.so among them, was refused as an app "built on cocos2d-x", for which there is no driver. It goes on
     * below as the others do (NativeActivity, then Java). The engine with nothing to start it is still one.
     */
    else if (w.has[L_COCOS] && !(info->activity[0] && mg_zip_find(zips[0], "classes.dex"))) strcpy(info->engine, "cocos2d-x");
    /*
     * A Flutter app is a Java app: its activity (FlutterActivity or the app's own on top of it) starts Flutter's engine,
     * libflutter.so, through JNI, and the engine runs the app's Dart code (libapp.so) and draws into the activity's
     * SurfaceView. The Java driver runs it as any other. Without bytecode and an activity there is nothing to start it.
     */
    else if (w.has[L_FLUTTER] && info->activity[0] && mg_zip_find(zips[0], "classes.dex")) strcpy(info->engine, "java");
    else if (w.has[L_FLUTTER]) strcpy(info->engine, "flutter");
    /*
     * android.app.lib_name on an activity of the app's own is a subclass of NativeActivity's -- unless the library has no
     * NativeActivity entry: then the activity is one that loads the library and starts it through JNI itself, and the app
     * is a Java one. Qt's QtActivity names its library so: 2GIS (libv4android.so, 277 MB) was taken for a NativeActivity,
     * and its start ended in "has no ANativeActivity_onCreate: it is not a NativeActivity library" (package 576).
     */
    else if (!strcmp(info->activity, "android.app.NativeActivity") || (!info->has_code && w.count) ||
             (info->native_library[0] && !(info->has_code && info->activity[0] && mg_zip_find(zips[0], "classes.dex") && native_entry(zips, nzips, info) == 0)))
        strcpy(info->engine, "native-activity");
    else if (w.has[L_SDL]) strcpy(info->engine, "sdl");
    /*
     * libGDX is a Java app as well: its AndroidApplication is an activity that draws with OpenGL ES on a GLSurfaceView, and
     * libgdx.so holds a few of its classes' natives (buffers, matrices), loaded with System.loadLibrary. ("Durak" was
     * refused as an engine with no driver.)
     */
    else if (w.has[L_GDX] && info->activity[0] && mg_zip_find(zips[0], "classes.dex")) strcpy(info->engine, "java");
    else if (w.has[L_GDX]) strcpy(info->engine, "libgdx");
    /*
     * Everything else that has bytecode and an activity to start is an ordinary Android app: Java, which loads what
     * native libraries it has itself (System.loadLibrary) when it wants them. The Java driver runs it.
     */
    else if (info->activity[0] && mg_zip_find(zips[0], "classes.dex")) strcpy(info->engine, "java");
    else strcpy(info->engine, "unknown");
    info->supported = mg_driver_supported(info->engine);

    /* A NativeActivity with no android.app.lib_name loads "main". */
    if (!strcmp(info->engine, "native-activity") && !info->native_library[0]) strcpy(info->native_library, "main");

    /*
     * The base of an app in several APKs, read without the split that has its native code: its manifest says there is one
     * for each kind of processor (android:requiredSplitTypes with "base__abi"), and none is here. Android does not install a
     * base without the splits it requires.
     */
    abi_split_missing = !info->nabis && strstr(info->required_split_types, "__abi") != NULL;

    if (info->split[0]) {
        info->runnable = false;
        snprintf(info->reason, sizeof(info->reason), "This APK is not an app by itself but a part of one (its split \"%s\"). Add the whole bundle (.apks or .xapk) it came in.",
                 info->split);
    } else if (abi_split_missing) {
        info->runnable = false;
        mg_strlcpy(info->reason, "This APK is the base of an app that comes in several APKs, and its native code is in a split APK of its own, which is not here. Add the whole bundle (.apks or .xapk) the app came in.",
                   sizeof(info->reason));
    } else if (!strcmp(info->engine, "java") && info->supported && (!info->nabis || info->abi[0])) {
        /*
         * Java, with no native libraries or with ones this processor can run: the Java driver runs it on the Dalvik runtime,
         * if the files for that are installed (the driver says when they are not). One whose libraries are all for other
         * processors falls through to the sentence about that.
         */
        info->runnable = true;
    } else if (!info->nabis) {
        /*
         * Nothing the Java driver can start, and no native code: no activity a launcher starts (a widget, a keyboard, a
         * service, a plug-in of another app), or no DEX file in this APK. (This said "written in Java only, which this layer
         * does not run yet" from before there was a Java driver: Sticky Note's widget and a framework-res.apk got it.)
         */
        info->runnable = false;
        if (!info->activity[0] && mg_zip_find(zips[0], "classes.dex"))
            mg_strlcpy(info->reason, "This app has no activity to start: it is a widget, a keyboard, a service or a part of another app, which is not run on its own.",
                       sizeof(info->reason));
        else if (!mg_zip_find(zips[0], "classes.dex"))
            mg_strlcpy(info->reason, "This APK has no code to run: no Java code (classes.dex) and no native code. It may hold resources for other apps, or be a part of a bundle.",
                       sizeof(info->reason));
        else
            mg_strlcpy(info->reason, "This app has no activity it can be started with.", sizeof(info->reason));
    } else if (!info->abi[0]) {
        info->runnable = false;
#if !MG_APK_ABI_ARM
        (void)any64;
        if (in_parts)
            snprintf(info->reason, sizeof(info->reason), "This app has no %s code: the bundle it came in has native code for %s only. This Megler runs its own processor's code and translates no other's; look for a version of the app built for %s.",
                     MG_GUEST_ABI, others[0] ? others : info->abis[0], MG_GUEST_ABI);
        else
            snprintf(info->reason, sizeof(info->reason), "This APK has no %s code (only %s). This Megler runs its own processor's code and translates no other's; look for a version of the app that includes %s.",
                     MG_GUEST_ABI, others[0] ? others : info->abis[0], MG_GUEST_ABI);
#else
        if (in_parts)
            snprintf(info->reason, sizeof(info->reason), "This app has no 32-bit ARM code: the bundle it came in has native code for %s only. Windows 10 Mobile is a 32-bit system and cannot run it; look for a version of the app that still includes armeabi-v7a.",
                     others[0] ? others : info->abis[0]);
        else
            snprintf(info->reason, sizeof(info->reason), "This APK has no 32-bit ARM code (only %s). Windows 10 Mobile is a 32-bit system and cannot run it; look for a version of the app that still includes armeabi-v7a.",
                     any64 ? "arm64-v8a" : info->abis[0]);
#endif
    } else if (!info->supported) {
        info->runnable = false;
        if (!strcmp(info->engine, "unknown"))
            mg_strlcpy(info->reason, "This app's native code is started and driven by its own Java code, which this layer does not run yet. Apps built on NativeActivity are the ones that start today.", sizeof(info->reason));
        else
            snprintf(info->reason, sizeof(info->reason), "This app is built on %s, and there is no driver for that engine yet.", info->engine);
    } else {
        info->runnable = true;
    }
    if (!strcmp(info->abi, "armeabi")) note(info, "Only ARMv5 (armeabi) code: it runs, with floating point done in software as the app was built.");
    /* Only where the device's probe has said that a thread does not stay in ARM state. */
    if (w.has[L_MONO] && mg_core_arm_state() < 0) note(info, "Unity with Mono: its JIT writes ARM-mode code at run time, which this phone does not keep running.");
    if (!strcmp(info->engine, "unity"))
        note(info, "The Unity driver is new and unfinished: the engine and the game's code start, and no Unity game has been seen to draw on a phone yet.");
    if (w.has[L_FLUTTER] && !strcmp(info->engine, "java"))
        note(info, "Built with Flutter: its engine runs inside the Java app and draws with OpenGL ES (there is no Vulkan here). Flutter apps are new here.");
    if (w.has[L_GDX] && !strcmp(info->engine, "java"))
        note(info, "Built with libGDX: the game runs as a Java app and draws with OpenGL ES.");
    if (w.has[L_COCOS] && !strcmp(info->engine, "java"))
        note(info, "With cocos2d-x: the engine runs inside the Java app, started by its activity, and draws with OpenGL ES.");
    if (w.has[L_DEXPROTECTOR])
        note(info, "Protected by DexProtector: most of the app's code is encrypted in the APK and decrypted while it starts by a packed native library made for Android's own runtime. Such an app may stop before its first screen.");
    if (!strcmp(info->engine, "native-activity") && info->has_code && strcmp(info->activity, "android.app.NativeActivity"))
        note(info, "Its activity (%s) is the app's own Java class on top of NativeActivity. That Java code is not run here: the app starts, and gets as far as it can without it.",
             info->activity);
    /* A base alone that says it needs its splits, and runs all the same as far as anyone can tell from here. */
    if (!info->split[0] && !abi_split_missing && info->split_required && !in_parts)
        note(info, "This APK is only the base of an app that comes in several APKs (its manifest says it needs them): pictures for some screen densities, some languages or other parts of it may be missing. If it came in a bundle (.apks or .xapk), add the bundle instead.");
}

/*
 * An APK's manifest, with its resources for the label: the archive is left open in `z` for the caller, and closed when
 * the APK cannot be read (info->error then says why).
 */
static bool read_manifest(const char *apk_path, mg_zip *z, mg_apk_info *info)
{
    const mg_zip_entry *me, *re;
    u8 *manifest, *resources = NULL;
    size_t msize = 0, rsize = 0;
    arsc res;
    memset(info, 0, sizeof(*info));
    if (!mg_zip_open(z, apk_path, info->error, sizeof(info->error))) return false;
    me = mg_zip_find(z, "AndroidManifest.xml");
    if (!me) {
        mg_strlcpy(info->error, "not an APK: there is no AndroidManifest.xml in it", sizeof(info->error));
        mg_zip_close(z);
        return false;
    }
    manifest = mg_zip_read(z, me, 16u << 20, &msize);
    re = mg_zip_find(z, "resources.arsc");
    if (re) resources = mg_zip_read(z, re, ARSC_MAX, &rsize);
    arsc_open(&res, resources, (u32)rsize);
    if (!manifest || !parse_manifest(info, &res, manifest, (u32)msize)) {
        mg_strlcpy(info->error, "the APK's manifest cannot be read", sizeof(info->error));
        free(manifest);
        free(resources);
        mg_zip_close(z);
        return false;
    }
    free(manifest);
    free(resources);
    return true;
}

/* The last part of a path, for a person. */
static const char *file_name(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back && (!slash || back > slash)) slash = back;
    return slash ? slash + 1 : path;
}

bool mg_apk_read_app(const char *apk_path, const char *const *splits, int nsplits, const char *record_dir, mg_apk_info *info)
{
    mg_zip *zips[1 + MG_APK_MAX_SPLITS];
    int given[1 + MG_APK_MAX_SPLITS];      /* for each archive read, 0 the base, or 1 + its place in `splits` */
    mg_apk_info *part = NULL;
    mg_bundle_record *record = NULL;
    u32 icon_score = 0, label_score = 0;
    int nz, k;
    zips[0] = (mg_zip *)mg_alloc(sizeof(mg_zip));
    if (!read_manifest(apk_path, zips[0], info)) {
        free(zips[0]);
        return false;
    }
    nz = 1;
    given[0] = 0;
    for (k = 0; k < nsplits; k++) {
        const char *name = file_name(splits[k]);
        if (nz > MG_APK_MAX_SPLITS) {
            note(info, "Only %d split APKs are read with an app: %s and those after it are left out.", MG_APK_MAX_SPLITS, name);
            break;
        }
        if (!part) part = (mg_apk_info *)mg_alloc(sizeof(*part));
        zips[nz] = (mg_zip *)mg_alloc(sizeof(mg_zip));
        if (!read_manifest(splits[k], zips[nz], part)) {
            note(info, "The split APK %s cannot be read (%s) and is left out.", name, part->error);
            free(zips[nz]);
            continue;
        }
        /* Android installs a split only with a base of its own package, and only one that says it is a split. */
        if (strcmp(part->package, info->package) || !part->split[0]) {
            if (strcmp(part->package, info->package)) note(info, "%s is a part of another app (%s) and is left out.", name, part->package);
            else note(info, "%s is not a split APK (its manifest names no split) and is left out.", name);
            mg_zip_close(zips[nz]);
            free(zips[nz]);
            continue;
        }
        mg_strlcpy(info->splits[info->nsplits++], part->split, sizeof(info->splits[0]));
        given[nz++] = k + 1;
    }
    free(part);
    if (record_dir) {
        record = (mg_bundle_record *)mg_alloc(sizeof(*record));
        if (mg_bundle_record_read(record_dir, record)) {
            for (k = 0; k < record->nleft_out && k < (int)MG_COUNT(info->left_out); k++) mg_strlcpy(info->left_out[k], record->left_out[k], sizeof(info->left_out[0]));
            info->nleft_out = k;
        } else {
            record->nabis = 0;
        }
    }
    /*
     * The label and the icon, through every APK's resources: a split has entries of the base's resources for its own
     * configurations, a density split the pictures for its density -- often the only pictures of the icon there are.
     */
    for (k = 0; k < nz && (info->icon_id || (info->label_id && !info->label[0])); k++) {
        const mg_zip_entry *re = mg_zip_find(zips[k], "resources.arsc");
        size_t rsize = 0;
        u8 *resources = re ? mg_zip_read(zips[k], re, ARSC_MAX, &rsize) : NULL;
        arsc res;
        if (!resources) continue;
        arsc_open(&res, resources, (u32)rsize);
        if (info->icon_id && arsc_string(&res, info->icon_id, true, info->icon_path, sizeof(info->icon_path), &icon_score)) info->icon_apk = given[k];
        if (info->label_id && !info->label[0]) arsc_string(&res, info->label_id, false, info->label, sizeof(info->label), &label_score);
        free(resources);
    }
    if (!info->label[0]) {
        const char *dot = strrchr(info->package, '.');
        mg_strlcpy(info->label, dot ? dot + 1 : info->package, sizeof(info->label));
    }
    /* An icon the resources did not lead to: the usual names, densest first, in the APKs in turn. */
    for (k = 0; k < nz && given[k] != info->icon_apk; k++) { }
    if (!info->icon_path[0] || k == nz || !mg_zip_find(zips[k], info->icon_path)) {
        static const char *const dens[] = { "xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi" };
        static const char *const kinds[] = { "mipmap", "drawable" };
        static const char *const names[] = { "ic_launcher", "icon", "app_icon", "ic_launcher_round" };
        static const char *const sufs[] = { "-v4", "" };
        static const char *const exts[] = { "png", "webp" };
        int d, n, s, x, a;
        info->icon_path[0] = 0;
        info->icon_apk = 0;
        for (d = 0; d < 5 && !info->icon_path[0]; d++)
            for (k = 0; k < 2 && !info->icon_path[0]; k++)
                for (n = 0; n < 4 && !info->icon_path[0]; n++)
                    for (s = 0; s < 2 && !info->icon_path[0]; s++)
                        for (x = 0; x < 2 && !info->icon_path[0]; x++)
                            for (a = 0; a < nz; a++) {
                                char path[200];
                                snprintf(path, sizeof(path), "res/%s-%s%s/%s.%s", kinds[k], dens[d], sufs[s], names[n], exts[x]);
                                if (mg_zip_find(zips[a], path)) {
                                    mg_strlcpy(info->icon_path, path, sizeof(info->icon_path));
                                    info->icon_apk = given[a];
                                    break;
                                }
                            }
    }
    classify(info, zips, nz, record ? record->abis : NULL, record ? record->nabis : 0);
    for (k = 0; k < nz; k++) {
        mg_zip_close(zips[k]);
        free(zips[k]);
    }
    free(record);
    return true;
}

bool mg_apk_read_info(const char *apk_path, mg_apk_info *info) { return mg_apk_read_app(apk_path, NULL, 0, NULL, info); }

typedef struct split_list {
    const char *dir;
    char **paths;
    int n, cap;
} split_list;

static MG_HOSTCB void split_seen(const char *name, bool is_dir, void *user)
{
    split_list *l = (split_list *)user;
    size_t n = strlen(name);
    if (is_dir || n < 5 || name[0] == '.') return;
    if (!(name[n - 4] == '.' && (name[n - 3] | 32) == 'a' && (name[n - 2] | 32) == 'p' && (name[n - 1] | 32) == 'k')) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->paths = (char **)mg_realloc(l->paths, sizeof(char *) * (size_t)l->cap);
    }
    l->paths[l->n++] = mg_sprintf("%s/%s", l->dir, name);
}

static int by_path(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int mg_apk_list_splits(const char *dir, char ***paths)
{
    split_list l;
    memset(&l, 0, sizeof(l));
    l.dir = dir;
    *paths = NULL;
    if (!dir || !dir[0] || !mg_path_list(dir, split_seen, &l, NULL)) return 0;
    if (l.n > 1) qsort(l.paths, (size_t)l.n, sizeof(char *), by_path);
    *paths = l.paths;
    return l.n;
}

void mg_apk_free_list(char **paths, int n)
{
    int i;
    for (i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

void mg_apk_info_free(mg_apk_info *info) { (void)info; }

const char *mg_apk_meta_value(const mg_apk_info *info, const char *name)
{
    int i;
    for (i = 0; i < info->nmeta; i++)
        if (!strcmp(info->meta[i].name, name)) return info->meta[i].value;
    return NULL;
}

/* ------------------------------------------------------------------ report */

typedef struct lib_report {
    mg_jbuf *j;
    const mg_zip *zip;
    const char *prefix;
    bool first;
    mg_apk_info *info;
    u32 arm_total, thumb_total;
} lib_report;

/* Where a virtual address is in the file, by the program headers; NULL when it is not in the file. */
static const u8 *elf_at(const u8 *d, size_t n, u64 vaddr, u64 need)
{
    const A_Ehdr *eh = (const A_Ehdr *)d;
    const A_Phdr *ph = (const A_Phdr *)(d + eh->e_phoff);
    int i;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || vaddr < ph[i].p_vaddr || vaddr - ph[i].p_vaddr > ph[i].p_filesz) continue;
        if (need > ph[i].p_filesz - (vaddr - ph[i].p_vaddr)) return NULL;
        if (ph[i].p_offset > n || ph[i].p_filesz > n - ph[i].p_offset) return NULL;
        return d + ph[i].p_offset + (vaddr - ph[i].p_vaddr);
    }
    return NULL;
}

static void lib_report_one(const mg_zip_entry *e, void *user)
{
    lib_report *r = (lib_report *)user;
    mg_jbuf *j = r->j;
    const char *name = e->name + strlen(r->prefix);
    const u8 *d;
    u8 *owned = NULL;
    size_t n = 0;
    u32 arm = 0, thumb = 0, missing = 0;
    bool parsed = false;
    if (strchr(name, '/') || strlen(name) < 4 || strcmp(name + strlen(name) - 3, ".so")) return;
    if (!r->first) mg_jraw(j, ",");
    r->first = false;
    mg_jraw(j, "{");
    mg_jkey(j, "name", true);
    mg_jstr(j, name);
    mg_jkey(j, "size", false);
    mg_jint(j, (long)e->usize);
    mg_jkey(j, "needs", false);
    mg_jraw(j, "[");

    owned = mg_zip_read(r->zip, e, 512u << 20, &n);
    d = owned;
    if (d && n > sizeof(A_Ehdr) && !memcmp(d, "\177ELF", 4) && d[4] == A_CLASS) {
        const A_Ehdr *eh = (const A_Ehdr *)d;
        if (eh->e_machine == A_MACHINE && eh->e_phentsize == sizeof(A_Phdr) && eh->e_phoff <= n && (size_t)eh->e_phnum * sizeof(A_Phdr) <= n - eh->e_phoff) {
            const A_Phdr *ph = (const A_Phdr *)(d + eh->e_phoff);
            const A_Dyn *dyn = NULL;
            u32 ndyn = 0, symtab = 0, strtab = 0, strsz = 0, hash = 0, gnu = 0, nsyms = 0, k;
            int i;
            bool first_need = true;
            for (i = 0; i < eh->e_phnum; i++) {
                if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset <= n && ph[i].p_filesz <= n - ph[i].p_offset) {
                    dyn = (const A_Dyn *)(d + ph[i].p_offset);
                    ndyn = (u32)(ph[i].p_filesz / sizeof(A_Dyn));
                }
            }
            for (k = 0; k < ndyn && dyn[k].d_tag != DT_NULL; k++) {
                if (dyn[k].d_tag == DT_SYMTAB) symtab = (u32)dyn[k].d_val;
                if (dyn[k].d_tag == DT_STRTAB) strtab = (u32)dyn[k].d_val;
                if (dyn[k].d_tag == DT_STRSZ) strsz = (u32)dyn[k].d_val;
                if (dyn[k].d_tag == DT_HASH) hash = (u32)dyn[k].d_val;
                if (dyn[k].d_tag == DT_GNU_HASH) gnu = (u32)dyn[k].d_val;
            }
            if (strtab && strsz) {
                const char *str = (const char *)elf_at(d, n, strtab, strsz);
                if (str && str[strsz - 1] == 0) {
                    for (k = 0; k < ndyn && dyn[k].d_tag != DT_NULL; k++) {
                        if (dyn[k].d_tag != DT_NEEDED || dyn[k].d_val >= strsz) continue;
                        if (!first_need) mg_jraw(j, ",");
                        first_need = false;
                        mg_jstr(j, str + dyn[k].d_val);
                    }
                    nsyms = elf_symbol_count(d, n, hash, gnu);
                    if (symtab && nsyms && nsyms < 4000000) {
                        const A_Sym *sym = (const A_Sym *)elf_at(d, n, symtab, nsyms * sizeof(A_Sym));
                        for (k = 1; sym && k < nsyms; k++) {
                            const char *sn;
                            if (sym[k].st_name >= strsz) continue;
                            sn = str + sym[k].st_name;
                            if (sym[k].st_shndx == SHN_UNDEF) {
                                /* An import: the system's, or (not checked here) another library's of the app. */
                                if (ELF_ST_BIND(sym[k].st_info) != STB_WEAK && !mg_bionic_find(sn) && !mg_egl_is_gl_name(sn)) missing++;
                            } else if (ELF_ST_TYPE(sym[k].st_info) == STT_FUNC) {
                                if (sym[k].st_value & 1) thumb++;
                                else arm++;
                            }
                        }
                        parsed = sym != NULL;
                    }
                }
            }
        }
    }
    free(owned);
    mg_jraw(j, "]");
    if (parsed) {
        mg_jkey(j, "armFunctions", false);
        mg_jint(j, (long)arm);
        mg_jkey(j, "thumbFunctions", false);
        mg_jint(j, (long)thumb);
        mg_jkey(j, "importsNotInSystem", false);
        mg_jint(j, (long)missing);
        r->arm_total += arm;
        r->thumb_total += thumb;
    }
    mg_jraw(j, "}");
}

static void microg_seen(const mg_zip_entry *e, void *user)
{
    (void)e;
    *(bool *)user = true;
}

/* Whether the APK is microG's Google Play services: it has files under org/microg/ (its address formats, among others). */
bool mg_apk_is_microg(const char *path)
{
    mg_zip z;
    char err[160];
    bool seen = false;
    if (!mg_zip_open(&z, path, err, sizeof(err))) return false;
    mg_zip_each(&z, "org/microg/", microg_seen, &seen);
    mg_zip_close(&z);
    return seen;
}

/* The report of an app in one or more APKs (megler.h): the base, its splits, and the folder with a bundle's record, if any. */
static char *report(const char *apk_path, char *const *splits, int nsplits, const char *record_dir)
{
    mg_apk_info *info = (mg_apk_info *)mg_alloc(sizeof(*info));
    mg_jbuf j;
    int i, k;
    memset(&j, 0, sizeof(j));
    mg_jraw(&j, "{");
    if (!mg_apk_read_app(apk_path, (const char *const *)splits, nsplits, record_dir, info)) {
        mg_jkey(&j, "ok", true);
        mg_jraw(&j, "false");
        mg_jkey(&j, "error", false);
        mg_jstr(&j, info->error);
        mg_jraw(&j, "}");
        free(info);
        return j.s;
    }
    mg_jkey(&j, "ok", true);
    mg_jraw(&j, "true");
    mg_jkey(&j, "package", false);
    mg_jstr(&j, info->package);
    mg_jkey(&j, "label", false);
    mg_jstr(&j, info->label);
    mg_jkey(&j, "versionName", false);
    mg_jstr(&j, info->version_name);
    mg_jkey(&j, "versionCode", false);
    mg_jint(&j, info->version_code);
    mg_jkey(&j, "minSdk", false);
    mg_jint(&j, info->min_sdk);
    mg_jkey(&j, "targetSdk", false);
    mg_jint(&j, info->target_sdk);
    if (info->nuses_libraries) {
        mg_jkey(&j, "usesLibraries", false);
        mg_jraw(&j, "[");
        for (i = 0; i < info->nuses_libraries; i++) {
            if (i) mg_jraw(&j, ",");
            mg_jstr(&j, info->uses_libraries[i]);
        }
        mg_jraw(&j, "]");
    }
    mg_jkey(&j, "abis", false);
    mg_jraw(&j, "[");
    for (i = 0; i < info->nabis; i++) {
        if (i) mg_jraw(&j, ",");
        mg_jstr(&j, info->abis[i]);
    }
    mg_jraw(&j, "]");
    if (info->abi[0]) {
        mg_jkey(&j, "abi", false);
        mg_jstr(&j, info->abi);
    }
    mg_jkey(&j, "engine", false);
    mg_jstr(&j, info->engine);
    mg_jkey(&j, "supported", false);
    mg_jraw(&j, info->supported ? "true" : "false");
    /* Google Play services that are microG's: the shell keeps them apart from Google's own, each app taking the one it is set to. */
    if (!strcmp(info->package, "com.google.android.gms") && mg_apk_is_microg(apk_path)) {
        mg_jkey(&j, "microg", false);
        mg_jraw(&j, "true");
    }
    mg_jkey(&j, "activity", false);
    mg_jstr(&j, info->activity);
    mg_jkey(&j, "orientation", false);
    mg_jstr(&j, info->orientation == MG_APK_LANDSCAPE ? "landscape" : info->orientation == MG_APK_PORTRAIT ? "portrait" : "unspecified");
    if (info->native_library[0]) {
        mg_jkey(&j, "nativeLibrary", false);
        mg_jstr(&j, info->native_library);
    }
    mg_jkey(&j, "hasIcon", false);
    mg_jraw(&j, info->icon_path[0] ? "true" : "false");
    /* The splits it was read with, and those the unpacking of its bundle left out. */
    if (info->nsplits || info->nleft_out) {
        mg_jkey(&j, "splits", false);
        mg_jraw(&j, "[");
        for (i = 0; i < info->nsplits; i++) {
            if (i) mg_jraw(&j, ",");
            mg_jstr(&j, info->splits[i]);
        }
        mg_jraw(&j, "]");
        mg_jkey(&j, "splitsLeftOut", false);
        mg_jraw(&j, "[");
        for (i = 0; i < info->nleft_out; i++) {
            if (i) mg_jraw(&j, ",");
            mg_jstr(&j, info->left_out[i]);
        }
        mg_jraw(&j, "]");
    }
    mg_jkey(&j, "libraries", false);
    mg_jraw(&j, "[");
    if (info->abi[0]) {
        /* The libraries of every APK of the app's, the base's first: a split for the processor has them in an app in parts. */
        lib_report r;
        char prefix[40];
        memset(&r, 0, sizeof(r));
        snprintf(prefix, sizeof(prefix), "lib/%s/", info->abi);
        r.j = &j;
        r.prefix = prefix;
        r.first = true;
        r.info = info;
        for (k = -1; k < nsplits; k++) {
            mg_zip z;
            char err[160];
            if (!mg_zip_open(&z, k < 0 ? apk_path : splits[k], err, sizeof(err))) continue;
            r.zip = &z;
            mg_zip_each(&z, prefix, lib_report_one, &r);
            mg_zip_close(&z);
        }
        if (r.arm_total) {
            if (info->nnotes < (int)MG_COUNT(info->notes))
                snprintf(info->notes[info->nnotes++], sizeof(info->notes[0]),
                         "%u of its %u exported functions are in ARM (not Thumb) mode. Whether a phone keeps such code running is what Device checks measures.",
                         r.arm_total, r.arm_total + r.thumb_total);
        }
    }
    mg_jraw(&j, "]");
    mg_jkey(&j, "runnable", false);
    mg_jraw(&j, info->runnable ? "true" : "false");
    if (!info->runnable) {
        mg_jkey(&j, "reason", false);
        mg_jstr(&j, info->reason);
    }
    mg_jkey(&j, "notes", false);
    mg_jraw(&j, "[");
    for (i = 0; i < info->nnotes; i++) {
        if (i) mg_jraw(&j, ",");
        mg_jstr(&j, info->notes[i]);
    }
    mg_jraw(&j, "]}");
    free(info);
    return j.s;
}

char *megler_apk_report(const char *apk_path) { return report(apk_path, NULL, 0, NULL); }

char *megler_apk_report_splits(const char *apk_path, const char *splits_dir)
{
    char **splits = NULL, *out;
    int n = mg_apk_list_splits(splits_dir, &splits);
    out = report(apk_path, splits, n, splits_dir && splits_dir[0] ? splits_dir : NULL);
    mg_apk_free_list(splits, n);
    return out;
}

/* The APK and its splits, their files and resource tables, for drawing an icon (mg_icon.h). */
typedef struct icon_apks {
    mg_zip zips[1 + MG_APK_MAX_SPLITS];
    u8 *tables[1 + MG_APK_MAX_SPLITS];
    arsc res[1 + MG_APK_MAX_SPLITS];
    int n;
} icon_apks;

static u8 *icon_read(void *user, const char *path, size_t *size)
{
    icon_apks *a = (icon_apks *)user;
    int k;
    for (k = 0; k < a->n; k++) {
        const mg_zip_entry *e = mg_zip_find(&a->zips[k], path);
        if (e) return mg_zip_read(&a->zips[k], e, 16u << 20, size);
    }
    return NULL;
}

static bool icon_value(void *user, u32 id, u8 *type, u32 *data, char *text, size_t cap)
{
    icon_apks *a = (icon_apks *)user;
    u32 best = 0;
    bool found = false;
    int k;
    for (k = 0; k < a->n; k++)
        if (arsc_raw(&a->res[k], id, type, data, text, cap, &best)) found = true;
    return found;
}

/* An app's APKs, the base first, each open with its resources.arsc read (the base's is a->zips[0] when it opened). */
static icon_apks *apks_open(const char *apk_path, char *const *splits, int nsplits)
{
    icon_apks *a = (icon_apks *)mg_alloc(sizeof(*a));
    int k;
    for (k = -1; k < nsplits && a->n < (int)MG_COUNT(a->zips); k++) {
        char err[160];
        const mg_zip_entry *e;
        size_t rsize = 0;
        if (!mg_zip_open(&a->zips[a->n], k < 0 ? apk_path : splits[k], err, sizeof(err))) continue;
        e = mg_zip_find(&a->zips[a->n], "resources.arsc");
        a->tables[a->n] = e ? mg_zip_read(&a->zips[a->n], e, ARSC_MAX, &rsize) : NULL;
        arsc_open(&a->res[a->n], a->tables[a->n], (u32)rsize);
        a->n++;
    }
    return a;
}

static void apks_close(icon_apks *a)
{
    int k;
    for (k = 0; k < a->n; k++) {
        mg_zip_close(&a->zips[k]);
        free(a->tables[k]);
    }
    free(a);
}

/* The icon an XML of those APKs describes (an adaptive icon, a vector, ...), drawn: a PNG, or NULL. */
static u8 *icon_drawn_in(icon_apks *a, const char *path, size_t *size)
{
    mg_icon_src src;
    src.read = icon_read;
    src.value = icon_value;
    src.user = a;
    return mg_icon_png(&src, path, 256, size);
}

/* The icon an XML describes (an adaptive icon, a vector, ...), drawn: a PNG, or NULL. */
static u8 *icon_drawn(const char *apk_path, char *const *splits, int nsplits, const char *path, size_t *size)
{
    icon_apks *a = apks_open(apk_path, splits, nsplits);
    u8 *png = icon_drawn_in(a, path, size);
    apks_close(a);
    return png;
}

u8 *mg_apk_icon_draw(const char *apk_path, const char *path, size_t *size) { return icon_drawn(apk_path, NULL, 0, path, size); }

/*
 * An icon picture the shell can show: a WebP one is made a PNG. Play Store's launcher icon is a WebP picture of its APK's,
 * and the phone's own decoders have no WebP in them: its library entry showed no icon, while its package's tiles, which the
 * core draws, had one. The core's WebP decoder gives B, G, R, A premultiplied, made straight R, G, B, A for the PNG.
 */
static u8 *icon_as_png(u8 *bytes, size_t *size)
{
    int w = 0, h = 0;
    size_t i, png_size = 0;
    u8 *rgba, *png;
    if (!bytes || !mg_webp_is(bytes, *size) || !mg_webp_size(bytes, *size, &w, &h) || w <= 0 || h <= 0 || w > 4096 || h > 4096) return bytes;
    rgba = (u8 *)mg_alloc((size_t)w * h * 4);
    if (!mg_webp_decode(bytes, *size, rgba, w, h, w * 4)) {
        free(rgba);
        return bytes;
    }
    for (i = 0; i < (size_t)w * h; i++) {
        u8 *px = rgba + i * 4, b = px[0], a = px[3];
        px[0] = a ? (u8)(px[2] * 255 / a) : 0;
        px[1] = a ? (u8)(px[1] * 255 / a) : 0;
        px[2] = a ? (u8)(b * 255 / a) : 0;
    }
    png = mg_png_encode(rgba, w, h, &png_size);
    free(rgba);
    if (!png) return bytes;
    free(bytes);
    *size = png_size;
    return png;
}

bool megler_apk_icon_splits(const char *apk_path, const char *splits_dir, uint8_t **data, size_t *size)
{
    mg_apk_info *info = (mg_apk_info *)mg_alloc(sizeof(*info));
    char **splits = NULL;
    int n = mg_apk_list_splits(splits_dir, &splits);
    u8 *bytes = NULL;
    *data = NULL;
    *size = 0;
    if (mg_apk_read_app(apk_path, (const char *const *)splits, n, NULL, info) && info->icon_path[0]) {
        size_t len = strlen(info->icon_path);
        if (len > 4 && !strcmp(info->icon_path + len - 4, ".xml")) {
            /* An icon that is no picture (an adaptive icon, a vector: an app for Android 8 and later may give no other): drawn. */
            bytes = icon_drawn(apk_path, splits, n, info->icon_path, size);
        } else {
            mg_zip z;
            char err[160];
            const mg_zip_entry *e;
            if (mg_zip_open(&z, info->icon_apk ? splits[info->icon_apk - 1] : apk_path, err, sizeof(err))) {
                e = mg_zip_find(&z, info->icon_path);
                if (e) bytes = mg_zip_read(&z, e, 8u << 20, size);
                mg_zip_close(&z);
            }
        }
    }
    /* None in the APKs: the icon the bundle came with, which megler_bundle_unpack keeps beside the splits. */
    if (!bytes && splits_dir && splits_dir[0]) {
        char *own = mg_sprintf("%s/%s", splits_dir, MG_BUNDLE_ICON);
        size_t got = 0;
        bytes = mg_file_read_all(own, &got);
        if (bytes && got > 0 && got <= (8u << 20)) *size = got;
        else {
            free(bytes);
            bytes = NULL;
        }
        free(own);
    }
    mg_apk_free_list(splits, n);
    free(info);
    bytes = icon_as_png(bytes, size);
    *data = bytes;
    return bytes != NULL;
}

bool megler_apk_icon(const char *apk_path, uint8_t **data, size_t *size) { return megler_apk_icon_splits(apk_path, NULL, data, size); }

/* ------------------------------------------------- the activities, for another app's launcher */

#define ATTR_EXPORTED 0x01010010u
#define ATTR_MIME_TYPE 0x01010026u
#define ATTR_SCHEME 0x01010027u
#define ATTR_HOST 0x01010028u

typedef struct acts_ctx {
    char package[200];
    const icon_apks *apks;     /* every APK's resources, for the labels */
    mg_jbuf out, act, filters;  /* the description; the activity being read; its filters */
    bool in_application, in_activity, in_filter, began, listed, exported_given, exported;
    int nfilters;
    /* the filter being read */
    char actions[16][160], categories[16][160], schemes[8][32], hosts[8][160], types[8][96];
    int nactions, ncategories, nschemes, nhosts, ntypes;
    /* the icons the app and its activities name, each once */
    u32 icons[64];
    int nicons;
} acts_ctx;

static void jbuf_reset(mg_jbuf *j)
{
    free(j->s);
    memset(j, 0, sizeof(*j));
}

static void acts_icon(acts_ctx *c, u32 id)
{
    int k;
    if (!id) return;
    for (k = 0; k < c->nicons; k++)
        if (c->icons[k] == id) return;
    if (c->nicons < (int)MG_COUNT(c->icons)) c->icons[c->nicons++] = id;
}

/* A label attribute's text: the string it is, or the resource it names in the phone's language, through every APK's table. */
static void acts_label(const acts_ctx *c, const attr *a, char *out, size_t cap)
{
    u32 best = 0;
    int k;
    out[0] = 0;
    if (a->type == 0x03) mg_strlcpy(out, a->text, cap);
    else if (a->type == 0x01 && a->data)
        for (k = 0; k < c->apks->n; k++) arsc_string(&c->apks->res[k], a->data, false, out, cap, &best);
}

static void acts_class(const acts_ctx *c, const char *name, char *out, size_t cap)
{
    if (name[0] == '.') snprintf(out, cap, "%s%s", c->package, name);
    else if (!strchr(name, '.') && name[0]) snprintf(out, cap, "%s.%s", c->package, name);
    else mg_strlcpy(out, name, cap);
}

/* `n` strings, each in `width` bytes from `items`, as a member that is an array of them. */
static void acts_array(mg_jbuf *j, const char *key, const char *items, int n, size_t width)
{
    int k;
    mg_jkey(j, key, false);
    mg_jraw(j, "[");
    for (k = 0; k < n; k++) {
        if (k) mg_jraw(j, ",");
        mg_jstr(j, items + k * width);
    }
    mg_jraw(j, "]");
}

static void acts_start(void *user, const char *tag, const attr *attrs, int nattrs)
{
    acts_ctx *c = (acts_ctx *)user;
    const attr *name = NULL;
    int i;
    for (i = 0; i < nattrs; i++)
        if (attrs[i].id == ATTR_NAME || (!attrs[i].id && !strcmp(attrs[i].name, "name"))) name = &attrs[i];
    if (!strcmp(tag, "manifest")) {
        for (i = 0; i < nattrs; i++)
            if (!attrs[i].id && !strcmp(attrs[i].name, "package")) mg_strlcpy(c->package, attrs[i].text, sizeof(c->package));
        mg_jraw(&c->out, "{");
        mg_jkey(&c->out, "package", true);
        mg_jstr(&c->out, c->package);
        c->began = true;
    } else if (!strcmp(tag, "application") && c->began && !c->listed) {
        char label[200] = "";
        u32 icon = 0;
        c->in_application = true;
        for (i = 0; i < nattrs; i++) {
            if (attrs[i].id == ATTR_LABEL) acts_label(c, &attrs[i], label, sizeof(label));
            if (attrs[i].id == ATTR_ICON && attrs[i].type == 0x01) icon = attrs[i].data;
        }
        mg_jkey(&c->out, "label", false);
        mg_jstr(&c->out, label);
        mg_jkey(&c->out, "icon", false);
        mg_jint(&c->out, (long long)icon);
        mg_jkey(&c->out, "activities", false);
        mg_jraw(&c->out, "[");
        acts_icon(c, icon);
        c->listed = true;
    } else if (c->in_application && !c->in_activity && (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias")) && name) {
        char text[200];
        c->in_activity = true;
        c->exported_given = false;
        c->nfilters = 0;
        jbuf_reset(&c->act);
        jbuf_reset(&c->filters);
        acts_class(c, name->text, text, sizeof(text));
        mg_jkey(&c->act, "name", true);
        mg_jstr(&c->act, text);
        for (i = 0; i < nattrs; i++) {
            const attr *a = &attrs[i];
            if (a->id == ATTR_TARGET_ACTIVITY && !strcmp(tag, "activity-alias")) {
                acts_class(c, a->text, text, sizeof(text));
                mg_jkey(&c->act, "target", false);
                mg_jstr(&c->act, text);
            } else if (a->id == ATTR_LABEL) {
                acts_label(c, a, text, sizeof(text));
                if (text[0]) {
                    mg_jkey(&c->act, "label", false);
                    mg_jstr(&c->act, text);
                }
            } else if (a->id == ATTR_ICON && a->type == 0x01 && a->data) {
                mg_jkey(&c->act, "icon", false);
                mg_jint(&c->act, (long long)a->data);
                acts_icon(c, a->data);
            } else if (a->id == ATTR_ENABLED && a->type == 0x12) {
                mg_jkey(&c->act, "enabled", false);
                mg_jraw(&c->act, a->data ? "true" : "false");
            } else if (a->id == ATTR_EXPORTED && a->type == 0x12) {
                c->exported_given = true;
                c->exported = a->data != 0;
            }
        }
    } else if (c->in_activity && !strcmp(tag, "intent-filter")) {
        c->in_filter = true;
        c->nactions = c->ncategories = c->nschemes = c->nhosts = c->ntypes = 0;
    } else if (c->in_filter && !strcmp(tag, "action") && name && c->nactions < (int)MG_COUNT(c->actions)) {
        mg_strlcpy(c->actions[c->nactions++], name->text, sizeof(c->actions[0]));
    } else if (c->in_filter && !strcmp(tag, "category") && name && c->ncategories < (int)MG_COUNT(c->categories)) {
        mg_strlcpy(c->categories[c->ncategories++], name->text, sizeof(c->categories[0]));
    } else if (c->in_filter && !strcmp(tag, "data")) {
        for (i = 0; i < nattrs; i++) {
            const attr *a = &attrs[i];
            if (a->id == ATTR_SCHEME && c->nschemes < (int)MG_COUNT(c->schemes)) mg_strlcpy(c->schemes[c->nschemes++], a->text, sizeof(c->schemes[0]));
            if (a->id == ATTR_HOST && c->nhosts < (int)MG_COUNT(c->hosts)) mg_strlcpy(c->hosts[c->nhosts++], a->text, sizeof(c->hosts[0]));
            if (a->id == ATTR_MIME_TYPE && c->ntypes < (int)MG_COUNT(c->types)) mg_strlcpy(c->types[c->ntypes++], a->text, sizeof(c->types[0]));
        }
    }
}

static void acts_end(void *user, const char *tag)
{
    acts_ctx *c = (acts_ctx *)user;
    if (!strcmp(tag, "intent-filter") && c->in_filter) {
        mg_jbuf *j = &c->filters;
        c->in_filter = false;
        if (c->nfilters++) mg_jraw(j, ",");
        mg_jraw(j, "{");
        mg_jkey(j, "id", true);
        mg_jint(j, c->nfilters);
        acts_array(j, "actions", c->actions[0], c->nactions, sizeof(c->actions[0]));
        acts_array(j, "categories", c->categories[0], c->ncategories, sizeof(c->categories[0]));
        acts_array(j, "schemes", c->schemes[0], c->nschemes, sizeof(c->schemes[0]));
        acts_array(j, "hosts", c->hosts[0], c->nhosts, sizeof(c->hosts[0]));
        acts_array(j, "types", c->types[0], c->ntypes, sizeof(c->types[0]));
        mg_jraw(j, "}");
    } else if ((!strcmp(tag, "activity") || !strcmp(tag, "activity-alias")) && c->in_activity) {
        c->in_activity = false;
        if (c->out.s && c->out.s[c->out.n - 1] != '[') mg_jraw(&c->out, ",");
        mg_jraw(&c->out, "{");
        mg_jraw(&c->out, c->act.s ? c->act.s : "");
        /* Not said: exported when it has a filter, as Android before 12 has it. */
        mg_jkey(&c->out, "exported", false);
        mg_jraw(&c->out, (c->exported_given ? c->exported : c->nfilters > 0) ? "true" : "false");
        mg_jkey(&c->out, "filters", false);
        mg_jraw(&c->out, "[");
        mg_jraw(&c->out, c->filters.s ? c->filters.s : "");
        mg_jraw(&c->out, "]}");
    } else if (!strcmp(tag, "application")) {
        c->in_application = false;
    }
}

/* The picture of icon resource `id` in those APKs: a PNG (a WebP one made one, an XML one drawn), or NULL. */
static u8 *icon_of_id(icon_apks *a, u32 id, size_t *size)
{
    char path[200] = "";
    u32 best = 0;
    size_t len;
    int k;
    u8 *bytes;
    for (k = 0; k < a->n; k++) arsc_string(&a->res[k], id, true, path, sizeof(path), &best);
    len = strlen(path);
    if (!len) return NULL;
    if (len > 4 && !strcmp(path + len - 4, ".xml")) return icon_drawn_in(a, path, size);
    bytes = icon_read(a, path, size);
    if (!bytes || *size == 0 || *size > (8u << 20)) {
        free(bytes);
        return NULL;
    }
    return icon_as_png(bytes, size);
}

/*
 * The certificates an APK is signed with, as megler.ApkSigner reads the running app's own: the APK Signing Block's v3
 * signer, else its v2 one, else the v1 PKCS#7 file under META-INF (.RSA, .DSA, .EC). Each is written into `j` as a JSON
 * string of its DER in hex digits (what android.content.pm.Signature(String) takes). How many were written.
 */
typedef struct cspan {
    const u8 *p;
    size_t n;
} cspan;

static u32 cle32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 cle64(const u8 *p) { return (u64)cle32(p) | (u64)cle32(p + 4) << 32; }

/* A value after its 32-bit little-endian length, taken off the front of `s`. */
static bool cspan_lp(cspan *s, cspan *out)
{
    u32 n;
    if (s->n < 4) return false;
    n = cle32(s->p);
    if (n > s->n - 4) return false;
    out->p = s->p + 4;
    out->n = n;
    s->p += 4 + (size_t)n;
    s->n -= 4 + (size_t)n;
    return true;
}

static void cert_hex(mg_jbuf *j, const u8 *der, size_t n, int written)
{
    static const char digits[] = "0123456789abcdef";
    char *hex = (char *)mg_alloc(n * 2 + 1);
    size_t k;
    for (k = 0; k < n; k++) {
        hex[2 * k] = digits[der[k] >> 4];
        hex[2 * k + 1] = digits[der[k] & 15];
    }
    hex[2 * n] = 0;
    if (written) mg_jraw(j, ",");
    mg_jstr(j, hex);
    free(hex);
}

static int certs_of_block(const mg_zip *z, mg_jbuf *j)
{
    u8 *tail, foot[24], *blk = NULL;
    size_t tn = (size_t)(z->size < 65557 ? z->size : 65557), i;
    long eocd = -1;
    u64 cd, size;
    cspan b, v2 = {0}, v3 = {0}, s, signers, signer, signed_data, digests, certs, c;
    int written = 0;
    if (tn < 22) return 0;
    tail = (u8 *)mg_alloc(tn);
    if (!mg_zip_pread(z, z->size - tn, tail, tn)) goto done;
    for (i = tn - 22 + 1; i-- > 0;)
        if (cle32(tail + i) == 0x06054b50u) {
            eocd = (long)i;
            break;
        }
    if (eocd < 0) goto done;
    cd = cle32(tail + eocd + 16);
    if (cd < 32 || !mg_zip_pread(z, cd - 24, foot, 24)) goto done;
    if (memcmp(foot + 8, "APK Sig Block 42", 16)) goto done;
    size = cle64(foot);
    if (size < 24 || size > cd || size > (16u << 20)) goto done;
    blk = (u8 *)mg_alloc((size_t)size - 24);
    if (!mg_zip_pread(z, cd - size, blk, (size_t)size - 24)) goto done;
    b.p = blk;
    b.n = (size_t)size - 24;
    while (b.n >= 12) {
        u64 plen = cle64(b.p);
        u32 id = cle32(b.p + 8);
        if (plen < 4 || plen - 4 > b.n - 12) break;
        if (id == 0x7109871au) { v2.p = b.p + 12; v2.n = (size_t)plen - 4; }
        if (id == 0xf05368c0u) { v3.p = b.p + 12; v3.n = (size_t)plen - 4; }
        b.p += 12 + (size_t)plen - 4;
        b.n -= 12 + (size_t)plen - 4;
    }
    s = v3.p ? v3 : v2;
    if (!s.p || !cspan_lp(&s, &signers) || !cspan_lp(&signers, &signer) || !cspan_lp(&signer, &signed_data) ||
        !cspan_lp(&signed_data, &digests) || !cspan_lp(&signed_data, &certs))
        goto done;
    while (certs.n && cspan_lp(&certs, &c)) cert_hex(j, c.p, c.n, written++);
done:
    free(blk);
    free(tail);
    return written;
}

/* One DER element off the front of `s`: its tag, its contents, and the whole of it. */
static bool der_next(cspan *s, u8 *tag, cspan *val, cspan *whole)
{
    size_t len, head = 2, k;
    if (s->n < 2) return false;
    *tag = s->p[0];
    len = s->p[1];
    if (len & 0x80) {
        size_t m = len & 0x7f;
        if (m == 0 || m > 4 || s->n < 2 + m) return false;
        for (len = 0, k = 0; k < m; k++) len = len << 8 | s->p[2 + k];
        head += m;
    }
    if (len > s->n - head) return false;
    if (val) { val->p = s->p + head; val->n = len; }
    if (whole) { whole->p = s->p; whole->n = head + len; }
    s->p += head + len;
    s->n -= head + len;
    return true;
}

typedef struct v1_find {
    const mg_zip_entry *e;
} v1_find;

/* Whether `s` is `upper` (letters in capitals) in either case. */
static bool same_ascii(const char *s, const char *upper)
{
    for (; *s && *upper; s++, upper++)
        if ((*s >= 'a' && *s <= 'z' ? *s - 32 : *s) != *upper) return false;
    return !*s && !*upper;
}

static void v1_each(const mg_zip_entry *e, void *user)
{
    v1_find *f = (v1_find *)user;
    const char *dot = strrchr(e->name, '.');
    if (f->e || !dot || strlen(e->name) < 10 || strchr(e->name + 9, '/')) return;
    if (same_ascii(dot, ".RSA") || same_ascii(dot, ".DSA") || same_ascii(dot, ".EC")) f->e = e;
}

static int certs_of_v1(const mg_zip *z, mg_jbuf *j)
{
    v1_find f = {0};
    size_t n = 0;
    u8 *p7, tag;
    cspan s, ci, sd, x, whole;
    int written = 0;
    mg_zip_each(z, "META-INF/", v1_each, &f);
    if (!f.e || !(p7 = mg_zip_read(z, f.e, 1u << 20, &n))) return 0;
    s.p = p7;
    s.n = n;
    /* ContentInfo { contentType, [0] SignedData { version, digestAlgorithms, encapContentInfo, [0] certificates, ... } } */
    if (der_next(&s, &tag, &ci, NULL) && tag == 0x30 && der_next(&ci, &tag, NULL, NULL) && tag == 0x06 &&
        der_next(&ci, &tag, &x, NULL) && tag == 0xa0 && der_next(&x, &tag, &sd, NULL) && tag == 0x30 &&
        der_next(&sd, &tag, NULL, NULL) && der_next(&sd, &tag, NULL, NULL) && der_next(&sd, &tag, NULL, NULL)) {
        cspan certs;
        if (der_next(&sd, &tag, &certs, NULL) && tag == 0xa0)
            while (certs.n && der_next(&certs, &tag, NULL, &whole))
                if (tag == 0x30) cert_hex(j, whole.p, whole.n, written++);
    }
    free(p7);
    return written;
}

bool megler_apk_describe_activities(const char *apk_path, const char *splits_dir, const char *out_dir)
{
    char **splits = NULL;
    int n = mg_apk_list_splits(splits_dir, &splits), k;
    icon_apks *a = apks_open(apk_path, splits, n);
    acts_ctx *c = (acts_ctx *)mg_alloc(sizeof(*c));
    const mg_zip_entry *me = a->n ? mg_zip_find(&a->zips[0], "AndroidManifest.xml") : NULL;
    size_t msize = 0;
    u8 *manifest = me ? mg_zip_read(&a->zips[0], me, 16u << 20, &msize) : NULL;
    xml_walker w;
    bool ok = false;
    char *path;
    c->apks = a;
    w.start = acts_start;
    w.end = acts_end;
    w.user = c;
    if (manifest && xml_walk(manifest, (u32)msize, &w) && c->began) {
        if (!c->listed) {
            mg_jkey(&c->out, "activities", false);
            mg_jraw(&c->out, "[");
        }
        mg_jraw(&c->out, "]");
        mg_jkey(&c->out, "certificates", false);
        mg_jraw(&c->out, "[");
        if (!certs_of_block(&a->zips[0], &c->out)) certs_of_v1(&a->zips[0], &c->out);
        mg_jraw(&c->out, "]}");
        mg_path_mkdirs(out_dir);
        path = mg_sprintf("%s/activities.json", out_dir);
        ok = mg_file_write_all(path, c->out.s, c->out.n);
        free(path);
        for (k = 0; ok && k < c->nicons; k++) {
            size_t size = 0;
            u8 *png = icon_of_id(a, c->icons[k], &size);
            if (!png) continue;
            path = mg_sprintf("%s/%08x.png", out_dir, (unsigned)c->icons[k]);
            mg_file_write_all(path, png, size);
            free(path);
            free(png);
        }
    }
    free(manifest);
    jbuf_reset(&c->out);
    jbuf_reset(&c->act);
    jbuf_reset(&c->filters);
    free(c);
    apks_close(a);
    mg_apk_free_list(splits, n);
    return ok;
}
