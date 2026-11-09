package megler.test.crash;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;

/** Over Away, of its affinity: once resumed it finishes the affinity (Away's comment), finishing at once itself. */
public class AwayTop extends Activity {
    private boolean asked;

    @Override
    protected void onResume() {
        super.onResume();
        if (asked) return;
        asked = true;
        new Handler().post(() -> {
            Away.sGone = true;
            finishAffinity();
            Log.i("CrashTest", "away-top finishAffinity finishing=" + isFinishing());
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "away-top onDestroy");
    }
}
