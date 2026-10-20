// Overlay of core/framework/src/meglet/Broadcasts.java (tools/build-framework.sh).
// It differs in the battery's broadcasts: what the sticky one says, that a receiver registered for it receives it, and that
// a change of the battery is broadcast at all.
//
// Why. Android keeps the last ACTION_BATTERY_CHANGED and gives it to whoever registers for it, in two ways at once: as the
// value of registerReceiver, and -- when a receiver was passed -- by a call of that receiver's onReceive soon after. The
// original did the first only, and an app that shows the battery from its receiver, which is the usual way, never heard and
// showed its zeros: Device Info's card read "Battery 0%  Voltage: 0 mV, Temperature: 0C". And the intent said four things
// (level, scale, status, plugged) with nothing for the voltage, the temperature, the health or the technology. Now the intent
// is made of what the core says the battery is (meglet.Native.battery, which is hk_battery_read in the core: one place, which
// the shell can fill from the phone), with the extras Android's has, and is delivered. And registerReceiver(null, filter),
// which only asks for a sticky intent, no longer puts its null into the list of receivers. And when the battery becomes
// another -- the shell says so to the core (meglet_set_battery), and the Java driver, from the main loop, calls batteryChanged
// here -- registered receivers hear ACTION_BATTERY_CHANGED again, and ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED
// is sent when the phone went on or off a charger: the original had a battery that never changed and sent neither.
//
// And it keeps the network's sticky broadcast as it keeps the battery's: CONNECTIVITY_ACTION, which Android keeps for the
// network it has and gives to whoever registers for it, the same two ways. The original kept none, and a receiver registered
// for it heard nothing until the network changed, which under Meglet is never. Asphalt 9 registers one in its activity's
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
// Everything else is the original's, unchanged.
package meglet;

import android.content.*;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** Broadcasts within the app: to receivers registered at run time and those in the manifest; the system's own few (battery, network) answered. */
public final class Broadcasts {
    private static final ArrayList<Object[]> sReceivers = new ArrayList<>();
    /** The sticky broadcasts Meglet keeps, as Android keeps them: one intent for each, the last that was sent. */
    private static final String[] STICKY = { Intent.ACTION_BATTERY_CHANGED, ConnectivityManager.CONNECTIVITY_ACTION };
    private Broadcasts() {}
    public static Intent register(BroadcastReceiver r, IntentFilter f) {
        /* With no receiver there is nothing to keep: the call only asks for the sticky intent. (A null in the list was a NullPointerException waiting for the first broadcast its filter matched.) */
        if (r != null) synchronized (sReceivers) { sReceivers.add(new Object[] { r, f }); }
        Intent first = null;
        if (f != null) for (String action : STICKY) {
            if (!f.hasAction(action)) continue;
            final Intent b = action.equals(Intent.ACTION_BATTERY_CHANGED) ? battery() : connectivity();
            /* A sticky broadcast reaches the receiver it was registered with as any broadcast does: later, on the main thread, if it is still registered then. Each the filter takes; the first is the value. */
            if (r != null) new Handler(Looper.getMainLooper()).post(() -> { if (registered(r)) r.onReceive(AppRunner.application(), b); });
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
     * core's has (hk_java.c, battery_news), and here are Android's broadcasts for it, in Android's order: ACTION_POWER_CONNECTED or
     * ACTION_POWER_DISCONNECTED when the phone went on a charger or off it (power 1 or 2; 0 for neither), then ACTION_BATTERY_CHANGED --
     * that one, as on Android, only to receivers registered while the app runs, never to one named in the manifest.
     */
    public static void batteryChanged(int power) {
        if (power != 0) send(AppRunner.application(), new Intent(power == 1 ? Intent.ACTION_POWER_CONNECTED : Intent.ACTION_POWER_DISCONNECTED));
        final Intent b = battery();
        final ArrayList<BroadcastReceiver> targets = new ArrayList<>();
        synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[1] != null && ((IntentFilter) e[1]).hasAction(Intent.ACTION_BATTERY_CHANGED)) targets.add((BroadcastReceiver) e[0]); }
        Handler h = new Handler(Looper.getMainLooper());
        for (BroadcastReceiver r : targets) h.post(() -> { if (registered(r)) r.onReceive(AppRunner.application(), b); });
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
    public static void send(Context from, Intent i) {
        final Context ctx = AppRunner.application() != null ? AppRunner.application() : from;
        ArrayList<BroadcastReceiver> targets = new ArrayList<>();
        /* Receivers registered at run time hear implicit intents only: one that names a component is for that component. */
        if (i.getComponent() == null)
            synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[1] != null && ((IntentFilter) e[1]).hasAction(i.getAction())) targets.add((BroadcastReceiver) e[0]); }
        for (Manifest.Component c : Manifest.match(Manifest.receivers, i)) {
            if (!enabled(ctx, c)) continue;
            try { targets.add((BroadcastReceiver) Class.forName(c.name).newInstance()); } catch (Throwable t) { android.util.Log.w("Meglet", "receiver " + c.name + ": " + t); }
        }
        Handler h = new Handler(Looper.getMainLooper());
        for (BroadcastReceiver r : targets) h.post(() -> r.onReceive(ctx, i));
    }
    /** Whether a receiver of the manifest is enabled: as the app last set it (PackageManager), or else as the manifest says. */
    private static boolean enabled(Context ctx, Manifest.Component c) {
        int state = PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
        try { state = ctx.getPackageManager().getComponentEnabledSetting(new ComponentName(ctx.getPackageName(), c.name)); } catch (Throwable t) { }
        return state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED || state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && c.enabled;
    }
}
