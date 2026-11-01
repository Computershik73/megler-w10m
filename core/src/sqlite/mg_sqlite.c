/*
 * What stands under SQLite in place of an operating system: its files, its
 * locks, its mutexes, its clock. See mg_sqlite.h; SQLite itself is compiled
 * by mg_sqlite3.c, with SQLITE_OS_OTHER, which is what makes it ask here.
 */
#include "mg_sqlite.h"

#if defined(MG_HAVE_SQLITE) && MG_HAVE_SQLITE

#include <stdio.h>

#include "sqlite3.h"

#include "../bionic/mg_fd.h"
#include "../host/mg_host.h"

/* The layer's own folder for what it writes (mg_api.c). */
const char *megler_data_dir(void);

/* ---------------------------------------------------------------- mutexes */

/*
 * SQLite's mutexes on the host layer's locks. A recursive one is a lock with
 * its owner and a count. The static ones (SQLite names them by number, from
 * two up) are a table.
 */
struct sqlite3_mutex {
    mg_lock lock;
    volatile u32 owner;
    int depth;
    int type;
};

static struct sqlite3_mutex g_static_mutexes[16];
static bool g_static_ready;

static int mutex_init(void)
{
    int i;
    if (!g_static_ready) {
        for (i = 0; i < (int)MG_COUNT(g_static_mutexes); i++) {
            mg_lock_init(&g_static_mutexes[i].lock);
            g_static_mutexes[i].type = i + 2;
        }
        g_static_ready = true;
    }
    return SQLITE_OK;
}

static int mutex_end(void) { return SQLITE_OK; }

