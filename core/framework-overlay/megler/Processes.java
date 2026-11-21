// Megler: the app's other processes -- a component's android:process, Play Store's ":download_service" -- each a class scope
// of the app's own APK (megler.ServiceClassLoader) with an Application of its own, as Android runs them apart: the code there
// has statics of its own and an Application made for that process, whose ActivityManager names it (getRunningAppProcesses
// has it for this pid).
//
// Why. Play Store's Application builds an object graph of its own for each of its processes, by the name ActivityManager gives
// the process it runs in (its afuu), and keeps it in statics (its xlr); its download service, made in the app's own process
// here, asked that graph for the download service's part and had the main process's ("dph cannot be cast to qdh"), and Play
// Store had nothing to download with. A service whose process is another one is made in that process's scope
// (megler.Services). Its binder reaches the rest of the app through transact: AIDL's asInterface takes an object of another
// scope for a remote one, its interface being another class there, as Google Play services' binders do (docs/MICROG.md).
//
// The scope's Application is made, attached and created once, on the main thread, the first time a component of that process
// is; its context is the app's, but for its class loader, its Application and its ActivityManager.
//
// Which process the app's own scope is. Android starts the process an activity's manifest names for it, and the app runs as
// the process of the activity it is started for (own, started): its launcher activity's, or the one's another app asked for.
// That is the package's own for most apps; the Google app has its launcher's GoogleAppActivity in ":googleapp", Gemini's link
// opens its GatewayActivity in ":search", and only three services are in the package's own process. Its Application
// builds the object graph (Hilt's component) of the process it is told it runs in; told the package's own, it built that
// one, without the entry points of ":googleapp", and GoogleAppActivity ended its start with "Cannot cast tdd to bkxw". Every
// place that names the process says the own scope's then: Application.getProcessName, ActivityThread.currentProcessName,
// Process.myProcessName, ActivityManager, and /proc/self's cmdline and status (Native.setProcessName). The package's own
// process is then a scope like any other, for its components.
//
// An activity of another process than the own scope's runs in that process's scope, with its Application, as its services
// do (megler.AppRunner.make; activityBase): the Google app's ":googleapp" opens activities of ":search". The providers of
// every process are made in the app's own scope as it starts, as before, and a receiver of the manifest's in another process
// is not run (megler.Broadcasts). Google Play services' own package, run as an app, keeps its processes one (apart), as in
// every app that hosts them (megler.MicroG). A process whose Application throws as it is made ends, as it crashes on
// Android: its components are not made from then on (Ended), and the app's other processes go on.
//
// An intent that goes from one scope to another -- a broadcast, a service's start or binding -- has the objects among its
// extras made again by the receiving scope's classes (forLoader), as Android's parcelling of it between processes makes
// them: Play Store's download service broadcasts its progress with a parcelled proto of its own scope's class, and the main
// process's receiver ended casting it to its own ("ProtoParsers$InternalDontUse cannot be cast to
// ProtoParsers$InternalDontUse").
//
// Each process has a list of security providers of its own (java.security.Security), as on Android: libcore keeps one for
// each of these scopes, a fresh process's at first (ownProviders; core/libcore-overlay/sun/security/jca/Providers.java.members).
// The Google app's Application puts Google's SslGuard first in the list of every process it runs in and ends when it cannot;
// with one list for all, its ":search" ended so whenever ":googleapp" had put SslGuard there before it.
package megler;

import android.app.ActivityManager;
import android.app.Application;
import android.content.Context;
import android.content.ContextWrapper;
import java.util.HashMap;

public final class Processes {
    private Processes() {}

