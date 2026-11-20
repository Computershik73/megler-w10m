package megler.test.procs;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.Service;
import android.content.Context;
import android.util.Log;
import java.io.FileInputStream;

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
