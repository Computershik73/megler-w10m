// Megler: new in the overlay. What is behind ContentResolver's sync calls: Android's SyncManager for the app's own sync adapters.
//
// They were stubs: the master switch read off, setSyncAutomatically and requestSync did nothing, and no sync adapter ever ran.
// Gmail said its account's sync was off and could not be turned on, its refresh spun for good, and its folders stayed empty
// but for the drafts made here (package 512).
//
// The app's sync adapters are its services that take android.content.SyncAdapter and name a sync-adapter XML in their meta-data
// of that name: the authority, the account type, and whether it is user visible, uploads, syncs accounts in parallel and is
// always syncable. A sync is asked for by the app (requestSync), by a setting turned on, or by a periodic sync; a manual one
// (SYNC_EXTRAS_MANUAL, or ignoring the settings) runs whatever the switches say, any other only with the master switch and the
// account's for that authority on. An account whose syncable state is not known yet (-1) is first initialized (an
// AbstractThreadedSyncAdapter with autoInitialize makes it syncable), then synced. A sync binds the adapter's service as
// Android does, starts the sync on the binder it hands out (AbstractThreadedSyncAdapter.getSyncAdapterBinder) and lets the
// service go when the sync is done; one at a time for an account and authority, a request that comes while it runs is made
// once more after it. The switches are kept in the app's data (shared preferences megler-sync); master and per-account sync
// start on, as on a phone where the account was set up. Status observers are told of each change, as Android tells them.
package megler;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.app.Application;
import android.content.AbstractThreadedSyncAdapter;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.SyncAdapterType;
import android.content.SyncResult;
import android.content.SyncStatusObserver;
import android.content.res.XmlResourceParser;
import android.os.Binder;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import org.xmlpull.v1.XmlPullParser;

public final class Syncs {
    private static final String TAG = "MeglerSync";
    private static final String NS = "http://schemas.android.com/apk/res/android";
    public static final int OBSERVER_SETTINGS = 1, OBSERVER_PENDING = 2, OBSERVER_ACTIVE = 4;

    /** Told the result of a sync (AbstractThreadedSyncAdapter.meglerStart). */
    public interface Done { void finished(SyncResult result); }

    /** The binder an AbstractThreadedSyncAdapter hands out: a sync is started through it. */
    public static final class AdapterBinder extends Binder {
        public final AbstractThreadedSyncAdapter adapter;
        public AdapterBinder(AbstractThreadedSyncAdapter adapter) { this.adapter = adapter; }
    }

    private static final class Adapter {
        String authority, accountType, service;
        boolean userVisible = true, uploads = true, parallel, always;
    }

    private static final class Listener {
        final int mask;
        final SyncStatusObserver observer;
        Listener(int mask, SyncStatusObserver observer) { this.mask = mask; this.observer = observer; }
    }

    private static final Object LOCK = new Object();
    private static List<Adapter> sAdapters;
    private static final HashMap<String, Long> sActive = new HashMap<>();
    private static final HashMap<String, Bundle> sPending = new HashMap<>();
    private static final ArrayList<Listener> sListeners = new ArrayList<>();
    private static final HashMap<String, Runnable> sPeriodic = new HashMap<>();
    private static Handler sHandler;
    private static int sSaid;

    private Syncs() {}

    private static void say(String what) {
        synchronized (LOCK) {
            if (sSaid >= 60) return;
            sSaid++;
        }
        android.util.Log.i(TAG, what);
    }

    private static Application app() { return AppRunner.application(); }

    private static SharedPreferences prefs() { return ContextImpl.app().getSharedPreferences("megler-sync", Context.MODE_PRIVATE); }

    private static String key(String kind, Account a, String authority) {
        return kind + "|" + (a != null ? a.type + "|" + a.name : "|") + "|" + authority;
    }

    private static Handler handler() {
        synchronized (LOCK) {
            if (sHandler == null) {
                HandlerThread t = new HandlerThread("megler-sync");
                t.start();
                sHandler = new Handler(t.getLooper());
            }
            return sHandler;
        }
    }

    /* ------------------------------------------------------------------ the adapters */

