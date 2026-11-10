// Megler: new in the overlay. Google Location Accuracy -- Android's setting under which Google Play services' network location
// finds where the phone is from the Wi-Fi networks around it -- asked of the user and set where Google's own Play services
// run in the app.
//
// Why. Google Play services 26 locates by Wi-Fi only while that setting is on: the "network" provider is on for it where the
// location's mode is on and network_location_opt_in is 1 in Google's settings (dhcg.a, dhcd/dhce: content://com.google.
// settings/partner, Google Services Framework's GoogleSettingsProvider), its network location operates only then (geup:
// "nlpEnabled", "userConsent"), and its fused provider asks it for fixes only then (gedl). Android asks the user as a phone is
// set up, or when an app's request of the location settings finds it off (LocationSettingsChecker, through the app's
// SettingsClient). Megler has no setup, and Transit checks the settings only while LocationManager says every provider is
// off: the setting was never asked, never on, and Transit had Windows' cell fixes of 300 to 660 m alone.
//
// When. The first time in a process that the app's own code binds Google Play services' location API (the action
// GoogleLocationManagerService.START, megler.MicroG.bind; not Google Play services' own features, whose context is theirs),
// where Google's own Play services are the app's (not microG's), and the app may have the location (a location permission,
// the location not switched off). Then, on a thread of its own:
//   - the app's answer kept in Megler (the shell's group "location_accuracy", Megler's own question; the app's Settings in
//     Megler change it) is Google's setting: written where the setting differs;
//   - none kept, and the setting not yet 0 or 1: the user is asked (megler_system_host.permission, ask), and the answer
//     written -- 1 for Allow, 0 for Don't Allow, nothing for a question put away unanswered;
//   - none kept and the setting decided (another app's answer): it stands.
// The setting is in Google Services Framework's database, which every app's Google Play services reads: one answer for all
// of them, as on Android. Google Play services follows it (a ContentObserver of its own on the setting's Uri), so a change
// reaches the app that runs at once.
//
// The log ("MeglerLocation") says what was found and done, once.
package megler;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;

public final class LocationAccuracy {
    private static final String TAG = "MeglerLocation";
    /** Google's settings (GoogleSettingsContract.Partner) and the key of Android's Location Accuracy in them. */
    public static final Uri PARTNER = Uri.parse("content://com.google.settings/partner");
    public static final String KEY = "network_location_opt_in";
    /** The shell's group of Megler's question (megler_system_host.permission). */
    public static final String GROUP = "location_accuracy";
    /** The action of Google Play services' location API, which an app's FusedLocationProviderClient and SettingsClient bind. */
    public static final String LOCATION_API = "com.google.android.location.internal.GoogleLocationManagerService.START";

    private static boolean sStarted;
    /** The setting as last read or written: 1 on, 0 off, -1 not decided. */
    private static volatile int sOn = -1;

    private LocationAccuracy() {}

    /** MicroG.bind: `from` binds Google Play services' service by `action`; the app's own binding of the location API settles the setting. */
    public static void bound(Context from, String action) {
        if (from == null || !LOCATION_API.equals(action) || GooglePackages.GMS.equals(from.getPackageName())) return;
        synchronized (LocationAccuracy.class) {
            if (sStarted) return;
            sStarted = true;
        }
        Thread t = new Thread(LocationAccuracy::settle, "megler-location-accuracy");
        t.setDaemon(true);
        t.start();
    }

    static void settle() {
        try {
            String why = null;
            if (!MicroG.installed() || MicroG.isMicroG()) why = "the Google Play services here are not Google's own";
            else if (Permissions.check("android.permission.ACCESS_FINE_LOCATION") != Permissions.GRANTED
                     && Permissions.check("android.permission.ACCESS_COARSE_LOCATION") != Permissions.GRANTED)
                why = "the app holds no location permission";
            else if (!LocationService.enabled()) why = "the phone's location is switched off";
            Context app = AppRunner.application();
            if (why == null && app == null) why = "the app has no Application yet";
            if (why != null) {
                android.util.Log.i(TAG, "Google Location Accuracy not settled: " + why);
                return;
            }
            int now = read(app);
            sOn = now;
            int kept = Native.permission(GROUP, false);
            String what;
            if (kept == Permissions.ALLOWED || kept == Permissions.REFUSED || kept == Permissions.REFUSED_FOR_GOOD) {
                int want = kept == Permissions.ALLOWED ? 1 : 0;
                if (now != want) write(app, want);
                what = (want == 1 ? "on" : "off") + ", as the user said in this app" + (now != want ? " (written; it was " + name(now) + ")" : "");
            } else if (now >= 0) {
                what = (now == 1 ? "on" : "off") + ", as the user said in another app";
            } else {
                int answer = Native.permission(GROUP, true);
                if (answer == Permissions.ALLOWED || answer == Permissions.REFUSED || answer == Permissions.REFUSED_FOR_GOOD) {
                    int want = answer == Permissions.ALLOWED ? 1 : 0;
                    write(app, want);
                    what = (want == 1 ? "on" : "off") + ": the user was asked, and said " + (want == 1 ? "Allow" : "Don't Allow");
                } else {
                    what = "not decided: the question was put away unanswered";
                }
            }
            android.util.Log.i(TAG, "Google Location Accuracy " + what + (sOn == 1
                    ? " -- Google Play services' network location may find the phone from the Wi-Fi networks around it"
                    : " -- Google Play services' network location stays off: the app has Windows' fixes alone"));
        } catch (Throwable e) {
            android.util.Log.w(TAG, "Google Location Accuracy could not be settled: " + e);
        }
    }

    private static String name(int v) { return v == 1 ? "on" : v == 0 ? "off" : "not decided"; }

    /** The setting: 1, 0, or -1 where it is not there or Google's settings cannot be read. */
    static int read(Context context) {
        try (Cursor c = context.getContentResolver().query(PARTNER, new String[] { "value" }, "name=?", new String[] { KEY }, null)) {
            if (c == null || !c.moveToFirst()) return -1;
            String v = c.getString(0);
            return "1".equals(v) ? 1 : "0".equals(v) ? 0 : -1;
        } catch (RuntimeException e) {
            android.util.Log.w(TAG, "Google's settings could not be read (" + PARTNER + "): " + e);
            return -1;
        }
    }

    /** The setting written as Google Play services writes it (foqo.b: an insert of the name and the value), and read back. */
    static void write(Context context, int value) {
        ContentValues v = new ContentValues();
        v.put("name", KEY);
        v.put("value", String.valueOf(value));
        context.getContentResolver().insert(PARTNER, v);
        sOn = read(context);
        if (sOn != value) android.util.Log.w(TAG, "Google's settings did not keep " + KEY + "=" + value + ": it reads " + name(sOn) + " (" + PARTNER + ")");
    }
}
