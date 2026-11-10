package megler.test.gmsown;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * An activity of Google Play services' whose onCreate fails, as their help screen's did when its layout's Google logo was not
 * there (HelpChimeraActivity, 0.1.0.686): it asks to hear its own destruction, as their lifecycle does, and to be finished
 * after, and throws. The app that hosts it goes on; the activity never was created, and must never be destroyed (their
 * lifecycle threw "State must be at least 'CREATED' to be moved to 'DESTROYED'" when it was).
 */
public class Broken extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        registerActivityLifecycleCallbacks(new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityCreated(Activity a, Bundle s) {}
            @Override public void onActivityStarted(Activity a) {}
            @Override public void onActivityResumed(Activity a) {}
            @Override public void onActivityPaused(Activity a) {}
            @Override public void onActivityStopped(Activity a) {}
            @Override public void onActivitySaveInstanceState(Activity a, Bundle s) {}
            @Override public void onActivityDestroyed(Activity a) { Log.i("GmsOwnTest", "broken destroyed"); }
        });
        new Handler(Looper.getMainLooper()).post(this::finish);
        throw new IllegalStateException("broken on purpose");
    }
}
