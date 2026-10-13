/*
 * android.content.SharedPreferences, kept.
 *
 * An app's small settings: Unity's PlayerPrefs are these, and a game may not
 * get past its first screens without them (a port that asks for a graphics
 * setting once, saves it and wants to be started again asks for ever if the
 * answer is forgotten). Each named set is a file of the app's own,
 * /data/data/<package>/shared_prefs/<name>.prefs as the app sees it: a line a
 * value, its type, its key and the value, with anything that is not plain
 * text written as %XX. It is read when the set is first asked for and written
 * whole at every commit() or apply(); these are small.
 *
 * A value read as another type than it was stored with gives the default
 * (Android throws ClassCastException, which the callers seen so far answer
 * with their default).
 */
#include "hk_jni_prefs.h"

#include <stdio.h>
#include <stdlib.h>

#include "../bionic/hk_bionic.h"
#include "../host/hk_host.h"
#include "hk_jni_loop.h"

#define PREFS "android/content/SharedPreferences"
#define EDITOR "android/content/SharedPreferences$Editor"

typedef struct pref {
    char *key;
    char type;                         /* i, l, f, z, s */
    hk_jvalue v;
    char *text;                        /* an s's */
    struct pref *next;
} pref;

typedef struct prefs {
    char *name;
    pref *first;
    hk_jobj *object, *editor;
    struct prefs *next;
} prefs;

static prefs *g_sets;
static hk_lock g_lock = HK_LOCK_INIT;

/* ------------------------------------------------------------------- file */

static char *file_path(const char *name)
{
    char *guest = hk_sprintf("%s/shared_prefs/%s.prefs", hk_fs_internal_dir(), name), *host = hk_fs_host_path(guest);
    free(guest);
    return host;
}

static void put_escaped(char **out, size_t *len, size_t *cap, const char *s)
{
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (*len + 4 > *cap) {
            *cap = *cap ? *cap * 2 : 256;
            *out = (char *)hk_realloc(*out, *cap);
        }
        if (ch <= ' ' || ch == '%' || ch == 0x7F) *len += (size_t)snprintf(*out + *len, 4, "%%%02X", ch);
        else (*out)[(*len)++] = (char)ch;
    }
}

static void put_text(char **out, size_t *len, size_t *cap, const char *s)
{
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        *cap = (*cap + n + 1) * 2;
        *out = (char *)hk_realloc(*out, *cap);
    }
    memcpy(*out + *len, s, n);
    *len += n;
}

static int hex_digit(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }

static char *unescaped(const char *s, size_t n)
{
    char *out = (char *)hk_alloc(n + 1);
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        /* %XX, when both digits are there. */
        if (s[i] == '%' && i + 2 < n && hex_digit(s[i + 1]) >= 0 && hex_digit(s[i + 2]) >= 0) {
            out[k++] = (char)(hex_digit(s[i + 1]) * 16 + hex_digit(s[i + 2]));
            i += 2;
            continue;
        }
        out[k++] = s[i];
    }
    out[k] = 0;
    return out;
}

static void save(const prefs *p)
{
    char *out = NULL, *path = file_path(p->name), *dir, *slash;
    size_t len = 0, cap = 0;
    const pref *e;
    char number[48];
    for (e = p->first; e; e = e->next) {
        number[0] = e->type;
        number[1] = ' ';
        number[2] = 0;
        put_text(&out, &len, &cap, number);
        put_escaped(&out, &len, &cap, e->key);
        put_text(&out, &len, &cap, " ");
        if (e->type == 's') {
            put_escaped(&out, &len, &cap, e->text ? e->text : "");
        } else {
            if (e->type == 'i') snprintf(number, sizeof(number), "%d", (int)e->v.i);
            else if (e->type == 'l') snprintf(number, sizeof(number), "%lld", (long long)e->v.j);
            else if (e->type == 'z') snprintf(number, sizeof(number), "%d", e->v.z ? 1 : 0);
            else {
                /* A float by its bits: what was stored is what comes back. */
                u32 bits;
                memcpy(&bits, &e->v.f, 4);
                snprintf(number, sizeof(number), "%08x", (unsigned)bits);
            }
            put_text(&out, &len, &cap, number);
        }
        put_text(&out, &len, &cap, "\n");
    }
    if (!path) {
        free(out);
        return;
    }
    dir = hk_strdup(path);
    slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    if (slash) {
        *slash = 0;
        hk_path_mkdirs(dir);
    }
    free(dir);
    if (!hk_file_write_all(path, out ? out : "", len)) hk_warn("preferences: %s could not be written", path);
    free(out);
    free(path);
}

