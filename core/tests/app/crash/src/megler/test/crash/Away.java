package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

/**
 * Of a task affinity of its own (the manifest's android:taskAffinity), started by Trampoline: it starts AwayTop, of the same
 * affinity, which calls finishAffinity, as Messenger's first-run screen does over its main activity on Back. Both go
 * together, and this one is not resumed on the way, as on Android (it was: Messenger's main activity, resumed for that
 * moment, asked for its first-run screen again); Trampoline, of another affinity, stays and comes back.
 */
public class Away extends Activity {
    static volatile boolean sGone;
    private int resumes;

    @Override
    protected void onResume() {
        super.onResume();
        resumes++;
        Log.i("CrashTest", "away onResume " + resumes + " finishing=" + isFinishing());
        if (resumes == 1) startActivity(new Intent(this, AwayTop.class));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "away onDestroy finishing=" + isFinishing());
    }
}
