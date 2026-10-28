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
// is; its context is the app's, but for its class loader, its Application and its ActivityManager. The process's providers,
// receivers and activities still run in the app's own scope.
//
// An intent that goes from one scope to another -- a broadcast, a service's start or binding -- has the objects among its
// extras made again by the receiving scope's classes (forLoader), as Android's parcelling of it between processes makes
// them: Play Store's download service broadcasts its progress with a parcelled proto of its own scope's class, and the main
// process's receiver ended casting it to its own ("ProtoParsers$InternalDontUse cannot be cast to
// ProtoParsers$InternalDontUse").
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
        final Context context;
        Scope(String name, ClassLoader loader) {
            this.name = name;
            this.loader = loader;
            this.context = new ProcessContext(this);
        }
        public ClassLoader classLoader() { return loader; }
        public Application application() { return app; }
    }

    /* Each process's scope once made; null for one that could not be. */
    private static final HashMap<String, Scope> sScopes = new HashMap<>();

    /** The scope of the process a component runs in, made the first time (on the main thread); null for the app's own process,
     *  and for one whose scope could not be made (the component then runs in the app's, as before). */
    public static Scope of(Manifest.Component c) {
        String process = Manifest.processOf(c);
        if (process == null || process.equals(Native.packageName())) return null;
        Scope s;
        synchronized (sScopes) {
            if (sScopes.containsKey(process)) return sScopes.get(process);
            s = make(process);
            sScopes.put(process, s);
        }
        return s;
    }

    private static Scope make(String process) {
        String apk = ContextImpl.app().getPackageCodePath();
        Scope s;
        try {
            s = new Scope(process, new ServiceClassLoader(apk));
        } catch (Throwable t) {
            android.util.Log.w("Megler", "process " + process + ": no class scope of its own (" + t + "); its components run in the app's");
            return null;
        }
        String cls = Manifest.applicationClass != null ? Manifest.applicationClass : Application.class.getName();
        try {
            Application a = Application.class.getName().equals(cls) ? new Application()
                : (Application) s.loader.loadClass(cls).getDeclaredConstructor().newInstance();
            a.meglerAttach(s.context);
            s.app = a;
            android.util.Log.i("Megler", "process " + process + ": a class scope of its own, and its Application, " + cls);
            a.onCreate();
        } catch (Throwable t) {
            android.util.Log.e("Megler", "process " + process + ": its Application failed", t);
            if (s.app == null) return null;
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

    /** The context of a process's scope: the app's, but for the class loader, the Application and the ActivityManager. */
    private static final class ProcessContext extends ContextWrapper {
        private final Scope scope;
        private ActivityManager activities;
        ProcessContext(Scope scope) { super(ContextImpl.app()); this.scope = scope; }
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