static sqlite3_mutex *mutex_alloc(int type)
{
    struct sqlite3_mutex *m;
    if (type >= 2) return type - 2 < (int)MG_COUNT(g_static_mutexes) ? &g_static_mutexes[type - 2] : NULL;
    m = (struct sqlite3_mutex *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    mg_lock_init(&m->lock);
    m->type = type;
    return m;
}

static void mutex_free(sqlite3_mutex *m)
{
    if (m && m->type < 2) free(m);
}

static void mutex_enter(sqlite3_mutex *m)
{
    u32 me = mg_thread_self_id();
    if (m->type == SQLITE_MUTEX_RECURSIVE && m->owner == me) {
        m->depth++;
        return;
    }
    mg_lock_acquire(&m->lock);
    m->owner = me;
    m->depth = 1;
}

static int mutex_try(sqlite3_mutex *m)
{
    u32 me = mg_thread_self_id();
    if (m->type == SQLITE_MUTEX_RECURSIVE && m->owner == me) {
        m->depth++;
        return SQLITE_OK;
    }
    if (!mg_lock_try(&m->lock)) return SQLITE_BUSY;
    m->owner = me;
    m->depth = 1;
    return SQLITE_OK;
}

static void mutex_leave(sqlite3_mutex *m)
{
    if (--m->depth > 0) return;
    m->owner = 0;
    mg_lock_release(&m->lock);
}

static int mutex_held(sqlite3_mutex *m) { return m == NULL || m->owner == mg_thread_self_id(); }
static int mutex_notheld(sqlite3_mutex *m) { return m == NULL || m->owner != mg_thread_self_id(); }

static const sqlite3_mutex_methods k_mutexes = { mutex_init, mutex_end, mutex_alloc, mutex_free, mutex_enter, mutex_try, mutex_leave, mutex_held, mutex_notheld };

/* ------------------------------------------------------------------ files */

/*
 * The locks SQLite takes on a database file, kept here: there is one process,
 * and its connections to one file must still exclude each other as SQLite's
 * protocol says -- any number of readers (SHARED); one who means to write
 * (RESERVED) beside them; then, to write, that one alone (EXCLUSIVE), with no
 * new reader let in while it waits for the others to finish (PENDING). A
 * file is known by its path as SQLite gives it, which is always the full one.
 */
typedef struct sq_node {
    char *path;
    int refs;                          /* open files of this path */
    int shared;                        /* how many of them hold SHARED or more */
    void *reserved, *pending, *exclusive;  /* the open file that holds each, or NULL */
    struct sq_node *next;
} sq_node;

typedef struct sq_file {
    sqlite3_file base;
    mg_file *f;
    sq_node *node;
    int held;                          /* SQLITE_LOCK_NONE .. SQLITE_LOCK_EXCLUSIVE */
    bool delete_on_close;
    char *path;
} sq_file;

static mg_lock g_nodes_lock = MG_LOCK_INIT;
static sq_node *g_nodes;

static sq_node *node_get(const char *path)
{
    sq_node *n;
    mg_lock_acquire(&g_nodes_lock);
    for (n = g_nodes; n; n = n->next)
        if (!strcmp(n->path, path)) break;
    if (!n) {
        n = (sq_node *)mg_alloc(sizeof(*n));
        n->path = mg_strdup(path);
        n->next = g_nodes;
        g_nodes = n;
    }
    n->refs++;
    mg_lock_release(&g_nodes_lock);
    return n;
}

static void node_put(sq_node *n)
{
    sq_node **at;
    mg_lock_acquire(&g_nodes_lock);
    if (--n->refs == 0) {
        for (at = &g_nodes; *at && *at != n; at = &(*at)->next) { }
        if (*at) *at = n->next;
        free(n->path);
        free(n);
    }
    mg_lock_release(&g_nodes_lock);
}

static int sq_unlock(sqlite3_file *file, int level);

static int sq_close(sqlite3_file *file)
{
    sq_file *s = (sq_file *)file;
    sq_unlock(file, SQLITE_LOCK_NONE);
    if (s->f) mg_file_close(s->f);
    if (s->delete_on_close && s->path) mg_path_remove(s->path, NULL);
    if (s->node) node_put(s->node);
    free(s->path);
    s->f = NULL;
    s->node = NULL;
    s->path = NULL;
    return SQLITE_OK;
}

static int sq_read(sqlite3_file *file, void *buf, int amount, sqlite3_int64 offset)
{
    sq_file *s = (sq_file *)file;
    i64 got = mg_file_pread(s->f, buf, (size_t)amount, (u64)offset, NULL);
    if (got < 0) return SQLITE_IOERR_READ;
    if (got == amount) return SQLITE_OK;
    /* Past the end: SQLite wants the rest zeroed and to be told. */
    memset((u8 *)buf + got, 0, (size_t)(amount - got));
    return SQLITE_IOERR_SHORT_READ;
}

static int sq_write(sqlite3_file *file, const void *buf, int amount, sqlite3_int64 offset)
{
    sq_file *s = (sq_file *)file;
    const u8 *p = (const u8 *)buf;
    while (amount > 0) {
        int err = 0;
        i64 put = mg_file_pwrite(s->f, p, (size_t)amount, (u64)offset, &err);
        if (put <= 0) return err == MG_E_NOSPC ? SQLITE_FULL : SQLITE_IOERR_WRITE;
        p += put;
        offset += put;
        amount -= (int)put;
    }
    return SQLITE_OK;
}

static int sq_truncate(sqlite3_file *file, sqlite3_int64 size) { return mg_file_truncate(((sq_file *)file)->f, (u64)size, NULL) ? SQLITE_OK : SQLITE_IOERR_TRUNCATE; }
static int sq_sync(sqlite3_file *file, int flags) { (void)flags; return mg_file_sync(((sq_file *)file)->f) ? SQLITE_OK : SQLITE_IOERR_FSYNC; }

static int sq_size(sqlite3_file *file, sqlite3_int64 *size)
{
    mg_stat st;
    if (!mg_file_stat(((sq_file *)file)->f, &st)) return SQLITE_IOERR_FSTAT;
    *size = (sqlite3_int64)st.size;
    return SQLITE_OK;
}

static int sq_lock(sqlite3_file *file, int level)
{
    sq_file *s = (sq_file *)file;
    sq_node *n = s->node;
    int rc = SQLITE_OK;
    if (!n || level <= s->held) return SQLITE_OK;
    mg_lock_acquire(&g_nodes_lock);
    if (s->held == SQLITE_LOCK_NONE) {
        /* A reader comes in unless somebody is writing or waiting to. */
        if ((n->pending && n->pending != s) || (n->exclusive && n->exclusive != s)) rc = SQLITE_BUSY;
        else {
            n->shared++;
            s->held = SQLITE_LOCK_SHARED;
        }
    }
    if (rc == SQLITE_OK && level >= SQLITE_LOCK_RESERVED && s->held < SQLITE_LOCK_RESERVED) {
        if (n->reserved && n->reserved != s) rc = SQLITE_BUSY;
        else {
            n->reserved = s;
            s->held = SQLITE_LOCK_RESERVED;
        }
    }
    if (rc == SQLITE_OK && level >= SQLITE_LOCK_EXCLUSIVE) {
        /* PENDING first, which keeps new readers out; EXCLUSIVE once this is the only reader left. */
        if ((n->pending && n->pending != s) || (n->reserved && n->reserved != s)) rc = SQLITE_BUSY;
        else {
            n->pending = s;
            if (s->held < SQLITE_LOCK_PENDING) s->held = SQLITE_LOCK_PENDING;
            if (n->shared > 1) rc = SQLITE_BUSY;
            else {
                n->exclusive = s;
                s->held = SQLITE_LOCK_EXCLUSIVE;
            }
        }
    }
    mg_lock_release(&g_nodes_lock);
    return rc;
}

static int sq_unlock(sqlite3_file *file, int level)
{
    sq_file *s = (sq_file *)file;
    sq_node *n = s->node;
    if (!n || s->held <= level) return SQLITE_OK;
    mg_lock_acquire(&g_nodes_lock);
    if (n->exclusive == s) n->exclusive = NULL;
    if (n->pending == s) n->pending = NULL;
    if (n->reserved == s) n->reserved = NULL;
    if (level == SQLITE_LOCK_NONE && s->held >= SQLITE_LOCK_SHARED) n->shared--;
    s->held = level;
    mg_lock_release(&g_nodes_lock);
    return SQLITE_OK;
}

static int sq_check_reserved(sqlite3_file *file, int *out)
{
    sq_file *s = (sq_file *)file;
    mg_lock_acquire(&g_nodes_lock);
    *out = s->node && (s->node->reserved || s->node->pending || s->node->exclusive);
    mg_lock_release(&g_nodes_lock);
    return SQLITE_OK;
}

static int sq_control(sqlite3_file *file, int op, void *arg) { (void)file; (void)op; (void)arg; return SQLITE_NOTFOUND; }
static int sq_sector(sqlite3_file *file) { (void)file; return 4096; }
static int sq_characteristics(sqlite3_file *file) { (void)file; return 0; }

/* Version 1: no shared memory, so no write-ahead log. */
static const sqlite3_io_methods k_io = { 1, sq_close, sq_read, sq_write, sq_truncate, sq_sync, sq_size, sq_lock, sq_unlock, sq_check_reserved, sq_control, sq_sector, sq_characteristics,
                                         NULL, NULL, NULL, NULL, NULL, NULL };

static int vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out_flags)
{
    sq_file *s = (sq_file *)file;
    char temp[700];
    int how = MG_O_READ, err = 0;
    (void)vfs;
    memset(s, 0, sizeof(*s));
    if (!name) {
        /* A file with no name is a scratch file SQLite wants gone when it closes it; it goes into the layer's data folder. */
        const char *dir = megler_data_dir();
        u32 r[2];
        mg_random(r, sizeof(r));
        snprintf(temp, sizeof(temp), "%s%smegler-sqlite-%08x%08x.tmp", dir ? dir : "", dir && dir[0] ? "/" : "", (unsigned)r[0], (unsigned)r[1]);
        name = temp;
        flags |= SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_CREATE | SQLITE_OPEN_READWRITE;
    }
    if (flags & SQLITE_OPEN_READWRITE) how |= MG_O_WRITE;
    if (flags & SQLITE_OPEN_CREATE) how |= MG_O_CREATE;
    if (flags & SQLITE_OPEN_EXCLUSIVE) how |= MG_O_EXCL;
    s->f = mg_file_open(name, how, &err);
    if (!s->f && (flags & SQLITE_OPEN_READWRITE) && !(flags & SQLITE_OPEN_CREATE) && err == MG_E_ACCESS) {
        /* It is there and may only be read: SQLite is given it so, and told. */
        s->f = mg_file_open(name, MG_O_READ, &err);
        flags = (flags & ~SQLITE_OPEN_READWRITE) | SQLITE_OPEN_READONLY;
    }
    if (!s->f) return SQLITE_CANTOPEN;
    s->path = mg_strdup(name);
    s->delete_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    /* Only a database itself is locked; its journal belongs to whoever holds the database. */
    if (flags & SQLITE_OPEN_MAIN_DB) s->node = node_get(name);
    if (out_flags) *out_flags = flags;
    s->base.pMethods = &k_io;
    return SQLITE_OK;
}

