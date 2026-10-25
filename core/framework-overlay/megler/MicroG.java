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
    private static MicroG instance;
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
    private ClassLoader loader;
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
    private static synchronized MicroG get() {
        if (GooglePackages.GMS.equals(Native.packageName())) return null;
        if (!checked) {
            checked = true;
            if (new File(APK).isFile()) try { instance = new MicroG(); }
            catch (Exception e) { android.util.Log.e("MicroG", "Cannot read " + APK, e); }
        }
        return instance;
    }
    public static boolean installed() { return get() != null; }

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
        MicroG g = get();
        if (g == null || i == null || PhoneApps.isLauncherIntent(i)) return null;
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        if (pkg == null) {
            String action = i.getAction();
            if (action == null || action.startsWith("android.") || Manifest.resolve(i) != null) return null;
        } else if (!GooglePackages.GMS.equals(pkg)) return null;
        Manifest.Component c = Manifest.resolveIn(g.activities, i);
        return c != null && c.enabled ? c : null;
    }
    /** Whether a component is one of GmsCore's activities (activity), which run in its scope here. */
    public static boolean hosts(Manifest.Component c) { MicroG g = get(); return g != null && c != null && g.activities.contains(c); }
    /** An activity of GmsCore's, made in its class scope, its Application made first as for anything of GmsCore's that runs here. */
    public static Activity newActivity(String cls) throws Exception {
        MicroG g = get();
        g.application();
        return (Activity) g.loader().loadClass(cls).newInstance();
    }
    /** The context GmsCore's activities here are based on, and their Application. */
    public static Context activityBase() { MicroG g = get(); return g == null ? null : g.context; }
    public static Application activityApplication() throws Exception { MicroG g = get(); return g == null ? null : g.application(); }
    public static PackageInfo packageInfo(String pkg) { MicroG g = get(); return g != null && GooglePackages.GMS.equals(pkg) ? g.info : null; }
    public static Context context(String pkg) { MicroG g = get(); return g != null && GooglePackages.GMS.equals(pkg) ? g.context : null; }
    public static ContentProvider provider(String authority) {
        MicroG g = get();
        if (g == null || authority == null) return null;
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
    private MicroG() throws Exception {
        AssetManager assets = AssetManager.meglerForPackage(APK, GooglePackages.GMS);
        Resources resources = new Resources(assets);
        byte[] manifest;
        try (java.io.InputStream in = assets.openNonAsset("AndroidManifest.xml"); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
            byte[] buf = new byte[8192]; int n;
            while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
            manifest = out.toByteArray();
        }
        ApplicationInfo a = new ApplicationInfo();
        a.packageName = a.processName = GooglePackages.GMS;
        a.sourceDir = a.publicSourceDir = APK;
        // microG's state is one for all apps, as on Android: the services' shared folder (mg_run.c), which microG's own app has
        // for its private files too. Where there is none, it is this app's own.
        File shared = new File(SHARED);
        a.dataDir = shared.isDirectory() ? new File(shared, GooglePackages.GMS).getPath() : new File(Native.dataDir(), "megler-services/" + GooglePackages.GMS).getPath();
        a.nativeLibraryDir = APK + "!/lib/armeabi-v7a";
        a.enabled = true; a.uid = android.os.Process.myUid();
        a.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        a.metaData = new Bundle(); info.applicationInfo = a;
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
        context = new PackageContext(resources);
        android.util.Log.i("MicroG", "GmsCore " + info.versionName + ": " + services.size() + " services, " + receivers.size() + " receivers, " + providers.size() + " providers");
    }
    private Entry find(Intent i, boolean internal) {
        if (i == null || !info.applicationInfo.enabled) return null;
        String pkg = i.getComponent() == null ? i.getPackage() : i.getComponent().getPackageName();
        if (pkg != null && !info.packageName.equals(pkg)) return null;
        for (Entry e : services) {
            if (!e.info.enabled || (!internal && !e.info.exported)) continue;
            if (i.getComponent() != null) { if (e.info.name.equals(i.getComponent().getClassName())) return e; }
            else for (IntentFilter f : e.filters) {
                if (f.hasAction(i.getAction()) && f.matchCategories(i.getCategories()) == null) return e;
            }
        }
        return null;
    }
    public static ServiceInfo resolve(Intent i) { MicroG g = get(); Entry e = g == null ? null : g.find(i, false); return e == null ? null : e.info; }
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
        MicroG g = get();
        if (g == null || i == null || !g.info.applicationInfo.enabled) return out;
        boolean disabled = (flags & PackageManager.MATCH_DISABLED_COMPONENTS) != 0;
        ComponentName c = i.getComponent();
        String pkg = c == null ? i.getPackage() : c.getPackageName();
        if (pkg != null && !g.info.packageName.equals(pkg)) return out;
        for (Entry e : g.services) {
            if (!e.info.enabled && !disabled) continue;
            if (c != null) {
                if (e.info.name.equals(c.getClassName())) { out.add(e.info); break; }
                continue;
            }
            for (IntentFilter f : e.filters)
                if (f.hasAction(i.getAction()) && f.matchCategories(i.getCategories()) == null) { (e.info.exported ? out : hidden).add(e.info); break; }
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
    /* One loader, one class scope of GmsCore's, whichever thread asks first: Play Store asked for GmsCore's flags provider on
       a thread of its own while its main thread started GmsCore, each made a loader, and GmsCore's classes were there twice,
       their statics apart -- the flags provider's PhenotypeFlag was never set up (Must call PhenotypeFlag.init() first). */
    private synchronized ClassLoader loader() {
        if (loader == null) try {
            loader = new ServiceClassLoader(APK);
        } catch (Exception e) { throw new IllegalStateException("Loading GmsCore", e); }
        return loader;
    }
    /* (running is changed on the main thread and read by stop and stopSelf on any thread: they lock it.) */
    private Running create(Entry e) throws Exception {
        Running r;
        synchronized (running) { r = running.get(e.info.name); }
        if (r != null) return r;
        application();
        r = new Running(); r.service = (Service) loader().loadClass(e.info.name).newInstance();
        r.service.meglerAttach(context); r.service.onCreate();
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
        try { a.meglerAttach(context); attached = a; } finally { attaching = null; }
        return a;
    }
    /*
     * GmsCore's Application attached before its first provider starts, on the main thread, as Android does it: Play Store asks
     * GmsCore's flags provider (Phenotype) at its start, on a thread of its own, before anything else of GmsCore's runs, and
     * without it every answer was an IllegalStateException. That thread waits for the main thread, though not for long: one the
     * main thread waits on itself would not get it otherwise, and the provider then answers as before.
     */
    private void attachOnMain() {
        if (attached != null) return;
        if (Looper.myLooper() == Looper.getMainLooper()) {
            try { attached(); } catch (Throwable t) { android.util.Log.e("MicroG", "Attaching GmsCore's Application failed", t); }
            return;
        }
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        onMain(() -> {
            try { attached(); } catch (Throwable t) { android.util.Log.e("MicroG", "Attaching GmsCore's Application failed", t); }
            finally { done.countDown(); }
        });
        try { done.await(10, java.util.concurrent.TimeUnit.SECONDS); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
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
    /**
     * Android's boot as microG would have heard it: BOOT_COMPLETED to its receivers that list it, once in each process that
     * hosts it, when microG's code first runs there (its Application is made: an app that only asks whether microG is there
     * does not load it). Each looks at its own setting: the check-in's (TriggerReceiver) checks in when device registration is on and
     * the last check-in is over twelve hours old; the others (cloud messaging, exposure notifications) do nothing while they
     * are off, as they are until the user turns them on.
     */
    private void boot() {
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
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) return false;
        Intent copy = new Intent(intent);
        Binding pending = new Binding(null, copy);
        synchronized (g.bindings) {
            if (g.bindings.containsKey(conn)) return true;
            g.bindings.put(conn, pending);
        }
        new Handler(Looper.getMainLooper()).post(() -> {
            synchronized (g.bindings) { if (g.bindings.get(conn) != pending) return; }
            ComponentName name = new ComponentName(GooglePackages.GMS, e.info.name);
            try {
                Running r = g.create(e);
                String key = copy.getAction() == null ? "" : copy.getAction();
                if (!r.binders.containsKey(key)) r.binders.put(key, r.service.onBind(copy));
                IBinder binder = r.binders.get(key);
                synchronized (g.bindings) {
                    if (g.bindings.get(conn) != pending) { g.destroyIfUnused(r); return; }
                    r.clients++; pending.running = r;
                }
                if (binder == null) conn.onNullBinding(name); else conn.onServiceConnected(name, binder);
            } catch (Throwable t) {
                android.util.Log.e("MicroG", "Binding " + e.info.name + " failed", t);
                synchronized (g.bindings) { if (g.bindings.get(conn) != pending) return; }
                conn.onNullBinding(name);
            }
        });
        return true;
    }
    public static boolean unbind(ServiceConnection conn) {
        MicroG g = get(); if (g == null) return false;
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
    public static ComponentName start(Context from, Intent intent) {
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) return null;
        Intent copy = new Intent(intent);
        Services.postStart(g.startIds, e.info.name, id -> {
            try { Running r = g.create(e); r.started = true; r.starts++; r.service.onStartCommand(copy, 0, id); }
            catch (Throwable t) { android.util.Log.e("MicroG", "Starting " + e.info.name + " failed", t); }
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
    private final class PackageContext extends ContextWrapper {
        private final Resources resources;
        private final boolean deviceProtected;
        private final HashMap<String, SharedPreferences> prefs = new HashMap<>();
        PackageContext(Resources r) { this(r, false); }
        PackageContext(Resources r, boolean deviceProtected) {
            super(ContextImpl.app());
            resources = r;
            this.deviceProtected = deviceProtected;
        }
        @Override public String getPackageName() { return GooglePackages.GMS; }
        @Override public String getOpPackageName() { return getPackageName(); }
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
        @Override public boolean bindService(Intent i, ServiceConnection c, int flags) { return Services.bind(this, i, c); }
        @Override public ComponentName startService(Intent i) { return Services.start(this, i); }
        @Override public boolean stopService(Intent i) { return Services.stop(i); }
        @Override public ComponentName startForegroundService(Intent i) { return Services.start(this, i); }
        @Override public void sendBroadcast(Intent i) { Broadcasts.send(this, i); }
        @Override public void sendBroadcast(Intent i, String permission) { Broadcasts.send(this, i); }
        @Override public void sendOrderedBroadcast(Intent i, String permission) { Broadcasts.send(this, i); }
    }
}
