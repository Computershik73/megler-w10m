// Another app's service, bound inside the app that asks for it.
//
// Every Android app is a Windows package of its own here, a process of its own, and nothing carries a Binder call from one
// to another: bindService for a service of another app answered false. Total Commander's plug-ins are such services (its LAN
// and Wi-Fi transfer plug-ins: the action com.android.tcplugins.IPluginFunctions, the service
// com.android.tcplugins.FileSystem.PluginService in the plug-in's package), and each plug-in "failed to connect".
//
// What Megler does for Google Play services (megler.MicroG) is done here for any small app: the other app's APK, which lies
// readable at /data/app/<package>-1/base.apk, is loaded in a class scope of its own inside the asking app's process
// (megler.ServiceClassLoader), its Application is made once, with a Context that answers for that package -- its name, its
// resources, its class loader, its ApplicationInfo, its folders -- and the service is made, bound, and its Binder handed to
// the caller's ServiceConnection on the main thread, as Android hands it. The caller's proxy and the service's stub are
// classes of two scopes, so a call goes through transact and a Parcel, as between two processes.
//
// Which services: those the other app lets others bind (megler.PhoneApps.service: exported, as the shell described them), by
// the intent's component or by its action within the package it names. Not Google's packages, ever: Google Play services and
// Play Store have their own ways (megler.MicroG, megler.PlayStore), and a handler (handle) may answer for a package before
// any of this, which is how Megler's own stand-ins for another package's service are to be added. Not a service whose
// permission is the system's to hold (android.permission.BIND_...) or one the asking app does not request. And not a large
// app: the hosted code and its heap are the asking app's, on phones with 1 GB, so an APK over MAX_APK_BYTES is refused, and
// more than MAX_HOSTS packages in one process. Each refusal is said in the log once, with its reason; what would be refused
// is not listed either (megler.PhoneApps.queryServices).
//
// The hosted package's data is not its own app's: that is another Windows package's folder, which this process cannot open.
// It is <shared>/hosted/<package> (megler.MicroG.SHARED, the folder every app of this Megler has; the asking app's own
// data folder where there is none), with files, databases, shared_prefs and the rest under it. So the settings a plug-in keeps
// when its service runs here are not those of the same app started from its own tile, and the other way round; two apps that
// host one package share its folder, and write it at once. A service that needs its app's sign-in has none here. The package's
// own code runs with the asking app's permissions, and what it throws on the main thread is the asking app's crash; a thread
// of its own that throws ends alone (megler.AppRunner.UncaughtLogging: a service scope's thread). Its content providers are
// not made, and only its base APK is read (no splits).
//
// Its activities, once it runs here: a plug-in's service and its screens are one app -- Total Commander's LAN plug-in asks
// its host to start its ConnectActivity ("New server"), which begins with a call of the service's static instance and tells
// the service of the server it saved by a broadcast within its package. In the plug-in's own app (megler.PhoneApps.start)
// there is no such instance, and the settings are another folder's. So an activity of a package that is hosted here already
// is made here too (activity, newActivity; megler.AppRunner.startActivity and make): in the package's class scope, on a
// Context of that package, with the Application its services have -- one another app may open when the asking app wants
// it, any of its own when the package's own code does. No package is hosted for an activity alone: an app that only starts
// another's activity gets that app, as before.
//
// The hosted code's own use of its services goes the same way: its Context's bindService, startService and stopService reach
// megler.Services, which finds none of the asking app's and asks here, where a hosted package's intents for itself are
// resolved in its own manifest, exported or not. (The LAN plug-in's service binds its own ForegroundService and casts the
// Binder to its class: one scope, one instance.)
package megler;

import android.app.Activity;
import android.app.Application;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.XmlBlock;
import android.os.IBinder;
import android.os.Looper;
import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import org.xmlpull.v1.XmlPullParser;

/** Other apps' services run inside this app: bound by an Intent, their Binder returned. */
public final class HostedApps {
    private HostedApps() {}

    /**
     * Megler's own answer for a package's services, asked before the package is looked for among the installed apps: the
     * Binder for the intent, or null to leave it to what follows (for Google's packages, nothing: they are never hosted).
     */
    public interface PackageHandler {
        IBinder bind(Context from, Intent intent);
    }

