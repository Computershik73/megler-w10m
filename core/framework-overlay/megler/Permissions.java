// Megler: new in the overlay. Android's runtime permissions for the app: which of the permissions its manifest asks for it
// holds, and the user asked for the dangerous ones when the app requests them, as on a phone with Android 6 and later.
//
// Why. The framework answered every permission granted, whatever its name and whether the manifest asked for it, and
// requestPermissions answered granted with no one asked. An app said it had the contacts, the camera or the text messages
// it never asked for, and could not be told no; and Windows asked instead, the first time anything in the app touched the
// camera, the microphone or the location -- Google Play services in the app looking for the location too.
//
// What decides, for one permission:
//   - not in the manifest, or asked for only up to an Android before this one (maxSdkVersion, megler.Manifest): denied;
//   - not dangerous -- normal (INTERNET, VIBRATE), the app's own and other apps' (C2D_MESSAGE), signature, special
//     (SYSTEM_ALERT_WINDOW, REQUEST_INSTALL_PACKAGES): granted with the install, as Android grants the normal ones. The
//     special ones too, as before: the screens of Android's settings that grant them are Megler's to answer;
//   - dangerous, of an app that targets Android 5.1 or older (or names no target: Android 1's), or of Google's own packages
//     (Play Store, Google Play services, its framework), which a phone has as system apps: granted with the install, as
//     Android grants them, unless the shell says the group is denied for good -- the user's Don't allow in the app's App
//     Permissions, as Android's settings revoke an old app's permission (or Windows' own no). Windows still asks for the
//     camera, the microphone and the location the first time they are used;
//   - any other dangerous one: its group's (Android 14's groups, GROUPS below), as the shell keeps it
//     (megler_system_host.permission): granted; denied, and may be asked again (shouldShowRequestPermissionRationale is true
//     then); denied for good (Windows' own setting, a second no, as Android 11 and later take it, or the user's Don't allow
//     in the app's App Permissions, which may refuse any group, those Megler has nothing behind too); or undecided (asked
//     when the app requests it: never asked, or asked every time and not yet in this run).
// requestPermissions asks the shell, on a thread of its own, for each group that is undecided or denied once, one after
// another -- Windows' own question for the camera, the microphone and the location, the shell's for the others -- and the
// answers come to onRequestPermissionsResult on the main thread, the activity paused and resumed around them as Android's
// permission dialog does it, where an answer changed. A request that needs no one asked, or whose questions were put away
// unanswered, is answered on the next turn of the main loop, unpaused: an app that asks again in onResume when it is told
// no would otherwise ask for ever.
//
// What the shell said of a group is kept until the app comes back to the front (megler.AppRunner.hostPause: the user may
// have changed Windows' settings, or the app's App Permissions in Megler, meanwhile) or asks for it again.
package megler;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;

public final class Permissions {
    private Permissions() {}

    /* PackageManager's answers. */
    public static final int GRANTED = 0, DENIED = -1;
    /* The shell's (megler.h, MEGLER_PERMISSION_*). */
    public static final int UNDECIDED = 0, ALLOWED = 1, REFUSED = 2, REFUSED_FOR_GOOD = 3;

    /* Android 14's dangerous permissions and their groups, by the group's name in small letters (megler_system_host.permission). */
    private static final HashMap<String, String> GROUPS = new HashMap<>();
    private static void group(String name, String... permissions) {
        for (String p : permissions) GROUPS.put("android.permission." + p, name);
    }
    static {
        group("calendar", "READ_CALENDAR", "WRITE_CALENDAR");
        group("call_log", "READ_CALL_LOG", "WRITE_CALL_LOG", "PROCESS_OUTGOING_CALLS");
        group("camera", "CAMERA");
        group("contacts", "READ_CONTACTS", "WRITE_CONTACTS", "GET_ACCOUNTS");
        group("location", "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION", "ACCESS_BACKGROUND_LOCATION");
        group("microphone", "RECORD_AUDIO");
        group("phone", "READ_PHONE_STATE", "READ_PHONE_NUMBERS", "CALL_PHONE", "ANSWER_PHONE_CALLS", "ADD_VOICEMAIL", "USE_SIP", "ACCEPT_HANDOVER");
        group("sensors", "BODY_SENSORS", "BODY_SENSORS_BACKGROUND");
        group("sms", "SEND_SMS", "RECEIVE_SMS", "READ_SMS", "RECEIVE_WAP_PUSH", "RECEIVE_MMS", "READ_CELL_BROADCASTS");
        group("storage", "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE");
        group("read_media_visual", "READ_MEDIA_IMAGES", "READ_MEDIA_VIDEO", "READ_MEDIA_VISUAL_USER_SELECTED", "ACCESS_MEDIA_LOCATION");
        group("read_media_aural", "READ_MEDIA_AUDIO");
        group("nearby_devices", "BLUETOOTH_SCAN", "BLUETOOTH_CONNECT", "BLUETOOTH_ADVERTISE", "NEARBY_WIFI_DEVICES", "UWB_RANGING");
        group("notifications", "POST_NOTIFICATIONS");
        group("activity_recognition", "ACTIVITY_RECOGNITION");
    }

