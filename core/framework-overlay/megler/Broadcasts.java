// megler-w10m overlay of Megler/src/java-framework/src/megler/Broadcasts.java (tools/build-framework.sh).
// It differs in the battery's broadcasts: what the sticky one says, that a receiver registered for it receives it, and that
// a change of the battery is broadcast at all.
//
// Why. Android keeps the last ACTION_BATTERY_CHANGED and gives it to whoever registers for it, in two ways at once: as the
// value of registerReceiver, and -- when a receiver was passed -- by a call of that receiver's onReceive soon after. The
// original did the first only, and an app that shows the battery from its receiver, which is the usual way, never heard and
// showed its zeros: Device Info's card read "Battery 0%  Voltage: 0 mV, Temperature: 0C". And the intent said four things
// (level, scale, status, plugged) with nothing for the voltage, the temperature, the health or the technology. Now the intent
// is made of what the core says the battery is (megler.Native.battery, which is hk_battery_read in the core: one place, which
// the shell can fill from the phone), with the extras Android's has, and is delivered. And registerReceiver(null, filter),
// which only asks for a sticky intent, no longer puts its null into the list of receivers. And when the battery becomes
// another -- the shell says so to the core (megler_set_battery), and the Java driver, from the main loop, calls batteryChanged
// here -- registered receivers hear ACTION_BATTERY_CHANGED again, and ACTION_POWER_CONNECTED or ACTION_POWER_DISCONNECTED
// is sent when the phone went on or off a charger: the original had a battery that never changed and sent neither.
// Everything else is the original's, unchanged.
package megler;

import android.content.*;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** Broadcasts within the app: to receivers registered at run time and those in the manifest; the system's own few (battery) answered. */
public final class Broadcasts {
    private static final ArrayList<Object[]> sReceivers = new ArrayList<>();
    private Broadcasts() {}
    public static Intent register(BroadcastReceiver r, IntentFilter f) {
        /* With no receiver there is nothing to keep: the call only asks for the sticky intent. (A null in the list was a NullPointerException waiting for the first broadcast its filter matched.) */
        if (r != null) synchronized (sReceivers) { sReceivers.add(new Object[] { r, f }); }
        if (f != null && f.hasAction(Intent.ACTION_BATTERY_CHANGED)) {
            final Intent b = battery();
            /* A sticky broadcast reaches the receiver it was registered with as any broadcast does: later, on the main thread, if it is still registered then. */
            if (r != null) new Handler(Looper.getMainLooper()).post(() -> { if (registered(r)) r.onReceive(AppRunner.application(), b); });
            return b;
        }
        return null;
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
    public static void unregister(BroadcastReceiver r) { synchronized (sReceivers) { for (int i = sReceivers.size() - 1; i >= 0; i--) if (sReceivers.get(i)[0] == r) sReceivers.remove(i); } }
    public static void send(Context from, Intent i) {
        final Context ctx = AppRunner.application() != null ? AppRunner.application() : from;
        ArrayList<BroadcastReceiver> targets = new ArrayList<>();
        synchronized (sReceivers) { for (Object[] e : sReceivers) if (((IntentFilter) e[1]).hasAction(i.getAction())) targets.add((BroadcastReceiver) e[0]); }
        for (Manifest.Component c : Manifest.receivers) {
            boolean match = i.getComponent() != null && c.name.equals(i.getComponent().getClassName());
            for (Manifest.Filter f : c.filters) if (f.actions.contains(i.getAction())) match = true;
            if (!match) continue;
            try { targets.add((BroadcastReceiver) Class.forName(c.name).newInstance()); } catch (Throwable t) { android.util.Log.w("Megler", "receiver " + c.name + ": " + t); }
        }
        Handler h = new Handler(Looper.getMainLooper());
        for (BroadcastReceiver r : targets) h.post(() -> r.onReceive(ctx, i));
    }
}
