// Overlay of core/framework/gen-src/android/app/ActivityGroup.java (tools/build-framework.sh).
// It differs in being Android's: an activity that runs others inside it through its LocalActivityManager (an overlay too),
// which it makes and moves with its own lifecycle. The generated stub had no manager (getLocalActivityManager gave null).
package android.app;

import android.os.Bundle;
import java.util.HashMap;

public class ActivityGroup extends Activity {
    private static final String STATES_KEY = "android:states";

    protected LocalActivityManager mLocalActivityManager;

    public ActivityGroup() { this(true); }

    public ActivityGroup(boolean singleActivityMode) { mLocalActivityManager = new LocalActivityManager(this, singleActivityMode); }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Bundle states = savedInstanceState != null ? savedInstanceState.getBundle(STATES_KEY) : null;
        mLocalActivityManager.dispatchCreate(states);
    }

    @Override
    protected void onResume() {
        super.onResume();
        mLocalActivityManager.dispatchResume();
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        Bundle state = mLocalActivityManager.saveInstanceState();
        if (state != null) outState.putBundle(STATES_KEY, state);
    }

    @Override
    protected void onPause() {
        super.onPause();
        mLocalActivityManager.dispatchPause(isFinishing());
    }

    @Override
    protected void onStop() {
        super.onStop();
        mLocalActivityManager.dispatchStop();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mLocalActivityManager.dispatchDestroy(isFinishing());
    }

    public HashMap<String, Object> onRetainNonConfigurationChildInstances() { return mLocalActivityManager.dispatchRetainNonConfigurationInstance(); }

    public Activity getCurrentActivity() { return mLocalActivityManager.getCurrentActivity(); }

    public final LocalActivityManager getLocalActivityManager() { return mLocalActivityManager; }
}
