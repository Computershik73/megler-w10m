// Overlay of core/framework/src/megler/Broadcasts.java (tools/build-framework.sh).
// It differs in the battery's broadcasts: what the sticky one says, that a receiver registered for it receives it, and that
// a change of the battery is broadcast at all.
//
// Why. Android keeps the last ACTION_BATTERY_CHANGED and gives it to whoever registers for it, in two ways at once: as the
// value of registerReceiver, and -- when a receiver was passed -- by a call of that receiver's onReceive soon after. The
// original did the first only, and an app that shows the battery from its receiver, which is the usual way, never heard and
// showed its zeros: Device Info's card read "Battery 0%  Voltage: 0 mV, Temperature: 0C". And the intent said four things
// (level, scale, status, plugged) with nothing for the voltage, the temperature, the health or the technology. Now the intent
// is made of what the core says the battery is (megler.Native.battery, which is mg_battery_read in the core: one place, which
// the shell can fill from the phone), with the extras Android's has, and is delivered. And registerReceiver(null, filter),
// which only asks for a sticky intent, no longer puts its null into the list of receivers. And when the battery becomes
// another -- the shell says so to the core (megler_set_battery), and the Java driver, from the main loop, calls batteryChanged
// here -- registered receivers hear ACTION_BATTERY_CHANGED again, and ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED
// is sent when the phone went on or off a charger: the original had a battery that never changed and sent neither.
//
// And it keeps the network's sticky broadcast as it keeps the battery's: CONNECTIVITY_ACTION, which Android keeps for the
// network it has and gives to whoever registers for it, the same two ways. The original kept none, and a receiver registered
// for it heard nothing until the network changed, which under Megler is never. Asphalt 9 registers one in its activity's
// onResume and tells its native code the connection's type from that receiver's onReceive and from nowhere else: its native
// code, never told, took the phone to have no network, never opened a socket, and said "no network" over a loading screen that
// stood still (package 275), while the game's Java side, which asks ConnectivityManager, reached its server. The intent says
// what ConnectivityManager says (a connected Wi-Fi network), with the extras Android's has.
//
// And it differs in who receives a broadcast (send). An intent that names a component goes to that component alone, as on
// Android: not to the receivers registered at run time, not to the manifest's receivers whose filters list its action.
// The original gave it to all of them. Spotify's WidgetUpdateCoordinator is a manifest receiver for its own ACTION_USER_LOGIN,
// ACTION_USER_LOGOUT and ACTION_END_SESSION, and passes each to its widget providers by an intent with the same action that names
// the provider: that intent came back to the coordinator, which passed it on again, without end, and the main thread spent
// its time on the deliveries and on the lock of its queue (one window of 63 s drew one frame). An implicit intent goes to
// the manifest's receivers whose filters take it as Android matches a filter (Manifest.match). And a receiver the manifest
// disables, or the app disabled with setComponentEnabledSetting, receives nothing, as on Android; Spotify has two.
// And a broadcast that names microG's package or one of its receivers goes to microG (megler.MicroG.send), which an app
// hosts: microG's settings send one to its check-in trigger.
// And a receiver of the manifest is made by the app's component factory (AppRunner.componentFactory), as on Android 9.
// A receiver registered at run time gets the Context that registered it, including one of microG's own. Giving all such
// receivers the client's Application made microG look up com.android.vending.microg.settings inside Play Store.
// And a receiver registered with a Handler hears on that Handler's thread, as on Android; the others on the main thread.
// GmsCore registers some of its own with a Handler of a thread of its own and makes blocking calls in onReceive, which it
// refuses to make on the main thread: run there, one ended Play Store.
// And a broadcast can be sent to the receivers that hold a permission, as Android's sendBroadcast(intent, receiverPermission)
// sends it: megler.Phone's SMS_RECEIVED to those that hold RECEIVE_SMS, PHONE_STATE with the caller's number to those that hold
// READ_CALL_LOG as well and without it to the others (a receiver's permissions are its package's, megler.Phone.holds).
// Everything else is the original's, unchanged.
package megler;

