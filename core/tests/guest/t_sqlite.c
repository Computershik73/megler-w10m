/*
 * SQLite itself as guest code: the amalgamation (build/sqlite/sqlite3.c) with its own unix VFS, as an app that carries
 * its own SQLite has it (Signal's SQLCipher, Telegram's inside libtmessages), over the layer's file calls: pread and
 * pwrite, fsync, ftruncate, fcntl's locks, the mapped -shm file of a write-ahead log, unlink and rename. What the tests
 * (mg_tests.c, test_guest_sqlite, and mg_tests --guest-sqlite) do with it:
 *
 *   a bank: ACCOUNTS accounts of START each, and a log of the transfers with a pad made of each one's numbers, some long
 *   enough for overflow pages. Threads with a connection each move money about; the sum never changes, every pad is what
 *   its row says, and PRAGMA integrity_check says ok -- also after the process was ended at any moment.
 *
 * Unlike the other guest libraries this one is built with the NDK (its headers and libc stand-in), since SQLite includes
 * the C library's headers: tools/build-guest.sh makes it only where the NDK and the amalgamation are.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define ACCOUNTS 64
#define START 1000

static char g_said[400];

/* What went wrong last, in words; or what a check counted. */
const char *t_sql_said(void) { return g_said; }

static int failed(sqlite3 *db, const char *what, int rc)
{
    snprintf(g_said, sizeof(g_said), "%.200s: %d (%.150s)", what, rc, db ? sqlite3_errmsg(db) : "no database");
    return rc ? rc : -1;
}

const char *t_sql_version(void) { return sqlite3_libversion(); }

void *t_sql_open(const char *path)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        failed(db, "open", rc);
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 30000);
    return db;
}

int t_sql_close(void *db) { return sqlite3_close((sqlite3 *)db); }

int t_sql_exec(void *db, const char *sql)
{
    int rc = sqlite3_exec((sqlite3 *)db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) failed((sqlite3 *)db, sql, rc);
    return rc;
}

/* The first column of a statement's first row as a number; -1000 less the error's code when it gives none. */
int t_sql_int(void *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, sql, -1, &st, NULL), v;
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    else v = -1000 - failed((sqlite3 *)db, sql, rc);
    sqlite3_finalize(st);
    return v;
}

/* PRAGMA integrity_check: 0 when it says ok, else what it says first is in t_sql_said. */
int t_sql_ok(void *db)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)db, "PRAGMA integrity_check", -1, &st, NULL), bad = 1;
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(st, 0);
        bad = !text || strcmp(text, "ok") != 0;
        if (bad) snprintf(g_said, sizeof(g_said), "integrity_check: %.300s", text ? text : "(null)");
    } else {
        failed((sqlite3 *)db, "integrity_check", rc);
    }
    sqlite3_finalize(st);
    return bad;
}

/* A transfer's pad: its length and bytes follow from its numbers. */
static int pad_size(int a, int b, int amount)
{
    return (a * 131 + b * 31 + amount * 7) % 1800 + ((a + b + amount) % 13 == 0 ? 6000 : 0);
}

static void pad_fill(unsigned char *pad, int n, int a, int b, int amount)
{
    int i;
    for (i = 0; i < n; i++) pad[i] = (unsigned char)(a + b * 3 + amount * 5 + i * 7);
}

/* The database made, in the journal mode asked for (a write-ahead log, or the rollback journal), and the bank in it. */
int t_sql_setup(const char *path, int wal)
{
    sqlite3 *db = (sqlite3 *)t_sql_open(path);
    int rc, i;
    if (!db) return -1;
    rc = t_sql_exec(db, wal ? "PRAGMA journal_mode = WAL" : "PRAGMA journal_mode = DELETE");
    if (rc == SQLITE_OK) rc = t_sql_exec(db, wal ? "PRAGMA synchronous = NORMAL" : "PRAGMA synchronous = FULL");
    if (rc == SQLITE_OK)
        rc = t_sql_exec(db, "CREATE TABLE IF NOT EXISTS acct(id INTEGER PRIMARY KEY, bal INTEGER);"
                            "CREATE TABLE IF NOT EXISTS log(id INTEGER PRIMARY KEY AUTOINCREMENT, a INTEGER, b INTEGER, amount INTEGER, pad BLOB);"
                            "CREATE INDEX IF NOT EXISTS log_a ON log(a, amount);");
    if (rc == SQLITE_OK && t_sql_int(db, "SELECT count(*) FROM acct") == 0) {
        rc = t_sql_exec(db, "BEGIN IMMEDIATE");
        for (i = 1; rc == SQLITE_OK && i <= ACCOUNTS; i++) {
            char sql[96];
            snprintf(sql, sizeof(sql), "INSERT INTO acct(id, bal) VALUES(%d, %d)", i, START);
            rc = t_sql_exec(db, sql);
        }
        if (rc == SQLITE_OK) rc = t_sql_exec(db, "COMMIT");
    }
    sqlite3_close(db);
    return rc;
}

