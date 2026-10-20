/*
 * SQLite's amalgamation, compiled into the core when it is there (see
 * mg_sqlite.h). Nothing else is in this file, and nothing of the core's is
 * included before it: SQLite has names of its own for the integer types
 * (u8, u32, i64) and must not see the core's, nor the core's macros.
 * What stands under it in place of an operating system is mg_sqlite.c.
 */
#if defined(MG_HAVE_SQLITE) && MG_HAVE_SQLITE

/* No operating system layer of SQLite's own, neither Unix's nor Windows': the one below, on every host. */
#define SQLITE_OS_OTHER 1
#define SQLITE_THREADSAFE 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_OMIT_DEPRECATED 1
/*
 * (Not SQLITE_DEFAULT_MEMSTATUS=0, common as it is: with it this version's full-text search corrupted the heap on 32-bit
 * ARM the second time a connection used it -- glibc's "double free or corruption", found by building SQLite with one option
 * at a time. The statistics cost a mutex for each allocation.)
 */
/* Temporary tables and indices in memory: nothing here hands out names for temporary files worth having. */
#define SQLITE_TEMP_STORE 3
/* As Android builds it, where an app's schema or its queries would notice. */
#define SQLITE_ENABLE_FTS3 1
#define SQLITE_ENABLE_FTS3_PARENTHESIS 1
#define SQLITE_ENABLE_FTS4 1
#define SQLITE_ENABLE_FTS5 1
#define SQLITE_DEFAULT_AUTOVACUUM 1
#define SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT 1048576
#define SQLITE_HAVE_ISNAN 1

/* It is somebody else's code, and compiles with warnings of its own at the levels this core is built at. */
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wempty-body"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "sqlite3.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#else

/* (A translation unit must have something in it.) */
typedef int mg_sqlite3_not_built;

#endif
