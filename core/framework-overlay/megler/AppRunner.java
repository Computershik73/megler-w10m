// Overlay of core/framework/src/megler/AppRunner.java (tools/build-framework.sh). It differs in one thing:
// startActivity with FLAG_ACTIVITY_CLEAR_TOP for an activity already in the stack does what Android does. The activities
// above it go; then, when its launch mode is standard and the intent has no FLAG_ACTIVITY_SINGLE_TOP, it is finished too
// and started again with the new intent, and otherwise the intent comes to it through onNewIntent. The original kept the
// old one, gave it the intent by setIntent alone, and called its onActivityResult(-1, RESULT_CANCELED, null), which
// Android never does. Instagram's signed-out screen, started again so, took that for its flow cancelled and finished,
// and with no activity left the app ended (status 0). The activity is found by its class, as Android compares
// realActivity: one started through an <activity-alias> is found by an intent that names the activity itself, where the
// original compared the names they were started as. An activity of launch mode singleTask or singleInstance already in
// the stack is brought back so too, with or without the flag, as Android brings back the one in its task (the original
// made another).
//
// And in one more: a media key from the phone (its media controls: the lock screen's and the volume flyout's play and
// pause) goes to the app's active media session first, as Android hands it to the session rather than to the window
// (android.media.session.MediaSession.meglerMediaKey); an app with none gets it as any other key.
//
// And an intent to install an APK (ACTION_INSTALL_PACKAGE, or ACTION_VIEW of an APK, naming no activity) goes to Megler's
// installer first (megler.Installs), as Android hands it to its package installer, rather than to an activity of the app's
// that takes the content or file scheme. And an intent for an activity of another app the phone has goes to the shell,
// which starts that app (megler.PhoneApps). Started for a result, its result comes back to the caller as the caller is in
// front again (megler.AppResults): Play Store hears so that microG's sign-in, in microG's app, added the account. And an
// implicit intent with data -- a link, an address, a file of a type -- is resolved as Android resolves it, among the app's
// activities and the other apps' (megler.Resolver): a web link to the apps approved for its domain, else to the default
// browser; "Open with" where several take it.
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
// And an activity whose onCreate throws is taken off the stack at once and never stopped or destroyed, the one below
// resumed (launch): one of Google Play services', which the app outlives, was destroyed later with no onCreate behind it.
//
// And each activity knows the package that started it (Activity.getReferrer): the app's own, or the system's for the one the
// shell starts for the user's tap on the app's tile (SHELL_REFERRER); one made again in its place keeps it (relaunch).
// And the intents for Android's documents UI, gallery, photo picker and camera app (ACTION_GET_CONTENT, ACTION_OPEN_DOCUMENT,
// ACTION_PICK of pictures, MediaStore.ACTION_PICK_IMAGES, ACTION_IMAGE_CAPTURE, ACTION_VIDEO_CAPTURE, the camera app's own) go
// to Windows' picker and camera (megler.Pickers), before the activities of the app's own and of others: they threw
// ActivityNotFoundException. Their answer comes as an event of the shell's (deliver, phase 12) and goes to the activity that
// asked once the app is in front again (hostPause), to the one made again in its place where it was (relaunch); and the
// provider of what was picked is the app's from its start (run).
//
// And the app runs as the Android process its manifest names for the activity it is started for (run, megler.Processes.own):
// the Google app's launcher activity is in ":googleapp", whose Application builds that process's object graph; told the
// package's own process, it built that one and GoogleAppActivity ended its start with "Cannot cast tdd to bkxw". An activity
// of another of the app's processes is made in that process's class scope, with its Application (make), as its services are.
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
        IntegrityProfile.install();
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
        // The process the app runs as, named before any of its code runs (megler.Processes.started): the one the manifest names
        // for the activity it is started for -- the one another app asked for, else its launcher activity -- as Android starts
        // that process for it. The Google app's launcher activity is in ":googleapp", and its Application builds the object
        // graph of the process it is told it runs in.
        String request = Native.requestedActivity();
        Manifest.Component first = request != null && !request.startsWith(PlaySplits.MARKER) ? askedFor(request) : null;
        Processes.started(first != null ? first : Manifest.activity(activityClass));
        installClassLoader();
        // (the flag cronet-netlog: Cronet's own network log of an app that carries Cronet, hooked before any of its code runs)
        CronetNetLog.install(classLoader());
        installComponentFactory();
        ContextImpl base = ContextImpl.app();
        // As Android's handleBindApplication, before the app's code can make anything of TLS: its network security configuration.
        android.security.net.config.NetworkSecurityConfigProvider.install(base);
        String appCls = applicationClass != null ? applicationClass : Manifest.applicationClass;
        // The Application is made and attached by the process's Instrumentation, as Android's LoadedApk.makeApplication has
        // it made (android.app.Instrumentation.newApplication, overlaid: the component factory makes it, applicationMade
        // below hears of it, it is attached), and its onCreate called by the one that is the process's then: the app may
        // have put its own in ActivityThread.mInstrumentation meanwhile, as TikTok Lite's "mini" app does while it is attached.
        Application app;
        sMakingApplication = true;
        try {
            app = android.app.ActivityThread.meglerInstrumentation().newApplication(classLoader(), appCls != null ? appCls : Application.class.getName(), base);
        } catch (ClassNotFoundException e) {
            if (sApp != null) throw e;         // (not its class: one its attachBaseContext looked for)
            android.util.Log.w("Megler", "no application class " + appCls);
            app = new Application();
            processApplication(app);
            app.meglerAttach(base);
        } finally {
            sMakingApplication = false;
        }
        if (sApp != app) processApplication(app);       // (an Instrumentation of the app's own that made it by itself)
        installProviders(app);
        // (and the provider of the files picked or taken for the app, megler.Pickers: links to them kept from an earlier run work)
        Pickers.install();
        Looper.setInputSink(AppRunner::deliver);
        applyInsets();
        android.app.ActivityThread.meglerInstrumentation().callApplicationOnCreate(app);
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
        // Started because another app asked for one of its activities (megler_start_activity before the app ran): that activity
        // is the app's first, with the intent the other app wrote, as Android starts the process for it -- not the launcher
        // activity and then it. Total Commander wakes its plug-ins by their LoadingActivity, and each opened with its main screen
        // as well. A request for nothing this app exports, or for an activity that was not made, leaves the start the launcher's:
        // startActivity makes the activity from the main loop, so the launcher's turn comes there too, after it. (The request
        // was taken before the Application was made, which runs in that activity's process; one that came while it was made
        // is taken now, as before, and its activity runs in its own process's scope where that is another.)
        if (request == null) request = Native.requestedActivity();
        // (An app's requests for its feature modules, which the Play Store is started for, are no activity: taken up from
        // the main loop after the launcher's activity, as they were before, megler.PlaySplits.)
        if (request != null && request.startsWith(PlaySplits.MARKER)) {
            final String splits = request;
            sHandler.post(() -> startRequested(splits));
            request = null;
        }
        Intent asked = request == null ? null : requested(request);
        if (asked != null) {
            final int launched = sLaunched;
            AppResults.requested(asked.getComponent().getClassName());
            try { startActivity(app, asked, -1, null); }
            catch (RuntimeException e) { android.util.Log.w("Megler", "the activity another app asked for was not started; the app starts as from its launcher", e); }
            sHandler.post(() -> {
                if (sLaunched != launched) return;
                try { launch(activityClass, main, null, -1, launcher, null, SHELL_REFERRER); }
                catch (RuntimeException e) { throw e; }
                catch (Exception e) { throw new RuntimeException(e); }
            });
        } else {
            launch(activityClass, main, null, -1, launcher, null, SHELL_REFERRER);
        }
        Looper.loop();
    }
    public static Application application() { return sApp; }
    /* run() is having the process's Application made (Instrumentation.newApplication). */
    private static boolean sMakingApplication;
    /** The process's Application from now on, for whoever asks: here, Application and the contexts, and ActivityThread, for
     *  code that asks it by reflection, its fields as well as its methods. */
    private static void processApplication(Application app) {
        sApp = app;
        android.app.ActivityThread.meglerApplicationMade(app);
        Application.meglerSet(app);
        ContextImpl.setApplication(app);
    }
    /**
     * The platform's Instrumentation has made an Application and is about to attach it: the process's own, when run() asked
     * for it, is the process's from this moment, before its attachBaseContext, where apps ask for it already (as run() set
     * it when it made it itself). One an app has made through an Instrumentation later is not the process's.
     */
    public static void applicationMade(Application app) {
        if (sMakingApplication && sApp == null) processApplication(app);
    }
    /* Android's LoggingHandler: FATAL EXCEPTION, the thread, the process and the stack. A thread of Google Play services' (or of
       another of the app's own processes, megler.Processes: the line names whose, Processes.threadsOf) ends
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
            android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName() + "\nProcess: " + Processes.own() + ", PID: " + android.os.Process.myPid(), e);
        }
    }
    static final Thread.UncaughtExceptionHandler SERVICE_THREAD_ENDS = (t, e) ->
        android.util.Log.e("Megler", Processes.threadsOf(Native.thrownInService(e)) + " thread " + t.getName() + " ended with an uncaught exception; on Android its process would, and the app goes on", e);
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
        // (An activity of another package that runs here, megler.HostedApps: that app's label, not this one's number read in
        // its resources.)
        CharSequence hosted = HostedApps.label(c);
        if (hosted != null) return hosted;
        try { if (Manifest.appLabel != 0) return c.getResources().getText(Manifest.appLabel); } catch (Exception e) {}
        return Native.packageName();
    }
    /*
     * Who starts the activity the shell starts for the user's tap on the app's tile, as Activity.getReferrer says it. On
     * Android that is the launcher, an app of its own; here it is Windows' Start, which is no Android package, and the
     * platform's own name stands for it, a package every app can see (PackageManager answers for "android").
     */
    static final String SHELL_REFERRER = "android";
    /* The activities made so far (launch): run() tells by it whether the activity another app asked for was. */
    private static int sLaunched;

    static Activity launch(String cls, Intent intent, Activity caller, int request, Manifest.Component info) throws Exception {
        return launch(cls, intent, caller, request, info, null, null);
    }
    /**
     * An activity made and started; `state` the one a recreated activity saved, given to onCreate and to onRestoreInstanceState;
     * `referrer` the package that started it where that is not the app (Activity.getReferrer).
     */
    static Activity launch(String cls, Intent intent, Activity caller, int request, Manifest.Component info, Bundle state, String referrer) throws Exception {
        android.util.Log.d("Megler", "launch " + cls);
        Activity prev = top();
        Activity a = make(cls, intent, caller, request, info);
        sLaunched++;
        if (referrer != null) a.meglerSetReferrer(referrer);
        final boolean prevResumed = prev != null && prev.isResumedMegler();
        if (prevResumed) prev.meglerPause();
        sStack.add(a);
        try {
            a.meglerCreate(state);
        } catch (RuntimeException | Error e) {
            /*
             * Its onCreate threw: on Android the app's process ends. Here the app goes on where the activity was Google Play
             * services' (startActivity says it failed); one of the app's own ends it still, as the exception goes on. Either
             * way the activity is taken off the stack at once, finished, any window it opened closed, and the one below
             * resumed as it was: it never was created, and is never stopped or destroyed -- Google Play services' help screen
             * failed in its layout so (a resource of theirs missing, 0.1.0.686), was finished, and its destruction at the next
             * idle ended in their lifecycle's "State must be at least 'CREATED' to be moved to 'DESTROYED'".
             */
            sStack.remove(a);
            a.meglerMarkFinished();
            ViewRoot.closeAll(a);
            if (prevResumed && top() == prev && !prev.isFinishing()) prev.meglerResume();
            updateOrientation();
            throw e;
        }
        if (!a.isFinishing()) { a.meglerStart(); if (state != null) a.meglerRestoreState(state); a.meglerPostCreate(state); a.meglerResume(); }
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
        // And an activity of another app whose services run here is that package's, made the same way (megler.HostedApps):
        // another process's on Android, which the app's Instrumentation is not asked for or told of (Activity.meglerOtherProcess).
        Activity other = HostedApps.newActivity(cls, intent, caller, request, info);
        if (other != null) {
            other.meglerOtherProcess();
            return other;
        }
        // The app's own are made by the process's Instrumentation (ActivityThread.mInstrumentation: the platform's asks the
        // app's component factory, as before; an app's own may make another class for the name, as TikTok Lite's "mini" app
        // does). The other two are other processes' on Android: the app's Instrumentation neither makes them nor is called
        // at the steps of their life (Activity.meglerOtherProcess).
        boolean gms = MicroG.hosts(info), screen = SystemScreens.isScreen(cls);
        // An activity the manifest puts in another process than the one the app runs as (megler.Processes.own) is made in that
        // process's class scope, on a context of that process and with its Application (megler.Processes), as its services
        // are; the app's Instrumentation, the own process's, is not asked for it or called at the steps of its life. The
        // Google app's ":googleapp" opens Gemini's screens in ":search", whose Application builds that process's object graph.
        // Its intent's objects are made again by that scope's classes, as a Parcel between two processes makes them.
        Processes.Scope process = gms || screen || info == null ? null : Processes.ofActivity(info);
        if (process != null) {
            Activity a = (Activity) process.classLoader().loadClass(cls).getDeclaredConstructor().newInstance();
            a.meglerOtherProcess();
            a.meglerAttach(process.activityBase(), process.application(), Processes.forLoader(intent, process.classLoader()), info, caller, request);
            return a;
        }
        Activity a = screen ? (Activity) Class.forName(cls).newInstance()
                : gms ? MicroG.newActivity(cls) : android.app.ActivityThread.meglerInstrumentation().newActivity(classLoader(), cls, intent);
        if (gms || screen) a.meglerOtherProcess();
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
        n.meglerSetReferrer(a.meglerReferrer());
        n.meglerSetRetained(retained);
        sStack.set(idx, n);
        for (Activity o : sStack) if (o.meglerCaller() == a) o.meglerSetCaller(n);
        AppResults.replaced(a, n);
        Pickers.replaced(a, n);
        n.meglerCreate(state);
        if (!n.isFinishing()) {
            n.meglerStart();
            n.meglerRestoreState(state);
            n.meglerPostCreate(state);
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
        Activity a = android.app.ActivityThread.meglerInstrumentation().newActivity(classLoader(), cls, intent);
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
    public static void startRequested(String request) {
        // (not an activity, but an app's requests for its feature modules, in the Play Store: megler.PlaySplits)
        if (PlaySplits.requested(request)) return;
        Intent i = requested(request);
        if (i == null) return;
        AppResults.requested(i.getComponent().getClassName());
        startActivity(sApp != null ? sApp : ContextImpl.app(), i, -1, null);
    }
    /*
     * The intent of another app's request: the one it wrote for the activity when the request names its file
     * (megler.PhoneApps.asked: its action, data, type, categories, flags and extras, which Total Commander's Wi-Fi plug-in
     * has the files to send in), else the plain one for the class; null, with a line in the log, for an activity the manifest
     * does not have or does not export.
     */
    /* The activity of the manifest another app's request names ("<class>" or "<class>~<its intent's file>"), where the app
       exports it (as requested below starts it); null otherwise. Read before the request's intent is. */
    private static Manifest.Component askedFor(String request) {
        int at = request.indexOf('~');
        Manifest.Component c = Manifest.activity(at < 0 ? request : request.substring(0, at));
        return c != null && c.exported ? c : null;
    }
    private static Intent requested(String request) {
        Object[] asked = PhoneApps.asked(request, classLoader());
        String cls = (String) asked[0];
        // (the component the intent reached: the activity, or an <activity-alias> of it, whose android:exported is the one
        // Android looks at)
        Manifest.Component c = Manifest.activity(((Intent) asked[1]).getComponent().getClassName());
        if (c == null || !c.exported) {
            android.util.Log.w("Megler", "another app asked for " + cls + ", which this app " + (c == null ? "does not have" : "does not export"));
            return null;
        }
        Intent i = ((Intent) asked[1]).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // (what the intent is like, not what it says: its data and extras are the user's)
        Bundle extras = i.getExtras();
        android.util.Log.i("Megler", "another app asked for " + cls + (request.length() == cls.length() ? "" : " with its intent: action " + i.getAction()
            + ", type " + i.getType() + ", " + (i.getData() == null ? "no" : i.getData().getScheme() + ":") + " data, flags 0x" + Integer.toHexString(i.getFlags())
            + ", " + (extras == null ? 0 : extras.size()) + " extra(s)"));
        return i;
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
        // Android's documents UI, gallery, photo picker and camera app (megler.Pickers): Windows' picker and camera, the result to
        // the caller once it is in front again. Before the app's own activities, as on Android, where these are the system's
        // (an app that takes GET_CONTENT itself, a file manager, asks the system's when it wants a file); and before Google
        // Play services', whose own photo picker an app may name.
        if (Pickers.start(from instanceof Activity ? (Activity) from : top(), i, request)) return;
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
        // An activity of another app whose services run in this process already (megler.HostedApps.activity): it runs here
        // too, in that package's class scope, on its context and with its Application -- a plug-in's screen and its service
        // are one app. Total Commander starts the LAN plug-in's ConnectActivity for it ("New server"), which begins with
        // the service's static instance and saves the server where the service reads it: in the plug-in's own app (below)
        // neither is there.
        final Manifest.Component plugged = HostedApps.activity(from, i);
        if (plugged != null) {
            final Activity caller = from instanceof Activity ? (Activity) from : top();
            final Intent intent = HostedApps.intentFor(plugged, i);
            final String cls = "activity-alias".equals(plugged.kind) && plugged.parentActivity != null ? plugged.parentActivity : plugged.name;
            android.util.Log.i("Megler", "hosted apps: " + intent.getComponent().getPackageName() + "/" + cls + " opens inside " + Native.packageName());
            sHandler.post(() -> {
                try { launch(cls, intent, caller, request, plugged); }
                catch (Throwable t) { android.util.Log.e("Megler", "hosted apps: starting " + cls + " failed", t); }
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
        // A link, an address, a file of a type, for no package (megler.Resolver): the activity of this app's or of another
        // app's that takes it, as Android resolves it -- a web link to the apps approved for its domain, else to the browser
        // --, or "Open with" where several do. Nothing taking it, it goes on below as before.
        if (Resolver.start(from, i, request)) return;
        Manifest.Component c = Manifest.resolve(i);
        android.util.Log.d("Megler", "startActivity " + i + (c != null ? " -> " + c.name : " (not an activity of this app)"));
        if (c != null) {
            startOwn(from, i, request, c);
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
            awaitWindowsPage(from instanceof Activity ? (Activity) from : top(), request);
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
    /** An activity of the app's own, `c`, started with the intent: brought back where it is on the stack and its launch mode or
     *  the intent's flags say so, made anew otherwise (megler.Resolver starts one so too). */
    static void startOwn(Context from, Intent i, int request, Manifest.Component c) {
        final Activity caller = from instanceof Activity ? (Activity) from : top();
        final Intent intent = new Intent(i);
        if (intent.getComponent() == null) intent.setComponent(new ComponentName(Native.packageName(), c.name));
        sHandler.post(() -> {
            try {
                // the activity itself, as Android compares realActivity: one started through an <activity-alias> is its target's,
                // whose launch mode is the one that counts
                String real = "activity-alias".equals(c.kind) && c.parentActivity != null ? c.parentActivity : c.name;
                Manifest.Component realInfo = real.equals(c.name) ? c : Manifest.activity(real);
                int mode = realInfo != null ? realInfo.launchMode : c.launchMode;
                /*
                 * FLAG_ACTIVITY_CLEAR_TOP, or an activity of launch mode singleTask (2) or singleInstance (3): one of it
                 * already on the stack is brought back with the intent and the activities above it go, as Android does
                 * with an instance of it in its task -- the app's stack is its one task here, and taskAffinity is not
                 * looked at. Viber's RegistrationActivity (singleTask) was made anew at every start, four of it in one
                 * session (0.1.0.699), where Android gives the one there onNewIntent.
                 */
                if ((intent.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP) != 0 || mode == 2 || mode == 3) {
                    // not one that is finishing, as Android passes over those: Instagram's main activity, finished in
                    // its onCreate, was started again (onRestart, onStart) and failed for the state it never made
                    for (int k = sStack.size() - 1; k >= 0; k--) if (!sStack.get(k).isFinishing() && sStack.get(k).getClass().getName().equals(real)) {
                        Activity existing = sStack.get(k);
                        while (top() != existing) finishNow(top(), false);
                        if ((intent.getFlags() & 0x20000000 /* SINGLE_TOP */) != 0 || mode != 0) {
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
                if ((intent.getFlags() & 0x20000000 /* SINGLE_TOP */) != 0 || mode == 1) {
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
    }
    /** Activity.finish: the one below comes back (with the result when it asked for one). */
    public static void finish(Activity a) { sHandler.post(() -> finishNow(a, true)); }
    private static void finishNow(Activity a, boolean resumeBelow) {
        android.util.Log.d("Megler", "finish " + a.getClass().getName());
        int idx = sStack.indexOf(a);
        if (idx < 0) return;
        // (Finishing from here on, as Android's finishActivityLocked marks one: an activity cleared from above another brought
        // back -- FLAG_ACTIVITY_CLEAR_TOP, a singleTask one -- or the app's end says isFinishing() in its onPause and onDestroy.)
        a.meglerMarkFinished();
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
    /** events: [n, then n x (phase, id, xbits, ybits), optional n x (timeLow, timeHigh)] -- phase 0 down, 1 move, 2 up, 3 cancel; 4 key (id = code, x = down); 5 back; 6 host pause (id 1) / resume (0); 7 text waiting; 8 insets changed; 9 web view events waiting; 10 the phone's messages and calls; 11 the surface resized or turned (megler.Screen); 12 the phone's picker or camera answered request `id` (megler.Pickers) */
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
                if (phase == 12) { Pickers.answered(id); continue; }
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
     * A page of Windows' own opened for an activity that asked for a result (Windows' location settings, its camera app:
     * megler.Pickers): it hears RESULT_CANCELED once the app is in front again, as from Android's Back on such a screen.
     */
    static void awaitWindowsPage(Activity caller, int request) {
        if (request < 0 || caller == null) return;
        sWindowsCaller = caller;
        sWindowsRequest = request;
        sHandler.postDelayed(() -> windowsSettingsBack(true), 5000);
    }
    /** Whether the shell's window is hidden now (hostPause): what comes for the app waits for it to be in front again (megler.Pickers). */
    static boolean pausedByHost() { return sPausedByHost; }
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
    /*
     * As Android's ActivityThread is told once no window of the process's is seen (ActivityManager's TRIM_MEMORY_UI_HIDDEN,
     * after the activities stopped): the activities, the services that run and the Application -- and through it the
     * callbacks registered with it -- are asked to let go of what only their UI needed. Glide clears its memory cache of
     * images there. It was never told, and a minimized app, which the shell keeps running (Settings, In the Background),
     * kept every cache it had. An exception of an app's there is logged, not the end of the app it would be on Android.
     */
    private static void trimMemory(int level) {
        ArrayList<ComponentCallbacks2> to = new ArrayList<>(sStack);
        to.addAll(Services.running());
        if (sApp != null) to.add(sApp);
        // (and the Applications of the app's other processes, megler.Processes: theirs are hidden too)
        to.addAll(Processes.applications());
        android.util.Log.i("Megler", "the window is hidden: onTrimMemory(" + level + ") to " + to.size() + " component(s)");
        for (ComponentCallbacks2 c : to) {
            try {
                c.onTrimMemory(level);
            } catch (RuntimeException e) {
                android.util.Log.w("Megler", "onTrimMemory of " + c.getClass().getName(), e);
            }
        }
    }
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
            trimMemory(android.content.ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN);
            return;
        }
        // Those it stopped start again, the lowest first, unless they went meanwhile or another covered them (a stop of the
        // stack's own, which hides their windows); then the top one resumes.
        ArrayList<Activity> stopped = new ArrayList<>(sStoppedByHost);
        sStoppedByHost.clear();
        // (Each meets the screen as it is now first, which may have been turned or resized while the window was hidden: made
        // again, it is where it is to be already.)
        // (The one in front that the phone's picker or camera has answered is made again only as far as started: its answer
        // comes before it resumes, below.)
        for (Activity s : stopped) {
            if (!sStack.contains(s) || s.isFinishing() || s.meglerStarted() || s.meglerWindowsHidden()) continue;
            if (ensureConfiguration(s, s != a ? TO_PAUSED : Pickers.waitsFor(s) ? TO_STARTED : TO_RESUMED) != s) continue;
            s.meglerRestart();
            s.meglerStart();
        }
        // What the phone's picker or camera answered while the app was hidden by it: to the activity that asked -- started
        // again, or made again in its place where the screen turned meanwhile -- before it resumes, as Android delivers a
        // result (onRestart, onStart, onActivityResult, onResume; to the new activity after a relaunch).
        Pickers.resumed();
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
