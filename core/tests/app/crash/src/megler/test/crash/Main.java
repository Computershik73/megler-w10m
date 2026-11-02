package megler.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

/** As Instagram's main activity: finishes in onCreate unless it is the task's root ("is not the root"), and needs onCreate's state in onStart. */
public class Main extends Activity {
    private Object made;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("CrashTest", "main onCreate root=" + isTaskRoot());
        if (!isTaskRoot()) {
            finish();
            return;
        }
        made = new Object();
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.i("CrashTest", "main onRestart finishing=" + isFinishing());
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i("CrashTest", "main onStart finishing=" + isFinishing() + " made=" + (made != null));
    }
}
