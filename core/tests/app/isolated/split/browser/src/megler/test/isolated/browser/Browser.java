package megler.test.isolated.browser;

import android.app.Activity;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

import java.lang.reflect.Field;
import java.util.Arrays;

import megler.test.isolated.App;
import megler.test.isolated.Counter;

/**
 * The activity of the feature split "browser" of an app with isolated splits, as Chrome 158's ChromeTabbedActivity is in its
 * split "chrome". Each step is a line of the log, "name ok" or "name FAILED" and why: what Android gives such an activity --
 * its class's loader the split's own, over the split alone, whose parent is the base's; its context's loader that one; the
 * split contexts createContextForSplit makes, a split that uses another, a split the app has not -- and what Chromium's
 * BundleUtils does with them.
 */
public class Browser extends Activity {
    private static final String T = "IsolatedTest";
    private int ok, failed;
    private String detail;

    private interface Step {
        boolean run() throws Throwable;
    }

    private void step(String name, Step s) {
        detail = null;
        boolean good;
        try {
            good = s.run();
        } catch (Throwable t) {
            good = false;
            detail = t.toString();
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED" + (detail == null ? "" : ": " + detail)) + (good && detail != null ? " (" + detail + ")" : ""));
    }

    /** Called by the split "more", which uses this one. */
    public static String name() {
        return "browser";
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView text = new TextView(this);
        text.setText("isolated splits");
        setContentView(text);
        final ClassLoader base = App.class.getClassLoader(), mine = Browser.class.getClassLoader();
        Log.i(T, "base loader " + base);
        Log.i(T, "split loader " + mine);

        // The base's loader has the base alone: not the split's file, not its classes.
        step("base-loader", () -> {
            detail = String.valueOf(base);
            try {
                Class.forName("megler.test.isolated.browser.Browser", false, base);
                return false;
            } catch (ClassNotFoundException expected) {
            }
            return base.toString().contains("base.apk") && !base.toString().contains("split_");
        });

        // This class's loader is the split's own: a PathClassLoader over the split alone, under the base's.
        step("split-loader", () -> mine != base && mine.getParent() == base && mine instanceof dalvik.system.PathClassLoader
            && mine.toString().contains("split_browser.apk") && !mine.toString().contains("base.apk") && !mine.toString().contains("split_more"));

        // A class of the base, through the split's loader's parent: one class for both, with the Application's count.
        step("base-class", () -> {
            detail = "started=" + Counter.started;
            return Counter.class.getClassLoader() == base && Class.forName("megler.test.isolated.Counter", false, mine) == Counter.class && Counter.started == 1;
        });

        // The activity's context has the split's loader (ContextImpl.createActivityContext), and the activity was made by it.
        step("activity-context", () -> getBaseContext().getClassLoader() == mine && getClassLoader() == mine);

        // The split's context, from the application's: the same loader, made once.
        step("split-context", () -> {
            Context c = getApplicationContext().createContextForSplit("browser");
            return c.getClassLoader() == mine && createContextForSplit("browser").getClassLoader() == mine;
        });

        // A split that uses this one: its loader's parent is this split's, whose classes its code sees; this loader has none of its.
        step("uses-split", () -> {
            ClassLoader more = getApplicationContext().createContextForSplit("more").getClassLoader();
            Class<?> c = more.loadClass("megler.test.isolated.more.More");
            String said = (String) c.getMethod("hello").invoke(null);
            detail = said;
            try {
                Class.forName("megler.test.isolated.more.More", false, mine);
                return false;
            } catch (ClassNotFoundException expected) {
            }
            return more.getParent() == mine && c.getClassLoader() == more && "more over browser true".equals(said);
        });

        // A split the app has not: NameNotFoundException.
        step("no-such-split", () -> {
            try {
                createContextForSplit("nope");
                return false;
            } catch (PackageManager.NameNotFoundException expected) {
                detail = expected.getMessage();
                return true;
            }
        });

        // What the package manager says: the app loads its splits in isolation, has both, and this activity is the split's.
        step("package-info", () -> {
            ApplicationInfo ai = getApplicationInfo();
            boolean isolated = (Boolean) ApplicationInfo.class.getMethod("requestsIsolatedSplitLoading").invoke(ai);
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(), 0);
            detail = "isolated=" + isolated + " splits=" + Arrays.toString(ai.splitNames) + " splitName=" + info.splitName;
            return isolated && Arrays.asList(ai.splitNames).contains("browser") && Arrays.asList(ai.splitNames).contains("more") && "browser".equals(info.splitName);
        });

        // As Chromium's BundleUtils.replaceClassLoader: the application's base context given the split's loader, by reflection on
        // ContextImpl's mClassLoader; the application context has it then (SplitChromeApplication does so for its split "chrome").
        step("replace-loader", () -> {
            Context c = getApplicationContext();
            while (c instanceof ContextWrapper && ((ContextWrapper) c).getBaseContext() != null) c = ((ContextWrapper) c).getBaseContext();
            Field f = c.getClass().getDeclaredField("mClassLoader");
            f.setAccessible(true);
            f.set(c, mine);
            return getApplicationContext().getClassLoader() == mine && Browser.class.getClassLoader().equals(getApplicationContext().getClassLoader());
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }

    @Override
    protected void onDestroy() {
        Log.i(T, "onDestroy");
        super.onDestroy();
    }
}
