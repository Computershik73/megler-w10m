// Overlay of core/framework/src/megler/AppRunner.java (tools/build-framework.sh). It differs in one thing:
// startActivity with FLAG_ACTIVITY_CLEAR_TOP for an activity already in the stack does what Android does. The activities
// above it go; then, when its launch mode is standard and the intent has no FLAG_ACTIVITY_SINGLE_TOP, it is finished too
// and started again with the new intent, and otherwise the intent comes to it through onNewIntent. The original kept the
// old one, gave it the intent by setIntent alone, and called its onActivityResult(-1, RESULT_CANCELED, null), which
// Android never does. Instagram's signed-out screen, started again so, took that for its flow cancelled and finished,
// and with no activity left the app ended (status 0). The activity is found by its class, as Android compares
// realActivity: one started through an <activity-alias> is found by an intent that names the activity itself, where the
// original compared the names they were started as.
//
// And in one more: a media key from the phone (its media controls: the lock screen's and the volume flyout's play and
// pause) goes to the app's active media session first, as Android hands it to the session rather than to the window
// (android.media.session.MediaSession.meglerMediaKey); an app with none gets it as any other key.
//
// And an intent to install an APK (ACTION_INSTALL_PACKAGE, or ACTION_VIEW of an APK, naming no activity) goes to Megler's
// installer first (megler.Installs), as Android hands it to its package installer, rather than to an activity of the app's
// that takes the content or file scheme. And an intent for an activity of another app the phone has goes to the shell,
// which starts that app (megler.PhoneApps). Started for a result, its result comes back to the caller as the caller is in
// front again (megler.AppResults): Play Store hears so that microG's sign-in, in microG's app, added the account.
//
// And the app's components are made by the factory its manifest names (android:appComponentFactory), as Android 9's
// ActivityThread makes them: the factory is made with the app's class loader and asked for the class loader first
// (instantiateClassLoader), then for the Application, the content providers, the activities, and the services and receivers
// (megler.Services, megler.Broadcasts). They had been made from their classes directly. Facebook's factory puts its own
// class loaders in place when asked for the class loader, and holds back each component until the code it loads after the
// start is there (most of its classes are in a second DEX file it unpacks itself); made directly, its first activity was
// not found yet and the app ended at once (ClassNotFoundException: com.facebook.katana.activity.FbMainTabActivity).
//
// And the screens of Android's Settings an app opens -- the accounts, the app's info -- are Megler's own (megler.SystemScreens),
// shown on top of the app as Android shows the Settings app's: they had opened nothing.
//
// Everything else is the original's, unchanged.
package megler;

import android.app.Activity;
import android.app.AppComponentFactory;
import android.app.Application;
import android.content.*;
import android.content.pm.ProviderInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.ArrayList;

/**
 * Runs the app as ActivityThread would: the Application (with its content providers first), then the launcher activity, then the
 * main looper; activities started later go on a stack and come back off it as they finish. Input from Megler is delivered here.
 */
public final class AppRunner {
    private static final ArrayList<Activity> sStack = new ArrayList<>();
    private static Application sApp;
    private static Handler sHandler;
    private static final int[] ids = new int[16];
    private static final float[] xs = new float[16], ys = new float[16];
    private static int down;
    private static long downTime;
    private static boolean sPausedByHost;

    private static ClassLoader sLoader;
    /** The app's class loader, as Android gives every app: a PathClassLoader over its APK and splits. Its classes say they come from
     *  it (Class.getClassLoader), and resources in the APKs are found through it. */
    public static ClassLoader classLoader() { return sLoader != null ? sLoader : AppRunner.class.getClassLoader(); }
    private static void installClassLoader() {
        try {
            StringBuilder path = new StringBuilder(Native.apkPath());
            String[] splits = Native.splitPaths();
            if (splits != null) for (String sp : splits) path.append(java.io.File.pathSeparatorChar).append(sp);
            sLoader = (ClassLoader) Class.forName("dalvik.system.PathClassLoader").getConstructor(String.class, String.class, ClassLoader.class)
                    .newInstance(path.toString(), android.content.pm.ApplicationInfo.self().nativeLibraryDir, AppRunner.class.getClassLoader());
            Native.setAppClassLoader(sLoader);
            Thread.currentThread().setContextClassLoader(sLoader);
        } catch (Throwable t) { android.util.Log.w("Megler", "the app's class loader: " + t); }
    }

