package megler.test.procs;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.Service;
import android.content.Context;
import android.util.Log;
import java.io.FileInputStream;
import java.security.MessageDigest;
import java.security.MessageDigestSpi;
import java.security.Provider;
import java.security.Security;

/**
 * The app's Application: made once in each process, which it says with the name it is told in its onCreate -- where an app's
 * Application chooses what it builds for its process, as the Google app's does -- and how many this process's classes made.
 */
public class App extends Application {
    /* The Applications this process's classes have made: one, as in a process of Android's. */
    static int made;
    /* The process it was made in, as its onCreate was told, and its number among this process's. */
    String madeIn;
    int serial;

    @Override
    public void onCreate() {
        super.onCreate();
        serial = ++made;
        madeIn = getProcessName();
        Log.i("ProcsTest", "application made in " + madeIn + " #" + serial);
        // Its process's list of security providers is its own, a fresh process's at first: the Application of each process
        // puts a provider of the same name first in it, as the Google app's puts Google's SslGuard there and ends if it cannot.
        String before = names();
        int at = Security.insertProviderAt(new Mark(madeIn), 1);
        Log.i("ProcsTest", "provider in " + madeIn + " at=" + at + " before=" + before);
        // (the Application of ":broken" fails: on Android that process crashes as it starts, Broken)
        if (madeIn != null && madeIn.endsWith(":broken")) throw new IllegalStateException("procs: the Application of :broken fails on purpose");
    }

    /** The provider each process's Application puts first, of one name in all, which says its process; it has a digest. */
    static final class Mark extends Provider {
        Mark(String process) {
            super("ProcsTest", 1.0, process);
            put("MessageDigest.ProcsMD", Digest.class.getName());
        }
    }

    /** The digest of Mark, which JCA's lookups find (MessageDigest.getInstance, with the provider's name and without). */
    public static final class Digest extends MessageDigestSpi {
        public Digest() {}
        @Override protected void engineUpdate(byte input) {}
        @Override protected void engineUpdate(byte[] input, int offset, int len) {}
        @Override protected byte[] engineDigest() { return new byte[0]; }
        @Override protected void engineReset() {}
    }

    /* The names of the process's security providers, in order. */
    private static String names() {
        StringBuilder b = new StringBuilder();
        for (Provider p : Security.getProviders()) b.append(b.length() == 0 ? "" : ",").append(p.getName());
        return b.toString();
    }

    /**
     * What the code of a process finds of the provider named ProcsTest: the first in its list, the one of that name, and the
     * providers of the digest ProcsMD without a provider's name and with it -- each by the process it says.
     */
    static String providers() {
        Provider first = Security.getProviders()[0], named = Security.getProvider("ProcsTest");
        String digest, digestNamed;
        try { digest = MessageDigest.getInstance("ProcsMD").getProvider().getInfo(); }
        catch (Exception e) { digest = "none(" + e.getClass().getSimpleName() + ")"; }
        try { digestNamed = MessageDigest.getInstance("ProcsMD", "ProcsTest").getProvider().getInfo(); }
        catch (Exception e) { digestNamed = "none(" + e.getClass().getSimpleName() + ")"; }
        return "first=" + first.getName() + (first instanceof Mark ? ":" + first.getInfo() : "") + " named=" + (named == null ? "none" : named.getInfo())
            + " digest=" + digest + " digestNamed=" + digestNamed;
    }

    /**
     * What a component is told of its process, every way an app asks -- Application.getProcessName, ActivityThread's and
     * Process's by reflection (as Play Store asks), the ActivityManager of its context for this pid, /proc/self/cmdline --
     * and the Application it has (its own and its context's), and the kind of loader its classes came from.
     */
    static String report(Context c) {
        String am = "none";
        ActivityManager m = (ActivityManager) c.getSystemService(Context.ACTIVITY_SERVICE);
        if (m != null && m.getRunningAppProcesses() != null)
            for (ActivityManager.RunningAppProcessInfo p : m.getRunningAppProcesses()) if (p.pid == android.os.Process.myPid()) { am = p.processName; break; }
        Application own = c instanceof Activity ? ((Activity) c).getApplication() : c instanceof Service ? ((Service) c).getApplication() : null;
        return "process=" + Application.getProcessName() + " thread=" + asked("android.app.ActivityThread", "currentProcessName")
            + " my=" + asked("android.os.Process", "myProcessName") + " am=" + am + " cmdline=" + cmdline()
            + " app=" + which(own) + " context=" + which(c.getApplicationContext()) + " loader=" + App.class.getClassLoader().getClass().getSimpleName();
    }

    /* An Application as this process's classes see it: the process it was made in and its number; another's by its class. */
    private static String which(Object o) {
        if (o instanceof App) return ((App) o).madeIn + "#" + ((App) o).serial;
        return o == null ? "null" : "a " + o.getClass().getName() + " of another loader";
    }

    private static String asked(String cls, String method) {
        try {
            return String.valueOf(Class.forName(cls).getMethod(method).invoke(null));
        } catch (Throwable t) {
            return "none(" + t.getClass().getSimpleName() + ")";
        }
    }

    private static String cmdline() {
        try (FileInputStream in = new FileInputStream("/proc/self/cmdline")) {
            byte[] b = new byte[256];
            int n = in.read(b), end = 0;
            while (end < Math.max(n, 0) && b[end] != 0) end++;
            return new String(b, 0, end, "UTF-8");
        } catch (Exception e) {
            return "none(" + e + ")";
        }
    }
}
