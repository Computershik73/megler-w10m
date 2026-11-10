/*
 * The manifest of the package of one Android app, made from Megler's own (mg_appx.h, docs/PER-APP-PACKAGES.md).
 *
 * Everything Megler's manifest says stays, its capabilities, dependencies, entry point and the publisher cache folder
 * among them, so that an app's package runs Megler as Megler runs; what makes it a package of its own is changed:
 *   - Identity's Name: "MeglerApp." and the Android package, which has only letters, digits, '.' and '_' (Android's rule),
 *     '_' made '-' (an identity name has no '_'), always 50 characters long, the most a name may have: one longer is cut
 *     and given a hash of the whole at its end, one shorter is filled with a dot and a hash of the package (its
 *     resources.pri is made from one for a placeholder as long, mg_appx_pri.c);
 *   - PhoneIdentity's PhoneProductId: a GUID made from the identity name, so the same for the same package each time,
 *     and a package made again for the same app is an update of it;
 *   - Properties' DisplayName, and VisualElements' DisplayName and the tile's ShortName: the app's label (the short name
 *     cut to 40 characters, as the schema has it);
 *   - the tile's and the splash screen's BackgroundColor, when the caller gives them (the app's icon is drawn on them);
 *   - the association of APKs with Megler (an Extension of the category windows.fileTypeAssociation), taken out: an APK
 *     opened on the phone is Megler's to install, and with every app's package claiming it Windows would ask which one.
 *     Its megler: links stay, by which a package of Megler's starts another (Launcher.LaunchUriAsync with that package as
 *     the target).
 * The version is Megler's here; the shell gives the package one of its own, "<major>.<minor>.<Megler's build>.<n>"
 * (app/Megler/shell/PackageVersions.h), so that a package made by a newer Megler updates one made by an older, and one
 * made again by the same Megler (the app updated in it) updates the one it made before.
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
    memcpy(name, "MeglerApp.", 10);
    n = 10;
    for (s = android_package; *s && n < sizeof(name) - 1; s++) {
        char c = *s;
        if (c == '_') c = '-';
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        name[n++] = c;
    }
    name[n] = 0;
    if (*s || n > 50) {
        /* Cut to 41 and a hash of the whole: "MeglerApp.com.example.a-very-long-nam.1a2b3c4d" */
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

/* Takes out the first element that begins with `start`, through the first `end` after it, with the white space before it on
   its line and the line's end after it. `xml` as it was when there is none; NULL when it does not end. */
