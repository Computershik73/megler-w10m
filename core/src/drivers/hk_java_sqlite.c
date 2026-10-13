/*
 * megler.Sqlite: databases for a Java app.
 *
 * The framework's android.database.sqlite is Java over 29 native methods that
 * are SQLite's C API almost one for one: a database is its sqlite3 *, a
 * statement its sqlite3_stmt *, both held by Java as longs. This is those
 * methods, as the original Megler has them on the system's SQLite of iOS; here
 * the SQLite is the one compiled into the core (sqlite/hk_sqlite.h), over the
 * host layer's files. The two collations Android registers on every
 * connection, LOCALIZED and UNICODE, are registered here too, since apps name
 * them in their schemas: LOCALIZED compares without regard to ASCII case,
 * UNICODE byte by byte, as the original's do (Android's own use ICU).
 *
 * A path is the app's, as Android names it (/data/data/<package>/databases/x.db);
 * SQLite is given the host's path for it. ":memory:" and the empty name stay
 * what they are to SQLite.
 *
 * Where the core was built without SQLite every method answers as a database
 * that cannot be opened -- SQLiteCantOpenDatabaseException in the app, which
 * apps are written to survive -- and the log says once, in a sentence, that
 * this build has none and what would give it one.
 */
#include <stdio.h>

#include "../bionic/hk_bionic.h"
#include "../dvm/hk_dvm_internal.h"
#include "../sqlite/hk_sqlite.h"
#include "hk_java.h"

#define N(name) static void name(hk_jcall *c)

#if defined(HK_HAVE_SQLITE) && HK_HAVE_SQLITE

#include "sqlite3.h"

#define DB ((sqlite3 *)(hk_addr)c->args[0].j)
#define ST ((sqlite3_stmt *)(hk_addr)c->args[0].j)

/* Why the last open failed: asked for with a database of 0, which is what a failed open hands back. */
static char g_open_error[512];
static hk_lock g_open_lock = HK_LOCK_INIT;

static int collate_nocase(void *user, int na, const void *a, int nb, const void *b)
{
    int n = na < nb ? na : nb, r = sqlite3_strnicmp((const char *)a, (const char *)b, n);
    (void)user;
    return r ? r : na - nb;
}

static int collate_binary(void *user, int na, const void *a, int nb, const void *b)
{
    int n = na < nb ? na : nb, r = memcmp(a, b, (size_t)n);
    (void)user;
    return r ? r : na - nb;
}

/* open(path, flags): Android's OPEN_READONLY (1) and CREATE_IF_NECESSARY (0x10000000). The handle, or 0. */
N(Sqlite_open)
{
    const char *path = hk_dvm_string_utf8(c->args[0].l);
    int asked = c->args[1].i, flags = (asked & 1) ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE, rc;
    char *host = NULL;
    sqlite3 *db = NULL;
    c->ret.j = 0;
    if (asked & 0x10000000) flags |= SQLITE_OPEN_CREATE;
    flags |= SQLITE_OPEN_FULLMUTEX;
    if (!hk_sqlite_start()) {
        hk_lock_acquire(&g_open_lock);
        hk_strlcpy(g_open_error, "SQLite would not start (code 1)", sizeof(g_open_error));
        hk_lock_release(&g_open_lock);
        return;
    }
    if (path && path[0] && strcmp(path, ":memory:")) host = hk_fs_host_path(path);
    rc = sqlite3_open_v2(host ? host : path && path[0] ? path : ":memory:", &db, flags, NULL);
    if (rc != SQLITE_OK) {
        hk_lock_acquire(&g_open_lock);
        snprintf(g_open_error, sizeof(g_open_error), "%s (code %d)", db ? sqlite3_errmsg(db) : sqlite3_errstr(rc), rc);
        hk_lock_release(&g_open_lock);
        hk_info("sqlite: %s could not be opened: %s", path ? path : "(no name)", g_open_error);
        if (db) sqlite3_close(db);
        free(host);
        return;
    }
    free(host);
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 2500);
    sqlite3_create_collation(db, "LOCALIZED", SQLITE_UTF8, NULL, collate_nocase);
    sqlite3_create_collation(db, "UNICODE", SQLITE_UTF8, NULL, collate_binary);
    c->ret.j = (i64)(hk_addr)db;
}

N(Sqlite_close) { if (DB) sqlite3_close_v2(DB); }

N(Sqlite_errmsg)
{
    if (DB) {
        c->ret.l = hk_dvm_new_string(sqlite3_errmsg(DB));
    } else {
        hk_lock_acquire(&g_open_lock);
        c->ret.l = hk_dvm_new_string(g_open_error);
        hk_lock_release(&g_open_lock);
    }
}

N(Sqlite_errcode) { c->ret.i = DB ? sqlite3_extended_errcode(DB) : SQLITE_CANTOPEN; }

