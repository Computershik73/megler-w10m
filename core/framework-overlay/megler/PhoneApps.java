// Megler: the other apps the phone has, for an app that lists or starts them -- a launcher (Nova, Niagara, a Launcher3),
// or an app that opens another. They are the apps of Megler's library, which the shell lists in a folder the core reads
// (megler_run_config.apps_dir: Native.appsList, Native.appIcon): each its package, its launcher activity, its label, its
// version, its icon. android.content.pm.PackageManager answers for them from here (queryIntentActivities of the
// launcher's intent, getLaunchIntentForPackage, getPackageInfo, getApplicationInfo, getInstalledPackages), and
// PackageItemInfo.loadIcon gives their icons; an activity of theirs started is started by the shell
// (Native.launchApp, megler_system_host.launch_app): their own package, or Megler with them.
package megler;

import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ResolveInfo;

public final class PhoneApps {
    private PhoneApps() {}

    public static final class App {
        public final String packageName, activity, label, versionName;
        public final long versionCode;
        public final int uid;
        App(String packageName, String activity, String label, String versionName, long versionCode, int uid) {
            this.packageName = packageName; this.activity = activity; this.label = label; this.versionName = versionName;
            this.versionCode = versionCode; this.uid = uid;
        }
    }

    private static java.util.List<App> sApps = new java.util.ArrayList<>();
    private static long sReadAt = -1;
    private static final java.util.HashMap<String, android.graphics.Bitmap> sIcons = new java.util.HashMap<>();

    /** The apps, but this one; read again when two seconds have passed, as the shell rewrites the list when the library changes. */
    public static synchronized java.util.List<App> all() {
        long now = android.os.SystemClock.uptimeMillis();
        if (sReadAt >= 0 && now - sReadAt < 2000) return sApps;
        sReadAt = now;
        String text = Native.appsList();
        java.util.ArrayList<App> apps = new java.util.ArrayList<>();
        if (text != null) {
            String own = Native.packageName();
            int uid = 10100;
            for (String line : text.split("\n")) {
                String[] f = line.replace("\r", "").split("\t", -1);
                if (f.length < 5 || f[0].isEmpty() || f[0].equals(own)) continue;
                long code = 0;
                try { code = Long.parseLong(f[4]); } catch (NumberFormatException e) {}
                apps.add(new App(f[0], f[1].isEmpty() ? null : f[1], f[2].isEmpty() ? f[0] : f[2], f[3], code, uid++));
            }
        }
        if (apps.size() != sApps.size()) android.util.Log.i("Megler", "phone apps: " + apps.size() + " besides this one");
        sApps = apps;
        return apps;
    }

    public static App find(String packageName) {
        if (packageName == null) return null;
        for (App a : all()) if (a.packageName.equals(packageName)) return a;
        return null;
    }

    /** The app's icon, or null. */
    public static android.graphics.drawable.Drawable icon(String packageName) {
        if (find(packageName) == null) return null;
        android.graphics.Bitmap b;
        synchronized (sIcons) {
            b = sIcons.get(packageName);
            if (b == null && !sIcons.containsKey(packageName)) {
                byte[] png = Native.appIcon(packageName);
                b = png == null ? null : android.graphics.BitmapFactory.decodeByteArray(png, 0, png.length);
                sIcons.put(packageName, b);
            }
        }
        return b == null ? null : new android.graphics.drawable.BitmapDrawable(ContextImpl.app().getResources(), b);
    }

    public static ApplicationInfo applicationInfo(App a) {
        ApplicationInfo i = new ApplicationInfo();
        i.packageName = i.processName = i.taskAffinity = a.packageName;
        i.className = null;
        i.nonLocalizedLabel = a.label;
        i.enabled = true;
        i.uid = a.uid;
        i.flags = ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        i.targetSdkVersion = i.minSdkVersion = android.os.Build.VERSION.SDK_INT;
        return i;
    }

    public static ActivityInfo activityInfo(App a, String activity) {
        ActivityInfo i = new ActivityInfo();
        i.name = activity != null ? activity : a.activity;
        i.packageName = a.packageName;
        i.nonLocalizedLabel = a.label;
        i.applicationInfo = applicationInfo(a);
        i.exported = true;
        i.enabled = true;
        i.processName = i.taskAffinity = a.packageName;
        return i;
    }

    public static ResolveInfo resolveInfo(App a) {
        ResolveInfo r = new ResolveInfo();
        r.activityInfo = activityInfo(a, null);
        return r;
    }

    public static PackageInfo packageInfo(App a, int flags) {
        PackageInfo p = new PackageInfo();
        p.packageName = a.packageName;
        p.versionName = a.versionName;
        p.versionCode = (int) a.versionCode;
        p.applicationInfo = applicationInfo(a);
        if ((flags & android.content.pm.PackageManager.GET_ACTIVITIES) != 0 && a.activity != null) p.activities = new ActivityInfo[] { activityInfo(a, null) };
        return p;
    }

    /** The launcher's intent: MAIN with LAUNCHER. */
    public static boolean isLauncherIntent(Intent i) {
        java.util.Set<String> c = i.getCategories();
        return Intent.ACTION_MAIN.equals(i.getAction()) && c != null && c.contains(Intent.CATEGORY_LAUNCHER);
    }

    /** An intent for an activity of another app the phone has: started by the shell, true; any other intent, false. */
    public static boolean start(Intent i) {
        ComponentName c = i.getComponent();
        String target = c != null ? c.getPackageName() : i.getPackage();
        if (target == null || target.equals(Native.packageName())) return false;
        App a = find(target);
        if (a == null) return false;
        String activity = c != null ? c.getClassName() : null;
        if (activity != null && activity.startsWith(".")) activity = target + activity;
        android.util.Log.i("Megler", "phone apps: starting " + target + (activity != null ? "/" + activity : ""));
        if (!Native.launchApp(target, activity)) android.util.Log.w("Megler", "phone apps: " + target + " was not started");
        return true;
    }
}
