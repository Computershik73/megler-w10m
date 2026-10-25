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
