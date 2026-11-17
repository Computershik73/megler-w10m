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
 * The locks SQLite takes on a database file, as its protocol has them: any
 * number of readers (SHARED); one who means to write (RESERVED) beside them;
 * then, to write, that one alone (EXCLUSIVE), with no new reader let in while
 * it waits for the others to finish (PENDING). Two places keep them. This
 * process's connections to a file are in a table here, the file known by its
 * path as SQLite gives it, which is always the full one. And every connection
 * takes them on its own file as byte-range locks, where SQLite's own layers
 * take them (PENDING's byte at 1 GB, RESERVED's after it, then 510 bytes for
 * the readers; sq_lock follows SQLite's own layers), which bind the
 * connections of other processes: every app here runs Google Play services,
 * whose databases are every app's (/data/megler-services), open in several
 * processes at once, and an app suspended in the middle of a transaction
 * keeps its locks. With the table alone, another app's connection took the
 * journal of a transaction under way for an abandoned one and rolled it back,
 * or wrote beside it; Phenotype's database had no table Packages in several
 * apps' logs (package 576).
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
    int held;                          /* SQLITE_LOCK_NONE .. SQLITE_LOCK_EXCLUSIVE; changed under g_nodes_lock */
    bool delete_on_close;
    char *path;
    u32 owner;                         /* the thread that took SHARED, while SHARED or more is held */
    u64 since;                         /* and when (mg_time_monotonic_ns) */
    struct sq_file *held_next;         /* in g_held, while SHARED or more is held */
} sq_file;

static mg_lock g_nodes_lock = MG_LOCK_INIT;
static sq_node *g_nodes;
/* The connections of this process that hold a lock on their database (SHARED or more): what another process may wait on. */
static volatile i32 g_locking;
/* And which they are, for the log and for the gate below. Under g_nodes_lock. */
static sq_file *g_held;

/*
 * The app suspended. Windows freezes a suspended app's process as it is, in the middle of whatever its threads do, and a
 * lock it holds on a file then stays held for as long as it is frozen: YouTube was suspended 16 s after its start with a
 * lock on one of Google Play services' databases, and for three hours every other app's connection to that database
 * waited its 2.5 s and threw "database is locked (code 5)" (a user's Lumia 950, package 696). Waiting for the count of
 * locks to come down to nothing was not enough: other threads took new ones meanwhile, and after. So from the moment the
 * shell says the app is being suspended (mg_sqlite_settle) until it says the app is back (mg_sqlite_resume) the locks
 * only come down. At each of SQLite's levels:
 *
 *   NONE to SHARED    A thread that holds no lock here waits in sq_lock, holding nothing, until the app is back: frozen
 *                     there, it keeps nobody waiting. A thread that holds a lock through another connection (in a
 *                     transaction of one database it reads a second) is let through: waiting, it would keep the lock
 *                     it has.
 *   SHARED to RESERVED, and either to EXCLUSIVE
 *                     As ever. Who is in the middle of a read or of a transaction goes on to its end, which is where it
 *                     lets go; turned away, it would keep its lock with nothing left to end it.
 *   PENDING           Not kept. A writer that finds readers still there is answered SQLITE_BUSY, as ever, but PENDING's
 *                     byte does not stay locked while it waits to ask again: frozen with that byte, it would keep every
 *                     new reader of every process out (the other apps' "database is locked ... while compiling: PRAGMA
 *                     journal_mode=WAL" was that: they could not so much as read). SQLite never sees PENDING, it asks
 *                     for EXCLUSIVE and is told yes or busy, so the byte is this layer's to give up; RESERVED is not,
 *                     SQLite counts on it once it has it. One held when the settling begins is given up there and then.
 *   Down, to SHARED or to NONE
 *                     As ever.
 *
 * What is still held when the shell's time is up is said in the log by file, level and thread: a transaction an app
 * leaves open across its own code is the app's to end, and nothing here can end it.
 */
static bool g_settling;                /* under g_nodes_lock */
static mg_cond g_gate = MG_COND_INIT;  /* told when the settling ends */
static volatile i32 g_waiting;         /* the threads waiting for that now */

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

