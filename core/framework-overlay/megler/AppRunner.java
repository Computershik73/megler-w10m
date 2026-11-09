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
// And Android's location settings (android.settings.LOCATION_SOURCE_SETTINGS: an app's "turn the location on", after it was
// told the location is off) open Windows' page of them (ms-settings:privacy-location), where the phone's location is switched
// on; a caller that asked for a result hears RESULT_CANCELED once it is in front again, as from Android's Settings' Back. They
// had opened nothing.
//
// And the app's network security configuration is put in place before its Application is made, where Android 9's
// handleBindApplication does it (android.security.net.config.NetworkSecurityConfigProvider says why): the authorities its
// XML names, Ozon's Russian Trusted Root CA among them, are trusted by TLS.
//
// And the screen's changes (megler.Screen: the shell resized or turned the app's surface, EV_SURFACE): each activity on the
// screen meets the new configuration at once, the others as they come back (ensureConfiguration), as Android's
// ActivityRecord.ensureActivityConfiguration has it -- one that does not take all that changed is made again in its place,
// at its place in the stack, with its saved state and its retained object, and brought back to where it was in its
// lifecycle (relaunch, as ActivityThread.handleRelaunchActivity; Activity.recreate goes the same way, where it made the new
// one on top of the stack, resumed, whatever the old one had been, and dropped what it retained). And how the activity in
// front wants the screen held -- its android:screenOrientation or setRequestedOrientation, "behind" and an activity that
// does not fill the screen leaving it to the one below -- is told to the shell as it changes (updateOrientation).
//
// And Android's screens about the Bluetooth radio an app starts (BluetoothAdapter.ACTION_REQUEST_ENABLE, the Bluetooth
// settings) are the phone's Bluetooth settings, the request's result RESULT_OK once the radio is on (megler.Bluetooth).
//
// And the Application, once made, is put in ActivityThread's fields (mInitialApplication, mAllApplications:
// ActivityThread.meglerApplicationMade) before its providers and its onCreate, as Android's handleBindApplication puts it
// there: code that reads them by reflection on another thread while the Application starts finds it (android/app/
// ActivityThread.java says who).
//
// And an intent for the phone's home screen (ACTION_MAIN with CATEGORY_HOME) sends the app to the back, as the launcher
// coming in front does on Android (isHome): Facebook's Back goes home so on Facebook Lite's sign-in screen and Messenger's
// first-run screen, and found no home.
//
// And Activity.finishAffinity finishes the activities of the caller's task affinity at once, none of them resumed on the
// way, and stops at one of another affinity (finishAffinity): it finished them one by one, each the top in its turn, and
// the one under each was resumed before it went too.
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
        // Watched from here, Application.onCreate and all: an app that stays on "starting" says in the log where its main thread is.
        Native.mainStarted();
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
        Screen.started();
        installClassLoader();
        installComponentFactory();
        ContextImpl base = ContextImpl.app();
        // As Android's handleBindApplication, before the app's code can make anything of TLS: its network security configuration.
        android.security.net.config.NetworkSecurityConfigProvider.install(base);
        String appCls = applicationClass != null ? applicationClass : Manifest.applicationClass;
        Application app;
        try { app = sFactory.instantiateApplication(classLoader(), appCls != null ? appCls : Application.class.getName()); }
        catch (ClassNotFoundException e) { android.util.Log.w("Megler", "no application class " + appCls); app = new Application(); }
        sApp = app;
        // (The process's from now on, for code that asks ActivityThread by reflection, its fields as well as its methods.)
        android.app.ActivityThread.meglerApplicationMade(app);
        Application.meglerSet(app);
        ContextImpl.setApplication(app);
        app.meglerAttach(base);
        installProviders(app);
        Looper.setInputSink(AppRunner::deliver);
        applyInsets();
        app.onCreate();
        Syncs.started();
        Installs.sweep(app);
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
    /** The activity at `i` in the stack, the lowest 0. */
    public static Activity activityAt(int i) { return sStack.get(i); }
    /** Whether `a` is above `b` in the stack (megler.ViewRoot puts an activity's window under those of the ones above it). */
    public static boolean isAbove(Activity a, Activity b) {
        int ia = sStack.indexOf(a), ib = sStack.indexOf(b);
        return ia >= 0 && ib >= 0 && ia > ib;
    }
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
        Activity a = make(cls, intent, caller, request, info);
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
        updateOrientation();
        return a;
    }
    /** An activity made by whoever makes it, and attached; not created yet. */
    private static Activity make(String cls, Intent intent, Activity caller, int request, Manifest.Component info) throws Exception {
        // Megler's screens of Android's Settings are the system's, not the app's: its factory does not make them. Google Play
        // services' activities hosted here are GmsCore's, made in its scope on its context with its Application (MicroG).
        boolean gms = MicroG.hosts(info);
        Activity a = SystemScreens.isScreen(cls) ? (Activity) Class.forName(cls).newInstance()
                : gms ? MicroG.newActivity(cls) : sFactory.instantiateActivity(classLoader(), cls, intent);
        Context base = gms ? MicroG.activityBase() : ContextImpl.forActivity();
        a.meglerAttach(base, gms ? MicroG.activityApplication() : sApp, intent, info, caller, request);
        return a;
    }

    /* Where a relaunched activity is brought: started, its window not shown yet (whoever asked goes on: a result, then resume);
       resumed; resumed and paused again (on the screen under a translucent or floating one); stopped. */
    static final int TO_STARTED = 0, TO_RESUMED = 1, TO_PAUSED = 2, TO_STOPPED = 3;

    /*
     * An activity made again in its place, as ActivityThread.handleRelaunchActivity does it for a configuration change it does
     * not take (changes: what changed) and for Activity.recreate (0): the old one paused, stopped, its state saved -- its views'
     * with it (Activity.onSaveInstanceState) -- what it retains kept (onRetainNonConfigurationInstance: androidx's
     * ViewModels), and destroyed, isChangingConfigurations true all the while; the same component (its manifest's, not the
     * class the app's component factory made of it; its target, for one started through an <activity-alias>, the intent
     * keeping the alias) made again with the same intent at the same place in the stack, given the state in onCreate and
     * onRestoreInstanceState and the retained object in getLastNonConfigurationInstance, and brought to `to`. Activities that
     * started others for a result are the callers of those still; the new one is.
     */
    static Activity relaunch(Activity a, int changes, int to) {
        int idx = sStack.indexOf(a);
        if (idx < 0) return a;
        Manifest.Component info = a.meglerInfo();
        String cls = info == null ? a.getClass().getName() : "activity-alias".equals(info.kind) && info.parentActivity != null ? info.parentActivity : info.name;
        android.util.Log.i("Megler", "relaunch " + a.getClass().getName() + " as " + a.getComponentName().getClassName()
                           + (changes != 0 ? " for the configuration changes 0x" + Integer.toHexString(changes) : ""));
        a.meglerChanging(changes);
        if (a.isResumedMegler()) a.meglerPause();
        a.meglerStop();
        Bundle state = a.meglerSaveState();
        Object retained = a.meglerRetain();
        a.meglerDestroy();
        Activity n;
        try { n = make(cls, a.getIntent(), a.meglerCaller(), a.meglerRequestCode(), info); }
        catch (Exception e) { sStack.remove(idx); exitWhenLast(); throw new RuntimeException(e); }
        n.meglerSetRetained(retained);
        sStack.set(idx, n);
        for (Activity o : sStack) if (o.meglerCaller() == a) o.meglerSetCaller(n);
        AppResults.replaced(a, n);
        n.meglerCreate(state);
        if (!n.isFinishing()) {
            n.meglerStart();
            n.meglerRestoreState(state);
            if (to == TO_RESUMED || to == TO_PAUSED) n.meglerResume();
            if (to == TO_PAUSED) n.meglerPause();
            else if (to == TO_STOPPED) n.meglerStop();
        }
        updateOrientation();
        return n;
    }

    /*
     * An activity on the screen, or coming back to it, meets the screen's configuration (megler.Screen), as Android's
     * ActivityRecord.ensureActivityConfiguration: what changed of the screen since it last met it is a relaunch (to `to`)
     * when it does not take all of it and a size it crosses is one its resources are made for (Screen.relaunchFor); heard
     * through onConfigurationChanged when it takes all of it; else only kept, its views laid out again all the same. The
     * activity in its place after it: itself, or the one made again.
     */
    static Activity ensureConfiguration(Activity a, int to) {
        android.content.res.Configuration was = a.meglerCurrentConfig(), now = Screen.configuration();
        if (was == null || a.isFinishing() || a.isDestroyed()) return a;
        int changes = Screen.changes(was, now);
        if (changes == 0) return a;
        int handled = a.meglerHandledChanges();
        if ((Screen.relaunchFor(changes, was, now) & ~handled) != 0) return relaunch(a, changes, to);
        a.meglerConfigurationChanged(now, (changes & ~handled) == 0);
        return a;
    }

    /** The screen was resized or turned (megler.Screen): each activity on the screen meets it now, the lowest first; one that is stopped, as it comes back. */
    static void screenChanged() {
        for (Activity a : new ArrayList<>(sStack)) {
            if (a.isFinishing() || !a.meglerStarted() || !sStack.contains(a)) continue;
            ensureConfiguration(a, a.isResumedMegler() ? TO_RESUMED : TO_PAUSED);
        }
    }

    /* What the shell was last told the app wants of the screen; -2: nothing yet. */
    private static int sOrientation = -2;
    /*
     * How the activity in front wants the screen held, told to the shell when it changes (Native.setOrientation, Android's
     * SCREEN_ORIENTATION_*): the top activity's android:screenOrientation or setRequestedOrientation, as Android's window
     * manager takes it -- "behind" (3), and an activity that does not fill the screen and asks for nothing (a translucent or
     * floating one), leave it to the one below. With no activity, nothing is asked (-1).
     */
    public static void updateOrientation() {
        int want = -1;
        for (int i = sStack.size() - 1; i >= 0; i--) {
            Activity a = sStack.get(i);
            if (a.isFinishing()) continue;
            int o = a.getRequestedOrientation();
            if (o == 3) continue;
            if (o == -1 && (a.getWindow().isFloating() || a.getWindow().meglerIsTranslucent())) continue;
            want = o;
            break;
        }
        if (want == sOrientation) return;
        sOrientation = want;
        Native.setOrientation(want);
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
            // Its extras in GmsCore's class scope, as a Parcel between two processes makes them (megler.Processes.forLoader):
            // Spotify's SignInHubActivity (play-services-auth, the app's classes) handed GmsCore's sign-in its own
            // SignInConfiguration, which GmsCore cast to its class of that name: ClassCastException, and the sign-in never came.
            final Intent intent = Processes.forLoader(new Intent(i), MicroG.gmsLoader());
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
        // Android's screens about the Bluetooth radio (BluetoothAdapter.ACTION_REQUEST_ENABLE and the like): the phone's
        // Bluetooth settings, the result as the radio turns out (megler.Bluetooth.request).
        if (Bluetooth.request(from instanceof Activity ? (Activity) from : top(), i, request)) return;
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
                                // (meeting the screen as it is now first: made again, the intent goes to the new one)
                                if (!up) existing = ensureConfiguration(existing, TO_STARTED);
                                if (!up && !existing.meglerVisible()) { existing.meglerRestart(); existing.meglerStart(); }
                                existing.meglerNewIntent(intent);
                                if (!up) existing.meglerResume();
                                updateOrientation();
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
        // The phone's home screen (ACTION_MAIN with CATEGORY_HOME, of no activity of the app's own): the app goes to the back, as
        // on Android when the launcher comes in front, and nothing of it finishes (moveTaskToBack's way).
        if (isHome(i)) {
            android.util.Log.d("Megler", "startActivity " + i + " -> the phone's home: the app goes to the back");
            toBack();
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
        // Android's location settings (an app's "turn the location on"): Windows' page of them, where the phone's location is
        // switched on; a caller that asked for a result hears the user backed out once it is in front again (windowsSettingsBack).
        if ("android.settings.LOCATION_SOURCE_SETTINGS".equals(action)) {
            android.util.Log.d("Megler", "startActivity " + i + " -> Windows' location settings (ms-settings:privacy-location)");
            Native.openUrl("ms-settings:privacy-location");
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            if (request >= 0 && caller != null) {
                sWindowsCaller = caller;
                sWindowsRequest = request;
                sHandler.postDelayed(() -> windowsSettingsBack(true), 5000);
            }
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
            boolean result = caller != null && a.meglerRequestCode() >= 0 && caller == below;
            // It meets the screen as it is now as it comes back (the screen may have been turned meanwhile): made again first when
            // it must be, the result going to the one made again, as Android hands a relaunched activity its pending results.
            if (!below.isResumedMegler()) below = ensureConfiguration(below, TO_STARTED);
            updateOrientation();
            // The result in the caller's class scope where the activity was of another's (Google Play services' hosted here),
            // as a Parcel between two processes makes it: GmsCore's sign-in hands its own GoogleSignInAccount back to the app's
            // SignInHubActivity, which has a class of that name of its own (megler.Processes.forLoader).
            if (result) {
                Intent data = a.meglerResultData();
                if (a.getClass().getClassLoader() != below.getClass().getClassLoader()) data = Processes.forLoader(data, below.getClass().getClassLoader());
                below.meglerActivityResult(a.meglerRequestCode(), a.meglerResultCode(), data);
            }
            if (!below.isResumedMegler()) {
                if (!below.meglerVisible()) { below.meglerRestart(); below.meglerStart(); }
                below.meglerResume();
            }
            endWhenShown(a);
            return;
        }
        a.meglerStop();
        a.meglerDestroy();
        updateOrientation();
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
    /*
     * Activity.finishAffinity(), as Android's ActivityStack.finishActivityAffinityLocked: the activity and those under it of
     * its task affinity (android:taskAffinity, the package's unless the manifest names another) go, together; those it
     * started, above it, stay, and so does the first one down of another affinity, which comes back if the top went. None
     * of those going comes back on the way: all are finishing before the first goes, as Android marks each finishing at
     * once. They went one after another here, each the top in its turn, so the one under it was resumed before it went
     * too. Messenger's first-run screen finishes so on Back when it finds no home to go to (0.1.0.673): its main activity,
     * resumed for that moment, asked for the first-run screen again (Messenger looks for a pending first-run at every
     * resume) and finished; the one it asked for finished in its onCreate, as the first was not destroyed yet; and the app
     * ended.
     */
    public static void finishAffinity(Activity a) {
        a.meglerMarkFinished();     /* isFinishing at once, as Activity.finishAffinity has it once the system took it */
        sHandler.post(() -> {
            int i = sStack.indexOf(a);
            if (i < 0) return;
            String affinity = affinity(a);
            ArrayList<Activity> going = new ArrayList<>();
            for (int k = i; k >= 0 && java.util.Objects.equals(affinity(sStack.get(k)), affinity); k--) going.add(sStack.get(k));
            for (Activity g : going) g.meglerMarkFinished();
            for (Activity g : going) finishNow(g, true);
        });
    }
    /** An activity's task affinity: its manifest's (megler.Manifest.Component.taskAffinity); the package's for one with none. */
    static String affinity(Activity a) {
        Manifest.Component c = a.meglerInfo();
        return c != null ? c.taskAffinity : Manifest.packageName;
    }
    /*
     * Activity.recreate, as Android does it: the same component made again (its manifest's, not the class the app's component
     * factory made of it) with the same intent, given the state the old one saved after it stopped. It made the old one's
     * class again with no state: Facebook's factory makes its splash screen's class for its main activity until the app is
     * ready and then recreates that activity to have the real one made -- and got its splash screen again, for good.
     */
    public static void recreate(Activity a) {
        sHandler.post(() -> {
            if (!sStack.contains(a) || a.isFinishing()) return;
            // One started through an <activity-alias> is its target made again, the intent keeping the alias, as on Android: Play
            // Store's main activity, recreated for new server flags, was looked for as the class com.android.vending.AssetBrowserActivity,
            // its launcher's alias, and the app ended on a ClassNotFoundException (package 570). (relaunch takes the target.)
            android.util.Log.d("Megler", "recreate " + a.getClass().getName() + " as " + a.getComponentName().getClassName());
            relaunch(a, 0, a.isResumedMegler() ? TO_RESUMED : a.meglerStarted() ? TO_PAUSED : TO_STOPPED);
        });
    }

    // ---- input from Megler
    /** events: [n, then n x (phase, id, xbits, ybits), optional n x (timeLow, timeHigh)] -- phase 0 down, 1 move, 2 up, 3 cancel; 4 key (id = code, x = down); 5 back; 6 host pause (id 1) / resume (0); 7 text waiting; 8 insets changed; 9 web view events waiting; 10 the phone's messages and calls; 11 the surface resized or turned (megler.Screen) */
    private static void deliver(int[] ev) {
        int n = ev[0];
        for (int k = 0; k < n; k++) {
            int phase = ev[1 + 4 * k], id = ev[2 + 4 * k];
            float x = Float.intBitsToFloat(ev[3 + 4 * k]), y = Float.intBitsToFloat(ev[4 + 4 * k]);
            try {
                // Anything but a finger: the moves kept before it go out first (moveBatched).
                if (phase >= 4) movesDispatched();
                if (phase == 5) { back(id); continue; }
                if (phase == 6) { hostPause(id != 0); continue; }
                if (phase == 7) { InputMethods.deliverPending(); continue; }
                if (phase == 8) { applyInsets(); continue; }
                if (phase == 9) { Web.deliver(); continue; }
                if (phase == 10) { Phone.deliver(); continue; }
                if (phase == 11) { Screen.changed(); continue; }
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
    /*
     * The shell's Back, numbered (megler_back; 0 for megler_key's), through the top window and its activity, and then what
     * the app did with it, which the shell waits for: went to the back (moveTaskToBack below, during it), ended its last
     * activity, changed what it shows (a frame asked for meanwhile: a page or a dialog went), or none of these yet -- a React
     * Native or Flutter app's Back goes on to JavaScript or Dart, which may send it to the back a little later.
     */
    private static boolean sBackLeaves;
    private static void back(int seq) {
        int frames = ViewRoot.frameRequests();
        sBackLeaves = false;
        try {
            ViewRoot r = ViewRoot.top();
            Activity a = top();
            if (r != null && a != null && r.getView() == a.getWindow().peekDecorView()) { a.meglerBack(); return; }
            long t = SystemClock.uptimeMillis();
            boolean h = ViewRoot.dispatchKey(new KeyEvent(t, t, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
            h |= ViewRoot.dispatchKey(new KeyEvent(t, t, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
            if (!h && a != null) a.meglerBack();
        } finally {
            Native.backDone(seq, sBackLeaves ? 2 : sStack.isEmpty() ? 3 : ViewRoot.frameRequests() != frames ? 0 : 1);
            sBackLeaves = false;
        }
    }
    /*
     * The app's task to the back (Activity.moveTaskToBack, and Android 12's Back on a task's root activity): nothing finishes,
     * as after Home on Android. Only the system can take the window away, as its answer to the phone's Back key: a Back being
     * answered says so (back above), and one asked for at another time makes the shell's next Back the system's
     * (Native.toBack). The activities are stopped when the window is hidden (hostPause) and started again when it comes back.
     * This ended the app before: Discord and TikTok Lite went with the user's Back on their first screen, and opening them
     * again was a cold start (package 669).
     */
    public static boolean moveTaskToBack(Activity a, boolean nonRoot) {
        if (!nonRoot && !isRoot(a)) return false;
        toBack();
        return true;
    }
    /* The app to the back: said to the Back being answered (back), or making the shell's next Back the system's. */
    private static void toBack() {
        sBackLeaves = true;
        Native.toBack();
    }
    /*
     * An intent for the phone's home screen: ACTION_MAIN with CATEGORY_HOME, as an app sends itself home -- Facebook's Back on
     * Facebook Lite's sign-in screen and on Messenger's first-run screen (NeueNuxActivity) does, through its launcher of
     * activities of another signature than its own (DifferentKeyIntentScope). The phone's home here is Windows' Start, whose
     * coming in front is the app going to the back. There was none: the intent found the app's own launcher activity (a
     * filter's categories were not looked at), Facebook removed that one as its own, found nothing else ("No matching
     * different-signature components"), and went on as without a home: Facebook Lite ended its process (status 0) at its
     * sign-in screen's Back, and Messenger finished its first-run screen, whose main activity below finished as well, and the
     * app ended (0.1.0.673, the user's logs of 2026-11-08).
     */
    static boolean isHome(Intent i) {
        return Intent.ACTION_MAIN.equals(i.getAction()) && i.hasCategory(Intent.CATEGORY_HOME);
    }
    /** The activity that opened Windows' location settings for a result, and its request code (-1: none waits). */
    private static Activity sWindowsCaller;
    private static int sWindowsRequest = -1;
    /**
     * The answer of Windows' location settings to an activity that opened them for a result: RESULT_CANCELED, as Android's
     * Settings answers its Back, when the app is in front again (hostPause) -- or after five seconds when the page never hid
     * the app (fromTimer), so that the caller is not left waiting.
     */
    private static void windowsSettingsBack(boolean fromTimer) {
        final Activity caller = sWindowsCaller;
        if (caller == null || (fromTimer && sPausedByHost)) return;
        final int request = sWindowsRequest;
        sWindowsCaller = null;
        sWindowsRequest = -1;
        caller.meglerActivityResult(request, Activity.RESULT_CANCELED, null);
    }
    /** The activities the host's hidden window stopped (hostPause), in the stack's order: started again as it comes back. */
    private static final ArrayList<Activity> sStoppedByHost = new ArrayList<>();
    private static void hostPause(boolean pause) {
        if (pause == sPausedByHost) return;
        sPausedByHost = pause;
        // Nothing laid out or drawn while the host's window is hidden, as Android draws no stopped app's windows.
        ViewRoot.hostHidden(pause);
        // a result another app's activity left for this one comes first, as onActivityResult comes before onResume
        if (!pause) AppResults.resumed();
        if (!pause) windowsSettingsBack(false);
        // (and a request to turn the Bluetooth radio on that showed the phone's settings: the radio as it is now)
        if (!pause) Bluetooth.resumed();
        // and what the shell said of the permission groups is asked again: the user may have changed Windows' settings meanwhile
        if (!pause) Permissions.forget();
        Activity a = top();
        if (pause) {
            if (a != null && a.isResumedMegler()) a.meglerPause();
            /*
             * The host pauses the app only when its window is hidden whole (minimized, the screen off, the app suspended), and
             * the activities still started stop then, as Android stops those no one sees; Compose's recomposer stops its frame
             * clock at ON_STOP. Minimized and playing, Spotify drew 15 to 25 frames a second for no one, its main thread 80 to
             * 97% busy (package 657).
             */
            for (int i = sStack.size() - 1; i >= 0; i--) {
                Activity s = sStack.get(i);
                if (!s.meglerStarted() || s.isFinishing()) continue;
                s.meglerStop(true);
                sStoppedByHost.add(0, s);
            }
            return;
        }
        // Those it stopped start again, the lowest first, unless they went meanwhile or another covered them (a stop of the
        // stack's own, which hides their windows); then the top one resumes.
        ArrayList<Activity> stopped = new ArrayList<>(sStoppedByHost);
        sStoppedByHost.clear();
        // (Each meets the screen as it is now first, which may have been turned or resized while the window was hidden: made
        // again, it is where it is to be already.)
        for (Activity s : stopped) {
            if (!sStack.contains(s) || s.isFinishing() || s.meglerStarted() || s.meglerWindowsHidden()) continue;
            if (ensureConfiguration(s, s == a ? TO_RESUMED : TO_PAUSED) != s) continue;
            s.meglerRestart();
            s.meglerStart();
        }
        a = top();
        if (a == null || a.isFinishing()) return;
        if (!a.isResumedMegler()) {
            Activity made = ensureConfiguration(a, TO_RESUMED);
            if (made != a) return;
        }
        if (!a.meglerStarted()) { a.meglerRestart(); a.meglerStart(); }
        if (!a.isResumedMegler()) a.meglerResume();
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
            moveBatched(now);
            return;
        } else {
            if (idx < 0) return;
            xs[idx] = x; ys[idx] = y;
            action = phase == 3 ? MotionEvent.ACTION_CANCEL : down == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP | (idx << 8);
        }
        movesDispatched();
        MotionEvent e = MotionEvent.meglerObtain(downTime, now, action, down, java.util.Arrays.copyOf(ids, down), java.util.Arrays.copyOf(xs, down), java.util.Arrays.copyOf(ys, down));
        ViewRoot.dispatchTouch(e);
        if (phase >= 2) {
            for (int i = idx; i + 1 < down; i++) { ids[i] = ids[i + 1]; xs[i] = xs[i + 1]; ys[i] = ys[i + 1]; }
            down--;
            if (phase == 3) down = 0;
        }
    }
    /*
     * Moves batched as Android batches them (its InputEventReceiver's consumeBatchedInputEvents): a move is kept rather than
     * dispatched, the moves after it are added to it as its history (MotionEvent.meglerAddSample), and the one event goes out
     * at the next frame, first in it (Choreographer.CALLBACK_INPUT); anything else -- a finger down or up, a key, the host's
     * pause -- has the moves before it go out first. Each move went out on its own: pollInput hands over all that came while
     * the main thread was busy, and Spotify on a Lumia 650, whose scroll listeners took 50 to 200 ms a move, worked through a
     * backlog of hundreds of them -- one stretch of 25.6 s without a frame was input alone, and its list answered a finger
     * seconds late (package 666).
     */
    private static MotionEvent sMoves;
    private static boolean sMovesAsked;
    private static final Runnable sMovesAtFrame = () -> { sMovesAsked = false; movesDispatched(); };

    private static void moveBatched(long now) {
        if (sMoves != null && sMoves.meglerSamePointers(ids, down)) {
            sMoves.meglerAddSample(now, xs, ys);
        } else {
            movesDispatched();
            sMoves = MotionEvent.meglerObtain(downTime, now, MotionEvent.ACTION_MOVE, down, java.util.Arrays.copyOf(ids, down), java.util.Arrays.copyOf(xs, down), java.util.Arrays.copyOf(ys, down));
        }
        if (!sMovesAsked) {
            sMovesAsked = true;
            android.view.Choreographer.getMainThreadInstance().postCallback(android.view.Choreographer.CALLBACK_INPUT, sMovesAtFrame, null);
        }
    }

    /** The moves kept so far (moveBatched) out now: at the frame, and before anything that came after them. */
    private static void movesDispatched() {
        MotionEvent m = sMoves;
        if (m == null) return;
        sMoves = null;
        ViewRoot.dispatchTouch(m);
    }

    static final class ViewRootDispatch { static void key(KeyEvent e) { if (!ViewRoot.dispatchKey(e)) { Activity a = top(); if (a != null) a.dispatchKeyEvent(e); } } }
}