    /** One of the app's other processes: its name, its class scope, its Application. */
    public static final class Scope {
        public final String name;
        final ClassLoader loader;
        Application app;
        /* What its Application threw as it was made: the process ended there, as it would on Android (make). */
        Throwable ended;
        final Context context;
        Scope(String name, ClassLoader loader) {
            this.name = name;
            this.loader = loader;
            this.context = new ProcessContext(this, ContextImpl.app());
        }
        public ClassLoader classLoader() { return loader; }
        public Application application() { return app; }
        /** The base context of an activity of this process (megler.AppRunner.make): one of its own, as an activity's is, with its
         *  own theme, and this process's class loader, Application and ActivityManager. */
        public Context activityBase() { return new ProcessContext(this, ContextImpl.forActivity()); }
    }

    /* The process the app's own scope runs as (own); null until megler.AppRunner names it (started). */
    private static volatile String sOwn;

    /** The Android process the app's own scope is (its class loader, its Application, the core's /proc/self): the process of the
     *  activity it was started for (started); the package's own before that, and for an app whose activities name none. */
    public static String own() {
        String o = sOwn;
        return o != null ? o : Native.packageName();
    }

    /**
     * The app runs as the process of `first`, the activity it is started for (its launcher activity, or the one another app asked
     * for): megler.AppRunner.run, before any of the app's code runs or its Application is made. Null: the package's own.
     */
    static void started(Manifest.Component first) {
        String process = first != null && apart() ? Manifest.processOf(first) : Native.packageName();
        sOwn = process;
        if (process.equals(Native.packageName())) return;
        Native.setProcessName(process);
        android.util.Log.i("Megler", "the app runs as the process " + process + ", its first activity's (" + first.name
            + "); its package's own process and its others are class scopes of their own");
    }

    /* Whether the app's processes run apart: not those of Google Play services' own package run as an app, whose processes
       are one here, as in every app that hosts them (megler.MicroG; megler.Broadcasts runs their receivers of every process). */
    private static boolean apart() { return !GooglePackages.GMS.equals(Native.packageName()); }

    /** The process of the code that asks, as Application.getProcessName tells it: the scope's of the closest of the app's classes
     *  up the stack (nameOf), else the own scope's. */
    public static String current() {
        String scoped = nameOf(dalvik.system.VMStack.getClosestUserClassLoader());
        return scoped != null ? scoped : own();
    }

    /** The scope of the process an activity of the app's runs in (megler.AppRunner.make): null for the own scope's, and for
     *  Google Play services' own package. */
    public static Scope ofActivity(Manifest.Component c) { return apart() ? of(c) : null; }

    /** The context a process's context is over (Scope.activityBase: an activity's own); any other context itself. */
    public static Context ownBase(Context c) { return c instanceof ProcessContext ? ((ProcessContext) c).getBaseContext() : c; }

    /** The Applications of the app's other processes made so far, for what the platform tells every one (megler.Screen's
     *  configuration, megler.AppRunner's trimMemory). */
    public static java.util.List<Application> applications() {
        java.util.ArrayList<Application> l = new java.util.ArrayList<>();
        for (Scope s : sNamed) if (s.app != null && s.ended == null) l.add(s.app);
        return l;
    }

    /**
     * What a component of a process that ended as its Application was made gets (of): on Android that process crashed as it
     * started, the component was never made, and the app's other processes go on.
     */
    public static final class Ended extends RuntimeException {
        Ended(Scope s, String component) {
            super("process " + s.name + " ended as its Application was made (" + s.ended + "), as it crashes on Android: " + component
                + " is not made", s.ended);
        }
    }

    /**
     * The platform has put its security providers in (AndroidKeyStore, the app's network security configuration's) and the
     * app's code is about to run (megler.AppRunner.run): another of the app's processes, made later, begins with the list of
     * providers as it is now, as a fresh process of Android's would. Each of them has a list of its own
     * (java.security.Security: core/libcore-overlay/sun/security/jca/Providers.java.members), which libcore keeps.
     */
    static void providersReady() {
        try {
            Class.forName("sun.security.jca.Providers").getMethod("meglerProcessStarted").invoke(null);
        } catch (ReflectiveOperationException | RuntimeException e) {
            android.util.Log.w("Megler", "the app's processes have one list of security providers between them: " + e);
        }
    }

