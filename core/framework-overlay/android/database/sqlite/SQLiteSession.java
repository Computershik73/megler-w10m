// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// SQLiteSession, as far as its transactions: what SQLiteDatabase.getThreadSession gives. Before Android 15's
// beginTransactionReadOnly the only way to a deferred transaction -- one that takes no write lock until something is
// written, so that readers do not wait for each other and for a writer -- is the session's beginTransaction with the mode
// 0, by reflection. androidx.sqlite (FrameworkSQLiteDatabase.beginTransactionReadOnly, which Room 2.7 begins each read
// transaction with) and Google Play services' databases (Phenotype and others) do that, and without getThreadSession fell
// back to SQLiteDatabase.beginTransaction, an exclusive one ("Unable to reflect getThreadSession" in GmsCore's log).
package android.database.sqlite;

import android.os.CancellationSignal;

/**
 * A thread's use of a database (hidden API). Megler's SQLiteDatabase is one connection that a transaction keeps for its
 * thread, so the session of every thread is the database's one, which answers for the thread that asks: its transaction
 * is the database's, begun in the mode asked for, and ends through either. The statements Android's session runs as well
 * (prepare, execute...) are not here: the database runs them.
 */
public final class SQLiteSession {
    /** No lock until the first statement: a read lock at a read, the write lock at a write ("BEGIN;"). */
    public static final int TRANSACTION_MODE_DEFERRED = 0;
    /** The write lock at once; others can read ("BEGIN IMMEDIATE;"). */
    public static final int TRANSACTION_MODE_IMMEDIATE = 1;
    /** The write lock at once, and nobody else reads either outside WAL ("BEGIN EXCLUSIVE;"). */
    public static final int TRANSACTION_MODE_EXCLUSIVE = 2;

    private final SQLiteDatabase mDatabase;

    SQLiteSession(SQLiteDatabase database) { mDatabase = database; }

    /** Whether the calling thread is in a transaction. */
    public boolean hasTransaction() { return mDatabase.inTransaction(); }
    public boolean hasNestedTransaction() { return mDatabase.inNestedTransaction(); }
    /** Whether the calling thread has the connection now. */
    public boolean hasConnection() { return mDatabase.isDbLockedByCurrentThread(); }

    /**
     * Begins a transaction of the mode, or one inside the thread's transaction. `connectionFlags` says which of Android's
     * pooled connections to take; there is one.
     */
    public void beginTransaction(int transactionMode, SQLiteTransactionListener transactionListener, int connectionFlags, CancellationSignal cancellationSignal) {
        if (cancellationSignal != null) cancellationSignal.throwIfCanceled();
        mDatabase.beginTransaction(transactionListener, transactionMode);
    }
    public void setTransactionSuccessful() { mDatabase.setTransactionSuccessful(); }
    public void endTransaction(CancellationSignal cancellationSignal) { mDatabase.endTransaction(); }
    public boolean yieldTransaction(long sleepAfterYieldDelayMillis, boolean throwIfUnsafe, CancellationSignal cancellationSignal) {
        if (throwIfUnsafe) {
            if (!mDatabase.inTransaction()) throw new IllegalStateException("Cannot perform this operation because there is no current transaction.");
            if (mDatabase.inNestedTransaction()) throw new IllegalStateException("Cannot perform this operation because a nested transaction is in progress.");
        }
        return mDatabase.yieldIfContendedSafely(sleepAfterYieldDelayMillis);
    }
}
