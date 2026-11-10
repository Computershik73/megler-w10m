package megler.test.gmsown;

import android.app.Application;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;
import com.google.android.gms.libs.identity.ClientIdentity;

/**
 * A service of Google Play services' package, run where an app hosts them: what their code is told of its process. Google's
 * own take the process whose name is their application's process name for their main one (GmsModuleFinder: only there is
 * their module configuration made again at once; elsewhere a scan that finds a change asks for another, without end). Hosted
 * in an app, their application runs in the app's process, which Application.getProcessName names to their code.
 *
 * And the attribution tag of their package context, of which the identity of a location request of their own is made
 * (ClientIdentity): none while the user has not said whether their own features may have the location, the user asked
 * the first time such an identity is made, and the tag given from the answer on where it is yes. Asked for plainly first,
 * which asks no one (Megler's framework says whether it has asked); then for an identity, at once (none yet: the question is
 * on its way); then until the answer has come, ten seconds at most. An attribution context keeps its own tag whatever was
 * answered.
 */
public class Probe extends Service {
    @Override
    public int onStartCommand(Intent intent, int flags, int id) {
        String application = getApplicationInfo().processName, current = Application.getProcessName();
        Log.i("GmsOwnTest", "main process " + (application != null && application.equals(current)) + " application=" + application + " current=" + current);
        final Context base = getBaseContext();
        final String plain = tagOf(base);
        final Object askedAfterPlain = asked();
        final String first = ClientIdentity.c(base);
        Log.i("GmsOwnTest", "attribution tag plain=" + plain + " asked=" + askedAfterPlain + " first=" + first + " asked=" + asked());
        Thread later = new Thread(() -> {
            String tag = null;
            for (int i = 0; i < 100 && tag == null; i++) {
                tag = ClientIdentity.c(base);
                if (tag == null) try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }
            Context attributed = null;
            try {
                attributed = (Context) Context.class.getMethod("createAttributionContext", String.class).invoke(base, "probe");
            } catch (Exception e) {
                Log.i("GmsOwnTest", "attribution context FAILED " + e);
            }
            Log.i("GmsOwnTest", "attribution tag later=" + tag + " again=" + ClientIdentity.c(base) + " attributed=" + (attributed == null ? null : tagOf(attributed)));
        }, "gms-own-probe");
        later.start();
        stopSelf(id);
        return START_NOT_STICKY;
    }

    /** Whether the user has been asked in this process (Megler's framework, megler.Permissions.gmsAsked). */
    private static Object asked() {
        try {
            return Class.forName("megler.Permissions").getMethod("gmsAsked").invoke(null);
        } catch (Exception e) {
            return "FAILED " + e;
        }
    }

    /** The context's own tag, asked for not by an identity: Android 11's Context.getAttributionTag, through reflection. */
    private static String tagOf(Context c) {
        try {
            return (String) Context.class.getMethod("getAttributionTag").invoke(c);
        } catch (Exception e) {
            return "FAILED " + e;
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