static int vfs_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    int err = 0;
    (void)vfs; (void)sync_dir;
    if (mg_path_remove(name, &err)) return SQLITE_OK;
    return err == MG_E_NOENT ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE;
}

static int vfs_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
    mg_stat st;
    (void)vfs; (void)flags;
    *out = mg_path_stat(name, &st, NULL) && !st.is_dir;
    return SQLITE_OK;
}

/*
 * Names come here as the host's own full paths (the Java side has turned the app's path into one), and are left as they are;
 * but a name in the app's SQL is the app's path, which is turned into the host's here: Gmail's EmailProvider attaches
 * "/data/data/com.google.android.gm/databases/EmailProviderBody.db", which could not be opened (SQLITE_CANTOPEN), and Gmail
 * ended at its start (package 509).
 */
static int vfs_full_path(sqlite3_vfs *vfs, const char *name, int n, char *out)
{
    char *host = mg_fs_app_path_host(name);
    const char *use = host ? host : name;
    int rc = SQLITE_OK;
    (void)vfs;
    if ((int)strlen(use) >= n) rc = SQLITE_CANTOPEN;
    else memcpy(out, use, strlen(use) + 1);
    free(host);
    return rc;
}

static int vfs_randomness(sqlite3_vfs *vfs, int n, char *out) { (void)vfs; mg_random(out, (size_t)n); return n; }
static int vfs_sleep(sqlite3_vfs *vfs, int microseconds) { (void)vfs; mg_sleep_ns((u64)microseconds * 1000u); return microseconds; }

