package megler.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.widget.TextView;

/**
 * Started by First for a result: shows itself, then gives its result and finishes, as an installer with nothing to install does.
 * Once, first, it recreates itself (Activity.recreate, as Facebook recreates its main activity when the app is ready): it is
 * stopped as one being made again, and the new one is the same component and has the state the old one saved -- and still
 * gives First its result.
 * What it begins as it finishes still finds it there: work its onPause posts to the main thread runs before it is destroyed,
 * as on Android, where a finished activity stops only once the one below is visible again. An OIDC library's sign-in saves
 * its result so, from a coroutine of the finishing activity's lifecycleScope (Pyaterochka's).
 */
public class Gate extends Activity {
    private static boolean sRecreated;
    static volatile boolean sDestroyed;   // finished and destroyed: First waits for it

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("gate");
        setContentView(text);
        Log.i("CrashTest", "gate onCreate");
        if (state != null) Log.i("CrashTest", "gate recreated state=" + state.getInt("gate.saved", -1) + " component=" + getComponentName().getClassName());
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        out.putInt("gate.saved", 7);
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (!isFinishing()) Log.i("CrashTest", "gate stop changing=" + isChangingConfigurations());
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (isFinishing()) new Handler().post(() -> Log.i("CrashTest", "gate work after finish destroyed=" + isDestroyed()));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            Log.i("CrashTest", "gate onDestroy finishing");
            sDestroyed = true;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        new Handler().post(() -> {
            if (!sRecreated) {
                sRecreated = true;
                recreate();
                return;
            }
            setResult(RESULT_OK);
            finish();
        });
    }
}