static pref *find(prefs *p, const char *key)
{
    pref *e;
    for (e = p->first; e; e = e->next)
        if (!strcmp(e->key, key)) return e;
    return NULL;
}

static pref *entry(prefs *p, const char *key)
{
    pref *e = find(p, key);
    if (e) return e;
    e = (pref *)hk_alloc(sizeof(*e));
    e->key = hk_strdup(key);
    e->next = p->first;
    p->first = e;
    return e;
}

static void load(prefs *p)
{
    char *path = file_path(p->name);
    size_t size = 0, at = 0;
    u8 *data = path ? hk_file_read_all(path, &size) : NULL;
    int count = 0;
    while (data && at < size) {
        const char *line = (const char *)data + at, *end = memchr(line, '\n', size - at), *key, *key_end;
        size_t n = end ? (size_t)(end - line) : size - at;
        at += n + 1;
        if (n < 4 || line[1] != ' ') continue;
        key = line + 2;
        key_end = memchr(key, ' ', n - 2);
        if (!key_end) continue;
        {
            char *k = unescaped(key, (size_t)(key_end - key)), *v = unescaped(key_end + 1, n - (size_t)(key_end + 1 - line));
            pref *e = entry(p, k);
            e->type = line[0];
            e->v.j = 0;
            if (e->type == 's') {
                e->text = v;
                v = NULL;
            } else if (e->type == 'i') {
                e->v.i = (i32)strtol(v, NULL, 10);
            } else if (e->type == 'l') {
                e->v.j = strtoll(v, NULL, 10);
            } else if (e->type == 'z') {
                e->v.z = v[0] == '1';
            } else if (e->type == 'f') {
                u32 bits = (u32)strtoul(v, NULL, 16);
                memcpy(&e->v.f, &bits, 4);
            }
            free(k);
            free(v);
            count++;
        }
    }
    hk_info("preferences: %s, %d values kept from before", p->name, count);
    free(data);
    free(path);
}

/* ---------------------------------------------------------------- objects */

static prefs *set_named(const char *name)
{
    prefs *p;
    hk_lock_acquire(&g_lock);
    for (p = g_sets; p; p = p->next)
        if (!strcmp(p->name, name)) break;
    if (!p) {
        p = (prefs *)hk_alloc(sizeof(*p));
        p->name = hk_strdup(name);
        p->object = hk_jni_new_object(PREFS);
        p->object->permanent = true;
        p->object->native = p;
        p->editor = hk_jni_new_object(EDITOR);
        p->editor->permanent = true;
        p->editor->native = p;
        load(p);
        p->next = g_sets;
        g_sets = p;
    }
    hk_lock_release(&g_lock);
    return p;
}

hk_jobj *hk_jni_prefs(const char *name) { return set_named(name && name[0] ? name : "default")->object; }

static prefs *of(const hk_jobj *o) { return o ? (prefs *)o->native : NULL; }

static void ctx_get(hk_jcall *c)
{
    /* getSharedPreferences(name, mode); getPreferences(mode), which is the activity's own set. */
    const char *name = c->sig[1] == 'L' ? hk_jni_string(c->args[0].l) : "activity";
    c->ret.l = hk_jni_prefs(name);
}

