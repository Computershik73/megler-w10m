package megler;

import android.content.ContentProvider;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;

/*
 * Google's own Play services' check-in, which its sign-in needs done. GmsCore's sign-in (PreAddAccountActivity's
 * CheckinHelper) waits 10 s for a check-in under way, then starts one and waits 10 s more (Phenotype's checkin_timeout of
 * com.google.android.gms.auth_account), and without an Android ID then it tells the user it could not reach Google's servers.
 * On a Lumia 650 the first check-in, in the process where GmsCore first started and did all its first start does, took 13 s:
 * the sign-in gave up 0.75 s before it ended, then waited for a second that had not begun 10 s later (package 616). The
 * Android ID is GServices' android_id, which the check-in writes to GSF's provider.
 */
public final class Checkin {
    private Checkin() {}

    private static final Uri GSERVICES = Uri.parse("content://com.google.android.gsf.gservices");

    /** The Android ID GmsCore's check-in got, as GmsCore reads it (GServices' android_id): 0 before its first check-in. */
    public static long androidId() {
        ContentProvider gservices = Gsf.provider("com.google.android.gsf.gservices");
        if (gservices == null) return 0;
        try (Cursor c = gservices.query(GSERVICES, null, null, new String[] { "android_id" }, null)) {
            if (c == null || !c.moveToFirst()) return 0;
            String value = c.getString(1);
            return value == null ? 0 : Long.parseLong(value);
        } catch (Throwable t) {
            android.util.Log.w("Megler", "checkin: GServices' android_id cannot be read: " + t);
            return 0;
        }
    }

    /**
     * Until GmsCore has an Android ID, for at most `ms`; true when it has one. The check-in GmsCore starts as it starts is under
     * way meanwhile; one is asked for (android.server.checkin.CHECKIN, as the system sends it) only when none has ended in a
     * third of the time, since a second would wait behind the first.
     */
    public static boolean await(long ms) {
        final long start = android.os.SystemClock.uptimeMillis();
        boolean asked = false;
        while (androidId() == 0) {
            long waited = android.os.SystemClock.uptimeMillis() - start;
            if (waited >= ms) {
                android.util.Log.w("Megler", "checkin: Google Play services has no Android ID after " + waited + " ms");
                return false;
            }
            if (!asked && waited >= ms / 3) {
                asked = true;
                android.util.Log.i("Megler", "checkin: no Android ID after " + waited + " ms: a check-in is asked for");
                MicroG.sendAsSystem(new Intent("android.server.checkin.CHECKIN"));
            }
            try { Thread.sleep(250); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return false; }
        }
        android.util.Log.i("Megler", "checkin: Google Play services has its Android ID after "
            + (android.os.SystemClock.uptimeMillis() - start) + " ms");
        return true;
    }
}