    /** The largest base APK whose code is run inside another app. */
    public static final long MAX_APK_BYTES = 16L << 20;
    /** How many other packages one app hosts. */
    public static final int MAX_HOSTS = 4;

    private static final String NS = "http://schemas.android.com/apk/res/android";
    private static final HashMap<String, PackageHandler> sHandlers = new HashMap<>();
    private static final HashMap<String, Host> sHosts = new HashMap<>();
    private static final HashMap<ServiceConnection, Binding> sBindings = new HashMap<>();
    /* The size of each app's base APK, read once for a version: { the version, the size }. */
    private static final HashMap<String, long[]> sSizes = new HashMap<>();
    /* What was said already: a refusal is said once, however often the app asks again. */
    private static final HashSet<String> sSaid = new HashSet<>();

    private static final class Entry {
        String name;
        boolean enabled = true;
        final ArrayList<String> actions = new ArrayList<>();
    }
    private static final class Running {
        Service service;
        int clients;
        boolean started, kept;
        final HashMap<String, IBinder> binders = new HashMap<>();
    }
    private static final class Binding {
        final Host host;
        final Intent intent;
        Running running;
        Binding(Host h, Intent i) { host = h; intent = i; }
    }
    private static final class Target {
        final Host host;
        final Entry entry;
        Target(Host h, Entry e) { host = h; entry = e; }
    }

    /**
     * Sets (or with null removes) the handler that answers for a package's services. Megler's own are set where the class
     * that answers is first used by megler.Services, or in a static block here: a handler set by a class nothing loads is
     * never asked.
     */
    public static void handle(String pkg, PackageHandler h) {
        synchronized (sHandlers) { if (h == null) sHandlers.remove(pkg); else sHandlers.put(pkg, h); }
    }

    /** The Context of a package hosted in this process, or null when it is not. */
    public static Context context(String pkg) {
        Host h;
        synchronized (sHosts) { h = sHosts.get(pkg); }
        return h == null ? null : h.context;
    }

    /**
     * Why this app would not bind that service of another app, or null when it would: what does not depend on what else
     * runs here (the count of hosted packages is checked when one is added).
     */
    static String refusal(PhoneApps.App a, PhoneApps.Service s) {
        if (GooglePackages.is(a.packageName)) return "Google's packages are not hosted";
        if (!s.enabled) return "the service is disabled";
        if (s.permission != null) {
            if (s.permission.startsWith("android.permission.BIND_")) return "only the system may bind it (" + s.permission + ")";
            // (Whether the permission is one only the same signer gets is not looked at: the app asks for it, as it must.)
            if (!Manifest.permissions.contains(s.permission)) return "this app does not ask for its permission " + s.permission;
        }
        long size = apkSize(a);
        if (size <= 0) return "its APK cannot be read";
        if (size > MAX_APK_BYTES)
            return "its APK has " + (size >> 20) + " MB, and an app runs another's code only up to " + (MAX_APK_BYTES >> 20) + " MB";
        return null;
    }
    private static String apk(String pkg) { return "/data/app/" + pkg + "-1/base.apk"; }
    private static long apkSize(PhoneApps.App a) {
        synchronized (sSizes) {
            long[] had = sSizes.get(a.packageName);
            if (had != null && had[0] == a.versionCode && had[1] > 0) return had[1];
            long size = new File(apk(a.packageName)).length();
            sSizes.put(a.packageName, new long[] { a.versionCode, size });
            return size;
        }
    }
    private static void said(String what) {
        synchronized (sSaid) { if (sSaid.size() >= 64 || !sSaid.add(what)) return; }
        android.util.Log.w("Megler", what);
    }
    /* An intent as the log names it: its component or its action, nothing of its data or extras. */
    private static String named(Intent i) {
        return i.getComponent() != null ? i.getComponent().flattenToShortString() : "the action " + i.getAction();
    }