    /*
     * The list of security providers of class scope `s` its own from now on, a fresh process's (providersReady), before any of
     * its code runs. The Google app's Application puts Google's SslGuard first in the list of every process it runs in, and
     * ends when a provider of that name is there: with one list for the whole app, its ":search" (Gemini's screens) ended so
     * whenever ":googleapp", its launcher's, was the app's own process, which had put SslGuard there first.
     */
    private static void ownProviders(Scope s) {
        int scope = s.loader instanceof ServiceClassLoader ? ((ServiceClassLoader) s.loader).scope() : 0;
        try {
            Class.forName("sun.security.jca.Providers").getMethod("meglerProcess", int.class).invoke(null, scope);
        } catch (ReflectiveOperationException | RuntimeException e) {
            android.util.Log.w("Megler", "process " + s.name + ": no list of security providers of its own (" + e + "); it has the app's");
        }
    }

    /* Each process's scope once made; null for one that could not be. */
    private static final HashMap<String, Scope> sScopes = new HashMap<>();
    /*
     * The scopes whose loaders nameOf knows, read without a lock: a scope is in it from before its Application is made, since
     * that Application asks its process's name in its onCreate (Play Store's chooses its graph of services by it, and built its
     * main process's in its background process while the scope was put in sScopes only after onCreate had returned).
     */
    private static volatile Scope[] sNamed = new Scope[0];

    /**
     * The name of the process whose scope class loader `l` is (or is under); null for the app's own process. The framework
     * asks it of the closest of the app's classes up the stack (VMStack.getClosestUserClassLoader): Play Store asks
     * ActivityThread.currentProcessName through reflection, whose caller is the boot class path's Method.invoke, and its
     * background process's Application was told the main process's name and built that process's graph of services
     * ("Cannot cast sum to rqc", package 552).
     */
    public static String nameOf(ClassLoader l) {
        Scope[] named = sNamed;
        if (l == null || named.length == 0) return null;
        for (ClassLoader at = l; at != null; at = at.getParent())
            for (Scope s : named)
                if (s.loader == at) return s.name;
        return null;
    }

    /**
     * Whose the threads of class scope `scope` are, for the log (Native.thrownInService says the scope): "process <name>'s" for
     * one of the app's other processes, and a service hosted here by its package. An uncaught exception of Facebook Lite's
     * ":fbns" was logged as that of "Google Play services' thread AppCreate".
     */
    public static String threadsOf(int scope) {
        ServiceClassLoader l = ServiceClassLoader.ofScope(scope);
        String process = nameOf(l);
        if (process != null) return "process " + process + "'s";
        if (l == null || MicroG.APK.equals(l.apk())) return "Google Play services'";
        if (Gsf.APK.equals(l.apk())) return "the Google Services Framework's";
        return "the hosted " + l.apk().substring(l.apk().lastIndexOf('/') + 1) + "'s";
    }

    private static void named(Scope s, boolean in) {
        synchronized (sScopes) {
            java.util.ArrayList<Scope> l = new java.util.ArrayList<>(java.util.Arrays.asList(sNamed));
            l.remove(s);
            if (in) l.add(s);
            sNamed = l.toArray(new Scope[0]);
        }
    }

    /** The scope of the process a component runs in, made the first time (on the main thread); null for the own scope's process
     *  (own: the package's, unless the app was started for an activity of another), and for one whose scope could not be made
     *  (the component then runs in the app's, as before). Ended, for a process that ended as its Application was made. */
    public static Scope of(Manifest.Component c) {
        String process = Manifest.processOf(c);
        if (process == null || process.equals(own())) return null;
        Scope s;
        synchronized (sScopes) {
            if (sScopes.containsKey(process)) {
                s = sScopes.get(process);
            } else {
                s = make(process);
                sScopes.put(process, s);
            }
        }
        if (s != null && s.ended != null) throw new Ended(s, c.name);
        return s;
    }