    private static AppComponentFactory sFactory = AppComponentFactory.DEFAULT;
    /** The factory the app's components are made by: its manifest's android:appComponentFactory, else the platform's. */
    public static AppComponentFactory componentFactory() { return sFactory; }
    /**
     * As LoadedApk makes the app's class loader: the manifest's factory made with the app's own class loader, and asked for the
     * class loader the app is to have (instantiateClassLoader), which is then the one the app's contexts give.
     */
    private static void installComponentFactory() {
        String name = Manifest.appComponentFactory;
        if (name == null) return;
        try {
            AppComponentFactory f = (AppComponentFactory) Class.forName(name, true, classLoader()).getDeclaredConstructor().newInstance();
            ClassLoader cl = f.instantiateClassLoader(classLoader(), android.content.pm.ApplicationInfo.self());
            sFactory = f;
            if (cl != null && cl != sLoader) {
                sLoader = cl;
                Thread.currentThread().setContextClassLoader(cl);
            }
            android.util.Log.i("Megler", "the app's components are made by its factory " + name);
        } catch (Throwable t) {
            android.util.Log.w("Megler", "the app's component factory " + name + " could not be made; the platform's makes them", t);
        }
    }

    public static void run(String activityClass, String applicationClass) throws Exception {
        android.webkit.MimeTypeMap.meglerInstallDefault();
        KeyStoreProvider.install();
        Looper.prepareMainLooper();
        sHandler = new Handler(Looper.getMainLooper());
        /* as RuntimeInit: every exception nothing caught is logged before any handler has it (its LoggingHandler, the pre-handler
           Thread.dispatchUncaughtException calls first), as the handler an app sets may end the process without a word -- Weather
           Channel's (Taboola's, under Chromium's) calls System.exit(0); and there is always a default handler (apps wrap it and
           call through). Android's kills the app; this one lets the thread end, as Megler did before. */
        UncaughtLogger uncaught = new UncaughtLogger();
        try { Thread.class.getMethod("setUncaughtExceptionPreHandler", Thread.UncaughtExceptionHandler.class).invoke(null, uncaught.logging); }
        catch (ReflectiveOperationException e) { android.util.Log.w("Megler", "no pre-handler for uncaught exceptions: " + e); }
        if (Thread.getDefaultUncaughtExceptionHandler() == null) Thread.setDefaultUncaughtExceptionHandler(uncaught);
        Manifest.read();
        installClassLoader();
        installComponentFactory();
        ContextImpl base = ContextImpl.app();
        String appCls = applicationClass != null ? applicationClass : Manifest.applicationClass;
        Application app;
        try { app = sFactory.instantiateApplication(classLoader(), appCls != null ? appCls : Application.class.getName()); }
        catch (ClassNotFoundException e) { android.util.Log.w("Megler", "no application class " + appCls); app = new Application(); }
        sApp = app;
        Application.meglerSet(app);
        ContextImpl.setApplication(app);
        app.meglerAttach(base);
        installProviders(app);
        Looper.setInputSink(AppRunner::deliver);
        applyInsets();
        app.onCreate();
        Syncs.started();
        Manifest.Component launcher = Manifest.activity(activityClass);
        Intent main = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        // started through the launcher's alias, the activity's component is the alias (Android compares that, not the class, when
        // the app later starts the target by name: LinkedIn's MainActivity starts a new one of itself and finishes)
        Manifest.Component entry = Manifest.launcher();
        String startedAs = entry != null && "activity-alias".equals(entry.kind) && activityClass.equals(entry.parentActivity) ? entry.name : activityClass;
        main.setComponent(new ComponentName(Native.packageName(), startedAs));
        main.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        launch(activityClass, main, null, -1, launcher);
        Looper.loop();
    }
    public static Application application() { return sApp; }
    /* Android's LoggingHandler: FATAL EXCEPTION, the thread, the process and the stack. A thread of Google Play services' ends
       alone, as GmsCore's own process would on Android: as the pre-handler runs before any other, it gives the thread a handler
       of its own that says so and goes no further. MicroG's default one (ServiceCrashes) did that, but an app's handler made
       default later sits over it: Weather Channel's Taboola handler called System.exit(0) when a thread of GmsCore's crashed. */
    static final class UncaughtLogging implements Thread.UncaughtExceptionHandler {
        volatile Throwable logged;
        public void uncaughtException(Thread t, Throwable e) {
            if (t != Looper.getMainLooper().getThread() && Native.thrownInService(e) != 0) {
                t.setUncaughtExceptionHandler(SERVICE_THREAD_ENDS);
                return;
            }
            logged = e;
            android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName() + "\nProcess: " + Native.packageName() + ", PID: " + android.os.Process.myPid(), e);
        }
    }
    static final Thread.UncaughtExceptionHandler SERVICE_THREAD_ENDS = (t, e) ->
        android.util.Log.e("Megler", "Google Play services' thread " + t.getName() + " ended with an uncaught exception; on Android its process would, and the app goes on", e);
    static final class UncaughtLogger implements Thread.UncaughtExceptionHandler {
        final UncaughtLogging logging = new UncaughtLogging();
        public void uncaughtException(Thread t, Throwable e) {
            if (logging.logged != e) logging.uncaughtException(t, e);   // as KillApplicationHandler's ensureLogging
            if (t == Looper.getMainLooper().getThread()) { if (e instanceof RuntimeException) throw (RuntimeException) e; if (e instanceof Error) throw (Error) e; throw new RuntimeException(e); }
        }
    }

    private static void installProviders(Application app) {
        /* as ActivityThread: providers with a higher initOrder first (Reddit builds its object graph in one that the startup
           initializers then wait for), the rest in manifest order */
        java.util.ArrayList<Manifest.Component> ordered = new java.util.ArrayList<>(Manifest.providers);
        java.util.Collections.sort(ordered, (x, y) -> Integer.compare(y.initOrder, x.initOrder));
        for (Manifest.Component p : ordered) {
            if (!p.enabled) continue;
            try {
                ContentProvider cp = sFactory.instantiateProvider(classLoader(), p.name);
                ProviderInfo info = new ProviderInfo();
                info.name = p.name; info.authority = p.authorities; info.packageName = Native.packageName(); info.exported = p.exported; info.metaData = p.metaData; info.grantUriPermissions = p.grantUriPermissions;
                info.applicationInfo = app.getApplicationInfo();
                // As the manifest says them: android:readPermission and writePermission, else android:permission for both
                // (Android's SearchIndexablesProvider refuses to start without READ_SEARCH_INDEXABLES there).
                info.readPermission = p.readPermission != null ? p.readPermission : p.permission;
                info.writePermission = p.writePermission != null ? p.writePermission : p.permission;
                cp.attachInfo(app, info);
                ContentResolver.register(p.authorities, cp);
            } catch (Throwable t) {
                android.util.Log.w("Megler", "content provider " + p.name + " did not start", t);
            }
        }
    }

    // ---- activities
    public static int stackSize() { return sStack.size(); }
    public static Activity top() { return sStack.isEmpty() ? null : sStack.get(sStack.size() - 1); }
    /**
     * Activity.isTaskRoot, as Android answers it: the lowest activity of the task that is not finishing (the lowest of all
     * when every one is). Not "started by none": Instagram's launcher activity starts its main activity and finishes, and
     * the main activity, root then, finishes itself in onCreate unless it is ("is not the root").
     */
    public static boolean isRoot(Activity a) {
        for (Activity b : sStack) if (!b.isFinishing()) return b == a;
        return !sStack.isEmpty() && sStack.get(0) == a;
    }
    public static CharSequence appLabel(Context c) {
        try { if (Manifest.appLabel != 0) return c.getResources().getText(Manifest.appLabel); } catch (Exception e) {}
        return Native.packageName();
    }
    static Activity launch(String cls, Intent intent, Activity caller, int request, Manifest.Component info) throws Exception {
        return launch(cls, intent, caller, request, info, null);
    }
    /** An activity made and started; `state` the one a recreated activity saved, given to onCreate and to onRestoreInstanceState. */
    static Activity launch(String cls, Intent intent, Activity caller, int request, Manifest.Component info, Bundle state) throws Exception {
        android.util.Log.d("Megler", "launch " + cls);
        Activity prev = top();
        // Megler's screens of Android's Settings are the system's, not the app's: its factory does not make them. Google Play
        // services' activities hosted here are GmsCore's, made in its scope on its context with its Application (MicroG).
        boolean gms = MicroG.hosts(info);
        Activity a = SystemScreens.isScreen(cls) ? (Activity) Class.forName(cls).newInstance()
                : gms ? MicroG.newActivity(cls) : sFactory.instantiateActivity(classLoader(), cls, intent);
        Context base = gms ? MicroG.activityBase() : ContextImpl.forActivity();
        a.meglerAttach(base, gms ? MicroG.activityApplication() : sApp, intent, info, caller, request);
        if (prev != null && prev.isResumedMegler()) prev.meglerPause();
        sStack.add(a);
        a.meglerCreate(state);
        if (!a.isFinishing()) { a.meglerStart(); if (state != null) a.meglerRestoreState(state); a.meglerResume(); }
        if (prev != null && !a.isFinishing()) {
            final Activity p = prev;
            // the one below stops once the new one covers it (a translucent or floating window leaves it visible, as Android's
            // ActivityInfo.isTranslucentOrFloating): TikTok Lite's consent, a translucent activity, took its feed's window away,
            // and the screen behind it was black, then all black once its own view had gone (package 619)
            if (!a.getWindow().isFloating() && !a.getWindow().meglerIsTranslucent()) p.meglerStop();
        }
        return a;
    }
    /**
     * An activity run inside another, as android.app.LocalActivityManager runs one (a TabActivity's tab, an ActivityGroup's
     * child): made by the app's factory and attached as AppRunner.launch does, its group its parent, and not on the stack --
     * its group is. Null when the app has no such activity. Its lifecycle is the manager's to move.
     */
    public static Activity makeChild(Activity parent, Intent intent) throws Exception {
        Manifest.Component info = Manifest.resolve(intent);
        if (info == null) {
            android.util.Log.w("Megler", "no activity for the child " + intent);
            return null;
        }
        // (An <activity-alias> runs the activity it names, as launch does.)
        String cls = "activity-alias".equals(info.kind) && info.parentActivity != null ? info.parentActivity : info.name;
        android.util.Log.d("Megler", "child " + cls + " of " + parent.getClass().getName());
        Activity a = sFactory.instantiateActivity(classLoader(), cls, intent);
        a.meglerSetParent(parent);
        a.meglerAttach(ContextImpl.forActivity(), sApp, intent, info, null, -1);
        return a;
    }
    /** The component an activity was started as: its class, or the alias it was reached through. */
    private static String startedAs(Activity a) {
        Intent i = a.getIntent();
        return i != null && i.getComponent() != null ? i.getComponent().getClassName() : a.getClass().getName();
    }
    /**
     * An activity another app asked this one for (megler_start_activity: microG's sign-in, which Play Store's AccountManager
     * asks for), from the main loop: started as Android starts an intent that names a component of this app -- if the
     * manifest has the activity and exports it, as Android requires of a component another app starts.
     */
    public static void startRequested(String cls) {
        Manifest.Component c = Manifest.activity(cls);
        if (c == null || !c.exported) {
            android.util.Log.w("Megler", "another app asked for " + cls + ", which this app " + (c == null ? "does not have" : "does not export"));
            return;
        }
        Context from = sApp != null ? sApp : ContextImpl.app();
        Intent i = new Intent().setClassName(Native.packageName(), cls).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        android.util.Log.i("Megler", "another app asked for " + cls);
        AppResults.requested(cls);
        startActivity(from, i, -1, null);
    }
    /** Context.startActivity: an activity of the app's own, or something the host can open (a web page, mail, the store). */
    public static void startActivity(Context from, Intent i, int request, Bundle opts) {
        // FLAG_ACTIVITY_FORWARD_RESULT: the activity started takes this one's place as its caller's, as on Android -- its result
        // goes to whoever started this one, which gives its own up. GmsCore's UnpackingRedirectActivity starts the sign-in so,
        // and finishes.
        if ((i.getFlags() & Intent.FLAG_ACTIVITY_FORWARD_RESULT) != 0 && request < 0 && from instanceof Activity && ((Activity) from).meglerCaller() != null) {
            Activity a = (Activity) from;
            Activity caller = a.meglerCaller();
            int forwarded = a.meglerRequestCode();
            a.meglerForwardedResult();
            startActivity(caller, new Intent(i).setFlags(i.getFlags() & ~Intent.FLAG_ACTIVITY_FORWARD_RESULT), forwarded, opts);
            return;
        }
        // An APK to install (a file manager's, a browser's download, an app store's): Megler's installer has it, as Android's
        // package installer would (megler.Installs). Asked for a result, the caller hears RESULT_OK: the APK went.
        // An app to uninstall (ACTION_DELETE of package:<name>), likewise: Megler asks the user.
        if (megler.Installs.isUninstall(i)) {
            android.util.Log.d("Megler", "startActivity " + i + " -> Megler's uninstaller");
            megler.Installs.uninstall(i.getData().getSchemeSpecificPart(), (android.content.IntentSender) null);
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            if (request >= 0 && caller != null) sHandler.post(() -> caller.meglerActivityResult(request, Activity.RESULT_OK, null));
            return;
        }
        if (megler.Installs.isInstall(i)) {
            android.util.Log.d("Megler", "startActivity " + i + " -> Megler's installer");
            megler.Installs.install(from, i);
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            if (request >= 0 && caller != null) sHandler.post(() -> caller.meglerActivityResult(request, Activity.RESULT_OK, null));
            return;
        }
        // An activity of Google Play services, which this app hosts (megler.MicroG.activity): it runs here, in GmsCore's class
        // scope and with its context, as GmsCore's services do -- its sign-in hands its screens objects of this process.
        final Manifest.Component hosted = MicroG.activity(i);
        if (hosted != null) {
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            final Intent intent = new Intent(i);
            if (intent.getComponent() == null) intent.setComponent(new ComponentName(GooglePackages.GMS, hosted.name));
            final String cls = "activity-alias".equals(hosted.kind) && hosted.parentActivity != null ? hosted.parentActivity : hosted.name;
            android.util.Log.d("Megler", "startActivity " + i + " -> Google Play services' " + cls + ", here");
            sHandler.post(() -> {
                try { launch(cls, intent, caller, request, hosted); }
                catch (Throwable t) { android.util.Log.e("Megler", "starting " + cls + " failed", t); }
            });
            return;
        }
        // An activity of another app the phone has (a launcher's tap, an app opening another): the shell starts that app
        // (megler.PhoneApps), as Android starts another app's activity in its own process. Asked for a result, the caller
        // has it when it is in front again (megler.AppResults).
        String[] other = megler.PhoneApps.target(i);
        if (other != null) {
            if (request >= 0) AppResults.await(from instanceof Activity ? (Activity) from : top(), request, other[0], other[1]);
            megler.PhoneApps.start(i);
            return;
        }
        Manifest.Component c = Manifest.resolve(i);
        android.util.Log.d("Megler", "startActivity " + i + (c != null ? " -> " + c.name : " (not an activity of this app)"));
        if (c != null) {
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            final Intent intent = new Intent(i);
            if (intent.getComponent() == null) intent.setComponent(new ComponentName(Native.packageName(), c.name));
            sHandler.post(() -> {
                try {
                    if ((intent.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0) {
                        // the activity itself, as Android compares realActivity: one started through an <activity-alias> is its target's
                        String real = "activity-alias".equals(c.kind) && c.parentActivity != null ? c.parentActivity : c.name;
                        // not one that is finishing, as Android passes over those: Instagram's main activity, finished in
                        // its onCreate, was started again (onRestart, onStart) and failed for the state it never made
                        for (int k = sStack.size() - 1; k >= 0; k--) if (!sStack.get(k).isFinishing() && sStack.get(k).getClass().getName().equals(real)) {
                            Activity existing = sStack.get(k);
                            while (top() != existing) finishNow(top(), false);
                            if ((intent.getFlags() & 0x20000000 /* SINGLE_TOP */) != 0 || c.launchMode != 0) {
                                // singleTop, singleTask, singleInstance, or asked for: the intent comes to it, as Android brings it
                                // back -- onRestart and onStart when it was stopped, onNewIntent, then onResume
                                boolean up = existing.isResumedMegler();
                                if (!up && !existing.meglerVisible()) { existing.meglerRestart(); existing.meglerStart(); }
                                existing.meglerNewIntent(intent);
                                if (!up) existing.meglerResume();
                                return;
                            }
                            // standard: it goes, and the activity is started again with this intent (below)
                            finishNow(existing, false);
                            break;
                        }
                    }
                    if ((intent.getFlags() & 0x20000000 /* SINGLE_TOP */) != 0 || c.launchMode == 1) {
                        Activity t = top();
                        if (t != null && !t.isFinishing() && startedAs(t).equals(intent.getComponent().getClassName())) { t.meglerNewIntent(intent); return; }
                    }
                    // an <activity-alias> starts the activity it names; the intent keeps the alias as its component, as on Android
                    launch("activity-alias".equals(c.kind) && c.parentActivity != null ? c.parentActivity : c.name, intent, caller, request, c);
                } catch (Throwable t) {
                    android.util.Log.e("Megler", "starting " + c.name + " failed", t);
                    if (t instanceof RuntimeException) throw (RuntimeException) t;
                    throw new RuntimeException(t);
                }
            });
            return;
        }
        android.net.Uri u = i.getData();
        String action = i.getAction();
        if (u != null && (Intent.ACTION_VIEW.equals(action) || Intent.ACTION_SENDTO.equals(action) || action == null || "android.intent.action.DIAL".equals(action))) {
            String s = u.toString();
            if (s.startsWith("market://")) s = "https://play.google.com/store/apps/" + s.substring(9);
            Native.openUrl(s);
            return;
        }
        if (Intent.ACTION_SEND.equals(action)) {
            String text = i.getStringExtra(Intent.EXTRA_TEXT);
            if (text != null) Native.share(text);
            return;
        }
        if ("android.intent.action.CHOOSER".equals(action)) { Intent inner = i.getParcelableExtra("android.intent.extra.INTENT"); if (inner != null) startActivity(from, inner, request, opts); return; }
        // A screen of Android's Settings that Megler has (megler.SystemScreens): shown on top of the app.
        final String screen = SystemScreens.screenFor(i);
        if (screen != null) {
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            final Intent intent = new Intent(i);
            android.util.Log.d("Megler", "startActivity " + i + " -> Megler's " + screen);
            sHandler.post(() -> {
                try { launch(screen, intent, caller, request, SystemScreens.component(screen)); }
                catch (Throwable t) { android.util.Log.e("Megler", "starting " + screen + " failed", t); }
            });
            return;
        }
        /* a screen of the system's (the account chooser, Settings pages, the permission dialog): Megler has none of them -- asked for a
           result, the caller hears the user backed out, as it would on a phone; otherwise nothing opens */
        String pkg = i.getComponent() != null ? i.getComponent().getPackageName() : i.getPackage();
        boolean system = "android".equals(pkg) || "com.android.settings".equals(pkg) || "com.android.permissioncontroller".equals(pkg)
            || (action != null && (action.startsWith("android.settings.") || action.startsWith("android.provider.Settings")));
        if (system) {
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            if (request >= 0 && caller != null) sHandler.post(() -> caller.meglerActivityResult(request, Activity.RESULT_CANCELED, null));
            return;
        }
        throw new ActivityNotFoundException("No Activity found to handle " + i);
    }
    /** Activity.finish: the one below comes back (with the result when it asked for one). */
    public static void finish(Activity a) { sHandler.post(() -> finishNow(a, true)); }
    private static void finishNow(Activity a, boolean resumeBelow) {
        android.util.Log.d("Megler", "finish " + a.getClass().getName());
        int idx = sStack.indexOf(a);
        if (idx < 0) return;
        boolean wasTop = idx == sStack.size() - 1;
        if (a.isResumedMegler()) a.meglerPause();
        sStack.remove(idx);
        AppResults.finished(a);
        Activity below = top();
        // (one below that is finishing too stays down: it goes when its own finish comes)
        boolean back = wasTop && below != null && resumeBelow && !below.isFinishing();
        if (back) {
            a.meglerHideFinished();
            Activity caller = a.meglerCaller();
            if (caller != null && a.meglerRequestCode() >= 0 && caller == below) below.meglerActivityResult(a.meglerRequestCode(), a.meglerResultCode(), a.meglerResultData());
            if (!below.isResumedMegler()) {
                if (!below.meglerVisible()) { below.meglerRestart(); below.meglerStart(); }
                below.meglerResume();
            }
            endWhenShown(a);
            return;
        }
        a.meglerStop();
        a.meglerDestroy();
        exitWhenLast();
    }
    /** The app ends with its last activity, unless one it started just before (finish(); startActivity(...)) is still to come. */
    private static void exitWhenLast() {
        if (sStack.isEmpty()) sHandler.post(() -> { if (sStack.isEmpty()) { endPending(); Native.exit(); } });
    }
    /*
     * A finished activity whose place the one below took (finishNow): hidden at once, and stopped and destroyed once the one
     * below has been drawn and the main thread has nothing left to do, as Android stops a finishing activity only when the
     * next one is visible, at its thread's idle report -- ten seconds at most. What the activity began as it finished goes
     * on until then: Pyaterochka's sign-in (an OIDC library's HandleRedirectActivity) saves the address it was sent back to
     * from a coroutine of its lifecycleScope, through DataStore, and finishes; destroyed in the same message, the coroutine
     * was cancelled before DataStore ran its edit on the main thread, nothing was saved, and the app's sign-in said "No
     * responseUri present" (package 576).
     */
    private static final ArrayList<Runnable> sEnding = new ArrayList<>();
    private static void endWhenShown(final Activity a) {
        final Runnable end = new Runnable() {
            private boolean mDone;
            @Override public void run() {
                if (mDone) return;
                mDone = true;
                sEnding.remove(this);
                sHandler.removeCallbacks(this);
                a.meglerStop();
                a.meglerDestroy();
                exitWhenLast();
            }
        };
        sEnding.add(end);
        android.view.Choreographer.getMainThreadInstance().postCallback(android.view.Choreographer.CALLBACK_COMMIT,
            () -> Looper.myQueue().addIdleHandler(() -> { end.run(); return false; }), null);
        sHandler.postDelayed(end, 10_000);
    }
    /** The finished activities not stopped yet, stopped and destroyed now: before the app ends. */
    private static void endPending() { for (Runnable r : new ArrayList<>(sEnding)) r.run(); }
    public static void finishAll() { sHandler.post(() -> { while (!sStack.isEmpty()) finishNow(top(), false); endPending(); }); }
    /** Activity.finishAffinity(): the activity and those under it in the task go; activities it started, above it, stay. */
    public static void finishAffinity(Activity a) {
        sHandler.post(() -> {
            int i = sStack.indexOf(a);
            if (i < 0) return;
            for (int k = i; k >= 0; k--) if (k < sStack.size()) finishNow(sStack.get(k), sStack.size() - 1 == k);
        });
    }
    /*
     * Activity.recreate, as Android does it: the same component made again (its manifest's, not the class the app's component
     * factory made of it) with the same intent, given the state the old one saved after it stopped. It made the old one's
     * class again with no state: Facebook's factory makes its splash screen's class for its main activity until the app is
     * ready and then recreates that activity to have the real one made -- and got its splash screen again, for good.
     */
    public static void recreate(Activity a) {
        sHandler.post(() -> {
            int idx = sStack.indexOf(a);
            if (idx < 0) return;
            String component = a.getComponentName().getClassName();
            // One started through an <activity-alias> is its target made again, the intent keeping the alias, as on Android: Play
            // Store's main activity, recreated for new server flags, was looked for as the class com.android.vending.AssetBrowserActivity,
            // its launcher's alias, and the app ended on a ClassNotFoundException (package 570).
            Manifest.Component c = Manifest.activity(component);
            String cls = c != null && "activity-alias".equals(c.kind) && c.parentActivity != null ? c.parentActivity : component;
            android.util.Log.d("Megler", "recreate " + a.getClass().getName() + " as " + component);
            a.meglerChanging(true);
            if (a.isResumedMegler()) a.meglerPause();
            a.meglerStop();
            Bundle state = a.meglerSaveState();
            a.meglerDestroy();
            sStack.remove(idx);
            try { launch(cls, a.getIntent(), a.meglerCaller(), a.meglerRequestCode(), c, state); }
            catch (Exception e) { throw new RuntimeException(e); }
        });
    }

    // ---- input from Megler
    /** events: [n, then n x (phase, id, xbits, ybits), optional n x (timeLow, timeHigh)] -- phase 0 down, 1 move, 2 up, 3 cancel; 4 key (id = code, x = down); 5 back; 6 host pause (id 1) / resume (0); 7 text waiting; 8 insets changed; 9 web view events waiting */
    private static void deliver(int[] ev) {
        int n = ev[0];
        for (int k = 0; k < n; k++) {
            int phase = ev[1 + 4 * k], id = ev[2 + 4 * k];
            float x = Float.intBitsToFloat(ev[3 + 4 * k]), y = Float.intBitsToFloat(ev[4 + 4 * k]);
            try {
                if (phase == 5) { back(); continue; }
                if (phase == 6) { hostPause(id != 0); continue; }
                if (phase == 7) { InputMethods.deliverPending(); continue; }
                if (phase == 8) { applyInsets(); continue; }
                if (phase == 9) { Web.deliver(); continue; }
                if (phase == 10) { Phone.deliver(); continue; }
                if (phase == 4) {
                    long t = SystemClock.uptimeMillis();
                    KeyEvent ke = new KeyEvent(t, t, ev[3 + 4 * k] != 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, id, 0);
                    // A media key (the phone's media controls send them) is the active media session's, as on Android;
                    // with none, it is the window's like any other key.
                    if (android.media.session.MediaSession.meglerMediaKey(ke)) continue;
                    if (!InputMethods.key(ke)) ViewRootDispatch.key(ke);
                    continue;
                }
                // Use the arrival time, even when several events were queued
                // during a slow frame. VelocityTracker needs the real spacing.
                int time = 1 + 4 * n + 2 * k;
                long eventTime = ev.length >= 1 + 6 * n
                        ? ((long) ev[time + 1] << 32) | (ev[time] & 0xffffffffL)
                        : SystemClock.uptimeMillis();
                touch(phase, id, x, y, eventTime);
            } catch (RuntimeException e) {
                android.util.Log.e("Megler", "input threw", e);
                throw e;
            }
        }
    }
    /** The screen's notch / home indicator areas and the keyboard, from the host. */
    private static void applyInsets() {
        int[] in = Native.insets();
        if (in == null || in.length < 4) return;
        ViewRoot.setInsets(in[0], in[1], in[2], in[3]);
        if (in.length > 4) ViewRoot.setImeHeight(in[4]);
    }
    private static void back() {
        ViewRoot r = ViewRoot.top();
        Activity a = top();
        if (r != null && a != null && r.getView() == a.getWindow().peekDecorView()) { a.meglerBack(); return; }
        long t = SystemClock.uptimeMillis();
        boolean h = ViewRoot.dispatchKey(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
        h |= ViewRoot.dispatchKey(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
        if (!h && a != null) a.meglerBack();
    }
    private static void hostPause(boolean pause) {
        if (pause == sPausedByHost) return;
        sPausedByHost = pause;
        // a result another app's activity left for this one comes first, as onActivityResult comes before onResume
        if (!pause) AppResults.resumed();
        Activity a = top();
        if (a == null) return;
        if (pause) { if (a.isResumedMegler()) a.meglerPause(); }
        else if (!a.isResumedMegler()) a.meglerResume();
    }
    private static void touch(int phase, int id, float x, float y, long now) {
        int idx = -1;
        for (int i = 0; i < down; i++) if (ids[i] == id) idx = i;
        int action;
        if (phase == 0) {
            if (idx >= 0 || down >= 16) return;
            idx = down++; ids[idx] = id; xs[idx] = x; ys[idx] = y;
            if (down == 1) downTime = now;
            action = down == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN | (idx << 8);
        } else if (phase == 1) {
            if (idx < 0) return;
            xs[idx] = x; ys[idx] = y;
            action = MotionEvent.ACTION_MOVE;
        } else {
            if (idx < 0) return;
            xs[idx] = x; ys[idx] = y;
            action = phase == 3 ? MotionEvent.ACTION_CANCEL : down == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (idx << 8);
        }
        MotionEvent e = MotionEvent.meglerObtain(downTime, now, action, down, java.util.Arrays.copyOf(ids, down), java.util.Arrays.copyOf(xs, down), java.util.Arrays.copyOf(ys, down));
        ViewRoot.dispatchTouch(e);
        if (phase >= 2) {
            for (int i = idx; i + 1 < down; i++) { ids[i] = ids[i + 1]; xs[i] = xs[i + 1]; ys[i] = ys[i + 1]; }
            down--;
            if (phase == 3) down = 0;
        }
    }
    static final class ViewRootDispatch { static void key(KeyEvent e) { if (!ViewRoot.dispatchKey(e)) { Activity a = top(); if (a != null) a.dispatchKeyEvent(e); } } }
}
