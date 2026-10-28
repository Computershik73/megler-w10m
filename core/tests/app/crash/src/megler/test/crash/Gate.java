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
 */
public class Gate extends Activity {
    private static boolean sRecreated;

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