    private static Scope make(String process) {
        String apk = ContextImpl.app().getPackageCodePath();
        Scope s;
        try {
            // Its native libraries are the app's, those of the splits too (ServiceClassLoader's second argument).
            s = new Scope(process, new ServiceClassLoader(apk, ContextImpl.app().getApplicationInfo().nativeLibraryDir));
        } catch (Throwable t) {
            android.util.Log.w("Megler", "process " + process + ": no class scope of its own (" + t + "); its components run in the app's");
            return null;
        }
        String cls = Manifest.applicationClass != null ? Manifest.applicationClass : Application.class.getName();
        named(s, true);
        ownProviders(s);
        try {
            Application a = Application.class.getName().equals(cls) ? new Application()
                : (Application) s.loader.loadClass(cls).getDeclaredConstructor().newInstance();
            a.meglerAttach(s.context);
            s.app = a;
            android.util.Log.i("Megler", "process " + process + ": a class scope of its own, and its Application, " + cls);
            a.onCreate();
        } catch (Throwable t) {
            /*
             * On Android the process ends here: ActivityThread.handleBindApplication throws "Unable to create application"
             * (or "Unable to instantiate"), the process crashes before the activity, service or receiver it was started for
             * is made, and the app's other processes go on. So it ends here too: its components are not made from now on
             * (of: Ended), where they were made in it with its Application half made, or, when that was not made at all, in
             * the app's own scope with another process's: the Google app's ":search", whose Application could not put its
             * SslGuard first (0.1.0.739), had its Gemini screen made so, and the screen's failure ended the whole app.
             * Threads its Application started run on, there being one native process; the scope keeps its name for them.
             */
            s.ended = t;
            android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: main\nProcess: " + process + ", PID: " + android.os.Process.myPid(),
                new RuntimeException("Unable to " + (s.app == null ? "instantiate" : "create") + " application " + cls + ": " + t, t));
            android.util.Log.e("Megler", "process " + process + " ended as its Application was made, as it crashes on Android: its"
                + " activities and services are not made from now on, and the app's other processes go on");
        }
        return s;
    }

    /**
     * The intent for code of class loader `l`: itself when its extras hold nothing of another class scope's, else a copy whose
     * extras' objects of other scopes -- Parcelables, in arrays, lists and bundles too -- are made again by `l`'s classes of
     * their names, through a Parcel (android.os.Parcel.readParcelable adapts an object to a loader), as a Parcel between two
     * processes makes them. An object `l` has no class of is left as it is.
     */
    public static android.content.Intent forLoader(android.content.Intent i, ClassLoader l) {
        if (i == null || l == null) return i;
        android.os.Bundle extras = i.getExtras();
        if (extras == null) return i;
        android.os.Bundle adapted = adapt(extras, l);
        if (adapted == extras) return i;
        android.content.Intent copy = new android.content.Intent(i);
        copy.replaceExtras(adapted);
        return copy;
    }

    /**
     * Bundle.setClassLoader: the bundle's own values of another class scope -- Parcelables, in arrays, lists and bundles too --
     * made again by `l`'s classes of their names, in place, as Android unparcels a bundle with the loader it is given.
     */
    public static void adaptBundle(android.os.Bundle b, ClassLoader l) {
        if (b == null || l == null) return;
        for (String key : new java.util.ArrayList<>(b.keySet())) {
            Object v = b.get(key), a = adaptValue(v, l);
            if (a != v) putValue(b, key, a);
        }
    }

    /* The bundle with its foreign objects made again, or itself when it has none. */
    private static android.os.Bundle adapt(android.os.Bundle b, ClassLoader l) {
        android.os.Bundle out = null;
        for (String key : b.keySet()) {
            Object v = b.get(key), a = adaptValue(v, l);
            if (a == v) continue;
            if (out == null) out = new android.os.Bundle(b);
            putValue(out, key, a);
        }
        return out != null ? out : b;
    }

    private static Object adaptValue(Object v, ClassLoader l) {
        if (v instanceof android.os.Bundle) return adapt((android.os.Bundle) v, l);
        if (v instanceof android.os.Parcelable) return foreign(v, l) ? adaptParcelable((android.os.Parcelable) v, l) : v;
        if (v instanceof android.os.Parcelable[]) {
            android.os.Parcelable[] a = (android.os.Parcelable[]) v, c = null;
            for (int k = 0; k < a.length; k++) {
                Object n = adaptValue(a[k], l);
                if (n != a[k]) { if (c == null) c = a.clone(); c[k] = (android.os.Parcelable) n; }
            }
            return c != null ? c : v;
        }
        if (v instanceof java.util.ArrayList) {
            java.util.ArrayList<?> a = (java.util.ArrayList<?>) v;
            java.util.ArrayList<Object> c = null;
            for (int k = 0; k < a.size(); k++) {
                Object n = adaptValue(a.get(k), l);
                if (n != a.get(k)) { if (c == null) c = new java.util.ArrayList<>(a); c.set(k, n); }
            }
            return c != null ? c : v;
        }
        return v;
    }

    /* Whether an object's class is of another app scope than `l`'s (the platform's classes are every scope's). */
    private static boolean foreign(Object v, ClassLoader l) {
        ClassLoader own = v.getClass().getClassLoader();
        return own != null && own != l && own != Object.class.getClassLoader() && own != android.app.Activity.class.getClassLoader();
    }

    private static Object adaptParcelable(android.os.Parcelable v, ClassLoader l) {
        android.os.Parcel p = android.os.Parcel.obtain();
        try {
            p.writeParcelable(v, 0);
            p.setDataPosition(0);
            Object o = p.readParcelable(l);
            return o != null ? o : v;
        } catch (Throwable t) {
            return v;
        } finally {
            p.recycle();
        }
    }

    @SuppressWarnings("unchecked")
    private static void putValue(android.os.Bundle b, String key, Object v) {
        if (v instanceof android.os.Bundle) b.putBundle(key, (android.os.Bundle) v);
        else if (v instanceof android.os.Parcelable) b.putParcelable(key, (android.os.Parcelable) v);
        else if (v instanceof android.os.Parcelable[]) b.putParcelableArray(key, (android.os.Parcelable[]) v);
        else if (v instanceof java.util.ArrayList) b.putParcelableArrayList(key, (java.util.ArrayList<android.os.Parcelable>) v);
    }

    /** The context of a process's scope: the app's (or an activity's base, Scope.activityBase), but for the class loader, the
     *  Application and the ActivityManager. */
    private static final class ProcessContext extends ContextWrapper {
        private final Scope scope;
        private ActivityManager activities;
        ProcessContext(Scope scope, Context base) { super(base); this.scope = scope; }
        @Override public ClassLoader getClassLoader() { return scope.loader; }
        @Override public Context getApplicationContext() { return scope.app != null ? scope.app : this; }
        @Override public Object getSystemService(String name) {
            if (!Context.ACTIVITY_SERVICE.equals(name)) return super.getSystemService(name);
            synchronized (this) {
                if (activities == null) activities = new ProcessActivityManager(scope.name);
                return activities;
            }
        }
    }

    /** An ActivityManager that has the process running with this pid named as the scope's. */
    private static final class ProcessActivityManager extends ActivityManager {
        private final String process;
        ProcessActivityManager(String process) { this.process = process; }
        @Override public java.util.List<RunningAppProcessInfo> getRunningAppProcesses() {
            java.util.ArrayList<RunningAppProcessInfo> l = new java.util.ArrayList<>();
            RunningAppProcessInfo i = new RunningAppProcessInfo(process, android.os.Process.myPid(), new String[] { Native.packageName() });
            i.importance = RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
            l.add(i);
            return l;
        }
    }
}