typedef struct worker {
    pthread_t thread;
    const char *path;
    int rounds;
    unsigned seed;
    int errors;
    int moved;
    char said[400];
} worker;

static unsigned next(unsigned *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static void worker_failed(worker *w, sqlite3 *db, const char *what, int rc)
{
    if (!w->errors++) snprintf(w->said, sizeof(w->said), "%.200s: %d (%.150s)", what, rc, db ? sqlite3_errmsg(db) : "no database");
}

static int worker_exec(worker *w, sqlite3 *db, const char *sql)
{
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) worker_failed(w, db, sql, rc);
    return rc;
}

static void *work(void *arg)
{
    worker *w = (worker *)arg;
    sqlite3 *db = NULL;
    sqlite3_stmt *note = NULL, *sum = NULL;
    static unsigned char pads[16][8192];                 /* (one per thread, by its seed's low bits: no stack this big) */
    unsigned char *pad = pads[w->seed & 15];
    int rc = sqlite3_open_v2(w->path, &db, SQLITE_OPEN_READWRITE, NULL), round;
    if (rc != SQLITE_OK) {
        worker_failed(w, db, "open", rc);
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 30000);
    rc = sqlite3_prepare_v2(db, "INSERT INTO log(a, b, amount, pad) VALUES(?, ?, ?, ?)", -1, &note, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(db, "SELECT sum(bal), count(*) FROM acct", -1, &sum, NULL);
    if (rc != SQLITE_OK) worker_failed(w, db, "prepare", rc);
    for (round = 0; rc == SQLITE_OK && round < w->rounds && w->errors < 8; round++) {
        int a = (int)(next(&w->seed) % ACCOUNTS) + 1, b = (int)(next(&w->seed) % ACCOUNTS) + 1, amount = (int)(next(&w->seed) % 50) + 1;
        int n = pad_size(a, b, amount), step;
        char sql[160];
        if (worker_exec(w, db, "BEGIN IMMEDIATE") != SQLITE_OK) continue;
        snprintf(sql, sizeof(sql), "UPDATE acct SET bal = bal - %d WHERE id = %d; UPDATE acct SET bal = bal + %d WHERE id = %d", amount, a, amount, b);
        step = worker_exec(w, db, sql);
        if (step == SQLITE_OK) {
            pad_fill(pad, n, a, b, amount);
            sqlite3_bind_int(note, 1, a);
            sqlite3_bind_int(note, 2, b);
            sqlite3_bind_int(note, 3, amount);
            sqlite3_bind_blob(note, 4, pad, n, SQLITE_STATIC);
            step = sqlite3_step(note);
            if (step == SQLITE_DONE) step = SQLITE_OK;
            else worker_failed(w, db, "insert", step);
            sqlite3_reset(note);
        }
        /* The log is kept short from its old end: pages freed and taken again. */
        if (step == SQLITE_OK && round % 24 == 23) step = worker_exec(w, db, "DELETE FROM log WHERE id IN (SELECT id FROM log ORDER BY id LIMIT 20)");
        if (step == SQLITE_OK) step = worker_exec(w, db, "COMMIT");
        if (step != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            continue;
        }
        w->moved++;
        /* What a reader sees between two writes: all the money, in all the accounts. */
        if (round % 4 == 3) {
            step = sqlite3_step(sum);
            if (step != SQLITE_ROW) worker_failed(w, db, "sum", step);
            else if (sqlite3_column_int(sum, 0) != ACCOUNTS * START || sqlite3_column_int(sum, 1) != ACCOUNTS) {
                snprintf(sql, sizeof(sql), "the sum is %d in %d accounts", sqlite3_column_int(sum, 0), sqlite3_column_int(sum, 1));
                worker_failed(w, db, sql, 0);
            }
            sqlite3_reset(sum);
        }
        /* (A busy checkpoint is no error: another connection reads or writes.) */
        if (round % 64 == 63) sqlite3_exec(db, round % 256 == 255 ? "PRAGMA wal_checkpoint(TRUNCATE)" : "PRAGMA wal_checkpoint(PASSIVE)", NULL, NULL, NULL);
    }
    sqlite3_finalize(note);
    sqlite3_finalize(sum);
    rc = sqlite3_close(db);
    if (rc != SQLITE_OK) worker_failed(w, NULL, "close", rc);
    return NULL;
}

typedef struct job {
    int threads;
    worker workers[16];
} job;

/* `threads` threads (16 at most) started, each to make `rounds` transfers through a connection of its own. */
void *t_sql_work_start(const char *path, int threads, int rounds, unsigned seed)
{
    job *j = (job *)calloc(1, sizeof(*j));
    int i;
    if (!j) return NULL;
    j->threads = threads > 16 ? 16 : threads;
    for (i = 0; i < j->threads; i++) {
        worker *w = &j->workers[i];
        w->path = path;
        w->rounds = rounds;
        w->seed = seed * 16u + (unsigned)i;
        if (pthread_create(&w->thread, NULL, work, w) != 0) {
            w->errors++;
            snprintf(w->said, sizeof(w->said), "no thread");
            j->threads = i;
            break;
        }
    }
    return j;
}

/* They are waited for: how many errors they met (the first of them in t_sql_said), and in *moved how many transfers. */
int t_sql_work_wait(void *started, int *moved)
{
    job *j = (job *)started;
    int i, errors = 0, said = 0;
    if (!j) return -1;
    if (moved) *moved = 0;
    for (i = 0; i < j->threads; i++) pthread_join(j->workers[i].thread, NULL);
    for (i = 0; i < 16; i++) {
        worker *w = &j->workers[i];
        errors += w->errors;
        if (moved) *moved += w->moved;
        if (w->errors && !said++) snprintf(g_said, sizeof(g_said), "thread %d: %.380s", i, w->said);
    }
    free(j);
    return errors;
}

int t_sql_work(const char *path, int threads, int rounds, unsigned seed)
{
    return t_sql_work_wait(t_sql_work_start(path, threads, rounds, seed), NULL);
}

/*
 * The database as it is now: 0 when integrity_check says ok, the money is all there and every row of the log has its
 * pad, with the count of the log's rows in t_sql_said; else what is wrong first is there.
 */
int t_sql_check(const char *path)
{
    sqlite3 *db = (sqlite3 *)t_sql_open(path);
    sqlite3_stmt *st = NULL;
    static unsigned char pad[8192];
    int bad, rc, rows = 0;
    if (!db) return 1;
    bad = t_sql_ok(db);
    if (!bad) {
        int sum = t_sql_int(db, "SELECT sum(bal) FROM acct"), accounts = t_sql_int(db, "SELECT count(*) FROM acct");
        if (sum != ACCOUNTS * START || accounts != ACCOUNTS) {
            if (sum > -1000) snprintf(g_said, sizeof(g_said), "the sum is %d in %d accounts", sum, accounts);
            bad = 2;
        }
    }
    if (!bad) {
        rc = sqlite3_prepare_v2(db, "SELECT id, a, b, amount, pad FROM log ORDER BY id", -1, &st, NULL);
        while (rc == SQLITE_OK && !bad) {
            int a, b, amount, n;
            rc = sqlite3_step(st);
            if (rc != SQLITE_ROW) break;
            a = sqlite3_column_int(st, 1);
            b = sqlite3_column_int(st, 2);
            amount = sqlite3_column_int(st, 3);
            n = pad_size(a, b, amount);
            pad_fill(pad, n, a, b, amount);
            if (sqlite3_column_bytes(st, 4) != n || (n && memcmp(sqlite3_column_blob(st, 4), pad, (size_t)n) != 0)) {
                snprintf(g_said, sizeof(g_said), "row %d of the log (%d to %d, %d): its pad of %d bytes is not what it was written as (%d bytes)",
                         sqlite3_column_int(st, 0), a, b, amount, sqlite3_column_bytes(st, 4), n);
                bad = 3;
            }
            rows++;
            rc = SQLITE_OK;
        }
        if (!bad && rc != SQLITE_DONE) {
            failed(db, "reading the log", rc);
            bad = 4;
        }
        sqlite3_finalize(st);
    }
    rc = sqlite3_close(db);
    if (!bad && rc != SQLITE_OK) {
        failed(NULL, "close", rc);
        bad = 5;
    }
    if (!bad) snprintf(g_said, sizeof(g_said), "%d rows in the log", rows);
    return bad;
}