    /* The hosted package whose code asks through that Context, or null when the asking app itself does. */
    private static Host hostOf(Context from) {
        String p = from == null ? null : from.getPackageName();
        if (p == null || p.equals(Native.packageName())) return null;
        synchronized (sHosts) { return sHosts.get(p); }
    }
    /* The hosted package that activity is one of, or null. */
    private static Host hostOf(Manifest.Component c) {
        if (c == null) return null;
        synchronized (sHosts) { for (Host h : sHosts.values()) if (h.activities.contains(c)) return h; }
        return null;
    }
    /* The package an intent is for: its component's, else the one it names, else the asking hosted package's own. */
    private static String packageOf(Host own, Intent i) {
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        return pkg != null ? pkg : own != null ? own.pkg : null;
    }
    private static IBinder handled(String pkg, Context from, Intent i) {
        PackageHandler h;
        synchronized (sHandlers) { h = sHandlers.get(pkg); }
        return h == null ? null : h.bind(from, i);
    }

    /* The package hosted, made when it is not yet; null, and said, when it cannot be. */
    private static Host host(PhoneApps.App a) {
        synchronized (sHosts) {
            Host h = sHosts.get(a.packageName);
            if (h != null) return h;
            if (sHosts.size() >= MAX_HOSTS) {
                said("hosted apps: " + a.packageName + " is not run here: " + sHosts.keySet() + " are already, and an app runs the code of " + MAX_HOSTS + " others at most");
                return null;
            }
            try { h = new Host(a); }
            catch (Exception | Error e) {
                android.util.Log.w("Megler", "hosted apps: " + a.packageName + " cannot be run here: " + e);
                return null;
            }
            sHosts.put(a.packageName, h);
            android.util.Log.i("Megler", "hosted apps: " + a.packageName + " " + a.versionName + " runs inside " + Native.packageName() + " for its services ("
                + h.entries.size() + " in its manifest), its data in " + h.info.dataDir);
            return h;
        }
    }

    /* The service an intent for `pkg` reaches and the package hosted for it, or null (said, when the package is installed). */
    private static Target target(Host own, String pkg, Intent i) {
        if (pkg == null || GooglePackages.is(pkg) || pkg.equals(Native.packageName())) return null;
        if (own != null && own.pkg.equals(pkg)) {
            Entry e = own.entry(i);
            if (e == null) said("hosted apps: " + pkg + " has no service for " + named(i) + " of its own");
            return e == null ? null : new Target(own, e);
        }
        if (i.getComponent() == null && i.getAction() == null) return null;
        PhoneApps.App a = PhoneApps.find(pkg);
        if (a == null) return null;
        PhoneApps.Service s = PhoneApps.service(a, i);
        if (s == null) { said("hosted apps: " + pkg + " has no service another app may bind for " + named(i)); return null; }
        String why = refusal(a, s);
        if (why != null) { said("hosted apps: " + pkg + "/" + s.name + " is not bound here: " + why); return null; }
        Host h = host(a);
        if (h == null) return null;
        Entry e = h.entries.get(s.name);
        if (e == null || !e.enabled) { said("hosted apps: " + pkg + "/" + s.name + " is not bound here: its APK's manifest has no such service now"); return null; }
        return new Target(h, e);
    }