static char *cut_element(char *xml, const char *start, const char *end)
{
    char *a = strstr(xml, start), *b;
    if (!a) return xml;
    b = strstr(a, end);
    if (!b) return NULL;
    b += strlen(end);
    while (a > xml && (a[-1] == ' ' || a[-1] == '\t')) a--;
    if (*b == '\r') b++;
    if (*b == '\n') b++;
    memmove(a, b, strlen(b) + 1);
    return xml;
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

/*
 * What an app's package declares to Windows of what Megler's declares: what the Android app's manifest asks for, and what
 * Megler itself needs in every package, nothing else. Android asks the user for a dangerous permission when the app wants it
 * (the framework's megler.Permissions); Windows asks for a device's -- the camera, the microphone, the location -- when an
 * app first opens it, and lists the app in its Privacy settings under each capability the package declares. Before, every
 * app's package declared all of Megler's: a calculator was listed as one that reads text messages and the call history, and
 * one whose Google Play services looked for the location had Windows ask for it.
 */
static const struct {
    const char *permission;
    unsigned caps;
} k_permission_caps[] = {
    /* Android's INTERNET lets an app reach the home network and take connections as well (the comment in Package.appxmanifest). */
    { "android.permission.INTERNET", MG_APPX_CAP_NETWORK },
    { "android.permission.CAMERA", MG_APPX_CAP_WEBCAM },
    { "android.permission.RECORD_AUDIO", MG_APPX_CAP_MICROPHONE },
    { "android.permission.ACCESS_FINE_LOCATION", MG_APPX_CAP_LOCATION },
    { "android.permission.ACCESS_COARSE_LOCATION", MG_APPX_CAP_LOCATION },
    { "android.permission.ACCESS_BACKGROUND_LOCATION", MG_APPX_CAP_LOCATION },
    /* The shared storage's Music, Pictures, Movies and DCIM are the phone's libraries (player/StorageHost.h). */
    { "android.permission.READ_EXTERNAL_STORAGE", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_MUSIC },
    { "android.permission.WRITE_EXTERNAL_STORAGE", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_MUSIC },
    { "android.permission.MANAGE_EXTERNAL_STORAGE", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_MUSIC },
    { "android.permission.MANAGE_MEDIA", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS | MG_APPX_CAP_MUSIC },
    { "android.permission.READ_MEDIA_IMAGES", MG_APPX_CAP_PICTURES },
    { "android.permission.READ_MEDIA_VIDEO", MG_APPX_CAP_VIDEOS },
    { "android.permission.READ_MEDIA_VISUAL_USER_SELECTED", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS },
    { "android.permission.ACCESS_MEDIA_LOCATION", MG_APPX_CAP_PICTURES | MG_APPX_CAP_VIDEOS },
    { "android.permission.READ_MEDIA_AUDIO", MG_APPX_CAP_MUSIC },
    /* The phone's text messages and calls as Megler hands them to an app (player/PhoneEvents.cpp, megler.Phone): what came in
       to RECEIVE_SMS and READ_SMS, calls to READ_PHONE_STATE (PHONE_STATE, told from the call history) and READ_CALL_LOG. */
    { "android.permission.RECEIVE_SMS", MG_APPX_CAP_CHAT },
    { "android.permission.READ_SMS", MG_APPX_CAP_CHAT },
    { "android.permission.READ_PHONE_STATE", MG_APPX_CAP_CALL_HISTORY },
    { "android.permission.READ_CALL_LOG", MG_APPX_CAP_CALL_HISTORY },
    /* The phone's Bluetooth LE radio (player/BluetoothHost.cpp, Windows.Devices.Bluetooth): Android 12's permissions of nearby
       devices, and those of the Androids before it. */
    { "android.permission.BLUETOOTH_SCAN", MG_APPX_CAP_BLUETOOTH },
    { "android.permission.BLUETOOTH_CONNECT", MG_APPX_CAP_BLUETOOTH },
    { "android.permission.BLUETOOTH_ADVERTISE", MG_APPX_CAP_BLUETOOTH },
    { "android.permission.BLUETOOTH", MG_APPX_CAP_BLUETOOTH },
    { "android.permission.BLUETOOTH_ADMIN", MG_APPX_CAP_BLUETOOTH },
};

unsigned mg_appx_capabilities(const char *const *permissions, int count, bool sms_retriever)
{
    /*
     * The pictures library in every package, as Megler's own: its diagnosis folder, Pictures\Megler\diag (shell/PicturesDiag.h),
     * is reached through it, and an app saves its own pictures to the shared storage's Pictures and DCIM without asking on
     * Android 10 and later (MediaStore), which are the phone's Pictures and Camera Roll only through it.
     */
    unsigned caps = MG_APPX_CAP_PICTURES;
    int i;
    size_t k;
    for (i = 0; i < count; i++)
        for (k = 0; permissions && permissions[i] && k < MG_COUNT(k_permission_caps); k++)
            if (!strcmp(permissions[i], k_permission_caps[k].permission)) caps |= k_permission_caps[k].caps;
    /* Google Play services' SMS Retriever, which the app hosts, hears the phone's messages for it: no SMS permission of its own. */
    if (sms_retriever) caps |= MG_APPX_CAP_CHAT;
    return caps;
}

/* Megler's capabilities by name, and which of an app's needs each is for. 0: never an app's. One not here is kept as it is. */
static const struct {
    const char *name;
    unsigned caps;
} k_capabilities[] = {
    { "internetClient", MG_APPX_CAP_NETWORK },
    { "internetClientServer", MG_APPX_CAP_NETWORK },
    { "privateNetworkClientServer", MG_APPX_CAP_NETWORK },
    { "picturesLibrary", MG_APPX_CAP_PICTURES },
    { "videosLibrary", MG_APPX_CAP_VIDEOS },
    { "musicLibrary", MG_APPX_CAP_MUSIC },
    { "chat", MG_APPX_CAP_CHAT },
    { "phoneCallHistoryPublic", MG_APPX_CAP_CALL_HISTORY },
    { "webcam", MG_APPX_CAP_WEBCAM },
    { "microphone", MG_APPX_CAP_MICROPHONE },
    { "location", MG_APPX_CAP_LOCATION },
    { "bluetooth", MG_APPX_CAP_BLUETOOTH },
    /*
     * Never an app's: the documents library and a memory card are reached only with file types declared, and an app's package
     * declares none (Megler's association of APKs is taken out); a folder there comes in through the folder picker
     * (ui/SharedFolders.h). Installing packages is Megler's alone (an app hands an APK to Megler: player/SystemHost.h).
     */
    { "documentsLibrary", 0 },
    { "removableStorage", 0 },
    { "packageManagement", 0 },
};

/*
 * The manifest's <Capabilities> made again with those of `caps` (MG_APPX_CAP_*) and those this file does not know (Megler's
 * own: codeGeneration, packageQuery, backgroundMediaPlayback), in Megler's order, which the schema wants (Capability elements
 * before DeviceCapability ones), and without Megler's comments, which speak of Megler. `xml` as it is when it has none; NULL
 * when the block does not end.
 */
static char *choose_capabilities(char *xml, unsigned caps)
{
    static const char open_tag[] = "<Capabilities>", close_tag[] = "</Capabilities>";
    char *start = strstr(xml, open_tag), *end, *at, *out, *o;
    const char *eol;
    char indent[32] = "    ";
    bool indented = false;
    if (!start) return xml;
    end = strstr(start, close_tag);
    if (!end) return NULL;
    /* The block's own line end and its elements' indent, as Megler's file has them. */
    eol = start[sizeof(open_tag) - 1] == '\r' ? "\r\n" : "\n";
    /* (Each element kept gets a line end and an indent of its own, the end tag one more: twice the file is more than enough.) */
    out = (char *)malloc(strlen(xml) * 2 + 512);
    if (!out) return NULL;
    o = out;
    memcpy(o, xml, (size_t)(start - xml) + sizeof(open_tag) - 1);
    o += (start - xml) + sizeof(open_tag) - 1;
    for (at = start + sizeof(open_tag) - 1; at < end;) {
        char *lt = strchr(at, '<'), *gt, *close, *name, *q;
        size_t k, nlen;
        bool keep = true;
        if (!lt || lt >= end) break;
        if (!strncmp(lt, "<!--", 4)) {
            char *done = strstr(lt, "-->");
            if (!done || done > end) break;
            at = done + 3;
            continue;
        }
        if (!indented) {
            /* The white space before the first element on its line. */
            char *line = lt;
            while (line > start && (line[-1] == ' ' || line[-1] == '\t')) line--;
            if ((size_t)(lt - line) < sizeof(indent)) {
                memcpy(indent, line, (size_t)(lt - line));
                indent[lt - line] = 0;
            }
            indented = true;
        }
        gt = strchr(lt, '>');
        if (!gt || gt > end) break;
        close = gt;
        if (gt[-1] != '/') {
            /* An element with children (a DeviceCapability's Device): through its end tag. */
            char tag[64], endtag[72];
            size_t tlen = strcspn(lt + 1, " \t\r\n/>");
            if (tlen >= sizeof(tag)) break;
            memcpy(tag, lt + 1, tlen);
            tag[tlen] = 0;
            snprintf(endtag, sizeof(endtag), "</%s>", tag);
            close = strstr(gt, endtag);
            if (!close || close > end) break;
            close += strlen(endtag) - 1;
        }
        name = strstr(lt, " Name=\"");
        if (name && name < gt) {
            name += 7;
            q = strchr(name, '"');
            nlen = q ? (size_t)(q - name) : 0;
            for (k = 0; k < MG_COUNT(k_capabilities); k++)
                if (strlen(k_capabilities[k].name) == nlen && !strncmp(name, k_capabilities[k].name, nlen))
                    keep = (k_capabilities[k].caps & caps) != 0;
        }
        if (keep) {
            size_t len = (size_t)(close + 1 - lt);
            o += sprintf(o, "%s%s", eol, indent);
            memcpy(o, lt, len);
            o += len;
        }
        at = close + 1;
    }
    /* The end tag on its own line, indented as the start tag was. */
    {
        char *line = start;
        while (line > xml && (line[-1] == ' ' || line[-1] == '\t')) line--;
        o += sprintf(o, "%s", eol);
        memcpy(o, line, (size_t)(start - line));
        o += start - line;
    }
    strcpy(o, end);
    free(xml);
    return out;
}

char *mg_appx_app_manifest(const char *megler, const char *android_package, const char *label, const char *tile_color,
                           const char *splash_color, int capabilities, char *err, size_t errlen)
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
    xml = (char *)malloc(strlen(megler) + 1);
    if (!xml) return NULL;
    strcpy(xml, megler);
#define STEP(call, what)                                                                                                 \
    do {                                                                                                                 \
        char *next = (call);                                                                                             \
        if (!next) {                                                                                                     \
            if (err && errlen) snprintf(err, errlen, "Megler's manifest has no %s", what);                                \
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
    STEP(cut_element(xml, "<uap:Extension Category=\"windows.fileTypeAssociation\"", "</uap:Extension>"), "the file type association's end");
    if (capabilities >= 0) STEP(choose_capabilities(xml, (unsigned)capabilities), "end to its capabilities");
#undef STEP
    return xml;
}
