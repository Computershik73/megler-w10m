package megler;

import android.content.*;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;

/** Broadcasts within the app: to receivers registered at run time and those in the manifest; the system's own few (battery) answered. */
public final class Broadcasts {
    private static final ArrayList<Object[]> sReceivers = new ArrayList<>();
    private Broadcasts() {}
    public static Intent register(Context context, BroadcastReceiver r, IntentFilter f) {
        if (r != null) synchronized (sReceivers) { sReceivers.add(new Object[] { r, f, context }); }
        if (f != null && f.hasAction(Intent.ACTION_BATTERY_CHANGED)) {
            Intent b = new Intent(Intent.ACTION_BATTERY_CHANGED);
            b.putExtra("level", 100); b.putExtra("scale", 100); b.putExtra("status", 5); b.putExtra("plugged", 0); b.putExtra("present", true);
            return b;
        }
        return null;
    }
    public static void unregister(BroadcastReceiver r) { synchronized (sReceivers) { for (int i = sReceivers.size() - 1; i >= 0; i--) if (sReceivers.get(i)[0] == r) sReceivers.remove(i); } }
    public static void send(Context from, Intent i) {
        final Context app = AppRunner.application() != null ? AppRunner.application() : from;
        ArrayList<Object[]> targets = new ArrayList<>();
        synchronized (sReceivers) { for (Object[] e : sReceivers) if (e[1] != null && ((IntentFilter) e[1]).hasAction(i.getAction())) targets.add(e); }
        for (Manifest.Component c : Manifest.receivers) {
            boolean match = i.getComponent() != null && c.name.equals(i.getComponent().getClassName());
            for (Manifest.Filter f : c.filters) if (f.actions.contains(i.getAction())) match = true;
            if (!match) continue;
            try { targets.add(new Object[] { (BroadcastReceiver) Class.forName(c.name).newInstance(), null, app }); } catch (Throwable t) { android.util.Log.w("Megler", "receiver " + c.name + ": " + t); }
        }
        Handler h = new Handler(Looper.getMainLooper());
        for (Object[] target : targets) h.post(() -> ((BroadcastReceiver) target[0]).onReceive((Context) target[2], i));
    }
}