static void manager_default(hk_jcall *c)
{
    char name[200];
    (void)c;
    snprintf(name, sizeof(name), "%s_preferences", hk_fs_package() ? hk_fs_package() : "app");
    c->ret.l = hk_jni_prefs(name);
}

static void p_edit(hk_jcall *c)
{
    prefs *p = of(c->self);
    c->ret.l = p ? p->editor : NULL;
}

/* getInt, getLong, getFloat, getBoolean, getString: the second argument is what to answer when there is nothing, or something else. */
static void p_get(hk_jcall *c)
{
    prefs *p = of(c->self);
    const char *key = hk_jni_string(c->args[0].l);
    char want = !strcmp(c->name, "getInt") ? 'i' : !strcmp(c->name, "getLong") ? 'l' : !strcmp(c->name, "getFloat") ? 'f' :
                !strcmp(c->name, "getBoolean") ? 'z' : 's';
    pref *e;
    c->ret = c->args[1];
    if (want == 's') c->ret.l = hk_jni_ref(c->args[1].l);
    if (!p || !key) return;
    hk_lock_acquire(&g_lock);
    e = find(p, key);
    if (e && e->type == want) {
        if (want == 's') {
            hk_jni_unref(c->ret.l);
            c->ret.l = hk_jni_new_string(e->text ? e->text : "");
        } else {
            c->ret = e->v;
        }
    }
    hk_lock_release(&g_lock);
}

static void p_contains(hk_jcall *c)
{
    prefs *p = of(c->self);
    const char *key = hk_jni_string(c->args[0].l);
    if (!p || !key) return;
    hk_lock_acquire(&g_lock);
    c->ret.z = find(p, key) != NULL;
    hk_lock_release(&g_lock);
}

/*
 * getAll(): everything, as a java.util.Map. Unity reads its PlayerPrefs this
 * way, once, when it starts, and keeps them itself from then on. The map is
 * a picture of the set as it was: its keys and its values (boxed: Integer,
 * Long, Float, Boolean, or a String), walked through entrySet() or keySet()
 * with an Iterator, or asked for by key.
 */
typedef struct snapshot {
    u32 count;
    hk_jobj **keys, **values;
} snapshot;

typedef struct walker {
    const snapshot *s;
    bool keys_only;
    u32 at;
} walker;

static hk_jobj *boxed(const pref *e)
{
    switch (e->type) {
    case 'i': return hk_jni_box("java/lang/Integer", 'I', e->v);
    case 'l': return hk_jni_box("java/lang/Long", 'J', e->v);
    case 'f': return hk_jni_box("java/lang/Float", 'F', e->v);
    case 'z': return hk_jni_box("java/lang/Boolean", 'Z', e->v);
    default: return hk_jni_new_string(e->text ? e->text : "");
    }
}

static void p_all(hk_jcall *c)
{
    prefs *p = of(c->self);
    snapshot *s = (snapshot *)hk_alloc(sizeof(*s));
    hk_jobj *map = hk_jni_new_object("java/util/Map");
    const pref *e;
    u32 i = 0;
    if (p) {
        hk_lock_acquire(&g_lock);
        for (e = p->first; e; e = e->next) s->count++;
        s->keys = (hk_jobj **)hk_alloc(sizeof(hk_jobj *) * (s->count + 1));
        s->values = (hk_jobj **)hk_alloc(sizeof(hk_jobj *) * (s->count + 1));
        for (e = p->first; e; e = e->next, i++) {
            s->keys[i] = hk_jni_new_string(e->key);
            s->keys[i]->permanent = true;
            s->values[i] = boxed(e);
            s->values[i]->permanent = true;
        }
        hk_lock_release(&g_lock);
    }
    map->native = s;
    c->ret.l = map;
}

static const snapshot *snap_of(const hk_jobj *o)
{
    static const snapshot empty;
    return o && o->native ? (const snapshot *)o->native : &empty;
}

static void map_size(hk_jcall *c) { c->ret.i = (i32)snap_of(c->self)->count; }
static void map_empty(hk_jcall *c) { c->ret.z = snap_of(c->self)->count == 0; }