    private static List<Adapter> adapters() {
        synchronized (LOCK) {
            if (sAdapters != null) return sAdapters;
            sAdapters = new ArrayList<>();
        }
        ArrayList<Adapter> found = new ArrayList<>();
        Manifest.read();
        for (Manifest.Component c : Manifest.services) {
            boolean takes = false;
            for (Manifest.Filter f : c.filters) takes |= f.actions.contains("android.content.SyncAdapter");
            if (!takes || c.metaData == null || !c.enabled) continue;
            int res = c.metaData.getInt("android.content.SyncAdapter", 0);
            if (res == 0) continue;
            try {
                Adapter a = parse(res);
                if (a == null) continue;
                a.service = c.name;
                found.add(a);
                say("adapter " + c.name + ": " + a.authority + " for accounts of " + a.accountType + (a.always ? ", always syncable" : "")
                    + (a.parallel ? ", accounts in parallel" : ""));
            } catch (Exception e) {
                android.util.Log.w(TAG, "the sync adapter of " + c.name + " could not be read: " + e);
            }
        }
        synchronized (LOCK) { sAdapters = found; }
        return found;
    }

    private static Adapter parse(int res) throws Exception {
        XmlResourceParser p = app().getResources().getXml(res);
        try {
            int t;
            while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                if (t != XmlPullParser.START_TAG || !"sync-adapter".equals(p.getName())) continue;
                Adapter a = new Adapter();
                a.authority = text(p, "contentAuthority");
                a.accountType = text(p, "accountType");
                a.userVisible = p.getAttributeBooleanValue(NS, "userVisible", true);
                a.uploads = p.getAttributeBooleanValue(NS, "supportsUploading", true);
                a.parallel = p.getAttributeBooleanValue(NS, "allowParallelSyncs", false);
                a.always = p.getAttributeBooleanValue(NS, "isAlwaysSyncable", false);
                return a.authority != null && a.accountType != null ? a : null;
            }
            return null;
        } finally {
            p.close();
        }
    }

    private static String text(XmlResourceParser p, String name) {
        int ref = p.getAttributeResourceValue(NS, name, 0);
        if (ref != 0) return app().getResources().getString(ref);
        return p.getAttributeValue(NS, name);
    }

    private static Adapter adapterFor(String authority, String accountType) {
        for (Adapter a : adapters())
            if (a.authority.equals(authority) && (accountType == null || a.accountType.equals(accountType))) return a;
        return null;
    }

    public static SyncAdapterType[] types() {
        List<Adapter> all = adapters();
        SyncAdapterType[] out = new SyncAdapterType[all.size()];
        for (int i = 0; i < out.length; i++) {
            Adapter a = all.get(i);
            out[i] = new SyncAdapterType(a.authority, a.accountType, a.userVisible, a.uploads, a.always, a.parallel, null, Native.packageName());
        }
        return out;
    }

    /* ------------------------------------------------------------------ the switches */

    public static boolean master() { return prefs().getBoolean("master", true); }

    public static void setMaster(boolean on) {
        boolean was = master();
        prefs().edit().putBoolean("master", on).apply();
        if (was == on) return;
        say("the master switch " + (on ? "on" : "off"));
        changed(OBSERVER_SETTINGS);
        if (on) request(null, null, new Bundle());
    }

    public static boolean automatic(Account account, String authority) { return account != null && prefs().getBoolean(key("auto", account, authority), true); }

    public static void setAutomatic(Account account, String authority, boolean on) {
        if (account == null || authority == null) return;
        boolean was = automatic(account, authority);
        prefs().edit().putBoolean(key("auto", account, authority), on).apply();
        if (was == on) return;
        say(authority + " for " + account.type + " " + (on ? "on" : "off"));
        changed(OBSERVER_SETTINGS);
        if (on) request(account, authority, new Bundle());
    }

    public static int syncable(Account account, String authority) {
        if (account == null || authority == null) return 0;
        int v = prefs().getInt(key("syncable", account, authority), Integer.MIN_VALUE);
        if (v != Integer.MIN_VALUE) return v;
        Adapter a = adapterFor(authority, account.type);
        return a == null ? 0 : a.always ? 1 : -1;
    }

    public static void setSyncable(Account account, String authority, int syncable) {
        if (account == null || authority == null) return;
        int v = syncable > 0 ? 1 : syncable < 0 ? -1 : 0;
        int was = syncable(account, authority);
        prefs().edit().putInt(key("syncable", account, authority), v).apply();
        if (was == v) return;
        changed(OBSERVER_SETTINGS);
        if (v > 0 && was <= 0) request(account, authority, new Bundle());
    }

    /* ------------------------------------------------------------------ syncs */

    public static boolean active(Account account, String authority) {
        synchronized (LOCK) {
            if (account != null) return sActive.containsKey(key("sync", account, authority));
            for (String k : sActive.keySet()) if (k.endsWith("|" + authority)) return true;
            return false;
        }
    }

    public static boolean pending(Account account, String authority) {
        synchronized (LOCK) {
            if (account != null) return sPending.containsKey(key("sync", account, authority));
            for (String k : sPending.keySet()) if (k.endsWith("|" + authority)) return true;
            return false;
        }
    }

    /** requestSync: the account's sync of the authority, or every account's of the adapter's type (null), or every authority's (null). */
    public static void request(Account account, String authority, Bundle extras) {
        final Bundle given = extras != null ? new Bundle(extras) : new Bundle();
        boolean manual = given.getBoolean(ContentResolver.SYNC_EXTRAS_MANUAL, false) || given.getBoolean(ContentResolver.SYNC_EXTRAS_IGNORE_SETTINGS, false);
        for (Adapter a : adapters()) {
            if (authority != null && !a.authority.equals(authority)) continue;
            if (account != null && !a.accountType.equals(account.type)) continue;
            Account[] accounts = account != null ? new Account[] { account } : AccountManager.get(app()).getAccountsByType(a.accountType);
            for (Account acc : accounts) {
                if (!manual && (!master() || !automatic(acc, a.authority))) continue;
                int s = syncable(acc, a.authority);
                if (s == 0) continue;
                Bundle x = new Bundle(given);
                if (s < 0) x.putBoolean(ContentResolver.SYNC_EXTRAS_INITIALIZE, true);
                enqueue(a, acc, x);
            }
        }
    }

    public static void cancel(Account account, String authority) {
        synchronized (LOCK) {
            for (String k : new ArrayList<>(sPending.keySet()))
                if ((account == null || k.startsWith("sync|" + account.type + "|" + account.name + "|")) && (authority == null || k.endsWith("|" + authority)))
                    sPending.remove(k);
        }
        changed(OBSERVER_PENDING);
        // A running sync is asked to stop through the adapter it was started on.
        ArrayList<Object[]> stop = new ArrayList<>();
        synchronized (LOCK) {
            for (java.util.Map.Entry<String, Object[]> e : sRunning.entrySet()) {
                Account a = (Account) e.getValue()[1];
                if ((account == null || account.equals(a)) && (authority == null || e.getKey().endsWith("|" + authority))) stop.add(e.getValue());
            }
        }
        for (Object[] r : stop) ((AbstractThreadedSyncAdapter) r[0]).meglerCancel((Account) r[1]);
    }
    /* the running syncs' adapters and accounts, by key */
    private static final HashMap<String, Object[]> sRunning = new HashMap<>();

    private static void enqueue(final Adapter a, final Account account, final Bundle extras) {
        final String k = key("sync", account, a.authority);
        synchronized (LOCK) {
            if (sActive.containsKey(k)) {
                // One at a time: this one once more after it, with these extras.
                sPending.put(k, extras);
                changed(OBSERVER_PENDING);
                return;
            }
            sActive.put(k, SystemClock.elapsedRealtime());
        }
        changed(OBSERVER_ACTIVE);
        final long started = SystemClock.elapsedRealtime();
        final boolean initialize = extras.getBoolean(ContentResolver.SYNC_EXTRAS_INITIALIZE, false);
        say((initialize ? "initializing " : "syncing ") + a.authority + " for an account of " + account.type
            + (extras.getBoolean(ContentResolver.SYNC_EXTRAS_MANUAL, false) ? " (asked for)" : ""));
        final ServiceConnection[] conn = new ServiceConnection[1];
        final java.util.concurrent.atomic.AtomicBoolean over = new java.util.concurrent.atomic.AtomicBoolean();
        final Done done = result -> {
            if (over.getAndSet(true)) return;
            Application app = app();
            if (conn[0] != null) {
                try { app.unbindService(conn[0]); } catch (RuntimeException e) { /* gone already */ }
            }
            finished(a, account, k, initialize, result, SystemClock.elapsedRealtime() - started);
        };
        conn[0] = new ServiceConnection() {
            @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                if (!(binder instanceof AdapterBinder)) {
                    android.util.Log.w(TAG, a.service + " handed out no AbstractThreadedSyncAdapter's binder (" + binder + "): no sync");
                    SyncResult r = new SyncResult();
                    r.databaseError = true;
                    done.finished(r);
                    return;
                }
                try {
                    AbstractThreadedSyncAdapter adapter = ((AdapterBinder) binder).adapter;
                    synchronized (LOCK) { sRunning.put(k, new Object[] { adapter, account }); }
                    adapter.meglerStart(account, a.authority, extras, done);
                } catch (RuntimeException e) {
                    android.util.Log.w(TAG, "the sync of " + a.authority + " could not start", e);
                    SyncResult r = new SyncResult();
                    r.databaseError = true;
                    done.finished(r);
                }
            }
            @Override public void onServiceDisconnected(ComponentName name) {}
            @Override public void onNullBinding(ComponentName name) {
                android.util.Log.w(TAG, a.service + " handed out no binder: no sync");
                SyncResult r = new SyncResult();
                r.databaseError = true;
                done.finished(r);
            }
        };
        Intent i = new Intent("android.content.SyncAdapter").setComponent(new ComponentName(Native.packageName(), a.service));
        boolean bound;
        try {
            bound = app().bindService(i, conn[0], Context.BIND_AUTO_CREATE);
        } catch (RuntimeException e) {
            android.util.Log.w(TAG, a.service + " could not be bound", e);
            bound = false;
        }
        if (!bound) {
            conn[0] = null;
            SyncResult r = new SyncResult();
            r.databaseError = true;
            done.finished(r);
        }
    }

    private static void finished(Adapter a, Account account, String k, boolean initialize, SyncResult result, long ms) {
        Bundle again;
        synchronized (LOCK) {
            sActive.remove(k);
            sRunning.remove(k);
            again = sPending.remove(k);
        }
        say((initialize ? "initialized " : "synced ") + a.authority + " in " + ms + " ms" + (result.hasError() ? ", with errors: " + result : result.madeSomeProgress() ? ":" + result.stats : ""));
        changed(OBSERVER_ACTIVE | (again != null ? OBSERVER_PENDING : 0));
        if (initialize && !result.hasError() && syncable(account, a.authority) > 0 && (master() && automatic(account, a.authority) || again != null)) {
            Bundle x = again != null ? again : new Bundle();
            x.remove(ContentResolver.SYNC_EXTRAS_INITIALIZE);
            enqueue(a, account, x);
        } else if (again != null) {
            again.remove(ContentResolver.SYNC_EXTRAS_INITIALIZE);
            enqueue(a, account, again);
        } else if (result.fullSyncRequested && !result.hasHardError()) {
            enqueue(a, account, new Bundle());
        }
    }

    /* ------------------------------------------------------------------ periodic syncs */

    public static void addPeriodic(final Account account, final String authority, Bundle extras, long seconds) {
        if (account == null || authority == null) return;
        final String k = key("periodic", account, authority);
        final Bundle x = extras != null ? new Bundle(extras) : new Bundle();
        final long ms = Math.max(seconds, 60) * 1000L;
        final Handler h = handler();
        Runnable r = new Runnable() {
            @Override public void run() {
                synchronized (LOCK) { if (sPeriodic.get(k) != this) return; }
                request(account, authority, x);
                h.postDelayed(this, ms);
            }
        };
        Runnable was;
        synchronized (LOCK) { was = sPeriodic.put(k, r); }
        if (was != null) h.removeCallbacks(was);
        h.postDelayed(r, ms);
    }

    public static void removePeriodic(Account account, String authority) {
        Runnable was;
        synchronized (LOCK) { was = sPeriodic.remove(key("periodic", account, authority)); }
        if (was != null) handler().removeCallbacks(was);
    }

    /* ------------------------------------------------------------------ observers */

    public static Object addListener(int mask, SyncStatusObserver observer) {
        if (observer == null) throw new IllegalArgumentException("you passed in a null callback");
        Listener l = new Listener(mask, observer);
        synchronized (LOCK) { sListeners.add(l); }
        return l;
    }

    public static void removeListener(Object handle) {
        synchronized (LOCK) { sListeners.remove(handle); }
    }

    private static void changed(final int which) {
        final ArrayList<Listener> now;
        synchronized (LOCK) { now = new ArrayList<>(sListeners); }
        if (now.isEmpty()) return;
        // As Android's binder thread does: on a thread of their own, not the caller's.
        handler().post(() -> {
            for (Listener l : now) {
                if ((l.mask & which) == 0) continue;
                try { l.observer.onStatusChanged(which & l.mask); } catch (RuntimeException e) { android.util.Log.w(TAG, "a sync status observer threw", e); }
            }
        });
    }
}