/* Where the locks lie in the file: SQLite's own places (sqlite3PendingByte), past any page it reads or writes. */
#define SQ_PENDING_BYTE 0x40000000u
#define SQ_RESERVED_BYTE (SQ_PENDING_BYTE + 1)
#define SQ_SHARED_FIRST (SQ_PENDING_BYTE + 2)
#define SQ_SHARED_SIZE 510

/* A range of the file locked for this connection: SQLITE_BUSY when another connection, of any process, holds it. */
static int os_lock(sq_file *s, u64 at, u64 n, bool exclusive)
{
    int err = 0;
    if (mg_file_lock(s->f, at, n, exclusive, &err)) return SQLITE_OK;
    return err == MG_E_BUSY ? SQLITE_BUSY : SQLITE_IOERR_LOCK;
}

/*
 * A wait for the readers to leave, given up: PENDING's byte is let go, and the connection has what it had before it asked
 * for EXCLUSIVE, RESERVED or SHARED. Asking again takes the byte again. Under g_nodes_lock.
 */
static void pending_drop(sq_file *s)
{
    sq_node *n = s->node;
    if (s->held != SQLITE_LOCK_PENDING) return;
    mg_file_unlock(s->f, SQ_PENDING_BYTE, 1);
    if (n->pending == s) n->pending = NULL;
    s->held = n->reserved == s ? SQLITE_LOCK_RESERVED : SQLITE_LOCK_SHARED;
}

/*
 * A thread about to read waits here while the app is being suspended, unless it holds a lock already (see "The app
 * suspended" above). Under g_nodes_lock, which the wait gives up. A frozen process counts nothing, so the waits that run
 * out are time the process ran: after ten seconds of them it was not frozen, or was not told that it is back, and the
 * gate opens by itself rather than keep the app's databases shut for good. True when it did, for the log.
 */
static bool gate_wait(void)
{
    u32 me = mg_thread_self_id();
    sq_file *s;
    int ran_out = 0;
    bool opened = false;
    if (!g_settling) return false;
    for (s = g_held; s; s = s->held_next)
        if (s->owner == me) return false;
    g_waiting++;
    while (g_settling) {
        if (mg_cond_wait_ns(&g_gate, &g_nodes_lock, 50000000ull) || ++ran_out < 200) continue;
        g_settling = false;
        mg_cond_broadcast(&g_gate);
        opened = true;
    }
    g_waiting--;
    return opened;
}

