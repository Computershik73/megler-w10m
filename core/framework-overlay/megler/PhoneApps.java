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
//
// Their signatures are the certificates of their APKs, from the same description (getPackageInfo with GET_SIGNATURES or
// GET_SIGNING_CERTIFICATES); a PackageInfo's are otherwise the running app's own, and Google Play, which compares an app's
// with its record, took every app it had installed for one "installed but certificate mismatch": not installed from it.
// Each has a uid of its own, from its package name (the first one listed had the running app's, 10100). And the app is
// told as Android tells it when the list changes (an app installed, updated or removed in Megler): ACTION_PACKAGE_ADDED,
// ACTION_PACKAGE_REPLACED, ACTION_PACKAGE_REMOVED and ACTION_PACKAGE_FULLY_REMOVED, the list being read again every three
// seconds once the app has asked for it; Google Play went on showing a removed app as installed.
//
// Their services another app may bind, as the shell described them beside the activities (services.json: Native.appServices;
// the exported ones, each with its label, its icon, its permission and its intent filters): getPackageInfo with GET_SERVICES
// and getServiceInfo tell of them, and queryIntentServices and resolveService find those an intent reaches -- by its
// component, by its action within the package it names, or by its action alone -- which megler.HostedApps would bind here.
// Total Commander lists its plug-ins so (the action com.android.tcplugins.IPluginFunctions, no package), and took every
// plug-in for not installed.
//
// Their splits, as Android names an installed app's (splits): splitNames and splitSourceDirs, /data/app/<package>-1/
// split_<name>.apk, where they can be read. Google Play knows from them which feature modules an app has, adds only those
// it lacks (megler.PlaySplits), and updates it with them; with none told, it updated TikTok Lite without the module it had
// added, and the app was its small start screen again (issue #6).
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
        final java.util.List<Manifest.Filter> filters;
        Activity(String name, String target, String label, int icon, boolean enabled, boolean exported, java.util.List<Manifest.Filter> filters) {
            this.name = name; this.target = target; this.label = label; this.icon = icon; this.enabled = enabled; this.exported = exported;
            this.filters = filters;
        }
        boolean launcher() {
            for (Manifest.Filter f : filters) if (f.actions.contains(Intent.ACTION_MAIN) && f.categories.contains(Intent.CATEGORY_LAUNCHER)) return true;
            return false;
        }
        /** Whether one of its filters takes the intent. */
        boolean takes(Intent i) { return filterFor(i, IntentMatch.typeOf(i), false) != null; }
        /** The first of its filters that takes the intent, of MIME type `type`; with `defaultOnly` one with CATEGORY_DEFAULT too, as
         *  startActivity resolves (megler.Resolver). Null when none does. */
        public Manifest.Filter filterFor(Intent i, String type, boolean defaultOnly) {
            for (Manifest.Filter f : filters) if (IntentMatch.matches(f, i, type, defaultOnly)) return f;
            return null;
        }
        /** Its filters, as the shell described them. */
        public java.util.List<Manifest.Filter> filters() { return filters; }
    }

    /** One of an app's services another app may bind, as the shell described it. */
    public static final class Service {
        public final String name, label, permission;
        public final int icon;
        public final boolean enabled;
        final java.util.List<Manifest.Filter> filters;
        Service(String name, String label, String permission, int icon, boolean enabled, java.util.List<Manifest.Filter> filters) {
            this.name = name; this.label = label; this.permission = permission; this.icon = icon; this.enabled = enabled; this.filters = filters;
        }
        /** Whether one of its filters takes the intent. */
        boolean takes(Intent i) {
            String type = IntentMatch.typeOf(i);
            for (Manifest.Filter f : filters) if (IntentMatch.matches(f, i, type, false)) return true;
            return false;
        }
    }

    /*
     * An intent filter as the shell described it (megler_apk_describe_activities), as Android's resolver has it: the action,
     * every category, the data -- schemes, hosts each with its port, paths and scheme-specific parts, types -- its priority
     * and autoVerify; megler.IntentMatch matches it as the app's own filters are matched. A description of before (Megler
     * 0.1.0.734 and earlier) has no ports, paths or parts, until the shell describes the app again.
     */
    static Manifest.Filter filter(org.json.JSONObject x) throws org.json.JSONException {
        Manifest.Filter f = new Manifest.Filter();
        f.actions.addAll(strings(x, "actions"));
        f.categories.addAll(strings(x, "categories"));
        f.schemes.addAll(strings(x, "schemes"));
        f.hosts.addAll(strings(x, "hosts"));
        f.mimeTypes.addAll(strings(x, "types"));
        org.json.JSONArray ports = x.optJSONArray("ports");
        for (int k = 0; k < f.hosts.size(); k++) f.ports.add(ports != null && k < ports.length() ? ports.optInt(k, -1) : -1);
        parts(x.optJSONArray("paths"), f.paths);
        parts(x.optJSONArray("ssps"), f.ssps);
        f.priority = x.optInt("priority", 0);
        f.autoVerify = x.optBoolean("autoVerify", false);
        return f;
    }

    private static void parts(org.json.JSONArray a, java.util.List<IntentMatch.Part> out) throws org.json.JSONException {
        for (int k = 0; a != null && k < a.length(); k++) {
            org.json.JSONObject o = a.getJSONObject(k);
            int kind = IntentMatch.kindOf(o.optString("kind"));
            if (kind >= 0 && o.has("value")) out.add(new IntentMatch.Part(kind, o.getString("value")));
        }
    }

    private static java.util.List<App> sApps = new java.util.ArrayList<>();
    private static long sReadAt = -1;
    private static final java.util.HashMap<String, android.graphics.Bitmap> sIcons = new java.util.HashMap<>();
    /* The activities read for each app, with the version they were read at: read again for another version. */
    private static final java.util.HashMap<String, Object[]> sActivities = new java.util.HashMap<>();
    /* The services read for each app: { the version they were read at, the list, when }. Most apps have none, and an intent
       with no package asks every app: an app found with none is asked again after a minute, not each time. */
    private static final java.util.HashMap<String, Object[]> sServices = new java.util.HashMap<>();

    /** The apps, but this one; read again when two seconds have passed, as the shell rewrites the list when the library changes. */
    public static synchronized java.util.List<App> all() {
        long now = android.os.SystemClock.uptimeMillis();
        if (sReadAt >= 0 && now - sReadAt < 2000) return sApps;
        boolean first = sReadAt < 0;
        sReadAt = now;
        String text = Native.appsList();
        java.util.ArrayList<App> apps = new java.util.ArrayList<>();
        if (text != null) {
            String own = Native.packageName();
            for (String line : text.split("\n")) {
                String[] f = line.replace("\r", "").split("\t", -1);
                if (f.length < 5 || f[0].isEmpty() || f[0].equals(own)) continue;
                long code = 0;
                try { code = Long.parseLong(f[4]); } catch (NumberFormatException e) {}
                apps.add(new App(f[0], f[1].isEmpty() ? null : f[1], f[2].isEmpty() ? f[0] : f[2], f[3], code, uidOf(f[0])));
            }
        }
        if (apps.size() != sApps.size()) android.util.Log.i("Megler", "phone apps: " + apps.size() + " besides this one");
        if (first) watch();
        else tellChanges(sApps, apps);
        sApps = apps;
        return apps;
    }

    /* A uid of the app's own, the same at each start: 10200 and up (the running app's is 10100). */
    private static int uidOf(String packageName) {
        return 10200 + (packageName.hashCode() & 0x7fffffff) % 9000;
    }

    /* The list read again every three seconds from now, so that what changes in it is told (all, tellChanges). */
    private static void watch() {
        Thread t = new Thread(() -> {
            for (;;) {
                try {
                    Thread.sleep(3000);
                } catch (InterruptedException e) {
                    return;
                }
                all();
            }
        }, "Megler phone apps");
        t.setDaemon(true);
        t.start();
    }

    /* The broadcasts Android sends when packages come, go and are updated, for what is between `before` and `now`. */
    private static void tellChanges(java.util.List<App> before, java.util.List<App> now) {
        java.util.HashMap<String, App> had = new java.util.HashMap<>();
        for (App a : before) had.put(a.packageName, a);
        java.util.ArrayList<Intent> told = new java.util.ArrayList<>();
        for (App a : now) {
            App b = had.remove(a.packageName);
            if (b == null) {
                told.add(packageIntent(Intent.ACTION_PACKAGE_ADDED, a, false));
            } else if (b.versionCode != a.versionCode) {
                told.add(packageIntent(Intent.ACTION_PACKAGE_REMOVED, b, true));
                told.add(packageIntent(Intent.ACTION_PACKAGE_ADDED, a, true));
                told.add(packageIntent(Intent.ACTION_PACKAGE_REPLACED, a, true));
            }
        }
        for (App b : had.values()) {
            Intent i = packageIntent(Intent.ACTION_PACKAGE_REMOVED, b, false);
            i.putExtra(Intent.EXTRA_DATA_REMOVED, true);
            told.add(i);
            told.add(packageIntent(Intent.ACTION_PACKAGE_FULLY_REMOVED, b, false));
        }
        if (told.isEmpty()) return;
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            android.content.Context app = AppRunner.application();
            if (app == null) return;
            for (Intent i : told) {
                android.util.Log.i("Megler", "phone apps: " + i.getAction() + " " + i.getData().getSchemeSpecificPart());
                app.sendBroadcast(i);
            }
        });
    }

    private static Intent packageIntent(String action, App a, boolean replacing) {
        Intent i = new Intent(action, android.net.Uri.fromParts("package", a.packageName, null));
        i.putExtra(Intent.EXTRA_UID, a.uid);
        i.putExtra(Intent.EXTRA_REPLACING, replacing);
        return i;
    }

    public static App find(String packageName) {
        if (packageName == null) return null;
        for (App a : all()) if (a.packageName.equals(packageName)) return a;
        return null;
    }

    /** The app's activities as the shell described them, or null when it has not. */
    @SuppressWarnings("unchecked")
    public static java.util.List<Activity> activities(App a) {
        return (java.util.List<Activity>) described(a)[1];
    }

    /** The app's signatures, its APK's certificates as the shell described them; null when it has not. */
    public static android.content.pm.Signature[] signatures(App a) {
        android.content.pm.Signature[] s = (android.content.pm.Signature[]) described(a)[2];
        return s != null ? s.clone() : null;
    }

    /* What the shell described of the app, read again for another version: { version, activities, signatures }. */
    private static Object[] described(App a) {
        synchronized (sActivities) {
            Object[] had = sActivities.get(a.packageName);
            if (had != null && (Long) had[0] == a.versionCode) return had;
            java.util.List<Activity> list = null;
            android.content.pm.Signature[] sigs = null;
            byte[] json = Native.appActivities(a.packageName);
            if (json != null) {
                try {
                    org.json.JSONObject d = new org.json.JSONObject(new String(json, java.nio.charset.StandardCharsets.UTF_8));
                    org.json.JSONArray certs = d.optJSONArray("certificates");
                    if (certs != null && certs.length() > 0) {
                        sigs = new android.content.pm.Signature[certs.length()];
                        for (int k = 0; k < certs.length(); k++) sigs[k] = new android.content.pm.Signature(certs.getString(k));
                    }
                    org.json.JSONArray acts = d.getJSONArray("activities");
                    list = new java.util.ArrayList<>();
                    for (int k = 0; k < acts.length(); k++) {
                        org.json.JSONObject o = acts.getJSONObject(k);
                        java.util.ArrayList<Manifest.Filter> filters = new java.util.ArrayList<>();
                        org.json.JSONArray fs = o.optJSONArray("filters");
                        for (int f = 0; fs != null && f < fs.length(); f++) filters.add(filter(fs.getJSONObject(f)));
                        list.add(new Activity(o.getString("name"), o.has("target") ? o.getString("target") : null, o.has("label") ? o.getString("label") : null,
                                              (int) o.optLong("icon", 0), o.optBoolean("enabled", true), o.optBoolean("exported", false), filters));
                    }
                } catch (RuntimeException | org.json.JSONException e) {
                    android.util.Log.w("Megler", "phone apps: the activities of " + a.packageName + " cannot be read: " + e);
                    list = null;
                }
            }
            Object[] d = new Object[] { a.versionCode, list, sigs };
            sActivities.put(a.packageName, d);
            return d;
        }
    }

    /** The app's services another app may bind, as the shell described them; none when it has none or is not described. */
    @SuppressWarnings("unchecked")
    public static java.util.List<Service> services(App a) {
        long now = android.os.SystemClock.uptimeMillis();
        synchronized (sServices) {
            Object[] had = sServices.get(a.packageName);
            if (had != null && (Long) had[0] == a.versionCode && (!((java.util.List<Service>) had[1]).isEmpty() || now - (Long) had[2] < 60000))
                return (java.util.List<Service>) had[1];
            java.util.ArrayList<Service> list = new java.util.ArrayList<>();
            byte[] json = Native.appServices(a.packageName);
            if (json != null) {
                try {
                    org.json.JSONArray all = new org.json.JSONObject(new String(json, java.nio.charset.StandardCharsets.UTF_8)).getJSONArray("services");
                    for (int k = 0; k < all.length(); k++) {
                        org.json.JSONObject o = all.getJSONObject(k);
                        java.util.ArrayList<Manifest.Filter> filters = new java.util.ArrayList<>();
                        org.json.JSONArray fs = o.optJSONArray("filters");
                        for (int f = 0; fs != null && f < fs.length(); f++) filters.add(filter(fs.getJSONObject(f)));
                        list.add(new Service(o.getString("name"), o.has("label") ? o.getString("label") : null, o.has("permission") ? o.getString("permission") : null,
                                             (int) o.optLong("icon", 0), o.optBoolean("enabled", true), filters));
                    }
                } catch (RuntimeException | org.json.JSONException e) {
                    android.util.Log.w("Megler", "phone apps: the services of " + a.packageName + " cannot be read: " + e);
                    list.clear();
                }
            }
            sServices.put(a.packageName, new Object[] { a.versionCode, list, now });
            return list;
        }
    }

    /** The app's service an intent is for: the one its component names, else the first whose filter takes it; or null. */
    public static Service service(App a, Intent i) {
        ComponentName c = i.getComponent();
        if (c == null && i.getAction() == null) return null;
        String name = c != null ? c.getClassName() : null;
        if (name != null && name.startsWith(".")) name = a.packageName + name;
        for (Service s : services(a)) if (name != null ? s.name.equals(name) : s.takes(i)) return s;
        return null;
    }

    /** As Android's package manager tells of it: its label its own or else the app's. */
    public static android.content.pm.ServiceInfo serviceInfo(App a, Service s) {
        android.content.pm.ServiceInfo i = new android.content.pm.ServiceInfo();
        i.name = s.name;
        i.packageName = a.packageName;
        i.nonLocalizedLabel = s.label != null ? s.label : a.label;
        i.icon = s.icon;
        i.applicationInfo = applicationInfo(a);
        i.exported = true;
        i.enabled = s.enabled;
        i.permission = s.permission;
        i.processName = a.packageName;
        return i;
    }

    /**
     * The services of the other apps an intent reaches: the one its component names, or those whose filters take its action
     * -- within the package it names, or in every app when it names none, as an app lists its plug-ins. Only those that would
     * be bound here when asked for (megler.HostedApps.refusal): what is listed can be used. Google's packages have their own
     * answers (megler.MicroG, megler.GooglePackages), and the app's own services are not these.
     */
    public static java.util.List<ResolveInfo> queryServices(Intent i) {
        java.util.ArrayList<ResolveInfo> l = new java.util.ArrayList<>();
        ComponentName c = i.getComponent();
        String target = c != null ? c.getPackageName() : i.getPackage();
        if (c == null && i.getAction() == null) return l;
        if (target != null && (target.equals(Native.packageName()) || GooglePackages.is(target))) return l;
        for (App a : all()) {
            if (target != null ? !target.equals(a.packageName) : GooglePackages.is(a.packageName)) continue;
            String name = c != null ? c.getClassName() : null;
            if (name != null && name.startsWith(".")) name = a.packageName + name;
            for (Service s : services(a)) {
                if (name != null ? !s.name.equals(name) : !s.takes(i)) continue;
                if (HostedApps.refusal(a, s) != null) continue;
                ResolveInfo r = new ResolveInfo();
                r.serviceInfo = serviceInfo(a, s);
                l.add(r);
            }
        }
        return l;
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
        // Where Android has an installed app's APK and its folders, as the running app's own are named: an app that lists
        // the others takes one with no sourceDir for no app at all (LibChecker showed itself alone, issue #9). The APK is
        // there to be read when this package can see the other's (mg_fs_other_apks); its data is its own.
        String dir = "/data/app/" + a.packageName + "-1";
        i.sourceDir = i.publicSourceDir = dir + "/base.apk";
        String abi = Abis.PRIMARY;
        i.nativeLibraryDir = dir + "/lib/" + (abi.equals("arm64-v8a") ? "arm64" : abi.equals("armeabi-v7a") ? "arm" : abi);
        i.dataDir = i.deviceProtectedDataDir = "/data/data/" + a.packageName;
        // (An app whose activities view web links, as Android's package parser marks it: Android's Settings offers its links so.)
        if (Resolver.hasDomainUrls(a.packageName)) i.privateFlags |= ApplicationInfo.PRIVATE_FLAG_HAS_DOMAIN_URLS;
        // Its splits beside it, as Android names an installed app's (sorted): there to be read too.
        String[] splits = splits(a);
        if (splits.length > 0) {
            i.splitNames = splits;
            i.splitSourceDirs = new String[splits.length];
            for (int k = 0; k < splits.length; k++) i.splitSourceDirs[k] = dir + "/split_" + splits[k] + ".apk";
            i.splitPublicSourceDirs = i.splitSourceDirs.clone();
        }
        return i;
    }

    /* The splits of each app as last read, { when, names }: Google Play asks for the PackageInfo of every app, and often. */
    private static final java.util.HashMap<String, Object[]> sSplits = new java.util.HashMap<>();

    /**
     * The app's splits by their names, sorted (Native.appSplits): the ones it was installed with and the feature modules
     * Google Play has added since (megler.PlaySplits), read again when two seconds have passed. Google Play reads them from
     * the PackageInfo to know which modules an app has, and updates it with those; it was told of none, and its update of
     * TikTok Lite installed it without the module that is the whole app (issue #6).
     */
    public static String[] splits(App a) {
        long now = android.os.SystemClock.uptimeMillis();
        synchronized (sSplits) {
            Object[] had = sSplits.get(a.packageName);
            if (had != null && now - (Long) had[0] < 2000) return ((String[]) had[1]).clone();
        }
        String[] names = Native.appSplits(a.packageName);
        if (names == null) names = new String[0];
        synchronized (sSplits) { sSplits.put(a.packageName, new Object[] { now, names }); }
        return names.clone();
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
        if (p.applicationInfo.splitNames != null) {
            p.splitNames = p.applicationInfo.splitNames.clone();
            p.splitRevisionCodes = new int[p.splitNames.length];
        }
        // Its own signatures, not the running app's (a PackageInfo's), and those only when asked for, as on Android; an app
        // the shell has not described has an empty one.
        p.signatures = null;
        p.signingInfo = null;
        if ((flags & (android.content.pm.PackageManager.GET_SIGNATURES | android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES)) != 0) {
            android.content.pm.Signature[] s = signatures(a);
            if (s == null) s = new android.content.pm.Signature[] { new android.content.pm.Signature("") };
            if ((flags & android.content.pm.PackageManager.GET_SIGNATURES) != 0) p.signatures = s;
            if ((flags & android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES) != 0) p.signingInfo = new android.content.pm.SigningInfo(s);
        }
        if ((flags & android.content.pm.PackageManager.GET_ACTIVITIES) != 0) {
            java.util.List<Activity> acts = activities(a);
            if (acts != null) {
                p.activities = new ActivityInfo[acts.size()];
                for (int k = 0; k < acts.size(); k++) p.activities[k] = activityInfo(a, acts.get(k));
            } else if (a.activity != null) p.activities = new ActivityInfo[] { activityInfo(a, (String) null) };
        }
        // Its services another app may bind: the others are not described, and are nobody else's.
        if ((flags & android.content.pm.PackageManager.GET_SERVICES) != 0) {
            java.util.List<Service> list = services(a);
            if (!list.isEmpty()) {
                p.services = new android.content.pm.ServiceInfo[list.size()];
                for (int k = 0; k < list.size(); k++) p.services[k] = serviceInfo(a, list.get(k));
            }
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
        // An intent that names the package and what to do, not who does it: the first of that app's activities whose filter
        // takes it, as Android resolves it within the package (MAIN alone is the app itself, its launcher's business).
        // An app the shell has not described has no activities to look among: it is started as from its launcher.
        java.util.List<Activity> acts = activities(a);
        if (activity == null && acts != null && i.getAction() != null && !Intent.ACTION_MAIN.equals(i.getAction()))
            for (Activity y : acts)
                if (y.enabled && y.exported && y.takes(i)) { activity = y.target != null ? y.target : y.name; break; }
        return new String[] { target, activity };
    }

    /*
     * The whole Intent goes with the request for another app's activity: written out (megler.Flat) into a file of the services'
     * shared folder, intents/<that app's package>/<token>, which the request names after the activity's class, "<class>~<token>"
     * -- the shell hands the name on as it is (megler_start_activity), and no class has a '~' in its name. The app asked reads
     * the file and deletes it (asked, from megler.AppRunner). Only the package and the class went, so that Total Commander's
     * Wi-Fi plug-in, asked to send files (an action, a type and the list of the files among the extras), opened with nothing to
     * send. With no shared folder, or a name too long for the shell's request (200 bytes), the class goes alone, as before.
     */
    private static final String INTENTS = MicroG.SHARED + "/intents";
    private static int sSent;

    private static String withIntent(String pkg, String activity, Intent i) {
        try {
            if (!new java.io.File(MicroG.SHARED).isDirectory()) return activity;
            java.io.File dir = new java.io.File(INTENTS, pkg);
            dir.mkdirs();
            sweep(dir);
            String token;
            synchronized (PhoneApps.class) { token = Integer.toHexString(android.os.Process.myPid()) + "-" + Long.toHexString(System.currentTimeMillis()) + "-" + (++sSent); }
            if (activity.length() + 1 + token.length() >= 200) return activity;
            byte[] bytes = Flat.intent(i);
            java.io.File file = new java.io.File(dir, token), tmp = new java.io.File(dir, token + ".new");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(tmp)) { out.write(bytes); }
            if (!tmp.renameTo(file)) { tmp.delete(); return activity; }
            return activity + "~" + token;
        } catch (Exception e) {
            android.util.Log.w("Megler", "phone apps: the intent for " + pkg + "/" + activity + " cannot be written (" + e + "); the activity is asked for without it");
            return activity;
        }
    }

    /* Intents nobody read (the app asked did not start): gone after a day, so that the folder does not grow. */
    private static void sweep(java.io.File dir) {
        java.io.File[] files = dir.listFiles();
        if (files == null) return;
        long old = System.currentTimeMillis() - 24 * 3600 * 1000L;
        for (java.io.File f : files) if (f.lastModified() < old) f.delete();
    }

    /**
     * What another app asked this one for (megler.AppRunner.startRequested): the activity's class and, when the request names
     * an Intent's file ("<class>~<token>", withIntent), that Intent, read and deleted, made by `l`'s classes; the plain intent
     * for the class otherwise. Its component is this app's activity either way. Never null: { class, Intent }.
     */
    public static Object[] asked(String request, ClassLoader l) {
        int at = request.indexOf('~');
        String cls = at < 0 ? request : request.substring(0, at), token = at < 0 ? null : request.substring(at + 1);
        Intent i = null;
        if (token != null && !token.isEmpty() && token.matches("[0-9a-f-]+")) {
            java.io.File file = new java.io.File(new java.io.File(INTENTS, Native.packageName()), token);
            try {
                java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
                try (java.io.FileInputStream in = new java.io.FileInputStream(file)) {
                    byte[] part = new byte[8192];
                    for (int n; (n = in.read(part)) > 0; ) bytes.write(part, 0, n);
                }
                i = Flat.intent(bytes.toByteArray(), l);
            } catch (Exception e) {
                android.util.Log.w("Megler", "phone apps: the intent another app wrote for " + cls + " cannot be read (" + e + ")");
            }
            file.delete();
        }
        if (i == null) i = new Intent();
        i.setComponent(new ComponentName(Native.packageName(), cls));
        i.setPackage(null);
        return new Object[] { cls, i };
    }

    /** An intent for an activity of another app the phone has, started by the shell; a result asked for (`request`, 0 or more)
     *  comes back to the caller when it is in front again (megler.AppResults), as AppRunner.startActivity has it. */
    static void startWith(android.content.Context from, Intent i, int request) {
        String[] t = target(i);
        if (t == null) return;
        if (request >= 0) AppResults.await(from instanceof android.app.Activity ? (android.app.Activity) from : AppRunner.top(), request, t[0], t[1]);
        start(i);
    }

    /** An intent for an activity of another app the phone has: started by the shell, true; any other intent, false. */
    public static boolean start(Intent i) {
        String[] t = target(i);
        if (t == null) return false;
        android.util.Log.i("Megler", "phone apps: starting " + t[0] + (t[1] != null ? "/" + t[1] : ""));
        if (!Native.launchApp(t[0], t[1] == null ? null : withIntent(t[0], t[1], i))) android.util.Log.w("Megler", "phone apps: " + t[0] + " was not started");
        return true;
    }
}