    /**
     * A service of another package by an Intent, in this process: its Binder, or null when there is none to give -- no
     * handler's answer, and the package not installed, the service not one another app may bind, or refused (refusal; said in
     * the log). The intent names the package by its component or setPackage. The service is made on the main thread, which
     * another thread waits for, and stays until the app ends: there is no connection to unbind.
     */
    public static IBinder binder(Context from, Intent intent) {
        if (intent == null) return null;
        Host own = hostOf(from);
        String pkg = packageOf(own, intent);
        if (pkg == null) return null;
        IBinder answer = handled(pkg, from, intent);
        if (answer != null) return answer;
        final Target t = target(own, pkg, intent);
        if (t == null) return null;
        final Intent copy = new Intent(intent);
        final IBinder[] out = new IBinder[1];
        final Runnable make = () -> {
            try {
                Running r = t.host.create(t.entry);
                r.kept = true;
                out[0] = t.host.binder(r, t.entry, copy);
            } catch (Throwable e) {
                android.util.Log.e("Megler", "hosted apps: binding " + t.host.pkg + "/" + t.entry.name + " failed", e);
            }
        };
        if (Looper.myLooper() == Looper.getMainLooper()) { make.run(); return out[0]; }
        final java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
        MicroG.onMain(() -> { try { make.run(); } finally { done.countDown(); } });
        try {
            if (!done.await(30, java.util.concurrent.TimeUnit.SECONDS))
                android.util.Log.w("Megler", "hosted apps: " + t.host.pkg + "/" + t.entry.name + " was not bound in 30 s: the main thread is busy");
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
        return out[0];
    }

    /**
     * Context.bindService for a service that is not the asking app's own (megler.Services.bind, after Google's): true when it
     * will be bound, and the connection hears of it on the main thread; false as Android answers for a service that is not
     * there.
     */
    static boolean bind(Context from, Intent i, ServiceConnection conn) {
        if (i == null || conn == null) return false;
        Host own = hostOf(from);
        String pkg = packageOf(own, i);
        if (pkg == null) return false;
        final IBinder answer = handled(pkg, from, i);
        if (answer != null) {
            final ComponentName name = i.getComponent() != null ? i.getComponent() : new ComponentName(pkg, String.valueOf(i.getAction()));
            MicroG.onMain(() -> conn.onServiceConnected(name, answer));
            return true;
        }
        final Target t = target(own, pkg, i);
        if (t == null) return false;
        final Host host = t.host;
        final Entry e = t.entry;
        final Intent copy = new Intent(i);
        final Binding pending = new Binding(host, copy);
        synchronized (sBindings) {
            if (sBindings.containsKey(conn)) return true;
            sBindings.put(conn, pending);
        }
        MicroG.onMain(() -> {
            synchronized (sBindings) { if (sBindings.get(conn) != pending) return; }
            ComponentName name = new ComponentName(host.pkg, e.name);
            try {
                Running r = host.create(e);
                IBinder binder = host.binder(r, e, copy);
                synchronized (sBindings) {
                    if (sBindings.get(conn) != pending) { host.destroyIfUnused(r); return; }
                    r.clients++; pending.running = r;
                }
                if (binder == null) conn.onNullBinding(name); else conn.onServiceConnected(name, binder);
            } catch (Throwable x) {
                android.util.Log.e("Megler", "hosted apps: binding " + name.flattenToShortString() + " failed", x);
                synchronized (sBindings) { if (sBindings.get(conn) != pending) return; }
                conn.onNullBinding(name);
            }
        });
        return true;
    }

    /** Context.unbindService: false when the connection is none of these. */
    static boolean unbind(ServiceConnection conn) {
        final Binding b;
        synchronized (sBindings) { b = sBindings.remove(conn); }
        if (b == null) return false;
        MicroG.onMain(() -> {
            if (b.running == null) return;
            if (--b.running.clients == 0) {
                try { b.running.service.onUnbind(b.intent); } catch (Throwable t) { android.util.Log.e("Megler", "hosted apps: unbinding a service of " + b.host.pkg + " failed", t); }
                b.running.binders.clear();
                b.host.destroyIfUnused(b.running);
            }
        });
        return true;
    }

    /**
     * Context.startService by a hosted package's own code for a service of its own (megler.Services.start): started on the
     * main thread, as the app's own are. Null for anything else: another app's service is bound here, not started.
     */
    static ComponentName start(Context from, Intent i) {
        final Host own = hostOf(from);
        if (own == null || i == null || !own.pkg.equals(packageOf(own, i))) return null;
        final Entry e = own.entry(i);
        if (e == null) { said("hosted apps: " + own.pkg + " has no service for " + named(i) + " of its own"); return null; }
        final Intent copy = new Intent(i);
        Services.postStart(own.startIds, e.name, id -> {
            try { Running r = own.create(e); r.started = true; r.service.onStartCommand(Processes.forLoader(copy, own.loader()), 0, id); }
            catch (Throwable t) { android.util.Log.e("Megler", "hosted apps: starting " + own.pkg + "/" + e.name + " failed", t); }
        });
        return new ComponentName(own.pkg, e.name);
    }

    /** Context.stopService for a hosted package's started service; false when the intent is for none that runs here. */
    static boolean stop(Intent i) {
        if (i == null) return false;
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        ArrayList<Host> hosts = new ArrayList<>();
        synchronized (sHosts) { if (pkg == null) hosts.addAll(sHosts.values()); else if (sHosts.containsKey(pkg)) hosts.add(sHosts.get(pkg)); }
        for (final Host h : hosts) {
            final Entry e = h.entry(i);
            if (e == null) continue;
            synchronized (h.running) { if (h.running.get(e.name) == null) continue; }
            MicroG.onMain(() -> {
                Running r;
                synchronized (h.running) { r = h.running.get(e.name); }
                if (r != null) { r.started = false; h.destroyIfUnused(r); }
            });
            return true;
        }
        return false;
    }

    /** Service.stopSelfResult for a hosted service: 1 when it stops, 0 when a later start keeps it, -1 when it is none of these. */
    static int stopSelf(Service service, int id) {
        ArrayList<Host> hosts;
        synchronized (sHosts) { if (sHosts.isEmpty()) return -1; hosts = new ArrayList<>(sHosts.values()); }
        for (final Host h : hosts) {
            String name = null; Running found = null;
            synchronized (h.running) { for (Map.Entry<String, Running> x : h.running.entrySet()) if (x.getValue().service == service) { name = x.getKey(); found = x.getValue(); } }
            if (found == null) continue;
            final Running r = found;
            return Services.postStop(h.startIds, name, id, () -> {
                boolean still;
                synchronized (h.running) { still = h.running.containsValue(r); }
                if (still) { r.started = false; h.destroyIfUnused(r); }
            }) ? 1 : 0;
        }
        return -1;
    }

    /**
     * The activity an intent opens in a package that runs here already for its services, or null (megler.AppRunner.startActivity
     * asks, after Google Play services' and before the phone's other apps). The intent names the package, by its component
     * or setPackage; the activity is one another app may open -- exported, enabled, behind no permission this app does not ask
     * for -- or any enabled one of the package when its own code asks (`from` its Context, its service or its activity). An
     * intent of the package's own code that names no package is its own when its action is not Android's and the asking app
     * has no activity for it. Never the launcher's intent, which is the package's own app.
     */
    public static Manifest.Component activity(Context from, Intent i) {
        // (What tells an intent is none of these first: every activity an app starts is asked of here.)
        synchronized (sHosts) { if (sHosts.isEmpty()) return null; }
        if (i == null || PhoneApps.isLauncherIntent(i)) return null;
        Host own = hostOf(from);
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        if (pkg == null) {
            String action = i.getAction();
            if (own == null || action == null || action.startsWith("android.") || Manifest.resolve(i) != null) return null;
            pkg = own.pkg;
        }
        Host h;
        synchronized (sHosts) { h = sHosts.get(pkg); }
        if (h == null) return null;
        ArrayList<Manifest.Component> open = new ArrayList<>();
        for (Manifest.Component c : h.activities)
            if (c.enabled && (own == h || (c.exported && (c.permission == null || Manifest.permissions.contains(c.permission))))) open.add(c);
        Manifest.Component c = Manifest.resolveIn(open, i);
        if (c == null && i.getComponent() != null && Manifest.resolveIn(h.activities, i) != null)
            said("hosted apps: " + named(i) + " is not opened here: it is not for other apps, or behind a permission this app does not ask for");
        return c;
    }

    /**
     * The intent an activity of a hosted package (activity) is started with: a copy, its extras in that package's class scope,
     * as a Parcel between two processes makes them (megler.Processes.forLoader), its component the activity's.
     */
    static Intent intentFor(Manifest.Component c, Intent i) {
        Host h = hostOf(c);
        if (h == null) return i;
        Intent intent = Processes.forLoader(new Intent(i), h.loader());
        if (intent.getComponent() == null) intent.setComponent(new ComponentName(h.pkg, c.name));
        return intent;
    }

    /**
     * An activity of a hosted package, made and attached (megler.AppRunner.make): by the package's classes, on a Context of
     * that package of its own, with the package's Application, made first as for its services. Null when the component is
     * none of a hosted package's.
     */
    static Activity newActivity(String cls, Intent intent, Activity caller, int request, Manifest.Component info) throws Exception {
        Host h = hostOf(info);
        return h == null ? null : h.activity(cls, intent, caller, request, info);
    }

    /** The label of the hosted package a Context is of (an activity of it whose manifest names no title), or null. */
    static CharSequence label(Context c) {
        synchronized (sHosts) { if (sHosts.isEmpty()) return null; }
        Host h = hostOf(c);
        return h == null ? null : h.info.nonLocalizedLabel != null ? h.info.nonLocalizedLabel : h.pkg;
    }

    /* One other package in this process: its APK's resources and manifest, its class scope, its Application, its services. */
    private static final class Host {
        final String pkg, apk;
        final ApplicationInfo info;
        final Resources resources;
        final HostContext context;
        /* Its preferences' objects: one file, one object, whichever of its contexts asks. */
        final HashMap<String, SharedPreferences> prefs = new HashMap<>();
        /* Its activities and their aliases, as its manifest has them: `exported` as Android reads it, a theme in each. */
        final ArrayList<Manifest.Component> activities;
        final HashMap<String, Entry> entries = new HashMap<>();
        final HashMap<String, Running> running = new HashMap<>();
        final HashMap<String, Integer> startIds = new HashMap<>();
        private ServiceClassLoader loader;
        /* Its Application once attached (onCreate may still run), the one being attached, and what its making threw. */
        private volatile Application application;
        private Application attaching;
        private Throwable failed;

        Host(PhoneApps.App a) throws Exception {
            pkg = a.packageName;
            apk = apk(pkg);
            AssetManager assets = AssetManager.meglerForPackage(apk, pkg);
            resources = new Resources(assets);
            info = PhoneApps.applicationInfo(a);
            File shared = new File(MicroG.SHARED);
            info.dataDir = info.deviceProtectedDataDir = info.credentialProtectedDataDir = new File(shared.isDirectory() ? shared : new File(Native.dataDir()), "hosted/" + pkg).getPath();
            byte[] manifest;
            try (java.io.InputStream in = assets.openNonAsset("AndroidManifest.xml"); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
                byte[] buf = new byte[8192]; int n;
                while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
                manifest = out.toByteArray();
            }
            XmlBlock.Parser p = new XmlBlock(manifest, apk).newParser();
            boolean inApp = false;
            Entry service = null;
            HashSet<String> closed = new HashSet<>();
            int event;
            while ((event = p.next()) != XmlPullParser.END_DOCUMENT) {
                String tag = p.getName();
                if (event == XmlPullParser.END_TAG) {
                    if ("application".equals(tag)) inApp = false;
                    else if ("service".equals(tag)) service = null;
                    continue;
                }
                if (event != XmlPullParser.START_TAG) continue;
                if ("manifest".equals(tag)) {
                    String inApk = p.getAttributeValue(null, "package");
                    if (!pkg.equals(inApk)) throw new IllegalArgumentException(apk + " is the APK of " + inApk);
                } else if ("uses-sdk".equals(tag)) {
                    info.minSdkVersion = p.getAttributeIntValue(NS, "minSdkVersion", 1);
                    info.targetSdkVersion = p.getAttributeIntValue(NS, "targetSdkVersion", info.minSdkVersion);
                } else if ("application".equals(tag)) {
                    inApp = true;
                    info.className = full(p.getAttributeValue(NS, "name"));
                    info.theme = p.getAttributeResourceValue(NS, "theme", 0);
                } else if (inApp && "service".equals(tag)) {
                    service = new Entry();
                    service.name = full(p.getAttributeValue(NS, "name"));
                    service.enabled = p.getAttributeBooleanValue(NS, "enabled", true);
                    if (service.name != null) entries.put(service.name, service);
                } else if (service != null && "action".equals(tag)) {
                    String action = p.getAttributeValue(NS, "name");
                    if (action != null) service.actions.add(action);
                } else if (inApp && ("activity".equals(tag) || "activity-alias".equals(tag))) {
                    // (One that says it is not for other apps; the rest are read below.)
                    if (!p.getAttributeBooleanValue(NS, "exported", true)) closed.add(full(p.getAttributeValue(NS, "name")));
                }
            }
            activities = Manifest.activitiesIn(manifest, apk, pkg);
            for (Manifest.Component c : activities) {
                // One that does not say is for other apps when it has a filter, as Android before 12 has it (and as the shell
                // describes the app's activities: megler_apk_describe_activities).
                if (!c.exported && !closed.contains(c.name) && !c.filters.isEmpty()) c.exported = true;
                // A theme of the package's own choosing in each: an activity with none would get the asking app's
                // (android.app.Activity.meglerAttach), a number of another app's resources.
                if (c.theme == 0) c.theme = defaultTheme();
            }
            context = new HostContext(resources, prefs);
        }
        /* The theme of what names none: the application's, else what the package's target SDK selects on Android
           (megler.ContextImpl.defaultTheme, which says it of the app itself). */
        int defaultTheme() {
            if (info.theme != 0) return info.theme;
            int t = info.targetSdkVersion;
            if (t < 11) return IR.style.Theme;
            if (t < 14) return IR.style.Theme_Holo;
            if (t < 24) return IR.style.Theme_DeviceDefault;
            return IR.style.Theme_DeviceDefault_Light_DarkActionBar;
        }
        private String full(String name) {
            return name == null ? null : name.startsWith(".") ? pkg + name : name.indexOf('.') < 0 ? pkg + "." + name : name;
        }
        synchronized ServiceClassLoader loader() {
            if (loader == null) {
                try { loader = new ServiceClassLoader(apk); }
                catch (java.io.IOException e) { throw new IllegalStateException("the code of " + pkg + " cannot be read: " + e, e); }
            }
            return loader;
        }
        /* Its service an intent of its own code reaches: the one of the component's class, else the first with the action. */
        Entry entry(Intent i) {
            ComponentName c = i.getComponent();
            if (c != null) {
                if (!pkg.equals(c.getPackageName())) return null;
                Entry e = entries.get(full(c.getClassName()));
                return e != null && e.enabled ? e : null;
            }
            if (i.getAction() == null) return null;
            for (Entry e : entries.values()) if (e.enabled && e.actions.contains(i.getAction())) return e;
            return null;
        }
        /* Its Application, made once (main thread): one whose making threw is not made again, and nothing of the package runs. */
        Application application() throws Exception {
            if (failed != null) throw new IllegalStateException("the Application of " + pkg + " failed here before", failed);
            if (application != null) return application;
            if (attaching != null) return attaching;
            try {
                Application a = info.className == null ? new Application() : (Application) loader().loadClass(info.className).newInstance();
                attaching = a;
                try { a.meglerAttach(context); } finally { attaching = null; }
                application = a;
                a.onCreate();
            } catch (Exception | Error t) { failed = t; throw t; }
            return application;
        }
        /* The running service of that name, made when it is not (main thread). */
        Running create(Entry e) throws Exception {
            Running r;
            synchronized (running) { r = running.get(e.name); }
            if (r != null) return r;
            application();
            r = new Running();
            r.service = (Service) loader().loadClass(e.name).newInstance();
            r.service.meglerAttach(context);
            synchronized (running) { running.put(e.name, r); }
            try { r.service.onCreate(); }
            catch (Exception | Error t) { synchronized (running) { running.remove(e.name); } throw t; }
            return r;
        }
        /* The service's Binder for an intent: onBind once for each intent Android would tell apart (megler.MicroG.bindKey). */
        IBinder binder(Running r, Entry e, Intent copy) {
            String key = MicroG.bindKey(copy);
            if (!r.binders.containsKey(key)) {
                IBinder b = r.service.onBind(Processes.forLoader(copy, loader()));
                r.binders.put(key, b);
                android.util.Log.i("Megler", "hosted apps: bound " + pkg + "/" + e.name + (copy.getAction() == null ? "" : " for " + copy.getAction())
                    + ": " + (b == null ? "it gives no binder" : b.getClass().getName()));
            }
            return r.binders.get(key);
        }
        void destroyIfUnused(Running r) {
            if (r.started || r.kept || r.clients != 0) return;
            synchronized (running) { if (!running.values().remove(r)) return; }
            try { r.service.onDestroy(); } catch (Throwable t) { android.util.Log.e("Megler", "hosted apps: a service of " + pkg + " failed as it was destroyed", t); }
        }
        /* One of its activities, made and attached (main thread): a base Context of its own, as each of an app's has. */
        Activity activity(String cls, Intent intent, Activity caller, int request, Manifest.Component c) throws Exception {
            Application app = application();
            Activity a = (Activity) loader().loadClass(cls).newInstance();
            a.meglerAttach(new HostContext(resources, prefs), app, intent, c, caller, request);
            return a;
        }

        /*
         * The hosted package's Context: the asking app's in what the system gives (its services, its package manager, its
         * content resolver, its permissions), the package's own in what tells one app from another -- as megler.MicroG's
         * PackageContext is for Google Play services, without what that one grants them.
         */
        final class HostContext extends ContextWrapper {
            private final Resources resources;
            private final HashMap<String, SharedPreferences> prefs;
            private Resources.Theme theme;
            private int themeId;
            private android.view.LayoutInflater inflater;
            HostContext(Resources r, HashMap<String, SharedPreferences> prefs) {
                super(ContextImpl.app());
                resources = r;
                this.prefs = prefs;
            }
            @Override public Context createConfigurationContext(Configuration c) {
                Configuration config = new Configuration(resources.getConfiguration());
                if (c != null) config.updateFrom(c);
                return new HostContext(new Resources(resources.getAssets(), resources.getDisplayMetrics(), config), prefs);
            }
            /* A theme of its own resources: the app's own where its manifest names one, else the platform's default. */
            @Override public synchronized void setTheme(int resid) { if (themeId != resid) { themeId = resid; theme = null; } }
            @Override public synchronized int getThemeResId() { return themeId; }
            @Override public synchronized Resources.Theme getTheme() {
                if (theme == null) {
                    theme = resources.newTheme();
                    theme.applyStyle(themeId != 0 ? themeId : defaultTheme(), true);
                }
                return theme;
            }
            /* An inflater of this context, not of the asking app's, whose resources its layouts' numbers would be read in. */
            @Override public synchronized Object getSystemService(String name) {
                if (!LAYOUT_INFLATER_SERVICE.equals(name)) return super.getSystemService(name);
                if (inflater == null) inflater = android.view.LayoutInflater.from(getBaseContext()).cloneInContext(this);
                return inflater;
            }
            @Override public String getPackageName() { return pkg; }
            @Override public String getOpPackageName() { return pkg; }
            @Override public ApplicationInfo getApplicationInfo() { return info; }
            @Override public String getPackageCodePath() { return apk; }
            @Override public String getPackageResourcePath() { return apk; }
            @Override public ClassLoader getClassLoader() { return loader(); }
            @Override public Context getApplicationContext() { Application a = application != null ? application : attaching; return a == null ? this : a; }
            @Override public Resources getResources() { return resources; }
            @Override public AssetManager getAssets() { return resources.getAssets(); }
            @Override public File getDataDir() { File data = new File(info.dataDir); data.mkdirs(); return data; }
            private File dir(String name) { File f = new File(getDataDir(), name); f.mkdirs(); return f; }
            @Override public File getDatabasePath(String name) { return name.startsWith("/") ? new File(name) : new File(dir("databases"), name); }
            @Override public File getFilesDir() { return dir("files"); }
            @Override public File getCacheDir() { return dir("cache"); }
            @Override public File getDir(String name, int mode) { return dir("app_" + name); }
            @Override public File getExternalFilesDir(String type) {
                File f = new File(Native.externalDir(), "Android/data/" + pkg + "/files" + (type == null ? "" : "/" + type));
                f.mkdirs();
                return f;
            }
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                synchronized (prefs) {
                    SharedPreferences p = prefs.get(name);
                    if (p == null) { p = new android.content.MeglerSharedPreferences(new File(dir("shared_prefs"), name + ".xml")); prefs.put(name, p); }
                    return p;
                }
            }
            @Override public Context createPackageContext(String other, int flags) throws PackageManager.NameNotFoundException {
                return pkg.equals(other) ? this : super.createPackageContext(other, flags);
            }
            @Override public Context createContextForSplit(String split) { return this; }
            // Its services are asked for as this package, not as the asking app: ContextWrapper asks its base, the app's own.
            @Override public boolean bindService(Intent i, ServiceConnection c, int flags) { return Services.bind(this, i, c); }
            @Override public ComponentName startService(Intent i) { return Services.start(this, i); }
            @Override public ComponentName startForegroundService(Intent i) { return Services.start(this, i); }
            @Override public boolean stopService(Intent i) { return Services.stop(i); }
            // And its activities, which may be its own (activity): the LAN plug-in's service starts its screens itself when
            // Total Commander does not for it.
            @Override public void startActivity(Intent i) { AppRunner.startActivity(this, i, -1, null); }
            @Override public void startActivity(Intent i, android.os.Bundle options) { AppRunner.startActivity(this, i, -1, options); }
        }
    }
}