static i32 map_index(const snapshot *s, hk_jobj *key)
{
    const char *k = hk_jni_string(key);
    u32 i;
    for (i = 0; k && i < s->count; i++)
        if (!strcmp(hk_jni_string(s->keys[i]), k)) return (i32)i;
    return -1;
}
static void map_get(hk_jcall *c)
{
    const snapshot *s = snap_of(c->self);
    i32 i = map_index(s, c->args[0].l);
    c->ret.l = i >= 0 ? s->values[i] : NULL;
}
static void map_has(hk_jcall *c) { c->ret.z = map_index(snap_of(c->self), c->args[0].l) >= 0; }

/* entrySet() and keySet(): a Set that is the same picture, saying which of the two it is. */
static void map_set(hk_jcall *c)
{
    hk_jobj *set = hk_jni_new_object("java/util/Set");
    walker *w = (walker *)hk_alloc(sizeof(*w));
    w->s = snap_of(c->self);
    w->keys_only = !strcmp(c->name, "keySet");
    set->native = w;
    c->ret.l = set;
}

static hk_jobj *walker_item(const walker *w, u32 i)
{
    hk_jobj *entry;
    walker *one;
    if (w->keys_only) return w->s->keys[i];
    entry = hk_jni_new_object("java/util/Map$Entry");
    one = (walker *)hk_alloc(sizeof(*one));
    one->s = w->s;
    one->at = i;
    entry->native = one;
    return entry;
}

static void set_size(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    c->ret.i = w ? (i32)w->s->count : 0;
}
static void set_iterator(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    hk_jobj *it = hk_jni_new_object("java/util/Iterator");
    walker *mine = (walker *)hk_alloc(sizeof(*mine));
    if (w) *mine = *w;
    mine->at = 0;
    it->native = mine;
    c->ret.l = it;
}
static void set_array(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    u32 n = w ? w->s->count : 0, i;
    hk_jobj *arr = hk_jni_new_object_array("java/lang/Object", n);
    for (i = 0; i < n; i++) arr->u.oarr.v[i] = walker_item(w, i);
    c->ret.l = arr;
}
static void it_has_next(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    c->ret.z = w && w->s && w->at < w->s->count;
}
static void it_next(hk_jcall *c)
{
    walker *w = (walker *)c->self->native;
    if (w && w->s && w->at < w->s->count) c->ret.l = walker_item(w, w->at++);
}
static void entry_key(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    c->ret.l = w && w->s && w->at < w->s->count ? w->s->keys[w->at] : NULL;
}
static void entry_value(hk_jcall *c)
{
    const walker *w = (const walker *)c->self->native;
    c->ret.l = w && w->s && w->at < w->s->count ? w->s->values[w->at] : NULL;
}

static void e_put(hk_jcall *c)
{
    prefs *p = of(c->self);
    const char *key = hk_jni_string(c->args[0].l);
    char type = !strcmp(c->name, "putInt") ? 'i' : !strcmp(c->name, "putLong") ? 'l' : !strcmp(c->name, "putFloat") ? 'f' :
                !strcmp(c->name, "putBoolean") ? 'z' : 's';
    c->ret.l = hk_jni_ref(c->self);
    if (!p || !key) return;
    hk_lock_acquire(&g_lock);
    {
        pref *e = entry(p, key);
        free(e->text);
        e->text = NULL;
        e->type = type;
        e->v.j = 0;
        if (type == 's') e->text = hk_strdup(hk_jni_string(c->args[1].l) ? hk_jni_string(c->args[1].l) : "");
        else if (type == 'z') e->v.z = c->args[1].z != 0;
        else if (type == 'i') e->v.i = c->args[1].i;
        else if (type == 'f') e->v.f = c->args[1].f;
        else e->v.j = c->args[1].j;
    }
    hk_lock_release(&g_lock);
}