N(Sqlite_exec)
{
    const char *sql = hk_dvm_string_utf8(c->args[1].l);
    c->ret.i = sql && DB ? sqlite3_exec(DB, sql, NULL, NULL, NULL) : SQLITE_MISUSE;
}

N(Sqlite_changes) { c->ret.i = DB ? sqlite3_changes(DB) : 0; }
N(Sqlite_lastInsertRowid) { c->ret.j = DB ? sqlite3_last_insert_rowid(DB) : 0; }
N(Sqlite_autocommit) { c->ret.i = DB ? sqlite3_get_autocommit(DB) : 1; }
N(Sqlite_busyTimeout) { if (DB) sqlite3_busy_timeout(DB, c->args[1].i); }

N(Sqlite_prepare)
{
    const char *sql = hk_dvm_string_utf8(c->args[1].l);
    sqlite3_stmt *st = NULL;
    if (!sql || !DB || sqlite3_prepare_v2(DB, sql, -1, &st, NULL) != SQLITE_OK) st = NULL;
    c->ret.j = (i64)(hk_addr)st;
}

N(Sqlite_finalize) { if (ST) sqlite3_finalize(ST); }
N(Sqlite_reset) { c->ret.i = ST ? sqlite3_reset(ST) : SQLITE_MISUSE; }
N(Sqlite_clearBindings) { if (ST) sqlite3_clear_bindings(ST); }
N(Sqlite_paramCount) { c->ret.i = ST ? sqlite3_bind_parameter_count(ST) : 0; }
N(Sqlite_readOnly) { c->ret.i = ST ? sqlite3_stmt_readonly(ST) : 0; }
N(Sqlite_bindNull) { c->ret.i = ST ? sqlite3_bind_null(ST, c->args[1].i) : SQLITE_MISUSE; }
N(Sqlite_bindLong) { c->ret.i = ST ? sqlite3_bind_int64(ST, c->args[1].i, c->args[2].j) : SQLITE_MISUSE; }
N(Sqlite_bindDouble) { c->ret.i = ST ? sqlite3_bind_double(ST, c->args[1].i, c->args[2].d) : SQLITE_MISUSE; }

/* A string is bound as the UTF-16 Java keeps it in: nothing is converted on the way in, and SQLite stores it as the database's encoding. */
N(Sqlite_bindString)
{
    i32 n = 0;
    const u16 *s = c->args[2].l ? hk_dvm_string_utf16(c->args[2].l, &n) : NULL;
    if (!ST) c->ret.i = SQLITE_MISUSE;
    else c->ret.i = s ? sqlite3_bind_text16(ST, c->args[1].i, s, n * 2, SQLITE_TRANSIENT) : sqlite3_bind_null(ST, c->args[1].i);
}

N(Sqlite_bindBlob)
{
    hk_jobj *b = c->args[2].l;
    if (!ST) c->ret.i = SQLITE_MISUSE;
    /* (An empty array is an empty blob, not NULL: SQLite makes a NULL of a null pointer, so it is given a pointer.) */
    else c->ret.i = b ? sqlite3_bind_blob(ST, c->args[1].i, b->u.arr.len ? b->u.arr.data : (const void *)"", (int)b->u.arr.len, SQLITE_TRANSIENT) : sqlite3_bind_null(ST, c->args[1].i);
}

N(Sqlite_step) { c->ret.i = ST ? sqlite3_step(ST) : SQLITE_MISUSE; }
N(Sqlite_columnCount) { c->ret.i = ST ? sqlite3_column_count(ST) : 0; }

N(Sqlite_columnName)
{
    const char *name = ST ? sqlite3_column_name(ST, c->args[1].i) : NULL;
    c->ret.l = hk_dvm_new_string(name ? name : "");
}

/* Android's Cursor field types: NULL 0, INTEGER 1, FLOAT 2, STRING 3, BLOB 4. */
N(Sqlite_columnType)
{
    int t = ST ? sqlite3_column_type(ST, c->args[1].i) : SQLITE_NULL;
    c->ret.i = t == SQLITE_INTEGER ? 1 : t == SQLITE_FLOAT ? 2 : t == SQLITE_TEXT ? 3 : t == SQLITE_BLOB ? 4 : 0;
}

N(Sqlite_columnLong) { c->ret.j = ST ? sqlite3_column_int64(ST, c->args[1].i) : 0; }
N(Sqlite_columnDouble) { c->ret.d = ST ? sqlite3_column_double(ST, c->args[1].i) : 0; }

N(Sqlite_columnText)
{
    const void *s = ST ? sqlite3_column_text16(ST, c->args[1].i) : NULL;
    int n = s ? sqlite3_column_bytes16(ST, c->args[1].i) : 0;
    c->ret.l = s ? hk_dvm_new_string_utf16((const u16 *)s, n / 2) : NULL;
}