import android.content.*;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** Broadcasts within the app: to receivers registered at run time and those in the manifest; the system's own few (battery, network) answered. */
public final class Broadcasts {
    private static final ArrayList<Object[]> sReceivers = new ArrayList<>();
    /** The sticky broadcasts Megler keeps, as Android keeps them: one intent for each, the last that was sent. */
    private static final String[] STICKY = { Intent.ACTION_BATTERY_CHANGED, ConnectivityManager.CONNECTIVITY_ACTION };
    private Broadcasts() {}
    public static Intent register(Context context, BroadcastReceiver r, IntentFilter f) { return register(context, r, f, null); }
    /** A receiver that hears on the thread of `handler` (the main thread when it is null). */
    public static Intent register(Context context, BroadcastReceiver r, IntentFilter f, Handler handler) {
        final Handler on = handler != null ? handler : new Handler(Looper.getMainLooper());
        /* With no receiver there is nothing to keep: the call only asks for the sticky intent. (A null in the list was a NullPointerException waiting for the first broadcast its filter matched.) */
        if (r != null) synchronized (sReceivers) { sReceivers.add(new Object[] { r, f, context, on }); }
        Intent first = null;
        if (f != null) for (String action : STICKY) {
            if (!f.hasAction(action)) continue;
            final Intent b = action.equals(Intent.ACTION_BATTERY_CHANGED) ? battery() : connectivity();
            /* A sticky broadcast reaches the receiver it was registered with as any broadcast does: later, on its thread, if it is still registered then. Each the filter takes; the first is the value. */
            if (r != null) on.post(() -> { if (registered(r)) r.onReceive(context, b); });
            if (first == null) first = b;
        }
        return first;
    }
    private static boolean registered(BroadcastReceiver r) {
        synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[0] == r) return true; }
        return false;
    }
    /**
     * The battery has become another: its charge, its status or what it is plugged into. The driver calls this on the main thread when the
     * core's has (mg_java.c, battery_news), and here are Android's broadcasts for it, in Android's order: ACTION_POWER_CONNECTED or
     * ACTION_POWER_DISCONNECTED when the phone went on a charger or off it (power 1 or 2; 0 for neither), then ACTION_BATTERY_CHANGED --
     * that one, as on Android, only to receivers registered while the app runs, never to one named in the manifest.
     */
    public static void batteryChanged(int power) {
        if (power != 0) send(AppRunner.application(), new Intent(power == 1 ? Intent.ACTION_POWER_CONNECTED : Intent.ACTION_POWER_DISCONNECTED));
        final Intent b = battery();
        final ArrayList<Object[]> targets = new ArrayList<>();
        synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[1] != null && ((IntentFilter) e[1]).hasAction(Intent.ACTION_BATTERY_CHANGED)) targets.add(e); }
        for (Object[] target : targets) ((Handler) target[3]).post(() -> { BroadcastReceiver r = (BroadcastReceiver) target[0]; if (registered(r)) r.onReceive((Context) target[2], b); });
    }
    /** The battery as Android broadcasts it, from what the core says it is now. */
    public static Intent battery() {
        int[] s = Native.battery();
        Intent b = new Intent(Intent.ACTION_BATTERY_CHANGED);
        b.putExtra("level", s[0]); b.putExtra("scale", s[1]); b.putExtra("status", s[2]); b.putExtra("plugged", s[3]); b.putExtra("health", s[4]);
        b.putExtra("voltage", s[5]); b.putExtra("temperature", s[6]); b.putExtra("present", s[7] != 0); b.putExtra("technology", Native.batteryTechnology());
        b.putExtra("battery_low", s[1] > 0 && s[0] * 100 / s[1] <= 15 && s[3] == 0); b.putExtra("invalid_charger", 0); b.putExtra("seq", 0);
        return b;
    }
    /** The network as Android's sticky CONNECTIVITY_ACTION says it: the one ConnectivityManager reports, with the same extras. */
    public static Intent connectivity() {
        NetworkInfo info = new ConnectivityManager().getActiveNetworkInfo();
        Intent b = new Intent(ConnectivityManager.CONNECTIVITY_ACTION);
        b.putExtra(ConnectivityManager.EXTRA_NETWORK_INFO, info);
        b.putExtra(ConnectivityManager.EXTRA_NETWORK_TYPE, info != null ? info.getType() : ConnectivityManager.TYPE_NONE);
        if (info == null || !info.isConnected()) b.putExtra(ConnectivityManager.EXTRA_NO_CONNECTIVITY, true);
        b.putExtra(ConnectivityManager.EXTRA_INET_CONDITION, info != null && info.isConnected() ? 100 : 0);
        return b;
    }
    public static void unregister(BroadcastReceiver r) { synchronized (sReceivers) { for (int i = sReceivers.size() - 1; i >= 0; i--) if (sReceivers.get(i)[0] == r) sReceivers.remove(i); } }
    public static void send(Context from, Intent i) { send(from, i, null, null, false); }
    /** To the receivers that hold `permission`. */
    public static void send(Context from, Intent i, String permission) { send(from, i, permission, null, false); }
    /** To the receivers that hold `permission` and, with `also` not null, that hold it as well (`holdsAlso`) or do not. */
    public static void send(Context from, Intent i, String permission, String also, boolean holdsAlso) {
        // A broadcast for microG's package goes to microG's receivers (megler.MicroG), as Android sends it to that app.
        if (MicroG.send(from, i)) return;
        // And one of the implicit broadcasts Google's system configuration lets through (CHECKIN_COMPLETE) to its receivers too.
        MicroG.sendImplicit(from, i);
        ArrayList<Object[]> targets = targets(from, i, permission, also, holdsAlso);
        Handler main = new Handler(Looper.getMainLooper());
        // (Each receiver has the intent as a Parcel between processes would make it for it: the objects of another class scope
        // among its extras made again by its own classes, megler.Processes.forLoader.)
        for (Object[] target : targets) (target[3] != null ? (Handler) target[3] : main).post(() -> {
            BroadcastReceiver r = (BroadcastReceiver) target[0];
            r.onReceive((Context) target[2], Processes.forLoader(i, r.getClass().getClassLoader()));
        });
    }

    /**
     * An ordered broadcast, as Context.sendOrderedBroadcast sends it: to the receivers one after another, the registered ones
     * of the higher priority first, each on its thread and given the result the one before left (getResultCode,
     * getResultData, getResultExtras), which it may change or end there (abortBroadcast); one that goes asynchronous
     * (goAsync) holds the next back until its PendingResult's finish(). Then `resultReceiver`, if any, on the thread of
     * `scheduler` (the main thread when it is null), with the result as it ended -- whatever the receivers did, as Android
     * gives it. The original's did nothing with the result receiver, and Play Store, which sends its own receivers one as it
     * takes an install up, never heard its answer (package 543).
     */
    public static void sendOrdered(Context from, Intent i, String permission, BroadcastReceiver resultReceiver, Handler scheduler, int code, String data, Bundle extras) {
        final Context ctx = AppRunner.application() != null ? AppRunner.application() : from;
        ArrayList<Object[]> targets;
        if (MicroG.send(from, i)) targets = new ArrayList<>();
        else {
            MicroG.sendImplicit(from, i);
            targets = targets(from, i, permission, null, false);
        }
        next(targets, 0, i, code, data, extras, resultReceiver, scheduler, ctx);
    }

    /** The ordered broadcast's receiver number `at` given the result, or, past the last (or ended), its result receiver. */
    private static void next(final ArrayList<Object[]> targets, final int at, final Intent i, final int code, final String data, final Bundle extras,
                             final BroadcastReceiver last, final Handler scheduler, final Context ctx) {
        final Handler main = new Handler(Looper.getMainLooper());
        if (at >= targets.size()) {
            if (last != null) (scheduler != null ? scheduler : main).post(() -> {
                BroadcastReceiver.PendingResult p = last.meglerBegin(false, code, data, extras, null);
                try { last.onReceive(ctx, i); } finally { last.meglerEnd(p); }
            });
            return;
        }
        final Object[] target = targets.get(at);
        (target[3] != null ? (Handler) target[3] : main).post(() -> {
            final BroadcastReceiver r = (BroadcastReceiver) target[0];
            final BroadcastReceiver.PendingResult[] mine = new BroadcastReceiver.PendingResult[1];
            mine[0] = r.meglerBegin(true, code, data, extras, () -> {
                BroadcastReceiver.PendingResult p = mine[0];
                next(targets, p.getAbortBroadcast() ? targets.size() : at + 1, i, p.getResultCode(), p.getResultData(), p.getResultExtras(false), last, scheduler, ctx);
            });
            try { r.onReceive((Context) target[2], Processes.forLoader(i, r.getClass().getClassLoader())); } finally { r.meglerEnd(mine[0]); }
        });
    }

    /** The receivers a broadcast reaches, each as {receiver, filter or null, context, handler or null}: registered ones first, by their priority. */
    private static ArrayList<Object[]> targets(Context from, Intent i, String permission, String also, boolean holdsAlso) {
        final Context ctx = AppRunner.application() != null ? AppRunner.application() : from;
        ArrayList<Object[]> targets = new ArrayList<>();
        /* Receivers registered at run time hear implicit intents only: one that names a component is for that component. */
        if (i.getComponent() == null)
            synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[1] != null && ((IntentFilter) e[1]).hasAction(i.getAction()) && allowed((Context) e[2], permission, also, holdsAlso)) targets.add(e); }
        for (Manifest.Component c : Manifest.match(Manifest.receivers, i)) {
            if (!enabled(ctx, c) || !allowed(ctx, permission, also, holdsAlso)) continue;
            // A receiver assigned to another Android process needs that process's
            // Application. Running it in the main process gives it the wrong one
            // (Play Store's instant-app receiver casts it to its process-specific type).
            // The process is named as Android names it (Manifest.processOf): none or an
            // empty name is the app's own (Play Store's AccountRemovedBroadcastReceiver says
            // ""). GmsCore's components of its other processes (persistent, ui: nearly all
            // of them) run in its one process here, as in every client that hosts it
            // (megler.MicroG).
            String process = Manifest.processOf(c);
            if (!process.equals(ctx.getPackageName()) && !GooglePackages.GMS.equals(ctx.getPackageName())) {
                android.util.Log.i("Megler", "receiver " + c.name + " belongs to process " + c.process + "; not dispatched in the main process");
                continue;
            }
            try { targets.add(new Object[] { AppRunner.componentFactory().instantiateReceiver(AppRunner.classLoader(), c.name, i), null, ctx, null }); } catch (Throwable t) { android.util.Log.w("Megler", "receiver " + c.name + ": " + t); }
        }
        // (A stable sort: the registered ones by their filters' priority, higher first, ahead of the manifest's.)
        java.util.Collections.sort(targets, (a, b) -> Integer.compare(b[1] != null ? ((IntentFilter) b[1]).getPriority() : Integer.MIN_VALUE,
                                                                      a[1] != null ? ((IntentFilter) a[1]).getPriority() : Integer.MIN_VALUE));
        return targets;
    }
    private static boolean allowed(Context c, String permission, String also, boolean holdsAlso) {
        return (permission == null || Phone.holds(c, permission)) && (also == null || Phone.holds(c, also) == holdsAlso);
    }
    /** Whether a receiver of the manifest is enabled: as the app last set it (PackageManager), or else as the manifest says. */
    private static boolean enabled(Context ctx, Manifest.Component c) {
        int state = PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        try { state = ctx.getPackageManager().getComponentEnabledSetting(new ComponentName(ctx.getPackageName(), c.name)); } catch (Throwable t) { }
        return state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED || state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && c.enabled;
    }
}
