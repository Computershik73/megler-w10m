// Overlay of core/framework/src/android/app/Activity.java (tools/build-framework.sh).
// It differs in two methods. initWindowDecorActionBar makes the window's decor before it asks whether the window has an action bar.
//
// Why. Whether a window has an action bar is the theme's to say (windowActionBar, windowNoTitle), and the window reads its
// theme when its decor is made, not before. Android's Activity.initWindowDecorActionBar therefore begins with
// window.getDecorView() ("Initializing the window decor can change window feature flags. Ensure that we have the correct set
// before performing the test below"). The original asks hasFeature(FEATURE_ACTION_BAR) first, and on a window whose decor
// nothing has asked for yet that is false whatever the theme says. So getActionBar() was null for an activity that asks for
// its bar before setContentView -- which is the usual order with ActionBarSherlock, whose getSupportActionBar() on Android
// 4 and later wraps Activity.getActionBar() and is null when that is: QuickShortcutMaker's About screen ended the app with
// "NullPointerException: ... ActionBar.setHomeButtonEnabled(Z)V on a null object reference", while its Settings screen,
// which happens to call getWindow().getDecorView() first, had its bar.
//
// And in getWindow: the window takes the manifest's android:windowSoftInputMode, as Activity.attach does on Android (when it
// says anything). The original read the attribute and never used it, so an activity was resized for the keyboard whatever
// it asked; ViewRoot (overlaid) now resizes, pans or leaves a window as its softInputMode says.
//
// And an activity that starts one of the app's activities for a result before its own window is on the screen (in onCreate,
// onStart or onResume) does not show its window when it resumes: it shows it when it resumes again, once the other has
// given its result. That is ActivityThread.handleResumeActivity's mStartedActivity ("willBeVisible"). The original showed
// the window after every onResume, so the window had focus and lost it to the activity it had started, which Android never
// gives it: Asphalt 9's MainActivity starts its data installer from onResume, creates its native game only in
// onActivityResult, and its onWindowFocusChanged calls into that game, which was not there yet (a read at 0x34, package
// 262). meglerVisible, which AppRunner asks whether an activity below must be started again, says whether the activity was
// started and not stopped, so that such an activity is not started twice. Everything else is the original's, unchanged.
package android.app;

import android.content.*;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.*;
import android.view.accessibility.AccessibilityEvent;
import java.util.ArrayList;