    /** The dangerous group a permission of Android's is in, or null for one that is not dangerous. ACCESS_MEDIA_LOCATION goes
     *  with the storage where the app asks for that (an app of Android 10 to 12), as it did there. */
    public static String groupOf(String permission) {
        if ("android.permission.ACCESS_MEDIA_LOCATION".equals(permission) && asks("android.permission.READ_EXTERNAL_STORAGE")) return "storage";
        return permission == null ? null : GROUPS.get(permission);
    }

    /** Whether the app's manifest asks for the permission on this Android. */
    public static boolean asks(String permission) {
        Manifest.read();
        return permission != null && Manifest.permissions.contains(permission);
    }

    /** Whether the app's dangerous permissions come with its install: an app of before Android 6, and Google's own packages. */
    public static boolean grantedWithInstall() {
        Manifest.read();
        if (!Manifest.usesSdk || Manifest.targetSdk < 23) return true;
        String pkg = Manifest.packageName;
        return GooglePackages.GMS.equals(pkg) || GooglePackages.VENDING.equals(pkg) || GooglePackages.GSF.equals(pkg);
    }

    private static final HashMap<String, Integer> sStates = new HashMap<>();

    /** What the shell says of a group (UNDECIDED, ALLOWED, REFUSED, REFUSED_FOR_GOOD), asked once until forget(). */
    public static int state(String group) {
        synchronized (sStates) {
            Integer s = sStates.get(group);
            if (s != null) return s;
        }
        int s = Native.permission(group, false);
        synchronized (sStates) { sStates.put(group, s); }
        return s;
    }

    /** What the shell said is forgotten: the app is in front again, and the user may have changed Windows' settings or the
     *  app's App Permissions meanwhile. */
    public static void forget() {
        synchronized (sStates) { sStates.clear(); }
    }

    /** Context.checkPermission, checkSelfPermission and their kind, and PackageManager.checkPermission of the app's own package. */
    public static int check(String permission) {
        if (!asks(permission)) return DENIED;
        String g = groupOf(permission);
        if (g == null) return GRANTED;
        // Granted with the install, unless the user took it away (Android's settings revoke an old app's permission so).
        if (grantedWithInstall()) return state(g) == REFUSED_FOR_GOOD ? DENIED : GRANTED;
        return state(g) == ALLOWED ? GRANTED : DENIED;
    }

    /** Activity.shouldShowRequestPermissionRationale: the user said no once, and may be asked again. */
    public static boolean shouldShowRationale(String permission) {
        if (!asks(permission)) return false;
        String g = groupOf(permission);
        return g != null && !grantedWithInstall() && state(g) == REFUSED;
    }

    private static Handler sMain;
    private static synchronized Handler main() {
        if (sMain == null) sMain = new Handler(Looper.getMainLooper());
        return sMain;
    }

    /**
     * Activity.requestPermissions (or a Fragment's, `fragment`): what Android's rules settle is answered as they settle it, the
     * user is asked for the rest, a group at a time; the answers come to the activity's meglerPermissionsResult on the main
     * thread, `asked` saying whether an answer changed.
     */
    public static void request(final Activity activity, final String[] permissions, final int code, final Object fragment) {
        final int[] results = new int[permissions.length];
        final LinkedHashSet<String> ask = new LinkedHashSet<>();
        for (int i = 0; i < permissions.length; i++) {
            String p = permissions[i], g = groupOf(p);
            results[i] = check(p);
            if (results[i] == DENIED && asks(p) && g != null && !grantedWithInstall() && state(g) != REFUSED_FOR_GOOD) ask.add(g);
        }
        say(permissions, results, ask);
        if (ask.isEmpty()) {
            main().post(() -> activity.meglerPermissionsResult(code, permissions, results, fragment, false));
            return;
        }
        Thread t = new Thread(() -> {
            boolean changed = false;
            for (String g : ask) {
                int before = state(g), s = Native.permission(g, true);
                changed |= s != before;
                synchronized (sStates) { sStates.put(g, s); }
            }
            for (int i = 0; i < permissions.length; i++) results[i] = check(permissions[i]);
            say(permissions, results, null);
            // (Paused around it only where an answer changed: a question put away unanswered, or one Windows could not put,
            // leaves the app as it was, and an app that asks again in onResume does not ask for ever.)
            final boolean answered = changed;
            main().post(() -> activity.meglerPermissionsResult(code, permissions, results, fragment, answered));
        }, "megler-permissions");
        t.setDaemon(true);
        t.start();
    }

    /** In the log: what was asked for and how it stands; with `ask`, which groups the user is asked about. */
    private static void say(String[] permissions, int[] results, LinkedHashSet<String> ask) {
        StringBuilder b = new StringBuilder(ask != null ? "permissions: the app requests" : "permissions: answered");
        for (int i = 0; i < permissions.length; i++)
            b.append(i == 0 ? " " : ", ").append(permissions[i] == null ? "null" : permissions[i].replace("android.permission.", ""))
             .append(results[i] == GRANTED ? " granted" : " denied");
        if (ask != null) b.append(ask.isEmpty() ? "; no one asked" : "; the user asked about " + String.join(", ", new ArrayList<>(ask)));
        android.util.Log.i("Megler", b.toString());
    }
}
