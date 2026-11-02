package megler;

import android.content.ContentProvider;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.ProviderInfo;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.content.res.AssetManager;
import android.content.res.Resources;
import android.content.res.XmlBlock;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import org.xmlpull.v1.XmlPullParser;

/** The separately shared, headless Google Services Framework APK and its content providers. */
public final class Gsf {
    public static final String APK = "/system/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk";
    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static Gsf instance;
    private static boolean checked;
    private final PackageInfo info;
    private final Resources resources;
    private final ArrayList<ProviderInfo> providers = new ArrayList<>();
    private final HashMap<String, ContentProvider> running = new HashMap<>();
    private final Context context = new PackageContext();
    private ClassLoader loader;

    private Gsf() throws Exception {
        info = ArchiveInfo.read(APK);
        if (info == null || !GooglePackages.GSF.equals(info.packageName)) throw new IllegalArgumentException("Not GSF");
        AssetManager assets = AssetManager.meglerForPackage(APK, GooglePackages.GSF);
        resources = new Resources(assets);
        ApplicationInfo a = info.applicationInfo;
        File shared = new File(MicroG.SHARED);
        a.dataDir = new File(shared.isDirectory() ? shared : new File(Native.dataDir(), "megler-services"), GooglePackages.GSF).getPath();
        a.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        a.uid = GooglePackages.sharedUid();
        a.enabled = true;
        info.signatures = new Signature[] {GooglePackages.signature()};
        info.signingInfo = new SigningInfo(info.signatures);
        byte[] manifest;
        try (InputStream in = assets.openNonAsset("AndroidManifest.xml"); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] chunk = new byte[8192]; int n;
            while ((n = in.read(chunk)) != -1) out.write(chunk, 0, n);
            manifest = out.toByteArray();
        }
        XmlBlock.Parser p = new XmlBlock(manifest, APK).newParser();
        boolean inApp = false;
        int event;
        while ((event = p.next()) != XmlPullParser.END_DOCUMENT) {
            if (event == XmlPullParser.END_TAG && "application".equals(p.getName())) inApp = false;
            if (event != XmlPullParser.START_TAG) continue;
            if ("application".equals(p.getName())) inApp = true;
            if (!inApp || !"provider".equals(p.getName())) continue;
            ProviderInfo provider = new ProviderInfo();
            String name = p.getAttributeValue(NS, "name");
            provider.name = name == null ? null : name.startsWith(".") ? GooglePackages.GSF + name : name.indexOf('.') < 0 ? GooglePackages.GSF + "." + name : name;
            provider.packageName = GooglePackages.GSF;
            provider.applicationInfo = a;
            provider.authority = p.getAttributeValue(NS, "authorities");
            provider.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
            provider.exported = p.getAttributeBooleanValue(NS, "exported", a.targetSdkVersion < 17);
            if (provider.enabled) providers.add(provider);
        }
        info.providers = providers.toArray(new ProviderInfo[0]);
        android.util.Log.i("Gsf", "Google Services Framework " + info.versionName + ": " + providers.size() + " providers");
    }

    private static synchronized Gsf get() {
        if (GooglePackages.GSF.equals(Native.packageName())) return null;
        if (!checked) {
            checked = true;
            if (new File(APK).isFile()) try { instance = new Gsf(); }
            catch (Exception e) { android.util.Log.e("Gsf", "Cannot load " + APK, e); }
        }
        return instance;
    }
    public static PackageInfo packageInfo(String packageName) { Gsf g = get(); return g != null && GooglePackages.GSF.equals(packageName) ? g.info : null; }
    public static Context context(String packageName) { Gsf g = get(); return g != null && GooglePackages.GSF.equals(packageName) ? g.context : null; }
    public static ContentProvider provider(String authority) {
        Gsf g = get();
        if (g == null || authority == null) return null;
        synchronized (g.running) {
            for (ProviderInfo p : g.providers) {
                if (!p.exported || p.authority == null || !java.util.Arrays.asList(p.authority.split(";")).contains(authority)) continue;
                ContentProvider found = g.running.get(p.name);
                if (found != null) return found;
                try {
                    found = (ContentProvider) g.loader().loadClass(p.name).newInstance();
                    found.attachInfo(g.context, p);
                    if ((GooglePackages.GSF + ".gservices.GservicesProvider").equals(p.name)) found = new Gservices(found);
                    g.running.put(p.name, found);
                    return found;
                } catch (Throwable t) {
                    android.util.Log.e("Gsf", "Starting provider " + p.name + " failed", t);
                    return null;
                }
            }
        }
        return null;
    }
    /*
     * GServices as the apps here read them from GSF's provider: as Google's checkin server sent them, but for two things.
     *  - The offer of a system update is not there (update_url, and the keys that describe its package). The server offers
     *    the model the phone reports, a Pixel, its Android 10 update; GmsCore's SystemUpdate cannot apply it (a streaming A/B
     *    update, for a phone with ro.build.ab_update) and began it again ten times a second, for as long as the app ran. A
     *    phone with no update pending is sent none of these keys.
     *  - enable_droidguard is true, until GmsCore has fetched its server flags (Phenotype), whose value comes first: GmsCore
     *    17.4's sign-in sends Google's server a DroidGuard result only with that flag on (Phenotype's auth_enable_droidguard of
     *    com.google.android.gms.auth_account, which reads GServices without a prefix), and without one the server refused the
     *    account ("MissingDroidguard"). A stand-in for the server's value, not the server's.
     * What is written goes to GSF's provider as it is.
     */
    private static final java.util.Set<String> HIDDEN = new java.util.HashSet<>(java.util.Arrays.asList(
        "update_url", "update_token", "update_title", "update_description", "update_size", "update_size_value",
        "update_streaming_property_files", "update_urgency"));
    private static final java.util.Map<String, String> GIVEN = java.util.Collections.singletonMap("enable_droidguard", "true");
    private static final class Gservices extends ContentProvider {
        private final ContentProvider real;
        Gservices(ContentProvider real) { this.real = real; }
        @Override public boolean onCreate() { return true; }
        /* A query by names (no path: a row for each name, its value or null) or by prefixes (the path "prefix"). */
        @Override public android.database.Cursor query(android.net.Uri u, String[] proj, String sel, String[] args, String sort) {
            android.database.Cursor c = real.query(u, proj, sel, args, sort);
            String path = u.getLastPathSegment();
            boolean prefix = "prefix".equals(path);
            if (c == null || args == null || (path != null && !prefix)) return c;
            android.database.MatrixCursor out = new android.database.MatrixCursor(c.getColumnNames());
            java.util.HashSet<String> seen = new java.util.HashSet<>();
            try {
                while (c.moveToNext()) {
                    String key = c.getString(0), value = c.getString(1);
                    if (HIDDEN.contains(key)) { if (prefix) continue; value = null; }
                    if (GIVEN.containsKey(key)) value = GIVEN.get(key);
                    seen.add(key);
                    out.addRow(new Object[] { key, value });
                }
            } finally { c.close(); }
            if (prefix) for (java.util.Map.Entry<String, String> e : GIVEN.entrySet()) {
                if (seen.contains(e.getKey())) continue;
                for (String a : args) if (a != null && e.getKey().startsWith(a)) { out.addRow(new Object[] { e.getKey(), e.getValue() }); break; }
            }
            return out;
        }
        @Override public String getType(android.net.Uri u) { return real.getType(u); }
        @Override public android.net.Uri insert(android.net.Uri u, android.content.ContentValues v) { return real.insert(u, v); }
        @Override public int bulkInsert(android.net.Uri u, android.content.ContentValues[] v) { return real.bulkInsert(u, v); }
        @Override public int delete(android.net.Uri u, String sel, String[] args) { return real.delete(u, sel, args); }
        @Override public int update(android.net.Uri u, android.content.ContentValues v, String sel, String[] args) { return real.update(u, v, sel, args); }
        @Override public android.os.Bundle call(String method, String arg, android.os.Bundle extras) { return real.call(method, arg, extras); }
    }
    /* One loader whichever thread asks first (megler.MicroG.loader). */
    private synchronized ClassLoader loader() {
        if (loader == null) try { loader = new ServiceClassLoader(APK); }
        catch (Exception e) { throw new IllegalStateException("Loading GSF", e); }
        return loader;
    }
    private final class PackageContext extends ContextWrapper {
        PackageContext() { super(ContextImpl.app()); }
        @Override public String getPackageName() { return GooglePackages.GSF; }
        @Override public String getOpPackageName() { return getPackageName(); }
        @Override public ApplicationInfo getApplicationInfo() { return info.applicationInfo; }
        @Override public String getPackageCodePath() { return APK; }
        @Override public String getPackageResourcePath() { return APK; }
        @Override public ClassLoader getClassLoader() { return loader(); }
        @Override public Context getApplicationContext() { return this; }
        @Override public Resources getResources() { return resources; }
        @Override public AssetManager getAssets() { return resources.getAssets(); }
        @Override public File getDataDir() { return new File(info.applicationInfo.dataDir); }
        private File dir(String name) { File f = new File(getDataDir(), name); f.mkdirs(); return f; }
        @Override public File getDatabasePath(String name) { return name.startsWith("/") ? new File(name) : new File(dir("databases"), name); }
        @Override public File getFilesDir() { return dir("files"); }
        @Override public File getCacheDir() { return dir("cache"); }
        @Override public File getDir(String name, int mode) { return dir("app_" + name); }
        @Override public Context createPackageContext(String pkg, int flags) throws android.content.pm.PackageManager.NameNotFoundException {
            return getPackageName().equals(pkg) ? this : super.createPackageContext(pkg, flags);
        }
    }
}