static int sq_lock(sqlite3_file *file, int level)
{
    sq_file *s = (sq_file *)file;
    sq_node *n = s->node;
    int rc = SQLITE_OK, held;
    bool got_pending = false, opened = false;
    if (!n || level <= s->held) return SQLITE_OK;
    mg_lock_acquire(&g_nodes_lock);
    /* (The app being suspended: who would begin to read waits until it is back, holding nothing.) */
    if (s->held == SQLITE_LOCK_NONE) opened = gate_wait();
    held = s->held;
    /* A reader, or a writer on its way to EXCLUSIVE, takes PENDING's byte first: a writer waiting holds it, and keeps new
       readers out. A reader takes it shared, as SQLite's Unix layer does (a file open only to be read takes no other
       kind there), and only while it comes in. */
    if (held == SQLITE_LOCK_NONE || (level == SQLITE_LOCK_EXCLUSIVE && held < SQLITE_LOCK_PENDING)) {
        if ((n->pending && n->pending != s) || (n->exclusive && n->exclusive != s)) rc = SQLITE_BUSY;
        else if ((rc = os_lock(s, SQ_PENDING_BYTE, 1, level == SQLITE_LOCK_EXCLUSIVE)) == SQLITE_OK) got_pending = true;
    }
    /* SHARED: the readers' range, shared with the other readers. */
    if (rc == SQLITE_OK && level == SQLITE_LOCK_SHARED) {
        if ((rc = os_lock(s, SQ_SHARED_FIRST, SQ_SHARED_SIZE, false)) == SQLITE_OK) {
            n->shared++;
            s->held = SQLITE_LOCK_SHARED;
            g_locking++;
            s->owner = mg_thread_self_id();
            s->since = mg_time_monotonic_ns();
            s->held_next = g_held;
            g_held = s;
        }
    }
    /* RESERVED: its byte, one writer's at a time. (Not on the way to EXCLUSIVE, which a connection rolling a hot journal
       back goes to from SHARED: another would take the journal for a writer's and read what is being undone.) */
    if (rc == SQLITE_OK && level == SQLITE_LOCK_RESERVED) {
        if (n->reserved && n->reserved != s) rc = SQLITE_BUSY;
        else if ((rc = os_lock(s, SQ_RESERVED_BYTE, 1, true)) == SQLITE_OK) {
            n->reserved = s;
            s->held = SQLITE_LOCK_RESERVED;
        }
    }
    /* EXCLUSIVE: PENDING kept, then the readers' range made this one's alone, once no other reads in any process; until
       then PENDING stays, and the range shared as it was. */
    if (rc == SQLITE_OK && level == SQLITE_LOCK_EXCLUSIVE) {
        if (n->reserved && n->reserved != s) rc = SQLITE_BUSY;
        else {
            n->pending = s;
            if (s->held < SQLITE_LOCK_PENDING) s->held = SQLITE_LOCK_PENDING;
            got_pending = false;
            if (n->shared > 1) rc = SQLITE_BUSY;
            else {
                mg_file_unlock(s->f, SQ_SHARED_FIRST, SQ_SHARED_SIZE);
                rc = os_lock(s, SQ_SHARED_FIRST, SQ_SHARED_SIZE, true);
                if (rc == SQLITE_OK) {
                    n->exclusive = s;
                    s->held = SQLITE_LOCK_EXCLUSIVE;
                } else if (os_lock(s, SQ_SHARED_FIRST, SQ_SHARED_SIZE, false) != SQLITE_OK) {
                    rc = SQLITE_IOERR_LOCK;
                }
            }
        }
        /* (The app being suspended: turned away, it does not keep PENDING while it waits to ask again.) */
        if (rc != SQLITE_OK && g_settling) pending_drop(s);
    }
    /* (PENDING's byte, taken only to come in as a reader, or by a writer turned away before it waited.) */
    if (got_pending) mg_file_unlock(s->f, SQ_PENDING_BYTE, 1);
    mg_lock_release(&g_nodes_lock);
    if (opened)
        mg_warn("sqlite: the app was being suspended, and has run ten seconds since without being told that it is back: its threads read "
                "and write databases again");
    return rc;
}

static int sq_unlock(sqlite3_file *file, int level)
{
    sq_file *s = (sq_file *)file;
    sq_node *n = s->node;
    sq_file **at;
    int rc = SQLITE_OK, held;
    if (!n || s->held <= level) return SQLITE_OK;
    mg_lock_acquire(&g_nodes_lock);
    /* (What is held is read under the lock: the settling may have taken a PENDING away from another thread meanwhile.) */
    held = s->held;
    if (held > level) {
        if (held >= SQLITE_LOCK_EXCLUSIVE) {
            mg_file_unlock(s->f, SQ_SHARED_FIRST, SQ_SHARED_SIZE);
            if (level == SQLITE_LOCK_SHARED && os_lock(s, SQ_SHARED_FIRST, SQ_SHARED_SIZE, false) != SQLITE_OK) rc = SQLITE_IOERR_UNLOCK;
        }
        if (held >= SQLITE_LOCK_RESERVED) mg_file_unlock(s->f, SQ_RESERVED_BYTE, 1);
        if (level == SQLITE_LOCK_NONE && held >= SQLITE_LOCK_SHARED) mg_file_unlock(s->f, SQ_SHARED_FIRST, SQ_SHARED_SIZE);
        if (held >= SQLITE_LOCK_PENDING) mg_file_unlock(s->f, SQ_PENDING_BYTE, 1);
        if (n->exclusive == s) n->exclusive = NULL;
        if (n->pending == s) n->pending = NULL;
        if (n->reserved == s) n->reserved = NULL;
        if (level == SQLITE_LOCK_NONE && held >= SQLITE_LOCK_SHARED) {
            n->shared--;
            g_locking--;
            for (at = &g_held; *at && *at != s; at = &(*at)->held_next) { }
            if (*at) *at = s->held_next;
            s->held_next = NULL;
        }
        s->held = level;
    }
    mg_lock_release(&g_nodes_lock);
    return rc;
}

