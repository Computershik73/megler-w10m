/*
 * SQLite inside the core.
 *
 * SQLite is not part of this repository. Where its amalgamation has been put
 * (build/sqlite/sqlite3.c and sqlite3.h, tools/sqlite-fetch.sh) the build
 * scripts define MG_HAVE_SQLITE and name that folder as an include path, and
 * mg_sqlite.c compiles the amalgamation into the core with an operating
 * system layer of this core's own under it: files through the host layer
 * (mg_file), so that a database is the same file handled the same way on
 * every host, locks kept for the process's connections and taken on the file
 * for other processes' (Google Play services' databases are every app's), and
 * mutexes that are the host layer's locks. Where it has not, everything here
 * compiles to nothing, mg_sqlite_available is false, and whoever wants a
 * database says so in the log (drivers/mg_java_sqlite.c).
 *
 * Built: thread-safe (serialized), no loading of extensions, no shared
 * libraries of any kind, temporary tables in memory, full-text search (FTS3,
 * FTS4, FTS5) as Android's own build has it. No write-ahead log: that needs
 * memory shared between connections, which this layer does not give; a
 * database asked to use one stays with its rollback journal, which is what
 * SQLite itself does on a file system without shared memory.
 */
#ifndef MG_SQLITE_H
#define MG_SQLITE_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this build has SQLite in it. */
bool mg_sqlite_available(void);
/*
 * Makes SQLite ready: its mutexes and its file layer are this core's, said to
 * it before its first use. Safe to call any number of times, from any thread.
 * False where there is no SQLite, or it would not start.
 */
bool mg_sqlite_start(void);

#ifdef __cplusplus
}
#endif

#endif
