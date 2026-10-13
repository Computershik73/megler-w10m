/*
 * Reading an APK: the binary AndroidManifest.xml, enough of resources.arsc to
 * turn "@string/app_name" into text and "@mipmap/ic_launcher" into a file, and
 * the native libraries' ELF headers. All of it is untrusted input; every
 * offset is checked against the buffer it came from.
 */
#include "hk_apk.h"

#include <stdio.h>

#include "../../include/megler.h"
#include "../bionic/hk_bionic.h"
#include "../drivers/hk_driver.h"
#include "../hk_core.h"
#include "../elf/hk_elf.h"
#include "../ndk/hk_egl.h"
#include "../zip/hk_zip.h"

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
    if (avail < 28 || hk_rd16(chunk) != 0x0001) return false;
    p->base = chunk;
    p->size = hk_rd32(chunk + 4);
    if (p->size > avail || p->size < 28) return false;
    p->count = hk_rd32(chunk + 8);
    p->utf8 = (hk_rd32(chunk + 16) & 0x100) != 0;
    p->strings_start = hk_rd32(chunk + 20);
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
    off = hk_rd32(p->base + hk_rd16(p->base + 2) + index * 4);
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
        len = hk_rd16(s);
        s += 2;
        if (len & 0x8000) {
            if (s + 2 > end) return false;
            len = ((len & 0x7FFF) << 16) | hk_rd16(s);
            s += 2;
        }
        if (len > (u32)(end - s) / 2) return false;
        for (i = 0; i < len && n + 4 < cap; i++) {
            u32 c = hk_rd16(s + i * 2);
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
                u32 lo = hk_rd16(s + i * 2 + 2);
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

typedef struct arsc {
    const u8 *data;
    u32 size;
    pool strings;
} arsc;

static bool arsc_open(arsc *a, const u8 *data, u32 size)
{
    memset(a, 0, sizeof(*a));
    if (!data || size < 12 || hk_rd16(data) != 0x0002) return false;
    a->data = data;
    a->size = hk_rd32(data + 4) < size ? hk_rd32(data + 4) : size;
    if (hk_rd16(data + 2) + 28u > a->size) return false;
    return pool_open(&a->strings, data + hk_rd16(data + 2), a->size - hk_rd16(data + 2));
}

/*
 * The value of resource `id` as text: a string resource's string, or a file
 * resource's path. Among configurations, `want_file` prefers the densest one
 * whose value ends in .png or .webp (an icon); otherwise the default
 * configuration is preferred. False when the resource is not a string.
 */
static bool arsc_string(const arsc *a, u32 id, bool want_file, char *out, size_t cap)
{
    u32 pkg_id = id >> 24, type_id = (id >> 16) & 0xFF, entry = id & 0xFFFF;
    u32 off, best_score = 0;
    bool found = false;
    if (!a->data) return false;
    off = hk_rd16(a->data + 2) + a->strings.size;
    while (off + 8 <= a->size) {
        const u8 *pk = a->data + off;
        u32 psize = hk_rd32(pk + 4), phead = hk_rd16(pk + 2), at;
        if (psize < 8 || psize > a->size - off) break;
        if (hk_rd16(pk) == 0x0200 && phead >= 284 && hk_rd32(pk + 8) == pkg_id) {
            for (at = phead; at + 8 <= psize;) {
                const u8 *ch = pk + at;
                u32 csize = hk_rd32(ch + 4), chead = hk_rd16(ch + 2);
                if (csize < 8 || csize > psize - at) break;
                if (hk_rd16(ch) == 0x0201 && chead >= 20 && ch[8] == type_id) {
                    u32 count = hk_rd32(ch + 12), start = hk_rd32(ch + 16), eoff = 0xFFFFFFFFu, flags = ch[9];
                    u32 cfg_size = chead >= 24 ? hk_rd32(ch + 20) : 0, density = cfg_size >= 16 && chead >= 36 ? hk_rd16(ch + 20 + 14) : 0;
                    bool is_default = true;
                    u32 k;
                    /* The default configuration is all zeros after its own size field. */
                    for (k = 4; k < cfg_size && 20 + k < chead; k++)
                        if (ch[20 + k]) is_default = false;
                    if (flags & 1) {
                        /* Sparse: (index, offset / 4) pairs of 16 bits each. */
                        for (k = 0; k < count && chead + k * 4 + 4 <= csize; k++)
                            if (hk_rd16(ch + chead + k * 4) == entry) eoff = (u32)hk_rd16(ch + chead + k * 4 + 2) * 4;
                    } else if (flags & 2) {
                        if (entry < count && chead + entry * 2 + 2 <= csize) {
                            u32 v = hk_rd16(ch + chead + entry * 2);
                            eoff = v == 0xFFFF ? 0xFFFFFFFFu : v * 4;
                        }
                    } else if (entry < count && chead + entry * 4 + 4 <= csize) {
                        eoff = hk_rd32(ch + chead + entry * 4);
                    }
                    if (eoff != 0xFFFFFFFFu && start <= csize && eoff <= csize - start && csize - start - eoff >= 16) {
                        const u8 *e = ch + start + eoff;
                        u32 esize = hk_rd16(e), eflags = hk_rd16(e + 2);
                        if (!(eflags & 1) && esize >= 8 && esize + 8 <= csize - start - eoff && e[esize + 3] == 0x03) {
                            char text[200];
                            if (pool_get(&a->strings, hk_rd32(e + esize + 4), text, sizeof(text))) {
                                size_t n = strlen(text);
                                bool png = n > 4 && !strcmp(text + n - 4, ".png"), webp = n > 5 && !strcmp(text + n - 5, ".webp");
                                u32 score;
                                /* Any PNG before any WebP (the phone has no WebP decoder), then the densest. */
                                if (want_file) score = png || webp ? (png ? 100000 : 1000) + (density == 0xFFFE || density == 0xFFFF ? 0 : density) : 1;
                                else score = is_default ? 1000 : 1;
                                if (score > best_score) {
                                    best_score = score;
                                    hk_strlcpy(out, text, cap);
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
    return found;
}

/* ----------------------------------------------------- AndroidManifest.xml */

/* The attributes this layer reads, by their resource ids (names can be stripped from the string pool; the ids cannot). */
#define ATTR_LABEL 0x01010001u
#define ATTR_ICON 0x01010002u
#define ATTR_NAME 0x01010003u
#define ATTR_HAS_CODE 0x0101000cu
#define ATTR_SCREEN_ORIENTATION 0x0101001eu
#define ATTR_VALUE 0x01010024u
#define ATTR_TARGET_ACTIVITY 0x01010202u
#define ATTR_MIN_SDK 0x0101020cu
#define ATTR_VERSION_CODE 0x0101021bu
#define ATTR_VERSION_NAME 0x0101021cu
#define ATTR_TARGET_SDK 0x01010270u

typedef struct attr {
    u32 id;                /* 0 when the attribute has no resource id */
    char name[48];
    u8 type;
    u32 data;
    char text[200];        /* a string value */
} attr;

typedef struct manifest_ctx {
    hk_apk_info *info;
    const arsc *res;
    pool strings;
    const u8 *resmap;
    u32 resmap_count;
    /* where the parser is */
    bool in_application, in_activity, in_filter;
    char cur_activity[200], cur_lib[96], cur_func[96];
    bool cur_main, cur_launcher, found_launcher;
    int cur_orientation;
    /* How each activity seen so far asks to be held, for an alias that comes after its target and has no say of its own. */
    struct {
        u32 name_hash;
        int orientation;
    } seen[96];
    int nseen;
    hk_apk_meta act_meta[16];
    int act_nmeta;
    u32 icon_ref, label_ref;
} manifest_ctx;

static void full_class_name(const hk_apk_info *info, const char *name, char *out, size_t cap)
{
    if (name[0] == '.') snprintf(out, cap, "%s%s", info->package, name);
    else if (!strchr(name, '.') && name[0]) snprintf(out, cap, "%s.%s", info->package, name);
    else hk_strlcpy(out, name, cap);
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
    case 0: case 6: case 8: case 11: return HK_APK_LANDSCAPE;
    case 1: case 7: case 9: case 12: return HK_APK_PORTRAIT;
    default: return HK_APK_UNSPECIFIED;
    }
}

static void add_meta(hk_apk_meta *list, int *n, int cap, const char *name, const char *value)
{
    if (*n >= cap || !name[0]) return;
    hk_strlcpy(list[*n].name, name, sizeof(list[*n].name));
    hk_strlcpy(list[*n].value, value, sizeof(list[*n].value));
    ++*n;
}

static void start_element(manifest_ctx *c, const char *tag, const attr *attrs, int nattrs)
{
    hk_apk_info *info = c->info;
    const attr *name = NULL, *value = NULL;
    int i;
    for (i = 0; i < nattrs; i++) {
        if (attrs[i].id == ATTR_NAME || (!attrs[i].id && !strcmp(attrs[i].name, "name"))) name = &attrs[i];
        if (attrs[i].id == ATTR_VALUE || (!attrs[i].id && !strcmp(attrs[i].name, "value"))) value = &attrs[i];
    }
    if (!strcmp(tag, "manifest")) {
        for (i = 0; i < nattrs; i++) {
            if (!attrs[i].id && !strcmp(attrs[i].name, "package")) hk_strlcpy(info->package, attrs[i].text, sizeof(info->package));
            if (attrs[i].id == ATTR_VERSION_CODE) info->version_code = (int)attrs[i].data;
            if (attrs[i].id == ATTR_VERSION_NAME) hk_strlcpy(info->version_name, attrs[i].text, sizeof(info->version_name));
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
                if (attrs[i].type == 0x03) hk_strlcpy(info->label, attrs[i].text, sizeof(info->label));
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
        c->cur_orientation = HK_APK_UNSPECIFIED;
        if (name) full_class_name(info, name->text, c->cur_activity, sizeof(c->cur_activity));
        for (i = 0; i < nattrs; i++) {
            if (attrs[i].id == ATTR_SCREEN_ORIENTATION) c->cur_orientation = orientation_of((i32)attrs[i].data);
            if (attrs[i].id == ATTR_TARGET_ACTIVITY && !strcmp(tag, "activity-alias")) {
                /* An alias is held as the activity it stands for is. */
                char target[200];
                u32 h;
                int k;
                full_class_name(info, attrs[i].text, target, sizeof(target));
                h = name_hash(target);
                for (k = 0; k < c->nseen; k++)
                    if (c->seen[k].name_hash == h) c->cur_orientation = c->seen[k].orientation;
            }
        }
        if (!strcmp(tag, "activity") && c->nseen < (int)HK_COUNT(c->seen)) {
            c->seen[c->nseen].name_hash = name_hash(c->cur_activity);
            c->seen[c->nseen++].orientation = c->cur_orientation;
        }
    } else if (!strcmp(tag, "intent-filter")) {
        c->in_filter = true;
    } else if (c->in_filter && c->in_activity && name) {
        if (!strcmp(tag, "action") && !strcmp(name->text, "android.intent.action.MAIN")) c->cur_main = true;
        if (!strcmp(tag, "category") && (!strcmp(name->text, "android.intent.category.LAUNCHER") || !strcmp(name->text, "android.intent.category.LEANBACK_LAUNCHER")))
            c->cur_launcher = true;
    } else if (!strcmp(tag, "meta-data") && name) {
        char text[200];
        if (value && value->type == 0x03) hk_strlcpy(text, value->text, sizeof(text));
        else if (value && value->type == 0x12) hk_strlcpy(text, value->data ? "true" : "false", sizeof(text));
        else if (value) snprintf(text, sizeof(text), "%d", (int)value->data);
        else text[0] = 0;
        if (c->in_activity) {
            if (!strcmp(name->text, "android.app.lib_name")) hk_strlcpy(c->cur_lib, text, sizeof(c->cur_lib));
            if (!strcmp(name->text, "android.app.func_name")) hk_strlcpy(c->cur_func, text, sizeof(c->cur_func));
            add_meta(c->act_meta, &c->act_nmeta, (int)HK_COUNT(c->act_meta), name->text, text);
        } else if (c->in_application) {
            add_meta(info->meta, &info->nmeta, (int)HK_COUNT(info->meta), name->text, text);
        }
    }
}

static void end_element(manifest_ctx *c, const char *tag)
{
    hk_apk_info *info = c->info;
    int i;
    if (!strcmp(tag, "intent-filter")) c->in_filter = false;
    else if (!strcmp(tag, "application")) c->in_application = false;
    else if (!strcmp(tag, "activity") || !strcmp(tag, "activity-alias")) {
        /* The first activity that is both MAIN and LAUNCHER is the app; failing that, the first MAIN one. */
        if (c->cur_main && (!c->found_launcher) && (c->cur_launcher || !info->activity[0])) {
            hk_strlcpy(info->activity, c->cur_activity, sizeof(info->activity));
            info->orientation = c->cur_orientation;
            hk_strlcpy(info->native_library, c->cur_lib, sizeof(info->native_library));
            hk_strlcpy(info->func_name, c->cur_func, sizeof(info->func_name));
            for (i = 0; i < c->act_nmeta; i++) add_meta(info->meta, &info->nmeta, (int)HK_COUNT(info->meta), c->act_meta[i].name, c->act_meta[i].value);
            if (c->cur_launcher) c->found_launcher = true;
        }
        c->in_activity = false;
    }
}

static bool parse_manifest(hk_apk_info *info, const arsc *res, const u8 *xml, u32 size)
{
    manifest_ctx c;
    u32 off;
    memset(&c, 0, sizeof(c));
    c.info = info;
    c.res = res;
    info->has_code = true;
    if (size < 8 || hk_rd16(xml) != 0x0003) return false;
    off = hk_rd16(xml + 2);
    while (off + 8 <= size) {
        const u8 *ch = xml + off;
        u32 type = hk_rd16(ch), head = hk_rd16(ch + 2), csize = hk_rd32(ch + 4);
        if (csize < 8 || csize > size - off || head > csize) break;
        if (type == 0x0001) {
            pool_open(&c.strings, ch, csize);
        } else if (type == 0x0180) {
            c.resmap = ch + head;
            c.resmap_count = (csize - head) / 4;
        } else if (type == 0x0102 && csize >= 36) {
            char tag[64];
            attr attrs[24];
            u32 astart = hk_rd16(ch + 24), asize = hk_rd16(ch + 26), acount = hk_rd16(ch + 28), i;
            int n = 0;
            pool_get(&c.strings, hk_rd32(ch + 20), tag, sizeof(tag));
            for (i = 0; i < acount && n < (int)HK_COUNT(attrs) && asize >= 20; i++) {
                const u8 *a = ch + 16 + astart + i * asize;
                u32 name_index;
                attr *out = &attrs[n];
                if (a + 20 > ch + csize) break;
                name_index = hk_rd32(a + 4);
                memset(out, 0, sizeof(*out));
                pool_get(&c.strings, name_index, out->name, sizeof(out->name));
                if (name_index < c.resmap_count) out->id = hk_rd32(c.resmap + name_index * 4);
                out->type = a[15];
                out->data = hk_rd32(a + 16);
                if (out->type == 0x03) pool_get(&c.strings, out->data, out->text, sizeof(out->text));
                else if (hk_rd32(a + 8) != 0xFFFFFFFFu) pool_get(&c.strings, hk_rd32(a + 8), out->text, sizeof(out->text));
                n++;
            }
            start_element(&c, tag, attrs, n);
        } else if (type == 0x0103 && csize >= 24) {
            char tag[64];
            pool_get(&c.strings, hk_rd32(ch + 20), tag, sizeof(tag));
            end_element(&c, tag);
        }
        off += csize;
    }
    if (!info->label[0] && c.label_ref) arsc_string(res, c.label_ref, false, info->label, sizeof(info->label));
    if (c.icon_ref) arsc_string(res, c.icon_ref, true, info->icon_path, sizeof(info->icon_path));
    return info->package[0] != 0;
}

/* ------------------------------------------------------------- libraries */

static void note(hk_apk_info *info, const char *fmt, ...)
{
    va_list ap;
    if (info->nnotes >= (int)HK_COUNT(info->notes)) return;
    va_start(ap, fmt);
    vsnprintf(info->notes[info->nnotes++], sizeof(info->notes[0]), fmt, ap);
    va_end(ap);
}

typedef struct lib_walk {
    const hk_zip *zip;
    hk_apk_info *info;
    const char *prefix;
    bool has[16];
    char first_lib[96];
    int count;
} lib_walk;

enum { L_UNITY, L_UE4, L_COCOS, L_GODOT, L_SDL, L_MAIN, L_GDX, L_FLUTTER, L_MONO, L_IL2CPP, L_VULKAN_ONLY };

static void lib_seen(const hk_zip_entry *e, void *user)
{
    lib_walk *w = (lib_walk *)user;
    const char *name = e->name + strlen(w->prefix);
    if (strchr(name, '/') || strlen(name) < 4 || strcmp(name + strlen(name) - 3, ".so")) return;
    w->count++;
    if (!w->first_lib[0]) hk_strlcpy(w->first_lib, name, sizeof(w->first_lib));
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
}

static void abi_seen(const hk_zip_entry *e, void *user)
{
    hk_apk_info *info = (hk_apk_info *)user;
    const char *abi = e->name + 4, *slash = strchr(abi, '/');
    char name[24];
    int i;
    if (!slash || (size_t)(slash - abi) >= sizeof(name) || strlen(slash) < 5 || strcmp(slash + strlen(slash) - 3, ".so")) return;
    memcpy(name, abi, (size_t)(slash - abi));
    name[slash - abi] = 0;
    for (i = 0; i < info->nabis; i++)
        if (!strcmp(info->abis[i], name)) return;
    if (info->nabis < (int)HK_COUNT(info->abis)) hk_strlcpy(info->abis[info->nabis++], name, sizeof(info->abis[0]));
}

static void classify(hk_apk_info *info, const hk_zip *z)
{
    lib_walk w;
    char prefix[40];
    int i;
    bool v7 = false, v5 = false, any64 = false;
    hk_zip_each(z, "lib/", abi_seen, info);
    for (i = 0; i < info->nabis; i++) {
        if (!strcmp(info->abis[i], "armeabi-v7a")) v7 = true;
        else if (!strcmp(info->abis[i], "armeabi")) v5 = true;
        else if (!strcmp(info->abis[i], "arm64-v8a")) any64 = true;
    }
    if (v7) strcpy(info->abi, "armeabi-v7a");
    else if (v5) strcpy(info->abi, "armeabi");

    memset(&w, 0, sizeof(w));
    w.zip = z;
    w.info = info;
    if (info->abi[0]) {
        snprintf(prefix, sizeof(prefix), "lib/%s/", info->abi);
        w.prefix = prefix;
        hk_zip_each(z, prefix, lib_seen, &w);
    }

    if (w.has[L_UNITY]) strcpy(info->engine, "unity");
    else if (w.has[L_UE4]) strcpy(info->engine, "unreal");
    else if (w.has[L_GODOT]) strcpy(info->engine, "godot");
    else if (w.has[L_COCOS]) strcpy(info->engine, "cocos2d-x");
    else if (w.has[L_FLUTTER]) strcpy(info->engine, "flutter");
    else if (!strcmp(info->activity, "android.app.NativeActivity") || (!info->has_code && w.count) || info->native_library[0]) strcpy(info->engine, "native-activity");
    else if (w.has[L_SDL]) strcpy(info->engine, "sdl");
    else if (w.has[L_GDX]) strcpy(info->engine, "libgdx");
    /*
     * Everything else that has bytecode and an activity to start is an ordinary Android app: Java, which loads what
     * native libraries it has itself (System.loadLibrary) when it wants them. The Java driver runs it.
     */
    else if (info->activity[0] && hk_zip_find(z, "classes.dex")) strcpy(info->engine, "java");
    else strcpy(info->engine, "unknown");
    info->supported = hk_driver_supported(info->engine);

    /* A NativeActivity with no android.app.lib_name loads "main". */
    if (!strcmp(info->engine, "native-activity") && !info->native_library[0]) strcpy(info->native_library, "main");

    if (!strcmp(info->engine, "java") && info->supported && (!info->nabis || info->abi[0])) {
        /*
         * Java, with no native libraries or with ones this processor can run: the Java driver runs it on the Dalvik runtime,
         * if the files for that are installed (the driver says when they are not). One whose libraries are all for other
         * processors falls through to the sentence about that.
         */
        info->runnable = true;
    } else if (!info->nabis) {
        info->runnable = false;
        hk_strlcpy(info->reason, "This app has no native code: it is written in Java only, which this layer does not run yet.", sizeof(info->reason));
    } else if (!info->abi[0]) {
        info->runnable = false;
        snprintf(info->reason, sizeof(info->reason), "This APK has no 32-bit ARM code (only %s). Windows 10 Mobile is a 32-bit system and cannot run it; look for a version of the app that still includes armeabi-v7a.",
                 any64 ? "arm64-v8a" : info->abis[0]);
    } else if (!info->supported) {
        info->runnable = false;
        if (!strcmp(info->engine, "unknown"))
            hk_strlcpy(info->reason, "This app's native code is started and driven by its own Java code, which this layer does not run yet. Apps built on NativeActivity are the ones that start today.", sizeof(info->reason));
        else
            snprintf(info->reason, sizeof(info->reason), "This app is built on %s, and there is no driver for that engine yet.", info->engine);
    } else {
        info->runnable = true;
    }
    if (!strcmp(info->abi, "armeabi")) note(info, "Only ARMv5 (armeabi) code: it runs, with floating point done in software as the app was built.");
    /* Only where the device's probe has said that a thread does not stay in ARM state. */
    if (w.has[L_MONO] && hk_core_arm_state() < 0) note(info, "Unity with Mono: its JIT writes ARM-mode code at run time, which this phone does not keep running.");
    if (!strcmp(info->engine, "unity"))
        note(info, "The Unity driver is new and unfinished: the engine and the game's code start, and no Unity game has been seen to draw on a phone yet.");
    if (!strcmp(info->engine, "native-activity") && info->has_code && strcmp(info->activity, "android.app.NativeActivity"))
        note(info, "Its activity (%s) is the app's own Java class on top of NativeActivity. That Java code is not run here: the app starts, and gets as far as it can without it.",
             info->activity);
}

bool hk_apk_read_info(const char *apk_path, hk_apk_info *info)
{
    hk_zip z;
    const hk_zip_entry *me, *re;
    u8 *manifest, *resources = NULL;
    size_t msize = 0, rsize = 0;
    arsc res;
    memset(info, 0, sizeof(*info));
    if (!hk_zip_open(&z, apk_path, info->error, sizeof(info->error))) return false;
    me = hk_zip_find(&z, "AndroidManifest.xml");
    if (!me) {
        hk_strlcpy(info->error, "not an APK: there is no AndroidManifest.xml in it", sizeof(info->error));
        hk_zip_close(&z);
        return false;
    }
    manifest = hk_zip_read(&z, me, 16u << 20, &msize);
    re = hk_zip_find(&z, "resources.arsc");
    if (re) resources = hk_zip_read(&z, re, 64u << 20, &rsize);
    arsc_open(&res, resources, (u32)rsize);
    if (!manifest || !parse_manifest(info, &res, manifest, (u32)msize)) {
        hk_strlcpy(info->error, "the APK's manifest cannot be read", sizeof(info->error));
        free(manifest);
        free(resources);
        hk_zip_close(&z);
        return false;
    }
    free(manifest);
    free(resources);
    if (!info->label[0]) {
        const char *dot = strrchr(info->package, '.');
        hk_strlcpy(info->label, dot ? dot + 1 : info->package, sizeof(info->label));
    }
    /* An icon the resources did not lead to: the usual names, densest first. */
    if (!info->icon_path[0] || !hk_zip_find(&z, info->icon_path)) {
        static const char *const dens[] = { "xxxhdpi", "xxhdpi", "xhdpi", "hdpi", "mdpi" };
        static const char *const kinds[] = { "mipmap", "drawable" };
        static const char *const names[] = { "ic_launcher", "icon", "app_icon", "ic_launcher_round" };
        static const char *const sufs[] = { "-v4", "" };
        static const char *const exts[] = { "png", "webp" };
        int d, k, n, s, x;
        info->icon_path[0] = 0;
        for (d = 0; d < 5 && !info->icon_path[0]; d++)
            for (k = 0; k < 2 && !info->icon_path[0]; k++)
                for (n = 0; n < 4 && !info->icon_path[0]; n++)
                    for (s = 0; s < 2 && !info->icon_path[0]; s++)
                        for (x = 0; x < 2; x++) {
                            char path[200];
                            snprintf(path, sizeof(path), "res/%s-%s%s/%s.%s", kinds[k], dens[d], sufs[s], names[n], exts[x]);
                            if (hk_zip_find(&z, path)) {
                                hk_strlcpy(info->icon_path, path, sizeof(info->icon_path));
                                break;
                            }
                        }
    }
    classify(info, &z);
    hk_zip_close(&z);
    return true;
}

void hk_apk_info_free(hk_apk_info *info) { (void)info; }

const char *hk_apk_meta_value(const hk_apk_info *info, const char *name)
{
    int i;
    for (i = 0; i < info->nmeta; i++)
        if (!strcmp(info->meta[i].name, name)) return info->meta[i].value;
    return NULL;
}

/* ------------------------------------------------------------------ report */

typedef struct jbuf {
    char *s;
    size_t n, cap;
} jbuf;

static void jraw(jbuf *j, const char *text)
{
    size_t len = strlen(text);
    if (j->n + len + 1 > j->cap) {
        j->cap = (j->n + len + 1) * 2;
        j->s = (char *)hk_realloc(j->s, j->cap);
    }
    memcpy(j->s + j->n, text, len + 1);
    j->n += len;
}

static void jstr(jbuf *j, const char *text)
{
    char tmp[8];
    jraw(j, "\"");
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
        jraw(j, tmp);
    }
    jraw(j, "\"");
}

static void jkey(jbuf *j, const char *key, bool first)
{
    if (!first) jraw(j, ",");
    jstr(j, key);
    jraw(j, ":");
}

static void jint(jbuf *j, long v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%ld", v);
    jraw(j, tmp);
}

typedef struct lib_report {
    jbuf *j;
    const hk_zip *zip;
    const char *prefix;
    bool first;
    hk_apk_info *info;
    u32 arm_total, thumb_total;
} lib_report;

/* Where a virtual address is in the file, by the program headers; NULL when it is not in the file. */
static const u8 *elf_at(const u8 *d, size_t n, u32 vaddr, u32 need)
{
    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)d;
    const Elf32_Phdr *ph = (const Elf32_Phdr *)(d + eh->e_phoff);
    int i;
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD || vaddr < ph[i].p_vaddr || vaddr - ph[i].p_vaddr > ph[i].p_filesz) continue;
        if (need > ph[i].p_filesz - (vaddr - ph[i].p_vaddr)) return NULL;
        if (ph[i].p_offset > n || ph[i].p_filesz > n - ph[i].p_offset) return NULL;
        return d + ph[i].p_offset + (vaddr - ph[i].p_vaddr);
    }
    return NULL;
}

static void lib_report_one(const hk_zip_entry *e, void *user)
{
    lib_report *r = (lib_report *)user;
    jbuf *j = r->j;
    const char *name = e->name + strlen(r->prefix);
    const u8 *d;
    u8 *owned = NULL;
    size_t n = 0;
    u32 arm = 0, thumb = 0, missing = 0;
    bool parsed = false;
    if (strchr(name, '/') || strlen(name) < 4 || strcmp(name + strlen(name) - 3, ".so")) return;
    if (!r->first) jraw(j, ",");
    r->first = false;
    jraw(j, "{");
    jkey(j, "name", true);
    jstr(j, name);
    jkey(j, "size", false);
    jint(j, (long)e->usize);
    jkey(j, "needs", false);
    jraw(j, "[");

    owned = hk_zip_read(r->zip, e, 512u << 20, &n);
    d = owned;
    if (d && n > sizeof(Elf32_Ehdr) && !memcmp(d, "\177ELF", 4) && d[4] == 1) {
        const Elf32_Ehdr *eh = (const Elf32_Ehdr *)d;
        if (eh->e_machine == EM_ARM && eh->e_phentsize == sizeof(Elf32_Phdr) && eh->e_phoff <= n && (size_t)eh->e_phnum * sizeof(Elf32_Phdr) <= n - eh->e_phoff) {
            const Elf32_Phdr *ph = (const Elf32_Phdr *)(d + eh->e_phoff);
            const Elf32_Dyn *dyn = NULL;
            u32 ndyn = 0, symtab = 0, strtab = 0, strsz = 0, hash = 0, gnu = 0, nsyms = 0, k;
            int i;
            bool first_need = true;
            for (i = 0; i < eh->e_phnum; i++) {
                if (ph[i].p_type == PT_DYNAMIC && ph[i].p_offset <= n && ph[i].p_filesz <= n - ph[i].p_offset) {
                    dyn = (const Elf32_Dyn *)(d + ph[i].p_offset);
                    ndyn = ph[i].p_filesz / sizeof(Elf32_Dyn);
                }
            }
            for (k = 0; k < ndyn && dyn[k].d_tag != DT_NULL; k++) {
                if (dyn[k].d_tag == DT_SYMTAB) symtab = dyn[k].d_val;
                if (dyn[k].d_tag == DT_STRTAB) strtab = dyn[k].d_val;
                if (dyn[k].d_tag == DT_STRSZ) strsz = dyn[k].d_val;
                if (dyn[k].d_tag == DT_HASH) hash = dyn[k].d_val;
                if (dyn[k].d_tag == DT_GNU_HASH) gnu = dyn[k].d_val;
            }
            if (strtab && strsz) {
                const char *str = (const char *)elf_at(d, n, strtab, strsz);
                if (str && str[strsz - 1] == 0) {
                    for (k = 0; k < ndyn && dyn[k].d_tag != DT_NULL; k++) {
                        if (dyn[k].d_tag != DT_NEEDED || dyn[k].d_val >= strsz) continue;
                        if (!first_need) jraw(j, ",");
                        first_need = false;
                        jstr(j, str + dyn[k].d_val);
                    }
                    /* How many symbols: the SysV table says outright; the GNU one by its last chain. */
                    if (hash) {
                        const u8 *h = elf_at(d, n, hash, 8);
                        if (h) nsyms = hk_rd32(h + 4);
                    }
                    if (!nsyms && gnu) {
                        const u8 *g = elf_at(d, n, gnu, 16);
                        if (g) {
                            u32 nb = hk_rd32(g), symoff = hk_rd32(g + 4), bloom = hk_rd32(g + 8), top = 0, b;
                            const u8 *buckets = nb < 0x1000000 && bloom < 0x1000000 ? elf_at(d, n, gnu + 16 + bloom * 4, nb * 4) : NULL;
                            if (buckets) {
                                for (b = 0; b < nb; b++)
                                    if (hk_rd32(buckets + b * 4) > top) top = hk_rd32(buckets + b * 4);
                                nsyms = symoff;
                                if (top >= symoff) {
                                    for (;; top++) {
                                        const u8 *chain = elf_at(d, n, gnu + 16 + bloom * 4 + nb * 4 + (top - symoff) * 4, 4);
                                        if (!chain) break;
                                        if (hk_rd32(chain) & 1) {
                                            nsyms = top + 1;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (symtab && nsyms && nsyms < 4000000) {
                        const Elf32_Sym *sym = (const Elf32_Sym *)elf_at(d, n, symtab, nsyms * sizeof(Elf32_Sym));
                        for (k = 1; sym && k < nsyms; k++) {
                            const char *sn;
                            if (sym[k].st_name >= strsz) continue;
                            sn = str + sym[k].st_name;
                            if (sym[k].st_shndx == SHN_UNDEF) {
                                /* An import: the system's, or (not checked here) another library's of the app. */
                                if (ELF_ST_BIND(sym[k].st_info) != STB_WEAK && !hk_bionic_find(sn) && !hk_egl_is_gl_name(sn)) missing++;
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
    jraw(j, "]");
    if (parsed) {
        jkey(j, "armFunctions", false);
        jint(j, (long)arm);
        jkey(j, "thumbFunctions", false);
        jint(j, (long)thumb);
        jkey(j, "importsNotInSystem", false);
        jint(j, (long)missing);
        r->arm_total += arm;
        r->thumb_total += thumb;
    }
    jraw(j, "}");
}

char *megler_apk_report(const char *apk_path)
{
    hk_apk_info info;
    jbuf j;
    hk_zip z;
    char err[160];
    int i;
    memset(&j, 0, sizeof(j));
    jraw(&j, "{");
    if (!hk_apk_read_info(apk_path, &info)) {
        jkey(&j, "ok", true);
        jraw(&j, "false");
        jkey(&j, "error", false);
        jstr(&j, info.error);
        jraw(&j, "}");
        return j.s;
    }
    jkey(&j, "ok", true);
    jraw(&j, "true");
    jkey(&j, "package", false);
    jstr(&j, info.package);
    jkey(&j, "label", false);
    jstr(&j, info.label);
    jkey(&j, "versionName", false);
    jstr(&j, info.version_name);
    jkey(&j, "versionCode", false);
    jint(&j, info.version_code);
    jkey(&j, "minSdk", false);
    jint(&j, info.min_sdk);
    jkey(&j, "targetSdk", false);
    jint(&j, info.target_sdk);
    jkey(&j, "abis", false);
    jraw(&j, "[");
    for (i = 0; i < info.nabis; i++) {
        if (i) jraw(&j, ",");
        jstr(&j, info.abis[i]);
    }
    jraw(&j, "]");
    if (info.abi[0]) {
        jkey(&j, "abi", false);
        jstr(&j, info.abi);
    }
    jkey(&j, "engine", false);
    jstr(&j, info.engine);
    jkey(&j, "supported", false);
    jraw(&j, info.supported ? "true" : "false");
    jkey(&j, "activity", false);
    jstr(&j, info.activity);
    jkey(&j, "orientation", false);
    jstr(&j, info.orientation == HK_APK_LANDSCAPE ? "landscape" : info.orientation == HK_APK_PORTRAIT ? "portrait" : "unspecified");
    if (info.native_library[0]) {
        jkey(&j, "nativeLibrary", false);
        jstr(&j, info.native_library);
    }
    jkey(&j, "hasIcon", false);
    jraw(&j, info.icon_path[0] ? "true" : "false");
    jkey(&j, "libraries", false);
    jraw(&j, "[");
    if (info.abi[0] && hk_zip_open(&z, apk_path, err, sizeof(err))) {
        lib_report r;
        char prefix[40];
        memset(&r, 0, sizeof(r));
        snprintf(prefix, sizeof(prefix), "lib/%s/", info.abi);
        r.j = &j;
        r.zip = &z;
        r.prefix = prefix;
        r.first = true;
        r.info = &info;
        hk_zip_each(&z, prefix, lib_report_one, &r);
        hk_zip_close(&z);
        if (r.arm_total) {
            if (info.nnotes < (int)HK_COUNT(info.notes))
                snprintf(info.notes[info.nnotes++], sizeof(info.notes[0]),
                         "%u of its %u exported functions are in ARM (not Thumb) mode. Whether a phone keeps such code running is what Device checks measures.",
                         r.arm_total, r.arm_total + r.thumb_total);
        }
    }
    jraw(&j, "]");
    jkey(&j, "runnable", false);
    jraw(&j, info.runnable ? "true" : "false");
    if (!info.runnable) {
        jkey(&j, "reason", false);
        jstr(&j, info.reason);
    }
    jkey(&j, "notes", false);
    jraw(&j, "[");
    for (i = 0; i < info.nnotes; i++) {
        if (i) jraw(&j, ",");
        jstr(&j, info.notes[i]);
    }
    jraw(&j, "]}");
    return j.s;
}

bool megler_apk_icon(const char *apk_path, uint8_t **data, size_t *size)
{
    hk_apk_info info;
    hk_zip z;
    const hk_zip_entry *e;
    char err[160];
    u8 *bytes;
    *data = NULL;
    *size = 0;
    if (!hk_apk_read_info(apk_path, &info) || !info.icon_path[0]) return false;
    if (!hk_zip_open(&z, apk_path, err, sizeof(err))) return false;
    e = hk_zip_find(&z, info.icon_path);
    bytes = e ? hk_zip_read(&z, e, 8u << 20, size) : NULL;
    hk_zip_close(&z);
    *data = bytes;
    return bytes != NULL;
}
