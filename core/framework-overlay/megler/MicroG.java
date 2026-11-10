package megler;

import android.app.Activity;
import android.app.Application;
import android.app.Service;
import android.content.*;
import android.content.pm.*;
import android.content.res.*;
import android.os.*;
import java.io.File;
import java.util.*;

/** Optional GmsCore APK service host. Services execute in the client's process. */
public final class MicroG {
    public static final String APK = "/system/priv-app/GmsCore/GmsCore.apk";
    /** The folder of the services every app shares (core/src/run/mg_run.c). */
    public static final String SHARED = "/data/megler-services";
    private static final String NS = "http://schemas.android.com/apk/res/android";
    /* Read without the class's lock (loaded): get() holds it while GmsCore is read, for seconds. */
    private static volatile MicroG instance;
    private static boolean checked;
    private final PackageInfo info = new PackageInfo();
    private final ArrayList<Entry> services = new ArrayList<>();
    private final ArrayList<Receiver> receivers = new ArrayList<>();
    private final ArrayList<ProviderInfo> providers = new ArrayList<>();
    /* GmsCore's activities, which an app that hosts it starts here (activity, newActivity). */
    private ArrayList<Manifest.Component> activities = new ArrayList<>();
    private final HashMap<String, ContentProvider> runningProviders = new HashMap<>();
    private final HashMap<String, Running> running = new HashMap<>();
    /* The id of the latest start of each of GmsCore's services here (megler.Services.postStart). */
    private final HashMap<String, Integer> startIds = new HashMap<>();
    private final Map<ServiceConnection, Binding> bindings = new IdentityHashMap<>();
    private final PackageContext context;
    private Application application;
    /* The Application made and attached (attached), before it is created (application). */
    private volatile Application attached;
    private Application attaching;
    private volatile ClassLoader loader;
    private static final class Entry {
        ServiceInfo info = new ServiceInfo();
        ArrayList<IntentFilter> filters = new ArrayList<>();
    }
    /** A receiver of microG's manifest, which hears broadcasts for microG (send), and the boot's (boot). */
    private static final class Receiver {
        ActivityInfo info = new ActivityInfo();
        ArrayList<IntentFilter> filters = new ArrayList<>();
    }
    private static final class Running {
        Service service;
        int starts, clients;
        boolean started;
        HashMap<String, IBinder> binders = new HashMap<>();
    }
    private static final class Binding {
        Running running;
        Intent intent;
        Binding(Running r, Intent i) { running = r; intent = i; }
    }
    /*
     * GmsCore read whole -- its resources, and its manifest's 395 services, 80 receivers and 41 providers with their filters, 1
     * MB of binary XML -- the first time something of it is used here: 2.7 to 3.8 s of an app's start on a Lumia 650. Every
     * app paid that as it started, AIDA64 and Apktool M too, before what is asked of GmsCore without its components
     * (installed, hosts, activity's first checks, packageInfoBasic, isMicroG) stopped reading it: where it still is, the log
     * says what asked for it first.
     */
    private static synchronized MicroG get() {
        if (GooglePackages.GMS.equals(Native.packageName())) return null;
        if (!checked) {
            checked = true;
            if (new File(APK).isFile()) try {
                long began = SystemClock.uptimeMillis();
                instance = new MicroG();
                android.util.Log.i("MicroG", "GmsCore read whole for this app in " + (SystemClock.uptimeMillis() - began) + " ms, first wanted by:",
                                   new Throwable("what asked"));
            }
            catch (Exception e) { android.util.Log.e("MicroG", "Cannot read " + APK, e); }
        }
        return instance;
    }
    /**
     * GmsCore as read already (get), or nothing: what is asked of it then is not worth reading it for. Not under the class's
     * lock, which get() holds while it reads GmsCore: Spotify's and Gmail's main threads spent 28 to 31% of ten seconds of
     * their start waiting here (hosts, for each activity) while another thread read it.
     */
    private static MicroG loaded() { return instance; }
    /** Whether Google Play services are here: their APK is (not read for it). */
    public static boolean installed() { return !GooglePackages.GMS.equals(Native.packageName()) && new File(APK).isFile(); }