static void drop(pref *e)
{
    free(e->key);
    free(e->text);
    free(e);
}

static void e_remove(hk_jcall *c)
{
    prefs *p = of(c->self);
    const char *key = hk_jni_string(c->args[0].l);
    pref **link;
    c->ret.l = hk_jni_ref(c->self);
    if (!p || !key) return;
    hk_lock_acquire(&g_lock);
    for (link = &p->first; *link; link = &(*link)->next)
        if (!strcmp((*link)->key, key)) {
            pref *e = *link;
            *link = e->next;
            drop(e);
            break;
        }
    hk_lock_release(&g_lock);
}

static void e_clear(hk_jcall *c)
{
    prefs *p = of(c->self);
    c->ret.l = hk_jni_ref(c->self);
    if (!p) return;
    hk_lock_acquire(&g_lock);
    while (p->first) {
        pref *e = p->first;
        p->first = e->next;
        drop(e);
    }
    hk_lock_release(&g_lock);
}

static void e_commit(hk_jcall *c)
{
    prefs *p = of(c->self);
    if (p) {
        hk_lock_acquire(&g_lock);
        save(p);
        hk_lock_release(&g_lock);
    }
    c->ret.z = 1;                      /* commit()'s answer; apply() has none */
}

static void ret_void(hk_jcall *c) { (void)c; }

static const hk_jhle k_table[] = {
    { "android/content/Context", "getSharedPreferences", NULL, ctx_get },
    { "android/app/Activity", "getPreferences", NULL, ctx_get },
    { "android/preference/PreferenceManager", "getDefaultSharedPreferences", NULL, manager_default },
    { PREFS, "edit", NULL, p_edit },
    { PREFS, "getInt", NULL, p_get },
    { PREFS, "getLong", NULL, p_get },
    { PREFS, "getFloat", NULL, p_get },
    { PREFS, "getBoolean", NULL, p_get },
    { PREFS, "getString", NULL, p_get },
    { PREFS, "contains", NULL, p_contains },
    { PREFS, "getAll", NULL, p_all },
    { "java/util/Map", "size", "()I", map_size },
    { "java/util/Map", "isEmpty", "()Z", map_empty },
    { "java/util/Map", "get", NULL, map_get },
    { "java/util/Map", "containsKey", NULL, map_has },
    { "java/util/Map", "entrySet", NULL, map_set },
    { "java/util/Map", "keySet", NULL, map_set },
    { "java/util/Set", "size", "()I", set_size },
    { "java/util/Set", "iterator", NULL, set_iterator },
    { "java/util/Set", "toArray", NULL, set_array },
    { "java/util/Iterator", "hasNext", "()Z", it_has_next },
    { "java/util/Iterator", "next", NULL, it_next },
    { "java/util/Map$Entry", "getKey", NULL, entry_key },
    { "java/util/Map$Entry", "getValue", NULL, entry_value },
    { PREFS, "registerOnSharedPreferenceChangeListener", NULL, ret_void },
    { PREFS, "unregisterOnSharedPreferenceChangeListener", NULL, ret_void },
    { EDITOR, "putInt", NULL, e_put },
    { EDITOR, "putLong", NULL, e_put },
    { EDITOR, "putFloat", NULL, e_put },
    { EDITOR, "putBoolean", NULL, e_put },
    { EDITOR, "putString", NULL, e_put },
    { EDITOR, "remove", NULL, e_remove },
    { EDITOR, "clear", NULL, e_clear },
    { EDITOR, "commit", NULL, e_commit },
    { EDITOR, "apply", NULL, e_commit },
    { NULL, NULL, NULL, NULL }
};

void hk_jni_prefs_install(void)
{
    static bool done;
    if (done) return;
    done = true;
    hk_jni_class(PREFS, "java/lang/Object");
    hk_jni_class(EDITOR, "java/lang/Object");
    /* The boxes a Map's values are. */
    hk_jni_loop_install();
    hk_jni_register(k_table);
}
