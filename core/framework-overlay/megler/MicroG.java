package megler;

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
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static MicroG instance;
    private static boolean checked;
    private final PackageInfo info = new PackageInfo();
    private final ArrayList<Entry> services = new ArrayList<>();
    private final HashMap<String, Running> running = new HashMap<>();
    private final HashMap<ServiceConnection, Binding> bindings = new HashMap<>();
    private final PackageContext context;
    private Application application;
    private ClassLoader loader;
    private static final class Entry {
        ServiceInfo info = new ServiceInfo();
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
    public static PackageInfo packageInfo(String pkg) { MicroG g = get(); return g != null && GooglePackages.GMS.equals(pkg) ? g.info : null; }
    public static Context context(String pkg) { MicroG g = get(); return g != null && GooglePackages.GMS.equals(pkg) ? g.context : null; }
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
        // Until Megler has a separate service process, keep GmsCore's state private to this client.
        a.dataDir = new File(Native.dataDir(), "megler-services/" + GooglePackages.GMS).getPath();
        a.nativeLibraryDir = APK + "!/lib/armeabi-v7a";
        a.enabled = true; a.uid = android.os.Process.myUid();
        a.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        a.metaData = new Bundle(); info.applicationInfo = a;
        XmlBlock.Parser p = new XmlBlock(manifest, APK).newParser();
        Entry current = null; IntentFilter filter = null; boolean inApp = false;
        int event;
        while ((event = p.next()) != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
            String tag = p.getName();
            if (event == org.xmlpull.v1.XmlPullParser.END_TAG) {
                if ("service".equals(tag)) current = null;
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
            } else if (current != null && "intent-filter".equals(tag)) {
                filter = new IntentFilter(); current.filters.add(filter);
            } else if (filter != null && "action".equals(tag)) filter.addAction(p.getAttributeValue(NS, "name"));
            else if (filter != null && "category".equals(tag)) filter.addCategory(p.getAttributeValue(NS, "name"));
        }
        info.services = new ServiceInfo[services.size()];
        for (int n = 0; n < services.size(); n++) info.services[n] = services.get(n).info;
        // The existing Google compatibility identity is also used for the trusted bundled replacement.
        info.signatures = new Signature[] {GooglePackages.signature()};
        info.signingInfo = new SigningInfo(info.signatures);
        context = new PackageContext(resources);
        android.util.Log.i("MicroG", "GmsCore " + info.versionName + ": " + services.size() + " services");
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
    private ClassLoader loader() {
        if (loader == null) try {
            loader = new ServiceClassLoader(APK);
        } catch (Exception e) { throw new IllegalStateException("Loading GmsCore", e); }
        return loader;
    }
    private Running create(Entry e) throws Exception {
        Running r = running.get(e.info.name);
        if (r != null) return r;
        if (application == null) {
            Application a = info.applicationInfo.className == null ? new Application() :
                (Application) loader().loadClass(info.applicationInfo.className).newInstance();
            a.meglerAttach(context); a.onCreate(); application = a;
        }
        r = new Running(); r.service = (Service) loader().loadClass(e.info.name).newInstance();
        r.service.meglerAttach(context); r.service.onCreate(); running.put(e.info.name, r);
        return r;
    }
    public static boolean bind(Context from, Intent intent, ServiceConnection conn) {
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) return false;
        Intent copy = new Intent(intent);
        new Handler(Looper.getMainLooper()).post(() -> {
            ComponentName name = new ComponentName(GooglePackages.GMS, e.info.name);
            try {
                if (g.bindings.containsKey(conn)) return;
                Running r = g.create(e);
                String key = copy.getAction() == null ? "" : copy.getAction();
                if (!r.binders.containsKey(key)) r.binders.put(key, r.service.onBind(copy));
                IBinder binder = r.binders.get(key);
                r.clients++; g.bindings.put(conn, new Binding(r, copy));
                if (binder == null) conn.onNullBinding(name); else conn.onServiceConnected(name, binder);
            } catch (Throwable t) {
                android.util.Log.e("MicroG", "Binding " + e.info.name + " failed", t);
                conn.onNullBinding(name);
            }
        });
        return true;
    }
    public static boolean unbind(ServiceConnection conn) {
        MicroG g = get(); if (g == null) return false;
        Binding b = g.bindings.remove(conn); if (b == null) return false;
        new Handler(Looper.getMainLooper()).post(() -> {
            if (--b.running.clients == 0) {
                b.running.service.onUnbind(b.intent); b.running.binders.clear();
                g.destroyIfUnused(b.running);
            }
        });
        return true;
    }
    private void destroyIfUnused(Running r) {
        if (!r.started && r.clients == 0) {
            running.values().remove(r); r.service.onDestroy();
        }
    }
    public static ComponentName start(Context from, Intent intent) {
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, GooglePackages.GMS.equals(from.getPackageName()));
        if (e == null) return null;
        Intent copy = new Intent(intent);
        new Handler(Looper.getMainLooper()).post(() -> {
            try { Running r = g.create(e); r.started = true; r.service.onStartCommand(copy, 0, ++r.starts); }
            catch (Throwable t) { android.util.Log.e("MicroG", "Starting " + e.info.name + " failed", t); }
        });
        return new ComponentName(GooglePackages.GMS, e.info.name);
    }
    public static boolean stop(Intent intent) {
        MicroG g = get(); Entry e = g == null ? null : g.find(intent, true);
        if (e == null) return false;
        Running r = g.running.get(e.info.name); if (r == null) return false;
        r.started = false; g.destroyIfUnused(r); return true;
    }
    public static boolean stopSelf(Service service) {
        MicroG g = get(); if (g == null) return false;
        for (Running r : new ArrayList<>(g.running.values())) if (r.service == service) {
            r.started = false; g.destroyIfUnused(r); return true;
        }
        return false;
    }
    private final class PackageContext extends ContextWrapper {
        private final Resources resources;
        private final HashMap<String, SharedPreferences> prefs = new HashMap<>();
        PackageContext(Resources r) { super(ContextImpl.app()); resources = r; }
        @Override public String getPackageName() { return GooglePackages.GMS; }
        @Override public String getOpPackageName() { return getPackageName(); }
        @Override public ApplicationInfo getApplicationInfo() { return info.applicationInfo; }
        @Override public String getPackageCodePath() { return APK; }
        @Override public String getPackageResourcePath() { return APK; }
        @Override public ClassLoader getClassLoader() { return loader(); }
        @Override public Context getApplicationContext() { return application == null ? this : application; }
        @Override public Resources getResources() { return resources; }
        @Override public AssetManager getAssets() { return resources.getAssets(); }
        @Override public File getDataDir() { return new File(info.applicationInfo.dataDir); }
        private File dir(String name) { File f = new File(getDataDir(), name); f.mkdirs(); return f; }
        @Override public File getDatabasePath(String name) { return name.startsWith("/") ? new File(name) : new File(dir("databases"), name); }
        @Override public Context createDeviceProtectedStorageContext() { return this; }
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
    }
}