N(Sqlite_columnBlob)
{
    const void *b;
    int n;
    hk_jobj *a;
    c->ret.l = NULL;
    if (!ST || sqlite3_column_type(ST, c->args[1].i) == SQLITE_NULL) return;
    b = sqlite3_column_blob(ST, c->args[1].i);
    n = sqlite3_column_bytes(ST, c->args[1].i);
    a = hk_dvm_new_array("[B", n);
    if (a && n) memcpy(a->u.arr.data, b, (size_t)n);
    c->ret.l = a;
}

N(Sqlite_version) { c->ret.l = hk_dvm_new_string(sqlite3_libversion()); }

static const hk_jhle k_sqlite[] = {
    { "megler/Sqlite", "open", "(Ljava/lang/String;I)J", Sqlite_open },
    { "megler/Sqlite", "close", "(J)V", Sqlite_close },
    { "megler/Sqlite", "errmsg", "(J)Ljava/lang/String;", Sqlite_errmsg },
    { "megler/Sqlite", "errcode", "(J)I", Sqlite_errcode },
    { "megler/Sqlite", "exec", "(JLjava/lang/String;)I", Sqlite_exec },
    { "megler/Sqlite", "changes", "(J)I", Sqlite_changes },
    { "megler/Sqlite", "lastInsertRowid", "(J)J", Sqlite_lastInsertRowid },
    { "megler/Sqlite", "autocommit", "(J)I", Sqlite_autocommit },
    { "megler/Sqlite", "busyTimeout", "(JI)V", Sqlite_busyTimeout },
    { "megler/Sqlite", "prepare", "(JLjava/lang/String;)J", Sqlite_prepare },
    { "megler/Sqlite", "finalizeStatement", "(J)V", Sqlite_finalize },
    { "megler/Sqlite", "reset", "(J)I", Sqlite_reset },
    { "megler/Sqlite", "clearBindings", "(J)V", Sqlite_clearBindings },
    { "megler/Sqlite", "paramCount", "(J)I", Sqlite_paramCount },
    { "megler/Sqlite", "readOnly", "(J)I", Sqlite_readOnly },
    { "megler/Sqlite", "bindNull", "(JI)I", Sqlite_bindNull },
    { "megler/Sqlite", "bindLong", "(JIJ)I", Sqlite_bindLong },
    { "megler/Sqlite", "bindDouble", "(JID)I", Sqlite_bindDouble },
    { "megler/Sqlite", "bindString", "(JILjava/lang/String;)I", Sqlite_bindString },
    { "megler/Sqlite", "bindBlob", "(JI[B)I", Sqlite_bindBlob },
    { "megler/Sqlite", "step", "(J)I", Sqlite_step },
    { "megler/Sqlite", "columnCount", "(J)I", Sqlite_columnCount },
    { "megler/Sqlite", "columnName", "(JI)Ljava/lang/String;", Sqlite_columnName },
    { "megler/Sqlite", "columnType", "(JI)I", Sqlite_columnType },
    { "megler/Sqlite", "columnLong", "(JI)J", Sqlite_columnLong },
    { "megler/Sqlite", "columnDouble", "(JI)D", Sqlite_columnDouble },
    { "megler/Sqlite", "columnText", "(JI)Ljava/lang/String;", Sqlite_columnText },
    { "megler/Sqlite", "columnBlob", "(JI)[B", Sqlite_columnBlob },
    { "megler/Sqlite", "version", "()Ljava/lang/String;", Sqlite_version },
    { NULL, NULL, NULL, NULL },
};

#else

#define NO_SQLITE "this build of the layer has no SQLite in it (it is compiled in when build/sqlite/sqlite3.c is there: tools/sqlite-fetch.sh)"

/* open: no database; and the reason once, where a person reads it. */
N(Sqlite_open)
{
    hk_note_once("java: the app asked for a database (%s), and " NO_SQLITE ". Every database is one that cannot be opened.", hk_dvm_string_utf8(c->args[0].l) ? hk_dvm_string_utf8(c->args[0].l) : "?");
    c->ret.j = 0;
}
N(Sqlite_errmsg) { c->ret.l = hk_dvm_new_string(NO_SQLITE); }
/* SQLITE_CANTOPEN, which the framework makes SQLiteCantOpenDatabaseException of. */
N(Sqlite_errcode) { c->ret.i = 14; }
N(Sqlite_version) { c->ret.l = hk_dvm_new_string("none"); }
N(Sqlite_nothing) { c->ret.j = 0; }

static const hk_jhle k_sqlite[] = {
    { "megler/Sqlite", "open", NULL, Sqlite_open },
    { "megler/Sqlite", "errmsg", NULL, Sqlite_errmsg },
    { "megler/Sqlite", "errcode", NULL, Sqlite_errcode },
    { "megler/Sqlite", "version", NULL, Sqlite_version },
    { "megler/Sqlite", NULL, NULL, Sqlite_nothing },
    { NULL, NULL, NULL, NULL },
};

#endif

const hk_jhle *hk_java_sqlite_natives(void) { return k_sqlite; }
