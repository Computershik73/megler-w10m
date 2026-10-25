// Overlay of core/framework/src/megler/Services.java (tools/build-framework.sh). It differs in one thing: a service of the
// app's is made by the app's component factory (megler.AppRunner.componentFactory: the manifest's
// android:appComponentFactory), as Android 9's ActivityThread makes it, not from its class directly. Facebook's factory holds
// a component back until the code it loads after the start is there. And a service stopped from another thread is destroyed
// on the main thread, as on Android, and a start has its id when it is asked for (postStart). And a service the manifest puts in
// another process (android:process) is made in that process's class scope, with its Application (megler.Processes). Everything
// else is the original's, unchanged.
package megler;

import android.app.Service;
import android.content.*;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import java.util.HashMap;

/** The app's own services (started and bound in this process, as on Android) and the system services Megler answers for. */
public final class Services {
    private static final HashMap<String, Service> sRunning = new HashMap<>();
    private static final HashMap<ServiceConnection, String> sBound = new HashMap<>();
    /* The id of the latest start of each of the app's services (postStart). */
    private static final HashMap<String, Integer> sLastStart = new HashMap<>();
    private Services() {}
    private static Manifest.Component find(Intent i) {
        String pkg = i.getComponent() == null ? i.getPackage() : i.getComponent().getPackageName();
        if (pkg != null && !pkg.equals(Native.packageName())) return null;
        if (i.getComponent() != null) { for (Manifest.Component c : Manifest.services) if (c.name.equals(i.getComponent().getClassName())) return c; }
        if (i.getAction() != null) for (Manifest.Component c : Manifest.services) for (Manifest.Filter f : c.filters) if (f.actions.contains(i.getAction())) return c;
        return null;
    }
    private static Service create(Manifest.Component c) throws Exception {
        Service s = sRunning.get(c.name);
        if (s == null) {
            Processes.Scope process = Processes.of(c);
            if (process != null) {
                s = (Service) process.classLoader().loadClass(c.name).getDeclaredConstructor().newInstance();
                s.meglerAttach(process.application());
            } else {
                s = AppRunner.componentFactory().instantiateService(AppRunner.classLoader(), c.name, null);
                s.meglerAttach(AppRunner.application() != null ? AppRunner.application() : ContextImpl.app());
            }
            synchronized (sRunning) { sRunning.put(c.name, s); }
            s.onCreate();
        }
        return s;
    }
    /** A job's service (android.app.job.JobScheduler, overlaid, runs the job): the app's service of that class, made as a
     *  started or bound one is (in its process's scope), or null when the app has none such. On the main thread. */
    public static Service service(ComponentName c) throws Exception {
        if (c == null) return null;
        Manifest.Component m = find(new Intent().setComponent(c));
        return m == null ? null : create(m);
    }
    public static ComponentName start(Context from, Intent i) {
        ComponentName external = MicroG.start(from, i);
        if (external != null) return external;
        Manifest.Component c = find(i);
        if (c == null) return null;
        postStart(sLastStart, c.name, id -> {
            try { Service s = create(c); s.onStartCommand(i, 0, id); } catch (Throwable t) { android.util.Log.e("Megler", "service " + c.name + " failed", t); }
        });
        return new ComponentName(Native.packageName(), c.name);
    }
    /*
     * A start of a service has its id when it is asked for, one more than its last start's, as Android's activity manager gives
     * it, and a stop by a start's id is decided then too (postStop); each posts to the main thread under the same lock, so the
     * service has them in the order they were decided. GmsCore's IntentOperationService starts itself again for the work it
     * still runs and then stops by the id of the work that ended: with ids given as the starts reached the service, that stop
     * ended it under its running work, which its next instance began again from the start; Play Store ran GmsCore's module
     * setup twenty times over, with threads of its own each time, until its memory ran out. The ids go on from one instance
     * of the service to the next.
     */
    static void postStart(HashMap<String, Integer> last, String name, java.util.function.IntConsumer start) {
        synchronized (last) {
            Integer was = last.get(name);
            int id = was == null ? 1 : was + 1;
            last.put(name, id);
            MicroG.onMain(() -> start.accept(id));
        }
    }
    /** Posts the stop when the id is the latest start's, or with none (-1); false when a later start keeps the service. */
    static boolean postStop(HashMap<String, Integer> last, String name, int id, Runnable stop) {
        synchronized (last) {
            Integer was = last.get(name);
            if (id >= 0 && was != null && was != id) return false;
            MicroG.onMain(stop);
            return true;
        }
    }
    /* Stopped on the main thread, whichever thread asks, and later, as on Android (megler.MicroG.onMain). */
    public static boolean stop(Intent i) {
        if (MicroG.stop(i)) return true;
        Manifest.Component c = find(i);
        if (c == null || !sRunning.containsKey(c.name)) return false;
        MicroG.onMain(() -> { Service s; synchronized (sRunning) { s = sRunning.remove(c.name); } if (s != null) s.onDestroy(); });
        return true;
    }
    /** Service.stopSelfResult: true when the service stops, false when a later start keeps it or it is not running. */
    public static boolean stopSelf(Service s, int id) {
        int hosted = MicroG.stopSelf(s, id);
        if (hosted >= 0) return hosted > 0;
        String k = null;
        synchronized (sRunning) { for (java.util.Map.Entry<String, Service> e : sRunning.entrySet()) if (e.getValue() == s) k = e.getKey(); }
        if (k == null) return false;
        final String name = k;
        return postStop(sLastStart, name, id, () -> {
            boolean still;
            synchronized (sRunning) { still = sRunning.get(name) == s; if (still) sRunning.remove(name); }
            if (still) s.onDestroy();
        });
    }
    public static boolean bind(Context from, Intent i, ServiceConnection conn) {
        if (MicroG.bind(from, i, conn)) return true;
        final IBinder play = PlayStore.bind(i);
        if (play != null) {
            final ComponentName cn = new ComponentName(GooglePackages.VENDING, i.getAction());
            new Handler(Looper.getMainLooper()).post(() -> conn.onServiceConnected(cn, play));
            return true;
        }
        Manifest.Component c = find(i);
        if (c == null) return false;
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                Service s = create(c);
                IBinder b = s.onBind(i);
                sBound.put(conn, c.name);
                ComponentName cn = new ComponentName(Native.packageName(), c.name);
                if (b != null) conn.onServiceConnected(cn, b); else conn.onNullBinding(cn);
            } catch (Throwable t) { android.util.Log.e("Megler", "binding " + c.name + " failed", t); }
        });
        return true;
    }
    public static void unbind(ServiceConnection conn) { if (!MicroG.unbind(conn)) sBound.remove(conn); }

    /** A system service by name, or null for those Megler has nothing for. */
    public static Object system(String name, Context c) {
        switch (name) {
        case Context.AUDIO_SERVICE: return new android.media.AudioManager();
        case Context.VIBRATOR_SERVICE: return new android.os.Vibrator();
        case Context.VIBRATOR_MANAGER_SERVICE: return new android.os.VibratorManager();
        case Context.CLIPBOARD_SERVICE: return new ClipboardManager();
        case Context.INPUT_METHOD_SERVICE: return new android.view.inputmethod.InputMethodManager();
        case Context.SENSOR_SERVICE: return new android.hardware.SensorManager();
        case Context.POWER_SERVICE: return new android.os.PowerManager();
        case Context.ACTIVITY_SERVICE: return new android.app.ActivityManager();
        case Context.CONNECTIVITY_SERVICE: return new android.net.ConnectivityManager();
        case Context.NOTIFICATION_SERVICE: return new android.app.NotificationManager();
        case Context.ALARM_SERVICE: return new android.app.AlarmManager();
        case Context.ACCESSIBILITY_SERVICE: return new android.view.accessibility.AccessibilityManager();
        case Context.UI_MODE_SERVICE: return new android.app.UiModeManager();
        case Context.DISPLAY_SERVICE: return new android.hardware.display.DisplayManager();
        case Context.KEYGUARD_SERVICE: return new android.app.KeyguardManager();
        case Context.USER_SERVICE: return new android.os.UserManager();
        case Context.USAGE_STATS_SERVICE: return new android.app.usage.UsageStatsManager(ContextImpl.app(), null);
        case Context.BATTERY_SERVICE: return new android.os.BatteryManager();
        case Context.STORAGE_SERVICE: return new android.os.storage.StorageManager();
        case Context.TELEPHONY_SERVICE: return new android.telephony.TelephonyManager();
        case Context.JOB_SCHEDULER_SERVICE: return new android.app.job.JobScheduler.Impl();
        case Context.APP_OPS_SERVICE: return new android.app.AppOpsManager();
        case Context.INPUT_SERVICE: return new android.hardware.input.InputManager();
        default: return null;
        }
    }
}