/* Milliseconds since noon, 24 November 4714 BC, which is how SQLite counts days. */
static int vfs_time64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
    (void)vfs;
    *out = (sqlite3_int64)(mg_time_realtime_ns() / 1000000u) + (sqlite3_int64)24405875 * 8640000;
    return SQLITE_OK;
}

static int vfs_time(sqlite3_vfs *vfs, double *out)
{
    sqlite3_int64 t = 0;
    vfs_time64(vfs, &t);
    *out = (double)t / 86400000.0;
    return SQLITE_OK;
}

static int vfs_last_error(sqlite3_vfs *vfs, int n, char *out) { (void)vfs; if (n > 0) out[0] = 0; return 0; }

static sqlite3_vfs g_vfs = { 2, (int)sizeof(sq_file), 1024, NULL, "megler", NULL, vfs_open, vfs_delete, vfs_access, vfs_full_path, NULL, NULL, NULL, NULL,
                             vfs_randomness, vfs_sleep, vfs_time, vfs_last_error, vfs_time64, NULL, NULL, NULL };

/* What SQLite calls where it has no operating system of its own (SQLITE_OS_OTHER). */
int sqlite3_os_init(void) { return sqlite3_vfs_register(&g_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }

bool mg_sqlite_available(void) { return true; }

bool mg_sqlite_start(void)
{
    static mg_lock once = MG_LOCK_INIT;
    static int state;                  /* 0 not yet, 1 started, -1 would not */
    mg_lock_acquire(&once);
    if (!state) {
        /* The mutexes must be named before SQLite makes its first one. */
        int rc = sqlite3_config(SQLITE_CONFIG_MUTEX, &k_mutexes);
        if (rc == SQLITE_OK) rc = sqlite3_initialize();
        state = rc == SQLITE_OK ? 1 : -1;
        if (rc != SQLITE_OK) mg_err("sqlite: it would not start (%d)", rc);
        else mg_info("sqlite: SQLite %s, on the host layer's files", sqlite3_libversion());
    }
    mg_lock_release(&once);
    return state == 1;
}

#else

bool mg_sqlite_available(void) { return false; }
bool mg_sqlite_start(void) { return false; }

#endif