    /**
     * The activity of GmsCore's an intent of this app opens, when it is for GmsCore's package (by its component, or its package
     * with an action): it runs here, in GmsCore's class scope and with its context, as its services do (megler.AppRunner).
     * GmsCore's sign-in started from here hands its screens this process's own objects -- the PendingIntent its authenticator
     * wraps in UNPACKING_REDIRECT, the AccountAuthenticatorResponse -- which its separate app's process could not have: Play
     * Store's "Sign in" opened that app's settings instead. Disabled activities, and launcher intents (GmsCore's own app), are not.
     * An intent that names no package is GmsCore's when its action is not Android's own and this app has no activity for it:
     * GmsCore's sign-in finds its next screen so (PRE_ADD_ACCOUNT).
     */
    public static Manifest.Component activity(Intent i) {
        // (What tells an intent is none of GmsCore's first: every activity an app starts is asked of here.)
        if (i == null || PhoneApps.isLauncherIntent(i)) return null;
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        if (pkg == null) {
            String action = i.getAction();
            if (action == null || action.startsWith("android.") || Manifest.resolve(i) != null) return null;
        } else if (!GooglePackages.GMS.equals(pkg)) return null;
        MicroG g = get();
        if (g == null) return null;
        java.util.ArrayList<Manifest.Component> on = new java.util.ArrayList<>();
        for (Manifest.Component c : g.activities) if (enabled(c)) on.add(c);
        return Manifest.resolveIn(on, i);
    }
    /* Whether one of GmsCore's components is enabled: as GmsCore set it while it ran (PackageManager.setComponentEnabledSetting,
       which the app's package manager keeps), else as its manifest says. GmsCore enables a module's activities when it sets
       the module up, and its manifest has them off: its account settings (SettingsLoaderActivity, VIEW_SETTINGS) went to
       GmsCore's own package, which showed an empty screen, where it is an activity of GmsCore's hosted here. */
    static boolean enabled(Manifest.Component c) {
        int s = ContextImpl.app().getPackageManager().getComponentEnabledSetting(new android.content.ComponentName(GooglePackages.GMS, c.name));
        return s == android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_ENABLED
            || (s == android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT && c.enabled);
    }
    /** Whether a component is one of GmsCore's activities (activity), which run in its scope here: asked of every activity an
        app starts, and none is until GmsCore was read for one (activity), so it is not read for the answer. */
    public static boolean hosts(Manifest.Component c) { MicroG g = loaded(); return g != null && c != null && g.activities.contains(c); }
    /** An activity of GmsCore's, made in its class scope, its Application made first as for anything of GmsCore's that runs here. */
    public static Activity newActivity(String cls) throws Exception {
        MicroG g = get();
        g.application();
        return (Activity) g.loader().loadClass(cls).newInstance();
    }
    /** The context GmsCore's activities here are based on, and their Application. */
    public static Context activityBase() { MicroG g = get(); return g == null ? null : g.context; }
    public static Application activityApplication() throws Exception { MicroG g = get(); return g == null ? null : g.application(); }
    /* (The package asked for first: get() reads GmsCore's manifest the first time, 3.8 s on a Lumia 650, and Android System
       WebView's start asks for its own package's context so -- inside TikTok Lite's cookie lock, which held its every request,
       its servers' speed tests too, until then; package 628.) */
    public static PackageInfo packageInfo(String pkg) {
        if (!GooglePackages.GMS.equals(pkg)) return null;
        MicroG g = get();
        return g != null ? g.info : null;
    }
    /**
     * packageInfo for what wants none of GmsCore's components -- its version, signatures and application: an app's check that
     * Google Play services are there and new enough (GoogleApiAvailability), PackageManager.getApplicationInfo -- from its
     * manifest's first tags alone (basic) until GmsCore is read whole; its services, receivers and providers are then null.
     */
    public static PackageInfo packageInfoBasic(String pkg) {
        if (!GooglePackages.GMS.equals(pkg)) return null;
        MicroG g = loaded();
        return g != null ? g.info : basic();
    }
    private static PackageInfo sBasic;
    private static boolean sBasicRead, sBasicMicroG;
    private static final Object sBasicLock = new Object();
    /* GmsCore's PackageInfo from its manifest's tags up to <application>, read from its APK alone (not its resources), and
       whether it is microG's (its manifest names org.microg's classes). Under a lock of its own: the class's is get()'s. */
    private static PackageInfo basic() {
        synchronized (sBasicLock) { return basicLocked(); }
    }
    private static PackageInfo basicLocked() {
        if (sBasicRead) return sBasic;
        sBasicRead = true;
        if (!installed()) return null;
        long began = SystemClock.uptimeMillis();
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(APK)) {
            java.util.zip.ZipEntry entry = zip.getEntry("AndroidManifest.xml");
            if (entry == null) return null;
            byte[] manifest;
            try (java.io.InputStream in = zip.getInputStream(entry); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
                byte[] buf = new byte[65536]; int n;
                while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
                manifest = out.toByteArray();
            }
            PackageInfo info = new PackageInfo();
            ApplicationInfo a = newApplicationInfo();
            info.applicationInfo = a;
            XmlBlock.Parser p = new XmlBlock(manifest, APK).newParser();
            int event;
            while ((event = p.next()) != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                if (event != org.xmlpull.v1.XmlPullParser.START_TAG) continue;
                String tag = p.getName();
                if ("manifest".equals(tag)) {
                    info.packageName = p.getAttributeValue(null, "package");
                    if (!GooglePackages.GMS.equals(info.packageName)) throw new IllegalArgumentException("Not GmsCore: " + info.packageName);
                    info.versionCode = p.getAttributeIntValue(NS, "versionCode", 0);
                    info.versionName = p.getAttributeValue(NS, "versionName");
                } else if ("uses-sdk".equals(tag)) {
                    a.minSdkVersion = p.getAttributeIntValue(NS, "minSdkVersion", 19);
                    a.targetSdkVersion = p.getAttributeIntValue(NS, "targetSdkVersion", a.minSdkVersion);
                } else if ("application".equals(tag)) {
                    a.className = full(p.getAttributeValue(NS, "name"));
                    a.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                    break;
                }
            }
            info.signatures = new Signature[] {GooglePackages.signature()};
            info.signingInfo = new SigningInfo(info.signatures);
            sBasicMicroG = contains(manifest, "org.microg.");
            sBasic = info;
            android.util.Log.i("MicroG", "GmsCore " + info.versionName + (sBasicMicroG ? " (microG's)" : "") + " from its manifest's first tags, in " +
                                         (SystemClock.uptimeMillis() - began) + " ms");
        } catch (Exception e) {
            android.util.Log.e("MicroG", "Cannot read the manifest of " + APK, e);
        }
        return sBasic;
    }
    /* Whether a binary XML's bytes hold `text`, in UTF-8 or UTF-16 (its string pool's two forms). */
    private static boolean contains(byte[] bytes, String text) {
        return indexOf(bytes, text.getBytes(java.nio.charset.StandardCharsets.UTF_8)) >= 0
            || indexOf(bytes, text.getBytes(java.nio.charset.StandardCharsets.UTF_16LE)) >= 0;
    }
    private static int indexOf(byte[] bytes, byte[] what) {
        outer:
        for (int i = 0; i + what.length <= bytes.length; i++) {
            for (int j = 0; j < what.length; j++) if (bytes[i + j] != what[j]) continue outer;
            return i;
        }
        return -1;
    }
    /* GmsCore's application as this app sees it, before its manifest says its class, SDK levels and whether it is enabled. */
    private static ApplicationInfo newApplicationInfo() {
        ApplicationInfo a = new ApplicationInfo();
        a.packageName = a.processName = GooglePackages.GMS;
        a.sourceDir = a.publicSourceDir = APK;
        // microG's state is one for all apps, as on Android: the services' shared folder (mg_run.c), which microG's own app has
        // for its private files too. Where there is none, it is this app's own.
        File shared = new File(SHARED);
        a.dataDir = shared.isDirectory() ? new File(shared, GooglePackages.GMS).getPath() : new File(Native.dataDir(), "megler-services/" + GooglePackages.GMS).getPath();
        // And its two storages' folders, as Android names them: the device protected one is where its contexts keep it
        // (PackageContext.getDataDir). GmsCore's DataDirPermissionsHelper makes a File of each when the phone has started, and with null
        // its executor's thread ended on a NullPointerException.
        a.credentialProtectedDataDir = a.dataDir;
        a.deviceProtectedDataDir = new File(a.dataDir, "device_protected").getPath();
        a.nativeLibraryDir = APK + "!/lib/" + Abis.PRIMARY;
        // Their splits, as Android lists an installed app's, sorted by name (Native.gmsSplitPaths); none in one APK.
        String[] splits = Native.gmsSplitPaths();
        if (splits != null && splits.length > 0) {
            a.splitSourceDirs = splits.clone();
            a.splitPublicSourceDirs = splits.clone();
            a.splitNames = new String[splits.length];
            for (int i = 0; i < splits.length; i++) a.splitNames[i] = splitName(splits[i]);
        }
        a.enabled = true; a.uid = GooglePackages.sharedUid();
        a.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        a.metaData = new Bundle();
        return a;
    }
    public static Context context(String pkg) {
        if (!GooglePackages.GMS.equals(pkg)) return null;
        MicroG g = get();
        return g != null ? g.context : null;
    }
    /** Whether the Google Play services hosted here are microG's (its classes are org.microg's), not Google's own. */
    public static boolean isMicroG() {
        MicroG g = loaded();
        if (g != null) return g.microg();
        if (basic() == null) return false;
        synchronized (sBasicLock) { return sBasicMicroG; }
    }
    private boolean microg() {
        for (Receiver r : receivers) if (r.info.name.startsWith("org.microg.")) return true;
        for (ProviderInfo p : providers) if (p.name != null && p.name.startsWith("org.microg.")) return true;
        return false;
    }
    /**
     * Whether a provider's authority may be Google Play services': theirs are com.google's -- all 49 of Google's own 26.41, all
     * but microG's subscribedfeeds of its -- so another is not worth reading GmsCore for (until it was read for something else).
     */
    public static boolean mayBeGmsAuthority(String authority) {
        return authority != null && (loaded() != null || authority.startsWith("com.google.") || "subscribedfeeds".equals(authority));
    }
    public static ContentProvider provider(String authority) {
        // (Every authority but the app's own and Android's few asks here: ContentResolver.)
        if (!mayBeGmsAuthority(authority)) return null;
        MicroG g = get();
        if (g == null) return null;
        g.attachOnMain();
        synchronized (g.runningProviders) {
            for (ProviderInfo info : g.providers) {
                if (!info.exported || info.authority == null || !Arrays.asList(info.authority.split(";")).contains(authority)) continue;
                ContentProvider running = g.runningProviders.get(info.name);
                if (running != null) return running;
                try {
                    running = (ContentProvider) g.loader().loadClass(info.name).newInstance();
                    running.attachInfo(g.context, info);
                    g.runningProviders.put(info.name, running);
                    return running;
                } catch (Throwable t) {
                    android.util.Log.e("MicroG", "Starting provider " + info.name + " failed", t);
                    return null;
                }
            }
        }
        return null;
    }
    private static String full(String name) {
        if (name == null) return null;
        return name.startsWith(".") ? GooglePackages.GMS + name : name.indexOf('.') < 0 ? GooglePackages.GMS + "." + name : name;
    }
    /* A split's name from its path, as Android installs one: /system/priv-app/GmsCore/split_config.xhdpi.apk is config.xhdpi. */
    private static String splitName(String path) {
        String n = new File(path).getName();
        if (n.endsWith(".apk")) n = n.substring(0, n.length() - 4);
        return n.startsWith("split_") ? n.substring(6) : n;
    }
    /*
     * Their splits, said once as they join their resources; and one with code or native libraries, of which nothing is taken
     * here (their classes and libraries are their base's alone): Play Store's bundle of Google's own has had none of either.
     */
    private static void saySplits(AssetManager assets, String[] splits) {
        StringBuilder names = new StringBuilder();
        for (String s : splits) {
            names.append(names.length() > 0 ? ", " : "").append(splitName(s));
            // (Its entries as the core has them already: the same path is the same APK, opened once.)
            int apk = Native.addApk(s);
            if (apk < 0) continue;
            String[] dex = Native.listApkFiles(apk, "classes"), libs = Native.listApkFiles(apk, "lib/");
            boolean code = false, native_ = libs != null && libs.length > 0;
            if (dex != null) for (String n : dex) code |= n.endsWith(".dex");
            if (code || native_) android.util.Log.w("MicroG", "Google Play services' split " + splitName(s) + " has " + (code ? native_ ? "code and native libraries" : "code" : "native libraries")
                + ", which are not taken from it here: their classes and libraries are their base's");
        }
        android.util.Log.i("MicroG", "Google Play services' splits, whose resources and files join their base's: " + names
            + " (" + assets.getApkPaths().length + " APKs)");
    }
    private MicroG() throws Exception {
        // With their splits, where they came in several APKs (Native.gmsSplitPaths): Play Store installs Google's own as a base
        // and splits of the phone's density and languages, and a resource whose every value is a density's is in the
        // density's split alone -- their account screens' Google logo (ProductLockupView), without which "Manage your Google
        // Account" ended Play Store on Resources$NotFoundException (0.1.0.685).
        String[] splits = Native.gmsSplitPaths();
        AssetManager assets = AssetManager.meglerForPackage(APK, GooglePackages.GMS, splits);
        Resources resources = new Resources(assets);
        if (splits != null && splits.length > 0) saySplits(assets, splits);
        byte[] manifest;
        try (java.io.InputStream in = assets.openNonAsset("AndroidManifest.xml"); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            manifest = out.toByteArray();
        }
        ApplicationInfo a = newApplicationInfo();
        info.applicationInfo = a;
        XmlBlock.Parser p = new XmlBlock(manifest, APK).newParser();
        Entry current = null; Receiver receiver = null; IntentFilter filter = null; boolean inApp = false;
        int event;
        while ((event = p.next()) != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
            String tag = p.getName();
            if (event == org.xmlpull.v1.XmlPullParser.END_TAG) {
                if ("service".equals(tag)) current = null;
                if ("receiver".equals(tag)) receiver = null;
                if ("intent-filter".equals(tag)) filter = null;
                if ("application".equals(tag)) inApp = false;
                continue;
            }
            if (event != org.xmlpull.v1.XmlPullParser.START_TAG) continue;
            if ("manifest".equals(tag)) {
                info.packageName = p.getAttributeValue(null, "package");
                if (!GooglePackages.GMS.equals(info.packageName)) throw new IllegalArgumentException("Not GmsCore: " + info.packageName);
                info.versionCode = p.getAttributeIntValue(NS, "versionCode", 0);
                info.versionName = p.getAttributeValue(NS, "versionName");
            } else if ("uses-sdk".equals(tag)) {
                a.minSdkVersion = p.getAttributeIntValue(NS, "minSdkVersion", 19);
                a.targetSdkVersion = p.getAttributeIntValue(NS, "targetSdkVersion", a.minSdkVersion);
            } else if ("application".equals(tag)) {
                inApp = true; a.className = full(p.getAttributeValue(NS, "name"));
                a.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
            } else if (inApp && "service".equals(tag)) {
                current = new Entry(); ServiceInfo s = current.info;
                s.name = full(p.getAttributeValue(NS, "name")); s.packageName = info.packageName;
                s.applicationInfo = a; s.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                s.exported = p.getAttributeBooleanValue(NS, "exported", true);
                s.permission = p.getAttributeValue(NS, "permission"); s.processName = a.processName;
                s.metaData = new Bundle(); services.add(current);
            } else if (inApp && "provider".equals(tag)) {
                ProviderInfo provider = new ProviderInfo();
                provider.name = full(p.getAttributeValue(NS, "name")); provider.packageName = info.packageName;
                provider.applicationInfo = a; provider.authority = p.getAttributeValue(NS, "authorities");
                provider.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                provider.exported = p.getAttributeBooleanValue(NS, "exported", false);
                provider.readPermission = p.getAttributeValue(NS, "readPermission");
                provider.writePermission = p.getAttributeValue(NS, "writePermission");
                provider.processName = a.processName;
                if (provider.enabled) providers.add(provider);
            } else if (inApp && "receiver".equals(tag)) {
                receiver = new Receiver(); ActivityInfo r = receiver.info;
                r.name = full(p.getAttributeValue(NS, "name")); r.packageName = info.packageName;
                r.applicationInfo = a; r.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                r.exported = p.getAttributeBooleanValue(NS, "exported", false);
                r.permission = p.getAttributeValue(NS, "permission"); r.processName = a.processName;
                receivers.add(receiver);
            } else if (current != null && "meta-data".equals(tag)) {
                /* A service's meta-data, as PackageManager's GET_META_DATA gives it: an account authenticator's description, by resource. */
                String key = p.getAttributeValue(NS, "name");
                int res = p.getAttributeResourceValue(NS, "resource", 0);
                if (key != null && res != 0) current.info.metaData.putInt(key, res);
                else if (key != null && p.getAttributeValue(NS, "value") != null) current.info.metaData.putString(key, p.getAttributeValue(NS, "value"));
            } else if ((current != null || receiver != null) && "intent-filter".equals(tag)) {
                filter = new IntentFilter(); (current != null ? current.filters : receiver.filters).add(filter);
                filter.setPriority(p.getAttributeIntValue(NS, "priority", 0));
            } else if (filter != null && "action".equals(tag)) filter.addAction(p.getAttributeValue(NS, "name"));
            else if (filter != null && "category".equals(tag)) filter.addCategory(p.getAttributeValue(NS, "name"));
        }
        info.services = new ServiceInfo[services.size()];
        for (int n = 0; n < services.size(); n++) info.services[n] = services.get(n).info;
        info.providers = providers.toArray(new ProviderInfo[0]);
        info.receivers = new ActivityInfo[receivers.size()];
        for (int n = 0; n < receivers.size(); n++) info.receivers[n] = receivers.get(n).info;
        // The existing Google compatibility identity is also used for the separately installed replacement.
        info.signatures = new Signature[] {GooglePackages.signature()};
        info.signingInfo = new SigningInfo(info.signatures);
        activities = Manifest.activitiesIn(manifest, APK, GooglePackages.GMS);
        // An activity of theirs that names no theme, of an application that names none (Google's own names none), has the
        // platform's default for their target, as Android gives it (Theme.DeviceDefault.Light.DarkActionBar). Activity's own
        // default is the app's theme, a style of the app's package, under whose number theirs has something else or nothing.
        for (Manifest.Component c : activities) if (c.theme == 0) c.theme = IR.style.Theme_DeviceDefault_Light_DarkActionBar;
        /*
         * Google's own Play services run in this process, and their application is told so: its process name is the one
         * Application.getProcessName gives their code here, this process's. They take the process whose name is their
         * application's for their main one, and only there is their module configuration made again at once when something
         * changed (GmsModuleFinder's ConfigUpdateIntentOperation, bjej.i); anywhere else a scan that finds a change asks for
         * another scan (MODULE_SCAN) and leaves it to that one. With "com.google.android.gms" there and the app's name here,
         * no scan was ever the main process's: each asked for the next, five scans a second without end, and every app that
         * hosts them made 10 MB of garbage a second on that thread (Aliucord, Facebook, VTB, Ozon, Play Store: a collection
         * every three seconds, "Beginning GMS chimera module scan (logged 1000 times)"). Their components keep the process
         * name of their manifest. microG's are left as they were.
         */
        if (!microg()) {
            a.processName = Native.packageName();
            android.util.Log.i("MicroG", "Google Play services' application runs in this process, " + a.processName + ", as its main one");
        }
        context = new PackageContext(resources);
        android.util.Log.i("MicroG", "GmsCore " + info.versionName + ": " + services.size() + " services, " + receivers.size() + " receivers, " + providers.size() + " providers");
    }
    /*
     * The service an intent names, or the one whose filter takes its action with the highest priority (the first of those
     * in the manifest), as PackageManager.resolveService chooses. Google Play services 26 declares an action of an API in a
     * service of its own and again in a Chimera proxy with priority -500 (GmsApiService): the proxy came first in the
     * manifest for some (Google Pay's), and took them.
     */
    /*
     * The services by the actions of their filters, in the manifest's order, and by their class names: made at the first
     * lookup (the manifest's lists do not change after it is read). Each lookup went over the 395 services and their filters,
     * an iterator each, and MicroG.query's were 2-3% of what Yandex Maps made (package 536).
     */
    private HashMap<String, ArrayList<Entry>> byAction;
    private HashMap<String, Entry> byName;
    private synchronized void index() {
        if (byAction != null) return;
        HashMap<String, ArrayList<Entry>> actions = new HashMap<>();
        HashMap<String, Entry> names = new HashMap<>();
        for (int n = 0; n < services.size(); n++) {
            Entry e = services.get(n);
            if (!names.containsKey(e.info.name)) names.put(e.info.name, e);
            for (int k = 0; k < e.filters.size(); k++) {
                IntentFilter f = e.filters.get(k);
                for (int a = 0; a < f.countActions(); a++) {
                    ArrayList<Entry> l = actions.get(f.getAction(a));
                    if (l == null) actions.put(f.getAction(a), l = new ArrayList<>());
                    if (l.isEmpty() || l.get(l.size() - 1) != e) l.add(e);
                }
            }
        }
        byName = names;
        byAction = actions;
    }
    /* The services whose filters have the intent's action (none for none), in the manifest's order. */
    private List<Entry> withAction(Intent i) {
        index();
        List<Entry> l = i.getAction() == null ? null : byAction.get(i.getAction());
        return l != null ? l : Collections.<Entry>emptyList();
    }
    private Entry named(String name) {
        index();
        return byName.get(name);
    }
    private Entry find(Intent i, boolean internal) {
        if (i == null || !info.applicationInfo.enabled) return null;
        String pkg = i.getComponent() == null ? i.getPackage() : i.getComponent().getPackageName();
        if (pkg != null && !info.packageName.equals(pkg)) return null;
        if (i.getComponent() != null) {
            Entry e = named(i.getComponent().getClassName());
            return e != null && e.info.enabled && (internal || e.info.exported) ? e : null;
        }
        Entry best = null;
        int bestPriority = Integer.MIN_VALUE;
        List<Entry> candidates = withAction(i);
        for (int n = 0; n < candidates.size(); n++) {
            Entry e = candidates.get(n);
            if (!e.info.enabled || (!internal && !e.info.exported)) continue;
            for (int k = 0; k < e.filters.size(); k++) {
                IntentFilter f = e.filters.get(k);
                if (f.hasAction(i.getAction()) && f.matchCategories(i.getCategories()) == null && f.getPriority() > bestPriority) {
                    best = e;
                    bestPriority = f.getPriority();
                }
            }
        }
        return best;
    }
    public static ServiceInfo resolve(Intent i) {
        if (!mayBeGms(i)) return null;
        MicroG g = get(); Entry e = g == null ? null : g.find(i, false); return e == null ? null : e.info;
    }
    /* Whether an intent may be for GmsCore: it names no package, or GmsCore's. One for another package's component (the app's
       own services, every one of which PackageManager and Context ask of here first) is not worth reading GmsCore for. */
    private static boolean mayBeGms(Intent i) {
        if (i == null) return false;
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        return pkg == null || GooglePackages.GMS.equals(pkg);
    }
    /**
     * What PackageManager.queryIntentServices lists of GmsCore's: as on Android, exported or not (exported matters to a
     * binding), and a disabled one only with MATCH_DISABLED_COMPONENTS. GmsCore tells an unlocked user so: it asks for its own
     * DirectBootHelperService, disabled and unexported, which Android lists with that flag once the user is unlocked; an empty
     * answer kept every API that is not direct boot aware from the client (People, Checkin). And its task scheduler looks for a
     * task's service among all those of its TASK_READY filter, none of them exported. The exported come first, so that
     * resolveService gives a client what it can bind, as before.
     */
    public static List<ServiceInfo> query(Intent i, int flags) {
        ArrayList<ServiceInfo> out = new ArrayList<>(), hidden = new ArrayList<>();
        if (!mayBeGms(i)) return out;
        MicroG g = get();
        if (g == null || i == null || !g.info.applicationInfo.enabled) return out;
        boolean disabled = (flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
        ComponentName c = i.getComponent();
        String pkg = c == null ? i.getPackage() : c.getPackageName();
        if (pkg != null && !g.info.packageName.equals(pkg)) return out;
        if (c != null) {
            Entry e = g.named(c.getClassName());
            if (e != null && (e.info.enabled || disabled)) out.add(e.info);
            return out;
        }
        List<Entry> candidates = g.withAction(i);
        for (int n = 0; n < candidates.size(); n++) {
            Entry e = candidates.get(n);
            if (!e.info.enabled && !disabled) continue;
            for (int k = 0; k < e.filters.size(); k++) {
                IntentFilter f = e.filters.get(k);
                if (f.hasAction(i.getAction()) && f.matchCategories(i.getCategories()) == null) { (e.info.exported ? out : hidden).add(e.info); break; }
            }
        }
        out.addAll(hidden);
        return out;
    }
    /** microG's enabled services whose filters take the action, exported or not: what the system finds by it (an account authenticator). */
    public static java.util.List<ServiceInfo> services(String action) {
        ArrayList<ServiceInfo> out = new ArrayList<>();
        MicroG g = get();
        if (g == null || action == null) return out;
        for (Entry e : g.services) {
            if (!e.info.enabled) continue;
            for (IntentFilter f : e.filters) if (f.hasAction(action)) { out.add(e.info); break; }
        }
        return out;
    }
    /**
     * A binding the system makes, as Android's account manager binds an authenticator: to a service of microG's whether it is
     * exported or not (microG's GoogleLoginService is not).
     */
    public static boolean bindAsSystem(Intent intent, ServiceConnection conn) {
        MicroG g = get();
        return g != null && g.find(intent, true) != null && bind(g.context, intent, conn);
    }
    /*
     * The services the system keeps bound (bindAsSystemNow), by service and binding key: each counted as one client of its
     * service once, so that it is never destroyed while the process lives, as the system's binding of it never ends.
     */
    private final Set<String> systemBindings = new HashSet<>();
    /**
     * A binding the system makes and keeps, as Android's location manager keeps Google Play services' geocode provider bound
     * (its GeocoderProxy): the binder of their service whose filter takes the intent, exported or not, made and bound on the
     * main thread -- at once when this is the main thread, else waited for, `timeoutMs` at most -- and the same binder again
     * after. Null where Google Play services are not here, none of their services takes it, or it failed. Nothing of theirs
     * is read for an intent of another package.
     */
    public static IBinder bindAsSystemNow(Intent intent, long timeoutMs) {
        if (!mayBeGms(intent)) return null;
        final MicroG g = get();
        final Entry e = g == null ? null : g.find(intent, true);
        if (e == null) return null;
        final Intent copy = new Intent(intent);
        java.util.concurrent.Callable<IBinder> bind = () -> {
            Running r = g.create(e);
            String key = bindKey(copy);
            if (!r.binders.containsKey(key)) {
                r.binders.put(key, r.service.onBind(copy));
                android.util.Log.i("MicroG", "bound " + e.info.name + " for " + key + ", as the system binds it");
            }
            if (g.systemBindings.add(e.info.name + "\n" + key)) r.clients++;
            return r.binders.get(key);
        };
        try {
            if (Looper.myLooper() == Looper.getMainLooper()) return bind.call();
            java.util.concurrent.FutureTask<IBinder> task = new java.util.concurrent.FutureTask<>(bind);
            onMain(task);
            return task.get(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS);
        } catch (java.util.concurrent.TimeoutException t) {
            android.util.Log.w("MicroG", "binding " + e.info.name + " as the system binds it: the main thread did not get to it in " + timeoutMs + " ms");
            return null;
        } catch (Throwable t) {
            Throwable cause = t instanceof java.util.concurrent.ExecutionException && t.getCause() != null ? t.getCause() : t;
            if (!(cause instanceof NotRestartedYet)) android.util.Log.e("MicroG", "Binding " + e.info.name + " as the system binds it failed", cause);
            return null;
        }
    }
    /**
     * Whether Google Play services here have an enabled service for the action (Geocoder.isPresent: a geocode provider), as
     * the system finds one. Until they are read for something else they are not read for this: their APK there and their
     * package enabled, Google's own and microG's both declaring a geocode provider (GmsCore's GeocodeService, microG's
     * GeocodeProviderService).
     */
    public static boolean geocoderPresent(String action) {
        if (!installed()) return false;
        MicroG g = loaded();
        if (g != null) return g.find(new Intent(action), true) != null;
        PackageInfo basic = basic();
        return basic != null && basic.applicationInfo != null && basic.applicationInfo.enabled;
    }
    /* One loader, one class scope of GmsCore's, whichever thread asks first: Play Store asked for GmsCore's flags provider on
       a thread of its own while its main thread started GmsCore, each made a loader, and GmsCore's classes were there twice,
       their statics apart -- the flags provider's PhenotypeFlag was never set up (Must call PhenotypeFlag.init() first). */
    /** GmsCore's class loader, made if it was not yet; null where there is no GmsCore (for KeyStoreProvider's old file). */
    static ClassLoader gmsLoader() {
        MicroG g = get();
        try { return g != null ? g.loader() : null; } catch (RuntimeException e) { return null; }
    }
    /**
     * Whether `l` is GmsCore's class loader or one under it (a module of its own), GmsCore as read and loaded already: never
     * reads or loads it for this. megler.LocationService tells Google Play services' own location requests from the app's so;
     * a scope of the app's own other processes is a ServiceClassLoader too, and is not theirs.
     */
    static boolean isGmsLoader(ClassLoader l) {
        MicroG g = loaded();
        ClassLoader gms = g == null ? null : g.loader;
        if (gms == null) return false;
        for (ClassLoader c = l; c != null; c = c.getParent())
            if (c == gms) return true;
        return false;
    }
    private synchronized ClassLoader loader() {
        if (loader == null) try {
            loader = new ServiceClassLoader(APK);
        } catch (Exception e) { throw new IllegalStateException("Loading GmsCore", e); }
        return loader;
    }
    /*
     * A service whose making failed is not made again for a while (RESTART_DELAY_MS), as Android's activity manager waits
     * before it starts a crashed service again: Google Play services 26's location service failed as it was made (a class
     * of Android's that was not here), its clients asked for it again at once, and it was made and failed 2500 times in 43
     * seconds while Play Store started. Meanwhile a start or a binding of it fails at once, said once.
     */
    private static final long RESTART_DELAY_MS = 10000;
    private final HashMap<String, Long> failedAt = new HashMap<>();
    private final Set<String> boundActions = Collections.synchronizedSet(new HashSet<>());
    private static final class NotRestartedYet extends IllegalStateException {
        NotRestartedYet(String name) { super(name + " failed as it was made, and is not made again yet"); }
    }
    /* (running is changed on the main thread and read by stop and stopSelf on any thread: they lock it.) */
    private Running create(Entry e) throws Exception {
        Running r;
        synchronized (running) { r = running.get(e.info.name); }
        if (r != null) return r;
        synchronized (failedAt) {
            Long at = failedAt.get(e.info.name);
            if (at != null && android.os.SystemClock.uptimeMillis() - at < RESTART_DELAY_MS) throw new NotRestartedYet(e.info.name);
        }
        application();
        try {
            r = new Running(); r.service = (Service) loader().loadClass(e.info.name).newInstance();
            r.service.meglerAttach(context); r.service.onCreate();
        } catch (Exception | Error t) {
            synchronized (failedAt) { failedAt.put(e.info.name, android.os.SystemClock.uptimeMillis()); }
            throw t;
        }
        synchronized (running) { running.put(e.info.name, r); }
        return r;
    }
    /*
     * An uncaught exception of a thread of GmsCore's here (Native.thrownInService) ends that thread alone, as it ends GmsCore's
     * own process on Android, which Android starts again while the app that used it goes on. GmsCore's crash handler, made the
     * process's default by its Application, passes what it does not handle on to the app's, and the app's ended the app: GmsCore's
     * metrics collection and its Constellation module crashed Play Store during its sign-in. The main thread's are the app's.
     */
    private static final class ServiceCrashes implements Thread.UncaughtExceptionHandler {
        private final Thread.UncaughtExceptionHandler under;
        ServiceCrashes(Thread.UncaughtExceptionHandler under) { this.under = under; }
        @Override public void uncaughtException(Thread t, Throwable e) {
            if (t != Looper.getMainLooper().getThread() && Native.thrownInService(e) != 0) {
                android.util.Log.e("Megler", "Google Play services' thread " + t.getName() + " ended with an uncaught exception; on Android its process would, and the app goes on", e);
                return;
            }
            if (under != null) under.uncaughtException(t, e);
        }
    }
    /**
     * microG's Application, made and attached once (attachBaseContext), as Android makes and attaches an app's before it starts
     * its providers; created (application) on the first thing of microG's that runs here. GmsCore's attachBaseContext sets up
     * its flags (PhenotypeFlag), which its providers read. One asked for while it is being attached (by what the attaching runs)
     * is that one.
     */
    private synchronized Application attached() throws Exception {
        if (attached != null) return attached;
        if (attaching != null) return attaching;
        Application a = info.applicationInfo.className == null ? new Application() :
            (Application) loader().loadClass(info.applicationInfo.className).newInstance();
        attaching = a;
        try { a.meglerAttach(context); attached = a; }
        finally {
            attaching = null;
            synchronized (attachEnded) { attachEnded.notifyAll(); }
        }
        return a;
    }
    /* What attachOnMain's waiters wait on: notified as an attaching ends, whichever thread and whatever made it. */
    private final Object attachEnded = new Object();
    /*
     * GmsCore's Application attached before its first provider starts, on the main thread, as Android does it: Play Store asks
     * GmsCore's flags provider (Phenotype) at its start, on a thread of its own, before anything else of GmsCore's runs, and
     * without it every answer was an IllegalStateException. That thread waits for the main thread, though not for long: one the
     * main thread waits on itself would not get it otherwise, and the provider then answers as before.
     *
     * It waits until GmsCore's Application is attached, by its own request or by anything else the main thread runs -- not
     * for its own request alone. Smash Hit's GoogleApiHandler asked for GmsCore's Chimera provider (the dynamic lookup of
     * the client library's service broker, with its GmsClientSupervisor's lock held) while the main thread was attaching
     * the Application for a binding of Google Play services' Games service; the main thread then delivered that binding's
     * onServiceConnected, which waits for the same lock, and the request behind it on the main thread's queue never ran: the
     * game stood still for the ten seconds of the wait at every start (0.1.0.681). On Android the provider is GmsCore's
     * process's, published once its Application is attached, and answers on a binder thread whatever the app's main thread
     * does.
     */
    private void attachOnMain() {
        if (attached != null) return;
        if (Looper.myLooper() == Looper.getMainLooper()) {
            try { attached(); } catch (Throwable t) { android.util.Log.e("MicroG", "Attaching GmsCore's Application failed", t); }
            return;
        }
        final boolean[] done = new boolean[1];
        onMain(() -> {
            try { attached(); } catch (Throwable t) { android.util.Log.e("MicroG", "Attaching GmsCore's Application failed", t); }
            finally { synchronized (attachEnded) { done[0] = true; attachEnded.notifyAll(); } }
        });
        long until = SystemClock.uptimeMillis() + 10000, left;
        synchronized (attachEnded) {
            while (attached == null && !done[0] && (left = until - SystemClock.uptimeMillis()) > 0) {
                try { attachEnded.wait(left); } catch (InterruptedException e) { Thread.currentThread().interrupt(); break; }
            }
        }
    }
    /** microG's Application, made and created once, on the first thing of microG's that runs here. */
    private Application application() throws Exception {
        if (application == null) {
            Application a = attached();
            a.onCreate(); application = a;
            Thread.setDefaultUncaughtExceptionHandler(new ServiceCrashes(Thread.getDefaultUncaughtExceptionHandler()));
            boot();
        }
        return application;
    }

    /**
     * A broadcast for microG: one that names a receiver of microG's, or microG's package (CheckinPreferences.setEnabled sends one
     * to its TriggerReceiver; an alarm's PendingIntent may hold one). Its receivers that take it hear it on the main thread, as
     * on Android, with microG's Application for their context. An intent from elsewhere reaches exported receivers only. False
     * when it is not for microG: the app's own broadcasts have it then.
     */
    public static boolean send(Context from, Intent intent) {
        if (intent == null) return false;
        String pkg = intent.getComponent() == null ? intent.getPackage() : intent.getComponent().getPackageName();
        if (!GooglePackages.GMS.equals(pkg)) return false;
        MicroG g = get();
        if (g == null) return false;
        g.deliver(intent, from != null && GooglePackages.GMS.equals(from.getPackageName()));
        return true;
    }
    /**
     * A broadcast of Android's system (megler.Accounts: the accounts changed, one was removed) to GmsCore's receivers, as the
     * system sends it to Google Play services: exported or not, whatever names the intent.
     */
    public static boolean sendAsSystem(Intent intent) {
        MicroG g = get();
        if (g == null || intent == null) return false;
        g.deliver(new Intent(intent).setPackage(GooglePackages.GMS), true);
        return true;
    }
    /*
     * The implicit broadcasts Google's system configuration lets reach the receivers of an app's manifest on Android 8 and later
     * (google.xml's allow-implicit-broadcast, which BiTGApps installs). GmsCore's checkin sends CHECKIN_COMPLETE to no package,
     * and its own receiver of it starts its first sync of its server flags (Phenotype): without it every flag of GmsCore's kept
     * its default, the DroidGuard of its sign-in among them, and Google's server refused the account ("MissingDroidguard").
     */
    private static final Set<String> IMPLICIT = new HashSet<>(Arrays.asList(
        "com.google.android.checkin.CHECKIN_COMPLETE", "com.google.gservices.intent.action.GSERVICES_CHANGED",
        "com.google.gservices.intent.action.GSERVICES_OVERRIDE", "com.google.android.c2dm.intent.RECEIVE",
        "com.google.android.onetimeinitializer.ONE_TIME_INITIALIZED"));
    /** One of those broadcasts, naming no package nor component, to microG's receivers too: from microG, to all that take it; from the app, to the exported ones. */
    public static void sendImplicit(Context from, Intent intent) {
        if (intent == null || intent.getComponent() != null || intent.getPackage() != null || !IMPLICIT.contains(intent.getAction())) return;
        MicroG g = get();
        if (g != null) g.deliver(intent, from != null && GooglePackages.GMS.equals(from.getPackageName()));
    }
    private void deliver(Intent intent, boolean internal) {
        for (Receiver r : receivers) {
            if (!r.info.enabled || (!internal && !r.info.exported)) continue;
            boolean takes = false;
            if (intent.getComponent() != null) takes = r.info.name.equals(intent.getComponent().getClassName());
            else for (IntentFilter f : r.filters) if (f.hasAction(intent.getAction()) && f.matchCategories(intent.getCategories()) == null) takes = true;
            if (!takes) continue;
            final Intent copy = new Intent(intent);
            new Handler(Looper.getMainLooper()).post(() -> {
                try {
                    BroadcastReceiver b = (BroadcastReceiver) loader().loadClass(r.info.name).newInstance();
                    b.onReceive(application(), copy);
                } catch (Throwable t) { android.util.Log.e("MicroG", "Receiver " + r.info.name + " failed on " + copy.getAction(), t); }
            });
        }
    }
    /*
     * microG's device registration (its check-in) on, unless the user has set it in microG's settings: Play Store has no
     * Android ID without a check-in, and every request of its front page was refused ("Server error", package 570); microG keeps
     * it off until it is turned on, which no one who starts Play Store through Megler would know to do. Set before the boot's
     * BOOT_COMPLETED, which the check-in trigger reads it for, in the preferences microG reads (its default ones, in the
     * services' shared folder: every app's microG sees it). Google's own Play services registers the device itself.
     */
    private void registrationOn() {
        if (!microg()) return;
        SharedPreferences p = context.getSharedPreferences(GooglePackages.GMS + "_preferences", 0);
        if (p.contains("checkin_enable_service")) return;
        p.edit().putBoolean("checkin_enable_service", true).commit();
        android.util.Log.i("MicroG", "device registration turned on (checkin_enable_service), as Play Store needs it; microG's settings turn it off");
    }
    /**
     * Android's boot as microG would have heard it: BOOT_COMPLETED to its receivers that list it, once in each process that
     * hosts it, when microG's code first runs there (its Application is made: an app that only asks whether microG is there
     * does not load it). Each looks at its own setting: the check-in's (TriggerReceiver) checks in when device registration is on and
     * the last check-in is over twelve hours old; the others (cloud messaging, exposure notifications) do nothing while they
     * are off, as they are until the user turns them on.
     */
    private void boot() {
        registrationOn();
        for (Receiver r : receivers) {
            if (!r.info.enabled) continue;
            for (IntentFilter f : r.filters) if (f.hasAction(Intent.ACTION_BOOT_COMPLETED)) {
                deliver(new Intent(Intent.ACTION_BOOT_COMPLETED).setComponent(new ComponentName(GooglePackages.GMS, r.info.name)), true);
                break;
            }
            // Google's GmsCore starts its device registration from CHECKIN, not BOOT_COMPLETED.
            // Send the same trigger Android's checkin scheduler sends after boot. The receiver
            // decides whether a checkin is due and obtains the ID from Google's server.
            if ("com.google.android.gms.checkin.CheckinServiceTriggerReceiver".equals(r.info.name)) {
                for (IntentFilter f : r.filters) if (f.hasAction("android.server.checkin.CHECKIN")) {
                    deliver(new Intent("android.server.checkin.CHECKIN")
                            .setComponent(new ComponentName(GooglePackages.GMS, r.info.name)), true);
                    break;
                }
            }
        }
    }

    public static boolean bind(Context from, Intent intent, ServiceConnection conn) {
        if (!mayBeGms(intent)) return false;
        // (The app's own binding of Google Play services' location API settles Google Location Accuracy: megler.LocationAccuracy.)
        LocationAccuracy.bound(from, intent.getAction());
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) {
            // a binding meant for Google Play services that none of its services takes: said, as the caller often says only that
            // it could not bind (its authenticator: "Unable to bind to auth delegate service")
            ComponentName c = intent == null ? null : intent.getComponent();
            if (g != null && c != null && (GooglePackages.GMS.equals(c.getPackageName()) || c.getClassName().startsWith("com.google.android.gms.")))
                android.util.Log.w("MicroG", "no service of Google Play services binds " + c.flattenToShortString() + " for " + from.getPackageName()
                    + (g.info.applicationInfo.enabled ? "" : ": the package is disabled"));
            return false;
        }
        Intent copy = new Intent(intent);
        Binding pending = new Binding(null, copy);
        // which service an action is bound to, once each: what a client's "Unexpected service id" (a broker that does not
        // serve the API asked of it) is to be read against
        if (intent.getComponent() == null && intent.getAction() != null && g.boundActions.add(intent.getAction()))
            android.util.Log.i("MicroG", "binding " + intent.getAction() + ": " + e.info.name);
        synchronized (g.bindings) {
            if (g.bindings.containsKey(conn)) return true;
            g.bindings.put(conn, pending);
        }
        new Handler(Looper.getMainLooper()).post(() -> {
            synchronized (g.bindings) { if (g.bindings.get(conn) != pending) return; }
            ComponentName name = new ComponentName(GooglePackages.GMS, e.info.name);
            try {
                Running r = g.create(e);
                String key = bindKey(copy);
                if (!r.binders.containsKey(key)) {
                    r.binders.put(key, r.service.onBind(copy));
                    android.util.Log.i("MicroG", "bound " + e.info.name + " for " + key);
                }
                IBinder binder = r.binders.get(key);
                synchronized (g.bindings) {
                    if (g.bindings.get(conn) != pending) { g.destroyIfUnused(r); return; }
                    r.clients++; pending.running = r;
                }
                if (binder == null) conn.onNullBinding(name); else conn.onServiceConnected(name, binder);
            } catch (Throwable t) {
                if (!(t instanceof NotRestartedYet)) android.util.Log.e("MicroG", "Binding " + e.info.name + " failed", t);
                synchronized (g.bindings) { if (g.bindings.get(conn) != pending) return; }
                conn.onNullBinding(name);
            }
        });
        return true;
    }
    /*
     * Which bindings of a service share one binder: those whose intents are the same as Android's Intent.filterEquals has it
     * (its action, data, type, identifier and categories; not its extras), for each of which Android calls onBind once. They
     * were told apart by the action alone. GmsCore's container services (GmsApiService, the bound brokers) serve every API of
     * the modules they hold, one broker each, and the intent a client is given for an API (the chimera provider's dynamic
     * lookup) is told from another's by more than its action: a broker made for one API was handed to clients of others, and
     * refused them ("Unexpected service id: 343" to GoogleAuth.API, 40, 41, 270...) -- Google Play services' own sign-in did
     * not connect, and WhatsApp's search for its Google Drive backup failed until it took too long.
     */
    public static String bindKey(Intent i) {
        StringBuilder k = new StringBuilder();
        k.append(i.getAction() == null ? "" : i.getAction());
        if (i.getData() != null) k.append(" data=").append(i.getData());
        if (i.getType() != null) k.append(" type=").append(i.getType());
        String id = identifierOf(i);
        if (id != null) k.append(" id=").append(id);
        if (i.getCategories() != null && !i.getCategories().isEmpty()) {
            java.util.TreeSet<String> c = new java.util.TreeSet<>(i.getCategories());
            k.append(" categories=").append(c);
        }
        return k.toString();
    }
    private static String identifierOf(Intent i) {
        try { return (String) Intent.class.getMethod("getIdentifier").invoke(i); } catch (Throwable t) { return null; }
    }
    public static boolean unbind(ServiceConnection conn) {
        // (Nothing is bound to GmsCore before it was read: Android System WebView's unbinding of its own service had Aurora
        // Store read it whole, 4.5 s on a Lumia 650.)
        MicroG g = loaded(); if (g == null) return false;
        Binding b;
        synchronized (g.bindings) { b = g.bindings.remove(conn); }
        if (b == null) return false;
        new Handler(Looper.getMainLooper()).post(() -> {
            if (b.running == null) return;
            if (--b.running.clients == 0) {
                b.running.service.onUnbind(b.intent); b.running.binders.clear();
                g.destroyIfUnused(b.running);
            }
        });
        return true;
    }
    private void destroyIfUnused(Running r) {
        if (!r.started && r.clients == 0) {
            synchronized (running) { running.values().remove(r); }
            r.service.onDestroy();
        }
    }
    /*
     * The starts of GmsCore's services by what they were started for, over ten seconds: one asked for a hundred times is said
     * (three times at most for each), with what asked -- GmsCore's code asking itself for the same work without end, as its
     * module scans did (MODULE_SCAN, five a second, in every app that hosts it), is otherwise seen only as garbage and time.
     */
    private static final HashMap<String, long[]> sStarts = new HashMap<>();
    private static void countStart(Intent intent, String service) {
        String key = (intent.getAction() != null ? intent.getAction() : "(no action)") + " -> " + service;
        long now = SystemClock.uptimeMillis();
        long[] c;
        boolean say;
        synchronized (sStarts) {
            c = sStarts.get(key);
            if (c == null) sStarts.put(key, c = new long[] { now, 0, 0 });
            if (now - c[0] > 10000) { c[0] = now; c[1] = 0; }
            say = ++c[1] == 100 && c[2]++ < 3;
        }
        if (say) android.util.Log.w("MicroG", "Google Play services asked for " + key + " 100 times in " + (now - c[0]) + " ms: the same work again and again",
                                    new Throwable("asked by"));
    }
    public static ComponentName start(Context from, Intent intent) {
        if (!mayBeGms(intent)) return null;
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) return null;
        countStart(intent, e.info.name);
        Intent copy = new Intent(intent);
        Services.postStart(g.startIds, e.info.name, id -> {
            try { Running r = g.create(e); r.started = true; r.starts++; r.service.onStartCommand(copy, 0, id); }
            catch (Throwable t) { if (!(t instanceof NotRestartedYet)) android.util.Log.e("MicroG", "Starting " + e.info.name + " failed", t); }
        });
        return new ComponentName(GooglePackages.GMS, e.info.name);
    }
    /* Android stops a service on its process's main thread, whichever thread asked, and later, never inside the call that asked
       (a service may stop itself in onStartCommand and go on with it): GmsCore's IndexWorkerChimeraService checks the thread in
       onDestroy, and Play Store's background stop of it ended Play Store. */
    static void onMain(Runnable r) { new Handler(Looper.getMainLooper()).post(r); }
    public static boolean stop(Intent intent) {
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, true);
        if (e == null) return false;
        synchronized (g.running) { if (g.running.get(e.info.name) == null) return false; }
        onMain(() -> {
            Running r;
            synchronized (g.running) { r = g.running.get(e.info.name); }
            if (r != null) { r.started = false; g.destroyIfUnused(r); }
        });
        return true;
    }
    /** Service.stopSelfResult for GmsCore's services: 1 when it stops, 0 when a later start keeps it, -1 when it is none of theirs running here. */
    public static int stopSelf(Service service, int id) {
        MicroG g = get(); if (g == null) return -1;
        String name = null; Running found = null;
        synchronized (g.running) { for (Map.Entry<String, Running> x : g.running.entrySet()) if (x.getValue().service == service) { name = x.getKey(); found = x.getValue(); } }
        if (found == null) return -1;
        final Running r = found;
        return Services.postStop(g.startIds, name, id, () -> {
            boolean still;
            synchronized (g.running) { still = g.running.containsValue(r); }
            if (still) { r.started = false; g.destroyIfUnused(r); }
        }) ? 1 : 0;
    }
    /** The attribution tag of Google Play services' package context where none was asked for, given only where the user lets
     *  their own features have the location (PackageContext.getAttributionTag). */
    static final String DEFAULT_ATTRIBUTION_TAG = "megler.app";
    /* The identity of a location request of Google Play services' own, which their location service checks (gerz.h): made of
       the context's tag by ClientIdentity.c(Context). The name is kept in their code; the method's is not. */
    private static final String IDENTITY = "com.google.android.gms.libs.identity.ClientIdentity";
    /** Whether the tag is asked for on this thread to make such an identity: looked at only until the user has been asked. */
    private static boolean forIdentity() {
        if (Permissions.gmsAsked()) return false;
        for (StackTraceElement e : new Throwable().getStackTrace()) if (IDENTITY.equals(e.getClassName())) return true;
        return false;
    }
    /* A package context's attribution context: that context in everything but its tag. */
    private static final class Attributed extends ContextWrapper {
        private final String tag;
        Attributed(Context base, String tag) { super(base); this.tag = tag; }
        @Override public String getAttributionTag() { return tag; }
        @Override public Context createAttributionContext(String other) { return getBaseContext().createAttributionContext(other); }
    }
    private final class PackageContext extends ContextWrapper {
        private final Resources resources;
        private final boolean deviceProtected;
        private final HashMap<String, SharedPreferences> prefs;
        PackageContext(Resources r) { this(r, false); }
        PackageContext(Resources r, boolean deviceProtected) { this(r, deviceProtected, new HashMap<>()); }
        /* (A configuration context keeps its preferences' objects: one file, one object, whichever context asks.) */
        private PackageContext(Resources r, boolean deviceProtected, HashMap<String, SharedPreferences> prefs) {
            super(ContextImpl.app());
            resources = r;
            this.deviceProtected = deviceProtected;
            this.prefs = prefs;
        }
        /*
         * A context of another configuration -- AppCompat's attachBaseContext2 wraps an activity's base in one where the app's
         * night mode or locales differ, Compose and Google Play services' own code make them for a locale -- is this package's
         * resources under that configuration, and this context in all else. It was the app's: ContextWrapper asks its base,
         * the app's ContextImpl, and Google Play services' activities and services hosted here were given the app's resources
         * there, its drawables and strings under their numbers, or none.
         */
        @Override public Context createConfigurationContext(Configuration c) {
            Configuration config = new Configuration(resources.getConfiguration());
            if (c != null) config.updateFrom(c);
            return new PackageContext(new Resources(resources.getAssets(), resources.getDisplayMetrics(), config), deviceProtected, prefs);
        }
        /*
         * A theme of their own, as Android's ContextImpl has: of their resources, the platform's default for their target
         * (Theme.DeviceDefault.Light.DarkActionBar) unless one was set. ContextWrapper asked the app's ContextImpl, whose theme
         * is the app's own, of the app's resources, and every activity of theirs hosted here began from a copy of it
         * (ContextThemeWrapper.initializeTheme copies its base's before it applies the activity's): the app's attributes, under
         * their numbers in the app's package, stood in theirs wherever their own theme set none under those numbers.
         */
        private Resources.Theme theme;
        private int themeId;
        @Override public synchronized void setTheme(int resid) { if (themeId != resid) { themeId = resid; theme = null; } }
        @Override public synchronized int getThemeResId() { return themeId; }
        @Override public synchronized Resources.Theme getTheme() {
            if (theme == null) {
                theme = resources.newTheme();
                theme.applyStyle(themeId != 0 ? themeId : IR.style.Theme_DeviceDefault_Light_DarkActionBar, true);
            }
            return theme;
        }
        @Override public String getPackageName() { return GooglePackages.GMS; }
        @Override public String getOpPackageName() { return getPackageName(); }
        /*
         * Google Play services' own location requests must carry an attribution tag of their own on Android 11 and later
         * (GmsCore's gerz.h: "internal location usage has a forbidden attribution tag", null among the forbidden), and the
         * identity of such a request is made of this context's tag (ClientIdentity.c). On a phone a request made through this
         * context in an app's process is the app's, another uid's; here the app and Google Play services are one, so it is
         * taken for Google Play services' own. Most are: their own features in every app's process -- personal safety,
         * semantic location, place suggestions (20 apps' logs of 0.1.0.681, 1110) -- besides the my-location layer of
         * Google's maps (Transit, 0.1.0.679). A context with no tag of its own answers DEFAULT_ATTRIBUTION_TAG, none of the
         * forbidden, only where the user lets their features have the location (Permissions.gmsLocation: one answer for
         * every app, kept by Megler); null otherwise, and the request is refused, as before. The user is asked the first
         * time such an identity is made with no answer (Permissions.askGmsLocation: once in this process, over the app, on
         * a thread of its own; this one is told null meanwhile). An attribution context keeps its own tag (Attributed).
         */
        @Override public String getAttributionTag() {
            int s = Permissions.gmsLocation();
            if (s == Permissions.ALLOWED) return DEFAULT_ATTRIBUTION_TAG;
            if (s == Permissions.UNDECIDED && forIdentity()) Permissions.askGmsLocation();
            return null;
        }
        @Override public ApplicationInfo getApplicationInfo() { return info.applicationInfo; }
        @Override public String getPackageCodePath() { return APK; }
        @Override public String getPackageResourcePath() { return APK; }
        @Override public ClassLoader getClassLoader() { return loader(); }
        @Override public Context getApplicationContext() { Application a = application != null ? application : attached; return a == null ? this : a; }
        @Override public Resources getResources() { return resources; }
        @Override public AssetManager getAssets() { return resources.getAssets(); }
        @Override public File getDataDir() {
            File data = new File(info.applicationInfo.dataDir);
            if (deviceProtected) data = new File(data, "device_protected");
            data.mkdirs();
            return data;
        }
        private File dir(String name) { File f = new File(getDataDir(), name); f.mkdirs(); return f; }
        @Override public File getDatabasePath(String name) { return name.startsWith("/") ? new File(name) : new File(dir("databases"), name); }
        @Override public boolean isDeviceProtectedStorage() { return deviceProtected; }
        @Override public boolean isCredentialProtectedStorage() { return !deviceProtected; }
        @Override public Context createDeviceProtectedStorageContext() { return deviceProtected ? this : new PackageContext(resources, true); }
        @Override public Context createCredentialProtectedStorageContext() { return deviceProtected ? new PackageContext(resources) : this; }
        @Override public File getFilesDir() { return dir("files"); }
        @Override public File getCacheDir() { return dir("cache"); }
        @Override public File getDir(String name, int mode) { return dir("app_" + name); }
        @Override public SharedPreferences getSharedPreferences(String name, int mode) {
            synchronized (prefs) {
                SharedPreferences p = prefs.get(name);
                if (p == null) { p = new android.content.MeglerSharedPreferences(new File(dir("shared_prefs"), name + ".xml")); prefs.put(name, p); }
                return p;
            }
        }
        @Override public Context createPackageContext(String pkg, int flags) throws PackageManager.NameNotFoundException {
            return getPackageName().equals(pkg) ? this : super.createPackageContext(pkg, flags);
        }
        // An attribution context and a split's are this package's still: Context's own wrapper asks its base, which here is the
        // app's, and Google Play services 26, which makes an attribution context for much of its work, then bound its own
        // services as the app (Play Store's sign-in: "Unable to bind to auth delegate service"). An attribution context is this
        // one with its tag (Attributed), which GmsCore's location service wants of its own requests: without it each was
        // refused, "internal location usage has a forbidden attribution tag \"null\"" (Weather Channel, package 505).
        @Override public Context createAttributionContext(String tag) { return tag == null ? this : new Attributed(this, tag); }
        @Override public Context createContextForSplit(String split) { return this; }
        @Override public boolean bindService(Intent i, ServiceConnection c, int flags) { return Services.bind(this, i, c); }
        @Override public ComponentName startService(Intent i) { return Services.start(this, i); }
        @Override public boolean stopService(Intent i) { return Services.stop(i); }
        @Override public ComponentName startForegroundService(Intent i) { return Services.start(this, i); }
        @Override public void sendBroadcast(Intent i) { Broadcasts.send(this, i); }
        @Override public void sendBroadcast(Intent i, String permission) { Broadcasts.send(this, i); }
        @Override public void sendOrderedBroadcast(Intent i, String permission) { Broadcasts.send(this, i); }
        // Google Play services holds its permissions as a phone's system app does, whatever the app it is in holds: its own
        // checks (the location it uses for itself, the accounts, the text messages its SMS Retriever hears) are its own. The
        // app's context answers for the app (megler.Permissions); this one, a wrapper of it, would have answered for the app too.
        @Override public int checkPermission(String permission, int pid, int uid) { return PackageManager.PERMISSION_GRANTED; }
    }
}