/* Whether a connection means to write: one of this process (the table), or of another (RESERVED's byte held elsewhere). */
static int sq_check_reserved(sqlite3_file *file, int *out)
{
    sq_file *s = (sq_file *)file;
    int reserved;
    mg_lock_acquire(&g_nodes_lock);
    reserved = s->node && (s->node->reserved || s->node->pending || s->node->exclusive);
    mg_lock_release(&g_nodes_lock);
    if (!reserved && s->node && s->held < SQLITE_LOCK_RESERVED) reserved = mg_file_locked_elsewhere(s->f, SQ_RESERVED_BYTE, 1);
    *out = reserved;
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

int mg_sqlite_locks_held(void) { return g_locking; }
int mg_sqlite_waiting(void) { return g_waiting; }

/* A database for the log: its name and the two folders it is in (the app's package and "databases"), not the host's whole path. */
static const char *path_tail(const char *path)
{
    const char *p;
    int folders = 0;
    if (!path) return "(no name)";
    for (p = path + strlen(path); p > path; p--)
        if ((p[-1] == '/' || p[-1] == '\\') && ++folders == 3) break;
    return p;
}

/* A lock's level for the log, and what the other apps cannot do while it is held. */
static const char *level_said(int held)
{
    switch (held) {
    case SQLITE_LOCK_SHARED: return "SHARED (a read: the other apps can read it, and cannot end a write)";
    case SQLITE_LOCK_RESERVED: return "RESERVED (a write begun: the other apps can read it, and cannot write)";
    case SQLITE_LOCK_PENDING: return "PENDING (a write waiting for readers: the other apps can neither read nor write)";
    case SQLITE_LOCK_EXCLUSIVE: return "EXCLUSIVE (a write being put into the file: the other apps can neither read nor write)";
    default: return "none";
    }
}

/* A lock still held when the time to settle is up, as the log is to say it: copied under g_nodes_lock, said without it. */
typedef struct sq_kept {
    char file[200];
    int held;
    u32 thread, ms;
} sq_kept;

bool mg_sqlite_settle(int ms)
{
    u64 until = mg_time_monotonic_ns() + (u64)(ms > 0 ? ms : 0) * 1000000u, now;
    sq_kept kept[8];
    sq_file *s;
    int n = 0, more = 0, i;
    mg_lock_acquire(&g_nodes_lock);
    g_settling = true;
    /* (A writer waiting for readers now: it keeps what it writes, and lets the readers of the other apps in.) */
    for (s = g_held; s; s = s->held_next) pending_drop(s);
    mg_lock_release(&g_nodes_lock);
    while (g_locking > 0 && mg_time_monotonic_ns() < until) mg_sleep_ns(5000000ull);
    mg_lock_acquire(&g_nodes_lock);
    now = mg_time_monotonic_ns();
    for (s = g_held; s; s = s->held_next) {
        if (n == (int)MG_COUNT(kept)) {
            more++;
            continue;
        }
        mg_strlcpy(kept[n].file, path_tail(s->path), sizeof(kept[n].file));
        kept[n].held = s->held;
        kept[n].thread = s->owner;
        kept[n].ms = (u32)((now - s->since) / 1000000u);
        n++;
    }
    mg_lock_release(&g_nodes_lock);
    for (i = 0; i < n; i++)
        mg_warn("sqlite: a lock is kept as the app is suspended: %s, %s, taken %u ms ago by thread %u", kept[i].file, level_said(kept[i].held),
                (unsigned)kept[i].ms, (unsigned)kept[i].thread);
    if (more) mg_warn("sqlite: and %d more locks are kept", more);
    return n == 0;
}

void mg_sqlite_resume(void)
{
    bool was;
    int waiting;
    mg_lock_acquire(&g_nodes_lock);
    was = g_settling;
    waiting = g_waiting;
    g_settling = false;
    mg_cond_broadcast(&g_gate);
    mg_lock_release(&g_nodes_lock);
    if (was && waiting > 0) mg_info("sqlite: the app is back: %d of its threads waited to read a database, and go on", waiting);
}

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
int mg_sqlite_locks_held(void) { return 0; }
int mg_sqlite_waiting(void) { return 0; }
bool mg_sqlite_settle(int ms) { (void)ms; return true; }
void mg_sqlite_resume(void) { }
bool mg_sqlite_start(void) { return false; }

#endif
