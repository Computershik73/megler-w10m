package meglet.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.widget.TextView;

/** Started by First for a result: shows itself, then gives its result and finishes, as an installer with nothing to install does. */
public class Gate extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("gate");
        setContentView(text);
        Log.i("CrashTest", "gate onCreate");
    }

    @Override
    protected void onResume() {
        super.onResume();
        new Handler().post(() -> {
            setResult(RESULT_OK);
            finish();
        });
    }
}
