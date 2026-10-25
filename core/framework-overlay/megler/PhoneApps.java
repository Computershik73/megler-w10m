// Megler: the other apps the phone has, for an app that lists or starts them -- a launcher (Nova, Niagara, a Launcher3),
// or an app that opens another. They are the apps of Megler's library, which the shell lists in a folder the core reads
// (megler_run_config.apps_dir: Native.appsList, Native.appIcon): each its package, its launcher activity, its label, its
// version, its icon. android.content.pm.PackageManager answers for them from here (queryIntentActivities of the
// launcher's intent, getLaunchIntentForPackage, getPackageInfo, getApplicationInfo, getInstalledPackages), and
// PackageItemInfo.loadIcon gives their icons; an activity of theirs started is started by the shell
// (Native.launchApp, megler_system_host.launch_app): their own package, or Megler with them.
//
// Their activities, every one, as the shell described them from their APKs beside the list (megler_apk_describe_activities:
// Native.appActivities, Native.appResIcon), for a launcher or a shortcut maker (QuickShortcutMaker) that lists them: the
// package's activities (getPackageInfo with GET_ACTIVITIES, getActivityInfo), each with its label, its icon and its intent
// filters; every launcher entry of an app, an alias's too; and those an intent names their package for, or asks for with
// ACTION_CREATE_SHORTCUT. An intent with no package that any app could take (a link) is not given them: it goes where it
// went before, to Windows. An app the shell has not described has its launcher activity alone, as before.
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

    /** One of an app's activities, or an alias of one (`target`), as the shell described it. */
    public static final class Activity {
        public final String name, target, label;
        public final int icon;
        public final boolean enabled, exported;
        final java.util.List<Filter> filters;
        Activity(String name, String target, String label, int icon, boolean enabled, boolean exported, java.util.List<Filter> filters) {
            this.name = name; this.target = target; this.label = label; this.icon = icon; this.enabled = enabled; this.exported = exported;
            this.filters = filters;
        }
        boolean launcher() {
            for (Filter f : filters) if (f.actions.contains(Intent.ACTION_MAIN) && f.categories.contains(Intent.CATEGORY_LAUNCHER)) return true;
            return false;
        }
        /** Whether one of its filters takes the intent. */
        boolean takes(Intent i) {
            for (Filter f : filters) if (f.takes(i)) return true;
            return false;
        }
    }

    /** An intent filter as the shell described it: as Android's resolver has it, the action, every category, the data. */
    static final class Filter {
        final java.util.List<String> actions, categories, schemes, hosts, types;
        Filter(java.util.List<String> actions, java.util.List<String> categories, java.util.List<String> schemes, java.util.List<String> hosts,
               java.util.List<String> types) {
            this.actions = actions; this.categories = categories; this.schemes = schemes; this.hosts = hosts; this.types = types;
        }
        boolean takes(Intent i) {
            if (i.getAction() != null ? !actions.contains(i.getAction()) : actions.isEmpty()) return false;
            if (i.getCategories() != null) for (String c : i.getCategories()) if (!categories.contains(c)) return false;
            android.net.Uri data = i.getData();
            String type = i.getType(), scheme = data != null ? data.getScheme() : null;
            if (type == null && scheme == null) return schemes.isEmpty() && types.isEmpty();
            if (type != null) {
                boolean typed = false;
                for (String t : types) if (t.equals(type) || t.equals("*/*") || (t.endsWith("/*") && type.startsWith(t.substring(0, t.length() - 1)))) typed = true;
                if (!typed) return false;
            } else if (!types.isEmpty()) return false;
            if (scheme != null) {
                // A filter of types alone takes content: and file: data (Android's IntentFilter.matchData).
                if (schemes.isEmpty()) return type != null && ("content".equals(scheme) || "file".equals(scheme));
                if (!schemes.contains(scheme)) return false;
                if (!hosts.isEmpty() && (data.getHost() == null || !hosts.contains(data.getHost()))) return false;
            }
            return true;
        }
    }

    private static java.util.List<App> sApps = new java.util.ArrayList<>();
    private static long sReadAt = -1;
    private static final java.util.HashMap<String, android.graphics.Bitmap> sIcons = new java.util.HashMap<>();
    /* The activities read for each app, with the version they were read at: read again for another version. */
    private static final java.util.HashMap<String, Object[]> sActivities = new java.util.HashMap<>();

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

    /** The app's activities as the shell described them, or null when it has not. */
    @SuppressWarnings("unchecked")
    public static java.util.List<Activity> activities(App a) {
        synchronized (sActivities) {
            Object[] had = sActivities.get(a.packageName);
            if (had != null && (Long) had[0] == a.versionCode) return (java.util.List<Activity>) had[1];
            java.util.List<Activity> list = null;
            byte[] json = Native.appActivities(a.packageName);
            if (json != null) {
                try {
                    org.json.JSONArray acts = new org.json.JSONObject(new String(json, java.nio.charset.StandardCharsets.UTF_8)).getJSONArray("activities");
                    list = new java.util.ArrayList<>();
                    for (int k = 0; k < acts.length(); k++) {
                        org.json.JSONObject o = acts.getJSONObject(k);
                        java.util.ArrayList<Filter> filters = new java.util.ArrayList<>();
                        org.json.JSONArray fs = o.optJSONArray("filters");
                        for (int f = 0; fs != null && f < fs.length(); f++) {
                            org.json.JSONObject x = fs.getJSONObject(f);
                            filters.add(new Filter(strings(x, "actions"), strings(x, "categories"), strings(x, "schemes"), strings(x, "hosts"), strings(x, "types")));
                        }
                        list.add(new Activity(o.getString("name"), o.has("target") ? o.getString("target") : null, o.has("label") ? o.getString("label") : null,
                                              (int) o.optLong("icon", 0), o.optBoolean("enabled", true), o.optBoolean("exported", false), filters));
                    }
                } catch (org.json.JSONException e) {
                    android.util.Log.w("Megler", "phone apps: the activities of " + a.packageName + " cannot be read: " + e);
                    list = null;
                }
            }
            sActivities.put(a.packageName, new Object[] { a.versionCode, list });
            return list;
        }
    }

    private static java.util.List<String> strings(org.json.JSONObject o, String key) throws org.json.JSONException {
        java.util.ArrayList<String> l = new java.util.ArrayList<>();
        org.json.JSONArray a = o.optJSONArray(key);
        for (int k = 0; a != null && k < a.length(); k++) l.add(a.getString(k));
        return l;
    }

    /** The activity (or alias) of that name, or null. */
    public static Activity activity(App a, String name) {
        java.util.List<Activity> l = activities(a);
        if (l == null || name == null) return null;
        for (Activity x : l) if (x.name.equals(name)) return x;
        return null;
    }

    /** The picture of the app's icon resource `id` (one its activities name), else the app's icon, else null. */
    public static android.graphics.drawable.Drawable icon(String packageName, int id) {
        if (find(packageName) == null) return null;
        if (id != 0) {
            String key = packageName + "#" + Integer.toHexString(id);
            android.graphics.Bitmap b;
            synchronized (sIcons) {
                b = sIcons.get(key);
                if (b == null && !sIcons.containsKey(key)) {
                    byte[] png = Native.appResIcon(packageName, id);
                    b = png == null ? null : android.graphics.BitmapFactory.decodeByteArray(png, 0, png.length);
                    sIcons.put(key, b);
                }
            }
            if (b != null) return new android.graphics.drawable.BitmapDrawable(ContextImpl.app().getResources(), b);
        }
        return icon(packageName);
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
        String name = activity != null ? activity : a.activity;
        Activity described = activity(a, name);
        if (described != null) return activityInfo(a, described);
        ActivityInfo i = new ActivityInfo();
        i.name = name;
        i.packageName = a.packageName;
        i.nonLocalizedLabel = a.label;
        i.applicationInfo = applicationInfo(a);
        i.exported = true;
        i.enabled = true;
        i.processName = i.taskAffinity = a.packageName;
        return i;
    }

    /** As Android's package manager tells of it: its label its own or else the app's; an alias names what it stands for. */
    public static ActivityInfo activityInfo(App a, Activity x) {
        ActivityInfo i = new ActivityInfo();
        i.name = x.name;
        i.targetActivity = x.target;
        i.packageName = a.packageName;
        i.nonLocalizedLabel = x.label != null ? x.label : a.label;
        i.icon = x.icon;
        i.applicationInfo = applicationInfo(a);
        i.exported = x.exported;
        i.enabled = x.enabled;
        i.processName = i.taskAffinity = a.packageName;
        return i;
    }

    public static ResolveInfo resolveInfo(App a) {
        ResolveInfo r = new ResolveInfo();
        r.activityInfo = activityInfo(a, (String) null);
        return r;
    }

    private static ResolveInfo resolveInfo(App a, Activity x) {
        ResolveInfo r = new ResolveInfo();
        r.activityInfo = activityInfo(a, x);
        return r;
    }

    /** The app's entries in a launcher: each enabled activity or alias that is MAIN and LAUNCHER, or else the one the list names. */
    public static java.util.List<ResolveInfo> launcherEntries(App a) {
        java.util.ArrayList<ResolveInfo> l = new java.util.ArrayList<>();
        java.util.List<Activity> acts = activities(a);
        if (acts != null) for (Activity x : acts) if (x.enabled && x.launcher()) l.add(resolveInfo(a, x));
        if (l.isEmpty() && a.activity != null) l.add(resolveInfo(a));
        return l;
    }

    /**
     * The activities of the other apps an intent is for, which is neither the launcher's nor one that names a class: those of
     * the package it names, or, with none named, those that make shortcuts (ACTION_CREATE_SHORTCUT), as a shortcut maker
     * lists them. Enabled and exported ones, whose filters take it.
     */
    public static java.util.List<ResolveInfo> query(Intent i) {
        java.util.ArrayList<ResolveInfo> l = new java.util.ArrayList<>();
        String target = i.getPackage();
        if (target == null && !Intent.ACTION_CREATE_SHORTCUT.equals(i.getAction())) return l;
        for (App a : all()) {
            if (target != null && !target.equals(a.packageName)) continue;
            java.util.List<Activity> acts = activities(a);
            if (acts != null) for (Activity x : acts) if (x.enabled && x.exported && x.takes(i)) l.add(resolveInfo(a, x));
        }
        return l;
    }

    public static PackageInfo packageInfo(App a, int flags) {
        PackageInfo p = new PackageInfo();
        p.packageName = a.packageName;
        p.versionName = a.versionName;
        p.versionCode = (int) a.versionCode;
        p.applicationInfo = applicationInfo(a);
        if ((flags & android.content.pm.PackageManager.GET_ACTIVITIES) != 0) {
            java.util.List<Activity> acts = activities(a);
            if (acts != null) {
                p.activities = new ActivityInfo[acts.size()];
                for (int k = 0; k < acts.size(); k++) p.activities[k] = activityInfo(a, acts.get(k));
            } else if (a.activity != null) p.activities = new ActivityInfo[] { activityInfo(a, (String) null) };
        }
        return p;
    }

    /** The launcher's intent: MAIN with LAUNCHER. */
    public static boolean isLauncherIntent(Intent i) {
        java.util.Set<String> c = i.getCategories();
        return Intent.ACTION_MAIN.equals(i.getAction()) && c != null && c.contains(Intent.CATEGORY_LAUNCHER);
    }

    /** For an intent for an activity of another app the phone has: that app's package and the activity (null when the intent
     *  names none); for any other intent, null. */
    public static String[] target(Intent i) {
        ComponentName c = i.getComponent();
        String target = c != null ? c.getPackageName() : i.getPackage();
        if (target == null || target.equals(Native.packageName())) return null;
        App a = find(target);
        if (a == null) return null;
        String activity = c != null ? c.getClassName() : null;
        if (activity != null && activity.startsWith(".")) activity = target + activity;
        // An alias has no class: what is started is the activity it stands for, as Android starts it.
        Activity x = activity(a, activity);
        if (x != null && x.target != null) activity = x.target;
        return new String[] { target, activity };
    }

    /** An intent for an activity of another app the phone has: started by the shell, true; any other intent, false. */
    public static boolean start(Intent i) {
        String[] t = target(i);
        if (t == null) return false;
        android.util.Log.i("Megler", "phone apps: starting " + t[0] + (t[1] != null ? "/" + t[1] : ""));
        if (!Native.launchApp(t[0], t[1])) android.util.Log.w("Megler", "phone apps: " + t[0] + " was not started");
        return true;
    }
}
