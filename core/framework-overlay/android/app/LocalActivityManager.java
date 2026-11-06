// Overlay of core/framework/gen-src/android/app/LocalActivityManager.java (tools/build-framework.sh).
// It differs in running activities inside another, as Android's does: the generated stub made none (startActivity gave null)
// and an app's TabActivity showed no tab of an activity, an ActivityGroup none of its children -- Samsung's TouchWiz launcher
// stopped at its start on removeAllActivities of the group's manager, null.
//
// How. Each child is made by megler.AppRunner.makeChild (the app's factory, attached with the group as its parent, kept off
// the stack of activities) and moved through its lifecycle as Android's LocalActivityManager moves it with its group's
// (dispatchCreate, dispatchResume and so on, which android.app.ActivityGroup calls): created and started when started, then
// resumed with the group; in single mode only the current one is resumed. Its window's decor is what startActivity gives,
// for android.widget.TabHost to show; a child's decor is no window of its own (Activity.meglerResume).
package android.app;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.Window;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

public class LocalActivityManager {
    private static final String TAG = "LocalActivityManager";
    // (The states a child and the group are in, as Android's.)
    static final int RESTORED = 0, INITIALIZING = 1, CREATED = 2, STARTED = 3, RESUMED = 4, DESTROYED = 5;

    private static final class Record {
        final String id;
        Intent intent;
        Activity activity;
        Window window;
        Bundle instanceState;
        int curState = RESTORED;
        Record(String id, Intent intent) { this.id = id; this.intent = intent; }
    }

    private final Activity mParent;
    private final boolean mSingleMode;
    private final Map<String, Record> mActivities = new HashMap<>();
    private final ArrayList<Record> mActivityArray = new ArrayList<>();
    private Record mResumed;
    private int mCurState = INITIALIZING;
    private boolean mFinishing;

    public LocalActivityManager(Activity parent, boolean singleMode) {
        mParent = parent;
        mSingleMode = singleMode;
    }

    private void moveToState(Record r, int desiredState) {
        if (r.curState == RESTORED || r.curState == DESTROYED) return;
        try {
            if (r.curState == INITIALIZING) {
                Activity a = megler.AppRunner.makeChild(mParent, r.intent);
                if (a == null) return;
                r.activity = a;
                a.meglerCreate(r.instanceState);
                r.window = a.getWindow();
                if (!a.isFinishing()) {
                    a.meglerStart();
                    if (r.instanceState != null) a.meglerRestoreState(r.instanceState);
                }
                r.instanceState = null;
                r.curState = STARTED;
                if (desiredState == RESUMED) {
                    a.meglerResume();
                    r.curState = RESUMED;
                }
                // (One started in the group's onCreate is ahead of the group, which catches it up as it goes on.)
                return;
            }
            Activity a = r.activity;
            switch (r.curState) {
                case CREATED:
                    if (desiredState == STARTED || desiredState == RESUMED) {
                        a.meglerRestart();
                        a.meglerStart();
                        r.curState = STARTED;
                    }
                    if (desiredState == RESUMED) {
                        a.meglerResume();
                        r.curState = RESUMED;
                    }
                    return;
                case STARTED:
                    if (desiredState == RESUMED) {
                        a.meglerResume();
                        r.instanceState = null;
                        r.curState = RESUMED;
                    }
                    if (desiredState == CREATED) {
                        a.meglerStop();
                        r.curState = CREATED;
                    }
                    return;
                case RESUMED:
                    if (desiredState == STARTED || desiredState == CREATED) {
                        a.meglerPause();
                        r.curState = STARTED;
                    }
                    if (desiredState == CREATED) {
                        a.meglerStop();
                        r.curState = CREATED;
                    }
                    return;
                default:
                    return;
            }
        } catch (RuntimeException e) {
            throw e;
        } catch (Exception e) {
            throw new RuntimeException("Unable to start activity " + r.intent + ": " + e, e);
        }
    }

