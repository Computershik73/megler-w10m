// Overlay of core/framework/gen-src/android/app/Instrumentation.java (tools/build-framework.sh): the generated class, whose
// every method was empty. It differs in what Android's ActivityThread asks its Instrumentation, which now does what
// Android's does, by Megler's means: making the Application and an activity (through the app's component factory, as
// Android's), and calling them at each step of their life -- callApplicationOnCreate, callActivityOnCreate, -OnStart,
// -OnRestart, -OnResume, -OnPause, -OnStop, -OnDestroy, -OnNewIntent, -OnPostCreate, -OnSaveInstanceState,
// -OnRestoreInstanceState, -OnUserLeaving -- and onException, which is false as Android's.
//
// Why. The process's Instrumentation is ActivityThread's hidden field mInstrumentation (overlaid), and Megler makes the
// app's Application and activities and calls their lifecycle through the one that is in it at that moment
// (megler.AppRunner, the overlaid Activity's megler* methods), as Android's ActivityThread and Activity do. An app puts its
// own in the field, a subclass that holds the platform's and passes on to it what it does not change: TikTok Lite from
// Google Play (its "mini" app, com.ss.android.ugc.aweme.minilite.ActivityDispatchService) does so while its Application is
// attached, to make another class for an activity's name (newActivity) until its feature module is there. The field was
// not there (NoSuchFieldException: mInstrumentation), and had it been, nothing would have asked what was put in it.
//
// What Megler has no like of stays as generated: monitors, the test runner's own calls (start, finish, sendKeySync...),
// and execStartActivity, which Megler's Activity and contexts do not start an activity through.
package android.app;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class Instrumentation {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final boolean DEBUG_START_ACTIVITY = false;
    public static final java.lang.String REPORT_KEY_IDENTIFIER = "id";
    public static final java.lang.String REPORT_KEY_STREAMRESULT = "stream";
    public static final java.lang.String TAG = "Instrumentation";
    public Instrumentation() {}

    // ---- what ActivityThread asks of it
    /**
     * As Android's: the Application made by the app's component factory, and attached to its context. (megler.AppRunner hears
     * of the process's own between the two, where it makes it known to those who ask for the process's Application, as it
     * did before the Application was made here.)
     */
    public Application newApplication(ClassLoader cl, String className, Context context) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Application app = megler.AppRunner.componentFactory().instantiateApplication(cl, className);
        megler.AppRunner.applicationMade(app);
        app.meglerAttach(context);
        return app;
    }
    public static Application newApplication(Class<?> clazz, Context context) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        Application app = (Application) clazz.newInstance();
        app.meglerAttach(context);
        return app;
    }
    public void callApplicationOnCreate(Application app) { app.onCreate(); }
    /** As Android's: the activity made by the app's component factory; the caller attaches it. */
    public Activity newActivity(ClassLoader cl, String className, Intent intent) throws InstantiationException, IllegalAccessException, ClassNotFoundException {
        return megler.AppRunner.componentFactory().instantiateActivity(cl, className, intent);
    }
    /** As Android's (which the platform itself does not call: tests do): an activity of that class, attached; of what Android
     *  attaches it with, Megler's activity takes the context, the Application, the intent, the parent and the title. */
    public Activity newActivity(Class<?> clazz, Context context, android.os.IBinder token, Application application, Intent intent, android.content.pm.ActivityInfo info,
            CharSequence title, Activity parent, String id, Object lastNonConfigurationInstance) throws InstantiationException, IllegalAccessException {
        Activity activity = (Activity) clazz.newInstance();
        if (parent != null) activity.meglerSetParent(parent);
        activity.meglerAttach(context, application, intent, null, null, -1);
        if (title != null) activity.setTitle(title);
        return activity;
    }
    /* The steps of an activity's life, each as Android's: the whole of the creation, the pause and the destruction (Activity's
       performCreate, performPause, performDestroy, with the callbacks around them), and the activity's own method alone for
       the others, which Activity's megler* methods call in the middle of theirs as Android's perform* methods do. Megler
       keeps no persistent state (android:persistableMode): the calls with one are those without. */
    public void callActivityOnCreate(Activity activity, Bundle icicle) { activity.meglerPerformCreate(icicle); }
    public void callActivityOnCreate(Activity activity, Bundle icicle, android.os.PersistableBundle persistentState) { activity.meglerPerformCreate(icicle); }
    public void callActivityOnDestroy(Activity activity) { activity.meglerPerformDestroy(); }
    public void callActivityOnRestoreInstanceState(Activity activity, Bundle savedInstanceState) { activity.onRestoreInstanceState(savedInstanceState); }
    public void callActivityOnRestoreInstanceState(Activity activity, Bundle savedInstanceState, android.os.PersistableBundle persistentState) { activity.onRestoreInstanceState(savedInstanceState); }
    public void callActivityOnPostCreate(Activity activity, Bundle savedInstanceState) { activity.onPostCreate(savedInstanceState); }
    public void callActivityOnPostCreate(Activity activity, Bundle savedInstanceState, android.os.PersistableBundle persistentState) { activity.onPostCreate(savedInstanceState); }
    public void callActivityOnNewIntent(Activity activity, Intent intent) { activity.onNewIntent(intent); }
    public void callActivityOnNewIntent(Activity activity, Intent intent, android.app.ComponentCaller caller) { activity.onNewIntent(intent); }
    public void callActivityOnStart(Activity activity) { activity.onStart(); }
    public void callActivityOnRestart(Activity activity) { activity.onRestart(); }
    public void callActivityOnResume(Activity activity) { activity.onResume(); }
    public void callActivityOnStop(Activity activity) { activity.onStop(); }
    public void callActivityOnSaveInstanceState(Activity activity, Bundle outState) { activity.onSaveInstanceState(outState); }
    public void callActivityOnSaveInstanceState(Activity activity, Bundle outState, android.os.PersistableBundle outPersistentState) { activity.onSaveInstanceState(outState); }
    public void callActivityOnPause(Activity activity) { activity.meglerPerformPause(); }
    public void callActivityOnUserLeaving(Activity activity) { activity.onUserInteraction(); activity.onUserLeaveHint(); }
    /** As Android's: false, the exception is not handled here (an app's own may say otherwise of its components'). */
    public boolean onException(Object obj, Throwable e) { return false; }

    /** What an activity started for a result gave: the code and the data it was made with, as Android's keeps them. */
    public static final class ActivityResult {
        private final int mResultCode;
        private final Intent mResultData;
        public ActivityResult(int resultCode, Intent resultData) { mResultCode = resultCode; mResultData = resultData; }
        public int getResultCode() { return mResultCode; }
        public Intent getResultData() { return mResultData; }
    }

    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static void checkStartActivityResult(int p0, java.lang.Object p1) {}
    public android.os.TestLooperManager acquireLooperManager(android.os.Looper p0) { return null; }
    public android.app.Instrumentation.ActivityMonitor addMonitor(android.content.IntentFilter p0, android.app.Instrumentation.ActivityResult p1, boolean p2) { return null; }
    public android.app.Instrumentation.ActivityMonitor addMonitor(java.lang.String p0, android.app.Instrumentation.ActivityResult p1, boolean p2) { return null; }
    public void addMonitor(android.app.Instrumentation.ActivityMonitor p0) {}
    public void addResults(android.os.Bundle p0) {}
    public void basicInit(android.content.Context p0, android.content.Context p1, android.app.UiAutomation p2) {}
    public void callActivityOnPictureInPictureRequested(android.app.Activity p0) {}
    public boolean checkMonitorHit(android.app.Instrumentation.ActivityMonitor p0, int p1) { return false; }
    public void endPerformanceSnapshot() {}
    public void execStartActivities(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, android.app.Activity p3, android.content.Intent[] p4, android.os.Bundle p5) {}
    public int execStartActivitiesAsUser(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, android.app.Activity p3, android.content.Intent[] p4, android.os.Bundle p5, int p6) { return 0; }
    public android.app.Instrumentation.ActivityResult execStartActivity(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, android.app.Activity p3, android.content.Intent p4, int p5, android.os.Bundle p6) { return null; }
    public android.app.Instrumentation.ActivityResult execStartActivity(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, java.lang.String p3, android.content.Intent p4, int p5, android.os.Bundle p6) { return null; }
    public android.app.Instrumentation.ActivityResult execStartActivity(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, java.lang.String p3, android.content.Intent p4, int p5, android.os.Bundle p6, android.os.UserHandle p7) { return null; }
    public android.app.Instrumentation.ActivityResult execStartActivityAsCaller(android.content.Context p0, android.os.IBinder p1, android.os.IBinder p2, android.app.Activity p3, android.content.Intent p4, int p5, android.os.Bundle p6, boolean p7, int p8) { return null; }
    public void execStartActivityFromAppTask(android.content.Context p0, android.os.IBinder p1, android.app.IAppTask p2, android.content.Intent p3, android.os.Bundle p4) {}
    public void finish(int p0, android.os.Bundle p1) {}
    public android.os.Bundle getAllocCounts() { return (android.os.Bundle) meglerProps.get("AllocCounts"); }
    public android.os.Bundle getBinderCounts() { return (android.os.Bundle) meglerProps.get("BinderCounts"); }
    public android.content.ComponentName getComponentName() { return (android.content.ComponentName) meglerProps.get("ComponentName"); }
    public android.content.Context getContext() { return (android.content.Context) meglerProps.get("Context"); }
    public java.lang.String getProcessName() { return (java.lang.String) meglerProps.get("ProcessName"); }
    public android.content.Context getTargetContext() { return (android.content.Context) meglerProps.get("TargetContext"); }
    public android.app.UiAutomation getUiAutomation() { return (android.app.UiAutomation) meglerProps.get("UiAutomation"); }
    public android.app.UiAutomation getUiAutomation(int p0) { return null; }
    public boolean invokeContextMenuAction(android.app.Activity p0, int p1, int p2) { return false; }
    public boolean invokeMenuActionSync(android.app.Activity p0, int p1, int p2) { return false; }
    public boolean isInstrumenting() { return (meglerProps.get("Instrumenting") instanceof Boolean ? (Boolean) meglerProps.get("Instrumenting") : false); }
    public boolean isProfiling() { return (meglerProps.get("Profiling") instanceof Boolean ? (Boolean) meglerProps.get("Profiling") : false); }
    public void onCreate(android.os.Bundle p0) {}
    public void onDestroy() {}
    public void onStart() {}
    public void removeMonitor(android.app.Instrumentation.ActivityMonitor p0) {}
    public void resetInTouchMode() {}
    public void runOnMainSync(java.lang.Runnable p0) {}
    public void sendCharacterSync(int p0) {}
    public void sendKeyDownUpSync(int p0) {}
    public void sendKeySync(android.view.KeyEvent p0) {}
    public void sendPointerSync(android.view.MotionEvent p0) {}
    public void sendStatus(int p0, android.os.Bundle p1) {}
    public void sendStringSync(java.lang.String p0) {}
    public void sendTrackballEventSync(android.view.MotionEvent p0) {}
    public void setAutomaticPerformanceSnapshots() {}
    public void setInTouchMode(boolean p0) { meglerProps.put("InTouchMode", Boolean.valueOf(p0)); }
    public void start() {}
    public android.app.Activity startActivitySync(android.content.Intent p0) { return null; }
    public android.app.Activity startActivitySync(android.content.Intent p0, android.os.Bundle p1) { return null; }
    public void startAllocCounting() {}
    public void startPerformanceSnapshot() {}
    public void startProfiling() {}
    public void stopAllocCounting() {}
    public void stopProfiling() {}
    public void waitForIdle(java.lang.Runnable p0) {}
    public void waitForIdleSync() {}
    public android.app.Activity waitForMonitor(android.app.Instrumentation.ActivityMonitor p0) { return null; }
    public android.app.Activity waitForMonitorWithTimeout(android.app.Instrumentation.ActivityMonitor p0, long p1) { return null; }
    public static class ActivityMonitor {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public ActivityMonitor() {}
        public ActivityMonitor(android.content.IntentFilter p0, android.app.Instrumentation.ActivityResult p1, boolean p2) {}
        public ActivityMonitor(java.lang.String p0, android.app.Instrumentation.ActivityResult p1, boolean p2) {}
        public android.content.IntentFilter getFilter() { return (android.content.IntentFilter) meglerProps.get("Filter"); }
        public int getHits() { return (meglerProps.get("Hits") instanceof Integer ? (Integer) meglerProps.get("Hits") : 0); }
        public android.app.Activity getLastActivity() { return (android.app.Activity) meglerProps.get("LastActivity"); }
        public android.app.Instrumentation.ActivityResult getResult() { return (android.app.Instrumentation.ActivityResult) meglerProps.get("Result"); }
        public boolean isBlocking() { return (meglerProps.get("Blocking") instanceof Boolean ? (Boolean) meglerProps.get("Blocking") : false); }
        public android.app.Instrumentation.ActivityResult onStartActivity(android.content.Context p0, android.content.Intent p1, android.os.Bundle p2) { return null; }
        public android.app.Instrumentation.ActivityResult onStartActivity(android.content.Intent p0) { return null; }
        public void onStartActivityResult(int p0, android.os.Bundle p1) {}
        public android.app.Activity waitForActivity() { return null; }
        public android.app.Activity waitForActivityWithTimeout(long p0) { return null; }
    }
    // ---- end of generated members
}
