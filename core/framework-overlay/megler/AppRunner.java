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
// Everything else is the original's, unchanged.
package megler;

import android.app.Activity;
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

    public static void run(String activityClass, String applicationClass) throws Exception {
        android.webkit.MimeTypeMap.meglerInstallDefault();
        KeyStoreProvider.install();
        Looper.prepareMainLooper();
        sHandler = new Handler(Looper.getMainLooper());
        /* as RuntimeInit: there is always a default handler (apps wrap it and call through). Android's kills the app; this one
           reports the exception and lets the thread end, as Megler did before. */
        if (Thread.getDefaultUncaughtExceptionHandler() == null) Thread.setDefaultUncaughtExceptionHandler(new UncaughtLogger());
        Manifest.read();
        installClassLoader();
        ContextImpl base = ContextImpl.app();
        String appCls = applicationClass != null ? applicationClass : Manifest.applicationClass;
        Application app;
        try { app = appCls != null ? (Application) Class.forName(appCls).newInstance() : new Application(); }
        catch (ClassNotFoundException e) { android.util.Log.w("Megler", "no application class " + appCls); app = new Application(); }
        sApp = app;
        Application.meglerSet(app);
        ContextImpl.setApplication(app);
        app.meglerAttach(base);
        installProviders(app);
        Looper.setInputSink(AppRunner::deliver);
        applyInsets();
        app.onCreate();
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
    static final class UncaughtLogger implements Thread.UncaughtExceptionHandler {
        public void uncaughtException(Thread t, Throwable e) {
            android.util.Log.e("AndroidRuntime", "FATAL EXCEPTION: " + t.getName(), e);
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
                ContentProvider cp = (ContentProvider) Class.forName(p.name).newInstance();
                ProviderInfo info = new ProviderInfo();
                info.name = p.name; info.authority = p.authorities; info.packageName = Native.packageName(); info.exported = p.exported; info.metaData = p.metaData; info.grantUriPermissions = p.grantUriPermissions;
                info.applicationInfo = app.getApplicationInfo();
                cp.attachInfo(app, info);
                ContentResolver.register(p.authorities, cp);
            } catch (Throwable t) {
                android.util.Log.w("Megler", "content provider " + p.name + " did not start: " + t);
            }
        }
    }

    // ---- activities
    public static int stackSize() { return sStack.size(); }
    public static Activity top() { return sStack.isEmpty() ? null : sStack.get(sStack.size() - 1); }
    public static CharSequence appLabel(Context c) {
        try { if (Manifest.appLabel != 0) return c.getResources().getText(Manifest.appLabel); } catch (Exception e) {}
        return Native.packageName();
    }
    static Activity launch(String cls, Intent intent, Activity caller, int request, Manifest.Component info) throws Exception {
        android.util.Log.d("Megler", "launch " + cls);
        Activity prev = top();
        Activity a = (Activity) Class.forName(cls).newInstance();
        ContextImpl base = ContextImpl.forActivity();
        a.meglerAttach(base, sApp, intent, info, caller, request);
        if (prev != null && prev.isResumedMegler()) prev.meglerPause();
        sStack.add(a);
        a.meglerCreate(null);
        if (!a.isFinishing()) { a.meglerStart(); a.meglerResume(); }
        if (prev != null && !a.isFinishing()) {
            final Activity p = prev;
            // the one below stops once the new one covers it (a translucent or floating window leaves it visible)
            if (!a.getWindow().isFloating()) p.meglerStop();
        }
        return a;
    }
    /** The component an activity was started as: its class, or the alias it was reached through. */
    private static String startedAs(Activity a) {
        Intent i = a.getIntent();
        return i != null && i.getComponent() != null ? i.getComponent().getClassName() : a.getClass().getName();
    }
    /** Context.startActivity: an activity of the app's own, or something the host can open (a web page, mail, the store). */
    public static void startActivity(Context from, Intent i, int request, Bundle opts) {
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
                        for (int k = sStack.size() - 1; k >= 0; k--) if (sStack.get(k).getClass().getName().equals(real)) {
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
        Activity below = top();
        if (wasTop && below != null && resumeBelow) {
            Activity caller = a.meglerCaller();
            if (caller != null && a.meglerRequestCode() >= 0 && caller == below) below.meglerActivityResult(a.meglerRequestCode(), a.meglerResultCode(), a.meglerResultData());
            if (!below.isResumedMegler()) {
                if (!below.meglerVisible()) { below.meglerRestart(); below.meglerStart(); }
                below.meglerResume();
            }
        }
        a.meglerStop();
        a.meglerDestroy();
        // the app ends with its last activity, unless one it started just before (finish(); startActivity(...)) is still to come
        if (sStack.isEmpty()) sHandler.post(() -> { if (sStack.isEmpty()) Native.exit(); });
    }
    public static void finishAll() { sHandler.post(() -> { while (!sStack.isEmpty()) finishNow(top(), false); }); }
    /** Activity.finishAffinity(): the activity and those under it in the task go; activities it started, above it, stay. */
    public static void finishAffinity(Activity a) {
        sHandler.post(() -> {
            int i = sStack.indexOf(a);
            if (i < 0) return;
            for (int k = i; k >= 0; k--) if (k < sStack.size()) finishNow(sStack.get(k), sStack.size() - 1 == k);
        });
    }
    public static void recreate(Activity a) {
        sHandler.post(() -> {
            int idx = sStack.indexOf(a);
            if (idx < 0) return;
            Bundle state = new Bundle();
            if (a.isResumedMegler()) a.meglerPause();
            a.meglerStop();
            a.meglerDestroy();
            sStack.remove(idx);
            try { launch(a.getClass().getName(), a.getIntent(), a.meglerCaller(), a.meglerRequestCode(), Manifest.activity(a.getClass().getName())); }
            catch (Exception e) { throw new RuntimeException(e); }
        });
    }

    // ---- input from Megler
    /** events: [n, then n x (phase, id, xbits, ybits)] -- phase 0 down, 1 move, 2 up, 3 cancel; 4 key (id = code, x = down); 5 back; 6 host pause (id 1) / resume (0); 7 text waiting; 8 insets changed; 9 web view events waiting */
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
                if (phase == 4) {
                    long t = SystemClock.uptimeMillis();
                    KeyEvent ke = new KeyEvent(t, t, ev[3 + 4 * k] != 0 ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, id, 0);
                    // A media key (the phone's media controls send them) is the active media session's, as on Android;
                    // with none, it is the window's like any other key.
                    if (android.media.session.MediaSession.meglerMediaKey(ke)) continue;
                    if (!InputMethods.key(ke)) ViewRootDispatch.key(ke);
                    continue;
                }
                touch(phase, id, x, y);
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
        Activity a = top();
        if (a == null) return;
        if (pause) { if (a.isResumedMegler()) a.meglerPause(); }
        else if (!a.isResumedMegler()) a.meglerResume();
    }
    private static void touch(int phase, int id, float x, float y) {
        long now = SystemClock.uptimeMillis();
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