    public Window startActivity(String id, Intent intent) {
        if (mCurState == INITIALIZING) throw new IllegalStateException("Activities can't be added until the containing group has been created.");
        boolean adding = false, sameIntent = false;
        Record r = mActivities.get(id);
        if (r == null) {
            r = new Record(id, intent);
            adding = true;
        } else if (r.intent != null) {
            sameIntent = r.intent.filterEquals(intent);
        }
        // Single mode: the one running is paused for the new one.
        if (mSingleMode) {
            Record old = mResumed;
            if (old != null && old != r && mCurState == RESUMED) moveToState(old, STARTED);
        }
        if (adding) {
            mActivities.put(id, r);
            mActivityArray.add(r);
        } else if (r.activity != null) {
            if (sameIntent && (intent.getFlags() & Intent.FLAG_ACTIVITY_CLEAR_TOP) == 0) {
                // The same thing shown again: resumed as it is, with its new intent when it asks for one.
                if ((intent.getFlags() & Intent.FLAG_ACTIVITY_SINGLE_TOP) != 0) r.activity.meglerNewIntent(intent);
                r.intent = intent;
                moveToState(r, mCurState);
                if (mSingleMode) mResumed = r;
                return r.window;
            }
            // Another activity, or this one again from the start: what is there goes.
            performDestroy(r, true);
        }
        r.intent = intent;
        r.curState = INITIALIZING;
        moveToState(r, mCurState);
        if (mSingleMode) mResumed = r;
        return r.window;
    }

    private Window performDestroy(Record r, boolean finish) {
        Window win = r.window;
        Activity a = r.activity;
        if (a != null) {
            if (r.curState == RESUMED) a.meglerPause();
            if (r.curState == RESUMED || r.curState == STARTED) a.meglerStop();
            if (finish) a.meglerMarkFinished();
            a.meglerDestroy();
        }
        r.activity = null;
        r.window = null;
        if (finish) r.instanceState = null;
        r.curState = DESTROYED;
        return win;
    }

    public Window destroyActivity(String id, boolean finish) {
        Record r = mActivities.get(id);
        Window win = null;
        if (r != null) {
            win = performDestroy(r, finish);
            if (finish) {
                mActivities.remove(id);
                mActivityArray.remove(r);
                if (mResumed == r) mResumed = null;
            }
        }
        return win;
    }

    public Activity getCurrentActivity() { return mResumed != null ? mResumed.activity : null; }
    public String getCurrentId() { return mResumed != null ? mResumed.id : null; }
    public Activity getActivity(String id) {
        Record r = mActivities.get(id);
        return r != null ? r.activity : null;
    }

    public void dispatchCreate(Bundle state) {
        if (state != null) {
            for (String id : state.keySet()) {
                try {
                    Bundle astate = state.getBundle(id);
                    Record r = mActivities.get(id);
                    if (r != null) {
                        r.instanceState = astate;
                    } else {
                        r = new Record(id, null);
                        r.instanceState = astate;
                        mActivities.put(id, r);
                        mActivityArray.add(r);
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Exception thrown when restoring LocalActivityManager state", e);
                }
            }
        }
        mCurState = CREATED;
    }

    public Bundle saveInstanceState() {
        Bundle state = null;
        for (Record r : mActivityArray) {
            if (r.instanceState == null && r.activity != null && r.curState != RESTORED && r.curState != DESTROYED)
                r.instanceState = r.activity.meglerSaveState();
            if (r.instanceState != null) {
                if (state == null) state = new Bundle();
                state.putBundle(r.id, r.instanceState);
            }
        }
        return state;
    }

    public void dispatchResume() {
        mCurState = RESUMED;
        if (mSingleMode) {
            if (mResumed != null) moveToState(mResumed, RESUMED);
        } else {
            for (Record r : new ArrayList<>(mActivityArray)) moveToState(r, RESUMED);
        }
    }

    public void dispatchPause(boolean finishing) {
        if (finishing) mFinishing = true;
        mCurState = STARTED;
        if (mSingleMode) {
            if (mResumed != null) moveToState(mResumed, STARTED);
        } else {
            for (Record r : new ArrayList<>(mActivityArray)) if (r.curState == RESUMED) moveToState(r, STARTED);
        }
    }

    public void dispatchStop() {
        mCurState = CREATED;
        for (Record r : new ArrayList<>(mActivityArray)) moveToState(r, CREATED);
    }

    public HashMap<String, Object> dispatchRetainNonConfigurationInstance() { return null; }

    public void removeAllActivities() { dispatchDestroy(true); }

    public void dispatchDestroy(boolean finishing) {
        for (Record r : new ArrayList<>(mActivityArray)) {
            if (r.activity != null) performDestroy(r, finishing);
        }
        mActivities.clear();
        mActivityArray.clear();
        mResumed = null;
    }
}
