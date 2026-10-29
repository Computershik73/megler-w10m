// Overlay of core/framework/gen-src/android/content/AbstractThreadedSyncAdapter.java (tools/build-framework.sh), which had the
// signatures alone: no context, no binder, and no sync of an app's ever ran (Gmail's folders stayed empty, package 512). This is
// Android's: the binder its service hands out (getSyncAdapterBinder), through which megler.Syncs starts a sync, and each sync on
// a thread of its own, onPerformSync with a client of the authority's provider and a SyncResult that goes back when it is done;
// with autoInitialize, an initialization request makes the account syncable and does no more; one sync at a time, or one an
// account with allowParallelSyncs; a cancel interrupts the sync's thread.
package android.content;

import android.accounts.Account;
import android.os.Bundle;
import android.os.IBinder;
import java.util.HashMap;

public abstract class AbstractThreadedSyncAdapter {
    public static final int LOG_SYNC_DETAILS = 2743;
    private static int sThreads;

    private final Context mContext;
    private final boolean mAutoInitialize, mAllowParallelSyncs;
    private final megler.Syncs.AdapterBinder mBinder = new megler.Syncs.AdapterBinder(this);
    /* the running syncs' threads, by account (null when syncs do not run in parallel) */
    private final HashMap<Account, Thread> mThreads = new HashMap<>();

    public AbstractThreadedSyncAdapter(Context context, boolean autoInitialize) { this(context, autoInitialize, false); }

    public AbstractThreadedSyncAdapter(Context context, boolean autoInitialize, boolean allowParallelSyncs) {
        mContext = context;
        mAutoInitialize = autoInitialize;
        mAllowParallelSyncs = allowParallelSyncs;
    }

    AbstractThreadedSyncAdapter() { this(null, false); }

    public Context getContext() { return mContext; }

    public final IBinder getSyncAdapterBinder() { return mBinder; }

    public abstract void onPerformSync(Account account, Bundle extras, String authority, ContentProviderClient provider, SyncResult syncResult);

    public void onSecurityException(Account account, Bundle extras, String authority, SyncResult syncResult) {}

    public void onSyncCanceled() {
        Thread t;
        synchronized (mThreads) { t = mThreads.isEmpty() ? null : mThreads.values().iterator().next(); }
        if (t != null) t.interrupt();
    }

    public void onSyncCanceled(Thread thread) { thread.interrupt(); }

    public boolean onUnsyncableAccount() { return true; }

    /** megler.Syncs: a sync of `account` for `authority`, on a thread of its own; `done` is given its result. */
    public final void meglerStart(final Account account, final String authority, final Bundle extras, final megler.Syncs.Done done) {
        if (mAutoInitialize && extras != null && extras.getBoolean(ContentResolver.SYNC_EXTRAS_INITIALIZE, false)) {
            try {
                if (ContentResolver.getIsSyncable(account, authority) < 0) ContentResolver.setIsSyncable(account, authority, 1);
            } finally {
                done.finished(new SyncResult());
            }
            return;
        }
        final Account key = mAllowParallelSyncs ? account : null;
        synchronized (mThreads) {
            if (mThreads.containsKey(key)) {
                done.finished(SyncResult.ALREADY_IN_PROGRESS);
                return;
            }
            Thread t = new Thread(() -> {
                SyncResult result = new SyncResult();
                ContentProviderClient client = null;
                try {
                    client = mContext.getContentResolver().acquireContentProviderClient(authority);
                    if (client != null) onPerformSync(account, extras, authority, client, result);
                    else result.databaseError = true;
                } catch (SecurityException e) {
                    onSecurityException(account, extras, authority, result);
                    result.databaseError = true;
                } finally {
                    if (client != null) client.release();
                    synchronized (mThreads) { mThreads.remove(key); }
                    done.finished(result);
                }
            }, "SyncAdapterThread-" + (++sThreads));
            mThreads.put(key, t);
            t.start();
        }
    }

    /** megler.Syncs: the sync of `account` cancelled. */
    public final void meglerCancel(Account account) {
        Thread t;
        synchronized (mThreads) { t = mThreads.get(mAllowParallelSyncs ? account : null); }
        if (t == null) return;
        if (mAllowParallelSyncs) onSyncCanceled(t);
        else onSyncCanceled();
    }
}