/** An activity, run by megler.AppRunner as ActivityThread runs one: attached to a base context, its lifecycle in order, its window on screen. */
public class Activity extends ContextThemeWrapper implements Window.Callback, KeyEvent.Callback, LayoutInflater.Factory2, View.OnCreateContextMenuListener,
        ComponentCallbacks2 {
    public static final int RESULT_OK = -1, RESULT_CANCELED = 0, RESULT_FIRST_USER = 1, DEFAULT_KEYS_DISABLE = 0, DEFAULT_KEYS_DIALER = 1,
        DEFAULT_KEYS_SHORTCUT = 2, DEFAULT_KEYS_SEARCH_LOCAL = 3, DEFAULT_KEYS_SEARCH_GLOBAL = 4, OVERRIDE_TRANSITION_OPEN = 0, OVERRIDE_TRANSITION_CLOSE = 1;
    protected static final int[] FOCUSED_STATE_SET = { android.R.attr.state_focused };
    private Window mWindow;
    private Intent mIntent = new Intent(Intent.ACTION_MAIN);
    private Application mApplication;
    private ComponentName mComponent;
    private CharSequence mTitle;
    private int mTitleColor;
    private boolean mFinished, mDestroyed, mResumed, mStarted, mCalled, mVisibleFromClient = true, mChangingConfigurations;
    /* started an activity of the app's for a result before its window was shown: the window waits for the next resume */
    private boolean mStartedActivity;
    private int mResultCode = RESULT_CANCELED;
    private Intent mResultData;
    private Activity mParent, mCaller;
    private int mRequestCode = -1;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private ActionBar mActionBar;
    private final ArrayList<Application.ActivityLifecycleCallbacks> mLifecycleCallbacks = new ArrayList<>();
    private FragmentManager mFragments;
    /* how far the platform fragments may go (FragmentManager's host states): created, activity created, started, resumed */
    private int mFragmentHost;
    final int meglerFragmentHost() { return mFragmentHost; }
    private void fragmentHost(int state) { mFragmentHost = state; if (mFragments != null) mFragments.meglerHostState(state); }
    private int mRequestedOrientation = -1;
    private megler.Manifest.Component mInfo;

    public Activity() { super(null, 0); }

    // ---- megler.AppRunner's side
    /** As Activity.attach: the base context, the window, the intent. */
    private boolean mAttaching;
    public final void meglerAttach(Context base, Application app, Intent intent, megler.Manifest.Component info, Activity caller, int requestCode) {
        /* as Activity.attach: the window is made after attachBaseContext, so getWindow() is null in it (AppCompat's delegate
           installs itself into a window it finds there, reading the theme before the base context is set -- Todoist) */
        mAttaching = true;
        try { attachBaseContext(base); } finally { mAttaching = false; }
        mApplication = app;
        mIntent = intent != null ? intent : new Intent(Intent.ACTION_MAIN);
        mInfo = info;
        mCaller = caller;
        mRequestCode = requestCode;
        mComponent = new ComponentName(getPackageName(), getClass().getName());
        int theme = info != null && info.theme != 0 ? info.theme : megler.ContextImpl.defaultTheme();
        setTheme(theme);
        if ("com.google.android.gms".equals(megler.Native.packageName())) {
            android.util.TypedValue value = new android.util.TypedValue();
            android.content.res.Resources.Bag bag = getResources().meglerBag(theme);
            android.util.Log.i("ThemeProbe", getClass().getName() + " component=" + (info == null ? "null" : info.name)
                    + " theme=0x" + Integer.toHexString(theme) + " windowActionBar="
                    + getTheme().resolveAttribute(0x7f0102c5, value, true)
                    + " bag=" + (bag == null ? "missing" : bag.n + "/" + bag.indexOf(0x7f0102c5))
                    + " config=" + getResources().getConfiguration()
                    + " applied=" + getTheme());
        }
        if (info != null && info.label != 0) { try { mTitle = getResources().getText(info.label); } catch (Exception e) {} }
        if (mTitle == null) mTitle = megler.AppRunner.appLabel(this);
        if (info != null && info.screenOrientation >= 0) mRequestedOrientation = info.screenOrientation;
    }
    /* API 29's pre / post lifecycle callbacks around each step: androidx.lifecycle's ReportFragment moves every ComponentActivity's
     * Lifecycle on them (ON_CREATE after onCreate, ON_PAUSE before onPause...), and Compose sets its content once that is CREATED */
    private interface Step { void on(Application.ActivityLifecycleCallbacks c); }
    private void each(Step s) { for (Application.ActivityLifecycleCallbacks c : callbacks()) s.on(c); }
    public final void meglerCreate(Bundle state) {
        // as Activity.attach: the activity is its inflater's private factory, after any the app sets (FragmentActivity makes
        // <fragment> and FragmentContainerView there)
        getLayoutInflater().setPrivateFactory(this);
        each(c -> c.onActivityPreCreated(this, state));
        mCalled = false;
        mFragmentHost = FragmentManager.HOST_CREATED;
        onCreate(state);
        if (!mCalled) throw new SuperNotCalledException("Activity " + getClass().getName() + " did not call through to super.onCreate()");
        fragmentHost(FragmentManager.HOST_ACTIVITY_CREATED);   /* as performCreate: fragments hear onActivityCreated after onCreate */
        /* as Activity.performCreate: visibility comes from the theme after onCreate, so setVisible(false) there does not hide
           the activity (Spotify's login calls it and never shows itself again) */
        try {
            android.content.res.TypedArray ta = getTheme().obtainStyledAttributes(new int[] { android.R.attr.windowNoDisplay });
            mVisibleFromClient = !ta.getBoolean(0, false);
            ta.recycle();
        } catch (RuntimeException e) { mVisibleFromClient = true; }
        if (!mFinished) onPostCreate(state);            /* as ActivityThread: an activity that finished in onCreate goes no further */
        each(c -> c.onActivityPostCreated(this, state));
    }
    public final void meglerStart() {
        each(c -> c.onActivityPreStarted(this));
        mCalled = false; onStart(); mStarted = true;
        fragmentHost(FragmentManager.HOST_STARTED);
        if (mLoaders != null) mLoaders.doStart();
        each(c -> c.onActivityPostStarted(this));
    }
    public final void meglerRestart() { onRestart(); }
    public final void meglerResume() {
        each(c -> c.onActivityPreResumed(this));
        mCalled = false;
        onResume();
        mResumed = true;
        fragmentHost(FragmentManager.HOST_RESUMED);
        onPostResume();
        each(c -> c.onActivityPostResumed(this));
        if (mVisibleFromClient && !mStartedActivity) makeVisible();
        mStartedActivity = false;
    }
    public final void meglerPause() {
        each(c -> c.onActivityPrePaused(this));
        fragmentHost(FragmentManager.HOST_STARTED);            /* fragments pause before the activity, as performPause */
        mResumed = false; onPause();
        each(c -> c.onActivityPostPaused(this));
        android.content.MeglerSharedPreferences.flushAll();      /* as QueuedWork after onPause */
    }
    public final void meglerStop() {
        if (mStarted) {                                  /* one that finished in onCreate never started, and does not stop */
            each(c -> c.onActivityPreStopped(this));
            fragmentHost(FragmentManager.HOST_ACTIVITY_CREATED);
            mStarted = false; onStop();
            if (mLoaders != null) mLoaders.doStop();
            each(c -> c.onActivityPostStopped(this));
        }
        if (mWindow != null && mWindow.peekDecorView() != null) { megler.ViewRoot r = megler.ViewRoot.of(mWindow.peekDecorView()); if (r != null) r.remove(); }
        /* its dialogs and popups hidden with it, as Android hides the windows of a token not visible (makeVisible shows them) */
        if (!mWindowsHidden) { mWindowsHidden = true; megler.ViewRoot.ownerStopped(this, true); }
    }
    public final void meglerDestroy() {
        each(c -> c.onActivityPreDestroyed(this));
        fragmentHost(FragmentManager.HOST_NONE);
        mDestroyed = true; onDestroy();
        if (mLoaders != null) mLoaders.doDestroy();
        each(c -> c.onActivityPostDestroyed(this));
        if (mWindow != null) mWindow.meglerDestroy();
        /* the windows it left open go with it, as ActivityThread.handleDestroyActivity has WindowManagerGlobal.closeAll remove them */
        megler.ViewRoot.closeAll(this);
    }
    /* Whether its windows other than its own are hidden: it was stopped and is not on the screen again (ViewRoot.ownerStopped). */
    private boolean mWindowsHidden;
    public final boolean meglerWindowsHidden() { return mWindowsHidden; }
    public final void meglerBack() { onBackPressedMegler(); }
    /* Whether it was started and not stopped since (AppRunner starts the activity below again when not); its window may be waiting (mStartedActivity). */
    public final boolean meglerVisible() { return mStarted || windowShown(); }
    private boolean windowShown() { return mWindow != null && mWindow.peekDecorView() != null && megler.ViewRoot.of(mWindow.peekDecorView()) != null; }
    public final Activity meglerCaller() { return mCaller; }
    public final int meglerRequestCode() { return mRequestCode; }
    /** It passed its caller's request on (FLAG_ACTIVITY_FORWARD_RESULT): its own result goes to no one. */
    public final void meglerForwardedResult() { mRequestCode = -1; }
    public final int meglerResultCode() { return mResultCode; }
    public final Intent meglerResultData() { return mResultData; }
    public final void meglerActivityResult(int request, int result, Intent data) { onActivityResult(request, result, data); }
    void makeVisible() {
        View decor = getWindow().getDecorView();
        if (megler.ViewRoot.of(decor) == null) getWindowManager().addView(decor, getWindow().getAttributes());
        decor.setVisibility(View.VISIBLE);
        if (mWindowsHidden) { mWindowsHidden = false; megler.ViewRoot.ownerStopped(this, false); }
    }
    private void onBackPressedMegler() {
        if (getWindow().meglerDispatchBack()) return;
        KeyEvent down = new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK), up = new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK);
        if (!megler.ViewRoot.dispatchKey(down)) onKeyDown(KeyEvent.KEYCODE_BACK, down);
        if (!megler.ViewRoot.dispatchKey(up)) onKeyUp(KeyEvent.KEYCODE_BACK, up);
    }

    // ---- lifecycle
    protected void onCreate(Bundle state) {
        mCalled = true;
        for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityCreated(this, state);
    }
    public void onCreate(Bundle state, android.os.PersistableBundle p) { onCreate(state); }
    protected void onPostCreate(Bundle state) {}
    protected void onStart() { mCalled = true; for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityStarted(this); }
    protected void onRestart() { mCalled = true; }
    protected void onResume() { mCalled = true; for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityResumed(this); }
    protected void onPostResume() { if (mActionBar != null) mActionBar.setShowHideAnimationEnabled(true); }
    protected void onPause() { mCalled = true; for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityPaused(this); }
    protected void onStop() { mCalled = true; for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityStopped(this); }
    protected void onDestroy() {
        mCalled = true;
        /* the managed dialogs dismissed, as Android's onDestroy dismisses them */
        if (mManagedDialogs != null) {
            for (int i = 0; i < mManagedDialogs.size(); i++) { Dialog d = mManagedDialogs.valueAt(i); if (d.isShowing()) d.dismiss(); }
            mManagedDialogs = null;
        }
        for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivityDestroyed(this);
    }
    protected void onSaveInstanceState(Bundle out) { for (Application.ActivityLifecycleCallbacks c : callbacks()) c.onActivitySaveInstanceState(this, out); }
    public void onSaveInstanceState(Bundle out, android.os.PersistableBundle p) { onSaveInstanceState(out); }
    protected void onRestoreInstanceState(Bundle state) {}
    protected void onNewIntent(Intent i) {}
    /** A new intent for this activity (singleTop, or brought back by CLEAR_TOP): as Android, paused around onNewIntent when it is up. */
    public void meglerNewIntent(Intent i) {
        boolean up = isResumedMegler();
        if (up) meglerPause();
        onNewIntent(i);
        if (up) meglerResume();
    }
    protected void onActivityResult(int request, int result, Intent data) {}
    public void onActivityReenter(int result, Intent data) {}
    protected void onUserLeaveHint() {}
    public void onUserInteraction() {}
    public void onRequestPermissionsResult(int request, String[] perms, int[] results) {}
    public void onConfigurationChanged(Configuration c) { mCalled = true; }
    public void onLowMemory() { mCalled = true; }
    public void onTrimMemory(int level) { mCalled = true; }
    public void onMultiWindowModeChanged(boolean m) {}
    public void onMultiWindowModeChanged(boolean m, Configuration c) {}
    public void onPictureInPictureModeChanged(boolean m) {}
    public void onPictureInPictureModeChanged(boolean m, Configuration c) {}
    public void onTopResumedActivityChanged(boolean top) {}
    public void onEnterAnimationComplete() {}
    public void onLocalVoiceInteractionStarted() {}
    public CharSequence onCreateDescription() { return null; }
    public Object onRetainNonConfigurationInstance() { return null; }
    public Object getLastNonConfigurationInstance() { return null; }
    private ArrayList<Application.ActivityLifecycleCallbacks> callbacks() {
        ArrayList<Application.ActivityLifecycleCallbacks> r = new ArrayList<>(mLifecycleCallbacks);
        if (mApplication != null) r.addAll(mApplication.meglerLifecycleCallbacks());
        return r;
    }
    public void registerActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks c) { mLifecycleCallbacks.add(c); }
    public void unregisterActivityLifecycleCallbacks(Application.ActivityLifecycleCallbacks c) { mLifecycleCallbacks.remove(c); }
    public boolean isResumed() { return mResumed; }
    public final boolean isResumedMegler() { return mResumed; }
    public boolean isFinishing() { return mFinished; }
    public boolean isDestroyed() { return mDestroyed; }
    public boolean isChangingConfigurations() { return mChangingConfigurations; }
    public boolean isTaskRoot() { return mCaller == null; }
    public boolean isChild() { return mParent != null; }
    public final Activity getParent() { return mParent; }
    public boolean isInMultiWindowMode() { return false; }
    public boolean isInPictureInPictureMode() { return false; }
    public boolean isVoiceInteraction() { return false; }
    public boolean isVoiceInteractionRoot() { return false; }
    public boolean isLocalVoiceInteractionSupported() { return false; }
    public boolean isImmersive() { return false; }
    public void setImmersive(boolean i) {}
    public boolean isLaunchedFromBubble() { return false; }
    public boolean isActivityTransitionRunning() { return false; }
    public void recreate() { megler.AppRunner.recreate(this); }

    // ---- the window
    public Window getWindow() {
        if (mWindow == null && mAttaching) return null;
        if (mWindow == null) {
            mWindow = new Window(this);
            mWindow.setCallback(this);
            mWindow.meglerSetActivityWindow(true);
            mWindow.setTitle(mTitle);
            if (mInfo != null && mInfo.windowSoftInputMode != 0) mWindow.setSoftInputMode(mInfo.windowSoftInputMode);
            if (mRequestedOrientation >= 0) megler.Native.setOrientation(mRequestedOrientation);
        }
        return mWindow;
    }
    public WindowManager getWindowManager() { return (WindowManager) getSystemService(WINDOW_SERVICE); }
    @Override public Object getSystemService(String name) {
        if (WINDOW_SERVICE.equals(name)) return new WindowManagerImpl(this);
        return super.getSystemService(name);
    }
    public final boolean requestWindowFeature(int f) { return getWindow().requestFeature(f); }
    public final void setFeatureDrawableResource(int f, int id) {}
    public final void setFeatureDrawable(int f, Drawable d) {}
    public final void setProgressBarVisibility(boolean v) {}
    public final void setProgressBarIndeterminateVisibility(boolean v) {}
    public final void setProgressBarIndeterminate(boolean v) {}
    public final void setProgress(int p) {}
    public void setContentView(int layout) { getWindow().setContentView(layout); initWindowDecorActionBar(); }
    public void setContentView(View v) { getWindow().setContentView(v); initWindowDecorActionBar(); }
    public void setContentView(View v, ViewGroup.LayoutParams p) { getWindow().setContentView(v, p); initWindowDecorActionBar(); }
    public void addContentView(View v, ViewGroup.LayoutParams p) { getWindow().addContentView(v, p); initWindowDecorActionBar(); }
    private void initWindowDecorActionBar() {
        Window w = getWindow();
        w.getDecorView();                  // the theme's features are the window's only once its decor is made: Android does the same
        if (mActionBar == null && w.hasFeature(Window.FEATURE_ACTION_BAR) && !w.isFloating()) mActionBar = new megler.SimpleActionBar(this, (megler.DecorView) w.getDecorView());
    }
    public <T extends View> T findViewById(int id) { return getWindow().findViewById(id); }
    public final <T extends View> T requireViewById(int id) { T v = findViewById(id); if (v == null) throw new IllegalArgumentException("ID does not reference a View inside this Activity"); return v; }
    public View getCurrentFocus() { return mWindow != null ? mWindow.getCurrentFocus() : null; }
    public LayoutInflater getLayoutInflater() { return getWindow().getLayoutInflater(); }
    public MenuInflater getMenuInflater() { return new MenuInflater(this); }
    public ActionBar getActionBar() { initWindowDecorActionBar(); return mActionBar; }
    public void setActionBar(android.widget.Toolbar t) { if (t != null) t.setTitle(getTitle()); }
    public void setTitle(CharSequence t) { mTitle = t; onTitleChanged(t, mTitleColor); }
    public void setTitle(int id) { setTitle(getText(id)); }
    public void setTitleColor(int c) { mTitleColor = c; }
    public final CharSequence getTitle() { return mTitle; }
    public final int getTitleColor() { return mTitleColor; }
    protected void onTitleChanged(CharSequence t, int color) { if (mWindow != null) mWindow.setTitle(t); }
    protected void onChildTitleChanged(Activity child, CharSequence t) {}
    public void onContentChanged() {}
    public void onWindowFocusChanged(boolean f) {}
    public void onAttachedToWindow() {}
    public void onDetachedFromWindow() {}
    public void onWindowAttributesChanged(WindowManager.LayoutParams p) {}
    public boolean hasWindowFocus() { View d = mWindow != null ? mWindow.peekDecorView() : null; return d != null && d.hasWindowFocus(); }
    /* as Android: once started, showing the activity again adds its window (Spotify's login hides itself until it has loaded) */
    public void setVisible(boolean v) {
        if (mVisibleFromClient == v) return;
        mVisibleFromClient = v;
        if (!mStarted) return;
        if (v) makeVisible();
        else if (mWindow != null && mWindow.peekDecorView() != null) mWindow.peekDecorView().setVisibility(View.INVISIBLE);
    }
    public void setRequestedOrientation(int o) { mRequestedOrientation = o; megler.Native.setOrientation(o); }
    public int getRequestedOrientation() { return mRequestedOrientation; }
    public void setShowWhenLocked(boolean b) {}
    public void setTurnScreenOn(boolean b) {}
    public void setInheritShowWhenLocked(boolean b) {}
    public void setTaskDescription(ActivityManager.TaskDescription d) {}
    public void setRecentsScreenshotEnabled(boolean e) {}
    public void reportFullyDrawn() {}
    public void setVolumeControlStream(int stream) {}
    public final int getVolumeControlStream() { return 3; }
    public final void setDefaultKeyMode(int m) {}
    public void overridePendingTransition(int a, int b) {}
    public void overridePendingTransition(int a, int b, int c) {}
    public void overrideActivityTransition(int type, int enter, int exit) {}
    public void overrideActivityTransition(int type, int enter, int exit, int bg) {}
    public void clearOverrideActivityTransition(int type) {}
    public void postponeEnterTransition() {}
    public void startPostponedEnterTransition() {}
    public void setEnterSharedElementCallback(SharedElementCallback c) {}
    public void setExitSharedElementCallback(SharedElementCallback c) {}
    public void enterPictureInPictureMode() {}
    public boolean enterPictureInPictureMode(PictureInPictureParams p) { return false; }
    public void setPictureInPictureParams(PictureInPictureParams p) {}
    public android.window.OnBackInvokedDispatcher getOnBackInvokedDispatcher() { return getWindow().getOnBackInvokedDispatcher(); }
    public android.window.SplashScreen getSplashScreen() { return megler.SplashScreens.of(this); }
    public final boolean isTaskRootMegler() { return isTaskRoot(); }

    // ---- intents and results
    public Intent getIntent() { return mIntent; }
    public void setIntent(Intent i) { mIntent = i; }
    public final Application getApplication() { return mApplication != null ? mApplication : Application.meglerGet(); }
    public ComponentName getComponentName() { return mComponent != null ? mComponent : new ComponentName(this, getClass()); }
    public String getLocalClassName() { String p = getPackageName(), c = getClass().getName(); return c.startsWith(p + ".") ? c.substring(p.length() + 1) : c; }
    public String getCallingPackage() { return mCaller != null ? getPackageName() : null; }
    public ComponentName getCallingActivity() { return mCaller != null ? mCaller.getComponentName() : null; }
    public String getLaunchedFromPackage() { return getCallingPackage(); }
    public int getTaskId() { return 1; }
    public final void setResult(int code) { mResultCode = code; mResultData = null; }
    public final void setResult(int code, Intent data) { mResultCode = code; mResultData = data; }
    @Override public void startActivity(Intent i) { startActivityForResult(i, -1); }
    @Override public void startActivity(Intent i, Bundle o) { startActivityForResult(i, -1, o); }
    public void startActivityForResult(Intent i, int request) { startActivityForResult(i, request, null); }
    public void startActivityForResult(Intent i, int request, Bundle o) {
        if (request >= 0 && !windowShown() && megler.Manifest.resolve(i) != null) mStartedActivity = true;
        megler.AppRunner.startActivity(this, i, request, o);
    }
    public void startActivityFromChild(Activity child, Intent i, int request) { startActivityForResult(i, request); }
    public void startActivityFromFragment(Fragment f, Intent i, int request) { startActivityForResult(i, request); }
    public boolean startActivityIfNeeded(Intent i, int request) { startActivityForResult(i, request); return true; }
    public boolean startNextMatchingActivity(Intent i) { return false; }
    /*
     * An IntentSender of a PendingIntent for an activity (IntentSender.meglerOf) starts that activity from this one, as Android
     * does: the fill-in intent's extras added, its flags as the mask and values say (FLAG_ACTIVITY_FORWARD_RESULT among them:
     * megler.AppRunner). GmsCore's sign-in unpacks its screen's PendingIntent so (UnpackingRedirectActivity), and these did
     * nothing: the sign-in ended at once, as cancelled. An IntentSender of anything else is sent.
     */
    public void startIntentSenderForResult(IntentSender s, int request, Intent fill, int mask, int values, int extra) throws IntentSender.SendIntentException {
        startIntentSenderForResult(s, request, fill, mask, values, extra, null);
    }
    public void startIntentSenderForResult(IntentSender s, int request, Intent fill, int mask, int values, int extra, Bundle o) throws IntentSender.SendIntentException {
        PendingIntent p = s == null ? null : s.meglerPending();
        if (p == null || !p.isActivity()) {
            if (s != null) s.sendIntent(this, 0, fill, null, null);
            return;
        }
        Intent i = new Intent(p.meglerIntent());
        if (fill != null) i.fillIn(fill, 0);
        i.setFlags((i.getFlags() & ~mask) | (values & mask));
        startActivityForResult(i, request, o);
    }
    public void finish() {
        if (mFinished) return;
        mFinished = true;
        // the app's last activity going ends the app: say where from, which is often all a log has to explain it
        if (megler.AppRunner.top() == this && megler.AppRunner.stackSize() == 1) android.util.Log.i("Megler", "the last activity finishes", new Throwable("finish() called from"));
        megler.AppRunner.finish(this);
    }
    public void finishAffinity() { megler.AppRunner.finishAffinity(this); }
    public void finishAndRemoveTask() { finishAffinity(); }
    public void finishAfterTransition() { finish(); }
    public void finishActivity(int request) {}
    public void finishFromChild(Activity child) { finish(); }
    public boolean moveTaskToBack(boolean nonRoot) { megler.Native.exit(); return true; }
    public boolean navigateUpTo(Intent up) { finish(); return true; }
    public boolean navigateUpToFromChild(Activity child, Intent up) { return navigateUpTo(up); }
    public boolean onNavigateUp() { if (mInfo != null && mInfo.parentActivity != null) { finish(); return true; } return false; }
    public boolean shouldUpRecreateTask(Intent target) { return false; }
    public Intent getParentActivityIntent() { return mInfo != null && mInfo.parentActivity != null ? new Intent().setClassName(getPackageName(), mInfo.parentActivity) : null; }
    public final void requestPermissions(String[] perms, int request) {
        final int[] r = new int[perms.length];
        mHandler.post(() -> onRequestPermissionsResult(request, perms, r));
    }
    public boolean shouldShowRequestPermissionRationale(String p) { return false; }
    public SharedPreferences getPreferences(int mode) { return getSharedPreferences(getLocalClassName(), mode); }
    public void runOnUiThread(Runnable r) { if (Looper.myLooper() == Looper.getMainLooper()) r.run(); else mHandler.post(r); }
    public final Cursor_ managedQueryMegler() { return null; }
    interface Cursor_ {}
    public FragmentManager getFragmentManager() { if (mFragments == null) mFragments = new FragmentManager(this); return mFragments; }
    public void onAttachFragment(Fragment f) {}
    private LoaderManagerImpl mLoaders;
    public LoaderManager getLoaderManager() { if (mLoaders == null) mLoaders = new LoaderManagerImpl(mStarted); return mLoaders; }
    /*
     * The activity's managed dialogs (showDialog, dismissDialog, removeDialog), as Android keeps them: made once by
     * onCreateDialog(id, args), kept by id, prepared (onPrepareDialog) and shown at each showDialog. They did nothing here:
     * WhatsApp asks "is this the right number?" through showDialog(21) when one taps Next on its phone number, and nothing
     * came up, so the registration went no further.
     */
    private android.util.SparseArray<Dialog> mManagedDialogs;
    public final void showDialog(int id) { showDialog(id, null); }
    public final boolean showDialog(int id, Bundle args) {
        if (mManagedDialogs == null) mManagedDialogs = new android.util.SparseArray<>();
        Dialog d = mManagedDialogs.get(id);
        if (d == null) {
            d = onCreateDialog(id, args);
            if (d == null) return false;
            mManagedDialogs.put(id, d);
        }
        onPrepareDialog(id, d, args);
        d.show();
        return true;
    }
    public void dismissDialog(int id) {
        Dialog d = mManagedDialogs == null ? null : mManagedDialogs.get(id);
        if (d == null) throw new IllegalArgumentException("no dialog with id " + id + " was ever shown via Activity#showDialog");
        d.dismiss();
    }
    public void removeDialog(int id) {
        Dialog d = mManagedDialogs == null ? null : mManagedDialogs.get(id);
        if (d == null) return;
        d.dismiss();
        mManagedDialogs.remove(id);
    }
    protected Dialog onCreateDialog(int id) { return null; }
    protected Dialog onCreateDialog(int id, Bundle args) { return onCreateDialog(id); }
    protected void onPrepareDialog(int id, Dialog dialog) { dialog.setOwnerActivity(this); }
    protected void onPrepareDialog(int id, Dialog dialog, Bundle args) { onPrepareDialog(id, dialog); }
    public boolean releaseInstance() { return false; }
    public void setLocusContext(android.content.LocusId id, Bundle b) {}
    public boolean showAssist(Bundle args) { return false; }
    public android.view.DragAndDropPermissions requestDragAndDropPermissions(DragEvent e) { return null; }
    public void startLockTask() {}
    public void stopLockTask() {}
    public void setDisablePreviewScreenshots(boolean d) {}
    public void triggerSearch(String q, Bundle d) {}
    public void startSearch(String q, boolean s, Bundle d, boolean g) {}
    public void takeKeyEvents(boolean b) {}
    public void registerScreenCaptureCallback(java.util.concurrent.Executor e, ScreenCaptureCallback cb) {}
    public void unregisterScreenCaptureCallback(ScreenCaptureCallback cb) {}

    // ---- events
    public boolean dispatchKeyEvent(KeyEvent e) {
        onUserInteraction();
        if (e.getKeyCode() == KeyEvent.KEYCODE_MENU && mActionBar != null && mActionBar.onMenuKeyEvent(e)) return true;
        Window w = getWindow();
        if (w.superDispatchKeyEvent(e)) return true;
        View decor = w.peekDecorView();
        return e.dispatch(this, decor != null ? decor.getKeyDispatcherState() : null, this);
    }
    public boolean dispatchKeyShortcutEvent(KeyEvent e) { return onKeyShortcut(e.getKeyCode(), e); }
    public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN) onUserInteraction();
        if (getWindow().superDispatchTouchEvent(e)) return true;
        return onTouchEvent(e);
    }
    public boolean dispatchTrackballEvent(MotionEvent e) { return onTrackballEvent(e); }
    public boolean dispatchGenericMotionEvent(MotionEvent e) { if (getWindow().superDispatchGenericMotionEvent(e)) return true; return onGenericMotionEvent(e); }
    public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent e) { return false; }
    public boolean onTouchEvent(MotionEvent e) {
        if (getWindow().shouldCloseOnTouchMegler() && e.getActionMasked() == MotionEvent.ACTION_DOWN) { finish(); return true; }
        return false;
    }
    public boolean onTrackballEvent(MotionEvent e) { return false; }
    public boolean onGenericMotionEvent(MotionEvent e) { return false; }
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK) { e.startTracking(); return true; }
        return false;
    }
    public boolean onKeyLongPress(int code, KeyEvent e) { return false; }
    public boolean onKeyUp(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK && !e.isCanceled()) { onBackPressed(); return true; }
        return false;
    }
    public boolean onKeyMultiple(int code, int n, KeyEvent e) { return false; }
    public boolean onKeyShortcut(int code, KeyEvent e) { return false; }
    public void onBackPressed() {
        if (mActionBar != null && mActionBar.collapseActionView()) return;
        if (mFragments != null && mFragments.popBackStackImmediate()) return;
        finishAfterTransition();
    }
    public boolean onSearchRequested() { return false; }
    public boolean onSearchRequested(SearchEvent e) { return false; }
    public void onPointerCaptureChanged(boolean c) {}
    public void onProvideAssistData(Bundle d) {}
    public void onProvideKeyboardShortcuts(java.util.List<KeyboardShortcutGroup> d, Menu m, int dev) {}
    public void onGetDirectActions(android.os.CancellationSignal s, java.util.function.Consumer<java.util.List<Object>> c) {}

    // ---- menus (no on-screen menu: the options menu is created so apps can find their items, and the menu key can open it later)
    public View onCreatePanelView(int f) { return null; }
    public boolean onCreatePanelMenu(int f, Menu m) { return f == Window.FEATURE_OPTIONS_PANEL && onCreateOptionsMenu(m); }
    public boolean onPreparePanel(int f, View v, Menu m) { return f != Window.FEATURE_OPTIONS_PANEL || onPrepareOptionsMenu(m); }
    public boolean onMenuOpened(int f, Menu m) { return true; }
    public boolean onMenuItemSelected(int f, MenuItem item) { return f == Window.FEATURE_OPTIONS_PANEL ? onOptionsItemSelected(item) : onContextItemSelected(item); }
    public void onPanelClosed(int f, Menu m) {}
    public boolean onCreateOptionsMenu(Menu m) { return true; }
    public boolean onPrepareOptionsMenu(Menu m) { return true; }
    public boolean onOptionsItemSelected(MenuItem item) { if (item.getItemId() == android.R.id.home && mInfo != null && mInfo.parentActivity != null) return onNavigateUp(); return false; }
    public void onOptionsMenuClosed(Menu m) {}
    public void openOptionsMenu() {}
    public void closeOptionsMenu() {}
    public void invalidateOptionsMenu() { if (mActionBar != null) mActionBar.invalidateOptionsMenu(); }
    public void onCreateContextMenu(ContextMenu m, View v, ContextMenu.ContextMenuInfo i) {}
    public void registerForContextMenu(View v) { v.setOnCreateContextMenuListener(this); }
    public void unregisterForContextMenu(View v) { v.setOnCreateContextMenuListener(null); }
    public void openContextMenu(View v) { v.showContextMenu(); }
    public void closeContextMenu() {}
    public boolean onContextItemSelected(MenuItem item) { return false; }
    public void onContextMenuClosed(Menu m) {}
    public ActionMode startActionMode(ActionMode.Callback cb) { return null; }
    public ActionMode startActionMode(ActionMode.Callback cb, int type) { return null; }
    public ActionMode onWindowStartingActionMode(ActionMode.Callback cb) { return null; }
    public ActionMode onWindowStartingActionMode(ActionMode.Callback cb, int type) { return null; }
    public void onActionModeStarted(ActionMode m) {}
    public void onActionModeFinished(ActionMode m) {}

    // ---- LayoutInflater.Factory2 (fragments in layouts)
    public View onCreateView(String name, Context c, android.util.AttributeSet a) { return null; }
    public View onCreateView(View parent, String name, Context c, android.util.AttributeSet a) {
        if (!"fragment".equals(name)) return onCreateView(name, c, a);
        return getFragmentManager().meglerInflateFragment(parent, c, a);
    }
    public void dump(String prefix, java.io.FileDescriptor fd, java.io.PrintWriter w, String[] args) {}
    static final class SuperNotCalledException extends android.util.AndroidRuntimeException { SuperNotCalledException(String s) { super(s); } }
    // ---- platform API stubs (tools/compat/genstubs.py)
    public interface ScreenCaptureCallback {
        void onScreenCaptured();
    }
            // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
            private final java.util.HashMap<String, Object> meglerFill = new java.util.HashMap<>();
            public static final int DONT_FINISH_TASK_WITH_ACTIVITY = 0;
            public static final java.lang.String DUMP_ARG_AUTOFILL = "--autofill";
            public static final java.lang.String DUMP_ARG_CONTENT_CAPTURE = "--contentcapture";
            public static final java.lang.String DUMP_ARG_DUMP_DUMPABLE = "--dump-dumpable";
            public static final java.lang.String DUMP_ARG_LIST_DUMPABLES = "--list-dumpables";
            public static final java.lang.String DUMP_ARG_TRANSLATION = "--translation";
            public static final int FINISH_TASK_WITH_ACTIVITY = 2;
            public static final int FINISH_TASK_WITH_ROOT_ACTIVITY = 1;
            public static final int FULLSCREEN_MODE_REQUEST_ENTER = 1;
            public static final int FULLSCREEN_MODE_REQUEST_EXIT = 0;
            public boolean addDumpable(android.util.Dumpable p0) { return false; }
            public boolean canStartActivityForResult() { return false; }
            public android.content.ComponentName contentCaptureClientGetComponentName() { return null; }
            public void convertFromTranslucent() {}
            public boolean convertToTranslucent(android.app.Activity.TranslucentConversionListener p0, android.app.ActivityOptions p1) { return false; }
            public android.app.PendingIntent createPendingResult(int p0, android.content.Intent p1, int p2) { return null; }
            public void dismissKeyboardShortcutsHelper() {}
            public void dispatchEnterAnimationComplete() {}
            public void dumpInternal(java.lang.String p0, java.io.FileDescriptor p1, java.io.PrintWriter p2, java.lang.String[] p3) {}
            public void enableTaskLocaleOverride() {}
            public void finishActivityFromChild(android.app.Activity p0, int p1) {}
            public android.content.pm.ActivityInfo getActivityInfo() { return null; }
            public android.app.ActivityThread getActivityThread() { return null; }
            public android.os.IBinder getActivityToken() { return null; }
            public android.os.IBinder getAssistToken() { return null; }
            public android.view.autofill.AutofillManager.AutofillClient getAutofillClient() { return null; }
            public android.app.ComponentCaller getCaller() { return null; }
            public int getChangingConfigurations() { return 0; }
            public android.view.contentcapture.ContentCaptureManager.ContentCaptureClient getContentCaptureClient() { return null; }
            public android.transition.Scene getContentScene() { return null; }
            public android.transition.TransitionManager getContentTransitionManager() { return (android.transition.TransitionManager) meglerFill.get("ContentTransitionManager"); }
            public android.app.ComponentCaller getCurrentCaller() { return null; }
            public android.app.ComponentCaller getInitialCaller() { return null; }
            public int getLaunchedFromUid() { return 0; }
            public int getMaxNumPictureInPictureActions() { return 0; }
            public android.media.session.MediaController getMediaController() { return (android.media.session.MediaController) meglerFill.get("MediaController"); }
            public int getNextAutofillId() { return 0; }
            public android.net.Uri getReferrer() { return null; }
            public android.view.SearchEvent getSearchEvent() { return null; }
            public android.os.IBinder getShareableActivityToken() { return null; }
            public android.app.VoiceInteractor getVoiceInteractor() { return null; }
            public boolean isBackgroundVisibleBehind() { return false; }
            public boolean isHandoffEnabled() { return (meglerFill.get("HandoffEnabled") instanceof Boolean ? (Boolean) meglerFill.get("HandoffEnabled") : false); }
            public boolean isHandoffFullTaskRecreationAllowed() { return false; }
            public boolean isOverlayWithDecorCaptionEnabled() { return (meglerFill.get("OverlayWithDecorCaptionEnabled") instanceof Boolean ? (Boolean) meglerFill.get("OverlayWithDecorCaptionEnabled") : false); }
            public boolean isVisibleForAutofill() { return false; }
            public android.database.Cursor managedQuery(android.net.Uri p0, java.lang.String[] p1, java.lang.String p2, java.lang.String p3) { return null; }
            public android.database.Cursor managedQuery(android.net.Uri p0, java.lang.String[] p1, java.lang.String p2, java.lang.String[] p3, java.lang.String p4) { return null; }
            public void onActivityResult(int p0, int p1, android.content.Intent p2, android.app.ComponentCaller p3) {}
            public void onBackgroundVisibleBehindChanged(boolean p0) {}
            public void onCreateNavigateUpTaskStack(android.app.TaskStackBuilder p0) {}
            public boolean onCreateThumbnail(android.graphics.Bitmap p0, android.graphics.Canvas p1) { return false; }
            public void onLocalVoiceInteractionStopped() {}
            public void onMovedToDisplay(int p0, android.content.res.Configuration p1) {}
            public boolean onNavigateUpFromChild(android.app.Activity p0) { return false; }
            public void onNewIntent(android.content.Intent p0, android.app.ComponentCaller p1) {}
            public void onNewSceneTransitionInfo(android.app.ActivityOptions.SceneTransitionInfo p0) {}
            public void onPerformDirectAction(java.lang.String p0, android.os.Bundle p1, android.os.CancellationSignal p2, java.util.function.Consumer p3) {}
            public boolean onPictureInPictureRequested() { return false; }
            public void onPictureInPictureUiStateChanged(android.app.PictureInPictureUiState p0) {}
            public void onPostCreate(android.os.Bundle p0, android.os.PersistableBundle p1) {}
            public void onPrepareNavigateUpTaskStack(android.app.TaskStackBuilder p0) {}
            public void onProvideAssistContent(android.app.assist.AssistContent p0) {}
            public android.net.Uri onProvideReferrer() { return null; }
            public void onRequestPermissionsResult(int p0, java.lang.String[] p1, int[] p2, int p3) {}
            public void onRestoreInstanceState(android.os.Bundle p0, android.os.PersistableBundle p1) {}
            public void onStateNotSaved() {}
            public void onVisibleBehindCanceled() {}
            public void onWindowDismissed(boolean p0, boolean p1) {}
            public void requestFullscreenMode(int p0, android.os.OutcomeReceiver p1) {}
            public void requestOpenInBrowserEducation() {}
            public void requestPermissions(java.lang.String[] p0, int p1, int p2) {}
            public void requestShowKeyboardShortcuts() {}
            public boolean requestVisibleBehind(boolean p0) { return false; }
            public void setActivityRecordInputSinkEnabled(boolean p0) {}
            public void setAllowCrossUidActivitySwitchFromBelow(boolean p0) {}
            public void setContentTransitionManager(android.transition.TransitionManager p0) { meglerFill.put("ContentTransitionManager", p0); }
            public void setFeatureDrawableAlpha(int p0, int p1) {}
            public void setFeatureDrawableUri(int p0, android.net.Uri p1) {}
            public void setFinishOnTouchOutside(boolean p0) {}
            public void setForceSendResultForMediaProjection() {}
            public void setHandoffEnabled(boolean p0, boolean p1) {}
            public void setIntent(android.content.Intent p0, android.app.ComponentCaller p1) {}
            public void setMediaController(android.media.session.MediaController p0) { meglerFill.put("MediaController", p0); }
            public void setOverlayWithDecorCaptionEnabled(boolean p0) { meglerFill.put("OverlayWithDecorCaptionEnabled", Boolean.valueOf(p0)); }
            public void setPersistent(boolean p0) {}
            public void setSecondaryProgress(int p0) {}
            public void setShouldDockBigOverlays(boolean p0) {}
            public boolean setTranslucent(boolean p0) { return false; }
            public void setVrModeEnabled(boolean p0, android.content.ComponentName p1) {}
            public boolean shouldDockBigOverlays() { return false; }
            public boolean shouldShowRequestPermissionRationale(java.lang.String p0, int p1) { return false; }
            public void showLockTaskEscapeMessage() {}
            public void startActivityAsCaller(android.content.Intent p0, android.os.Bundle p1, boolean p2, int p3) {}
            public void startActivityAsCaller(android.content.Intent p0, android.os.Bundle p1, boolean p2, int p3, int p4) {}
            public void startActivityAsUser(android.content.Intent p0, android.os.Bundle p1, android.os.UserHandle p2) {}
            public void startActivityAsUser(android.content.Intent p0, android.os.UserHandle p1) {}
            public void startActivityForResult(java.lang.String p0, android.content.Intent p1, int p2, android.os.Bundle p3) {}
            public void startActivityForResultAsUser(android.content.Intent p0, int p1, android.os.Bundle p2, android.os.UserHandle p3) {}
            public void startActivityForResultAsUser(android.content.Intent p0, int p1, android.os.UserHandle p2) {}
            public void startActivityForResultAsUser(android.content.Intent p0, java.lang.String p1, int p2, android.os.Bundle p3, android.os.UserHandle p4) {}
            public void startActivityFromChild(android.app.Activity p0, android.content.Intent p1, int p2, android.os.Bundle p3) {}
            public void startActivityFromFragment(android.app.Fragment p0, android.content.Intent p1, int p2, android.os.Bundle p3) {}
            public boolean startActivityIfNeeded(android.content.Intent p0, int p1, android.os.Bundle p2) { return false; }
            public void startIntentSender(android.content.IntentSender p0, android.content.Intent p1, int p2, int p3, int p4) { startIntentSender(p0, p1, p2, p3, p4, null); }
            public void startIntentSender(android.content.IntentSender p0, android.content.Intent p1, int p2, int p3, int p4, android.os.Bundle p5) {
                try { startIntentSenderForResult(p0, -1, p1, p2, p3, p4, p5); } catch (IntentSender.SendIntentException e) { throw new RuntimeException(e); }
            }
            public void startIntentSenderForResult(android.content.IntentSender p0, java.lang.String p1, int p2, android.content.Intent p3, int p4, int p5, android.os.Bundle p6) {}
            public void startIntentSenderForResultInner(android.content.IntentSender p0, java.lang.String p1, int p2, android.content.Intent p3, int p4, int p5, android.os.Bundle p6) {}
            public void startIntentSenderFromChild(android.app.Activity p0, android.content.IntentSender p1, int p2, android.content.Intent p3, int p4, int p5, int p6) {}
            public void startIntentSenderFromChild(android.app.Activity p0, android.content.IntentSender p1, int p2, android.content.Intent p3, int p4, int p5, int p6, android.os.Bundle p7) {}
            public void startLocalVoiceInteraction(android.os.Bundle p0) {}
            public void startManagingCursor(android.database.Cursor p0) {}
            public boolean startNextMatchingActivity(android.content.Intent p0, android.os.Bundle p1) { return false; }
            public void stopLocalVoiceInteraction() {}
            public void stopManagingCursor(android.database.Cursor p0) {}
            public void unregisterRemoteAnimations() {}
            public void updateUiTranslationState(int p0, android.view.translation.TranslationSpec p1, android.view.translation.TranslationSpec p2, java.util.List p3, android.view.translation.UiTranslationSpec p4) {}
            // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public interface TranslucentConversionListener {
        void onTranslucentConversionComplete(boolean p0);
    }
    // ---- end of generated nested classes
}
