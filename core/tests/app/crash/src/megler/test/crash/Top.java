package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.widget.TextView;

/**
 * Started over First, it starts First again with FLAG_ACTIVITY_CLEAR_TOP, as Instagram goes back to its signed-out screen:
 * step "standard" with that flag alone (Android finishes First and makes a new one), step "single" with
 * FLAG_ACTIVITY_SINGLE_TOP as well (First stays and hears onNewIntent). Either way this one goes.
 */
public class Top extends Activity {
    private String step;
    private boolean asked;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        step = getIntent().getStringExtra("step");
        TextView text = new TextView(this);
        text.setText("top " + step);
        setContentView(text);
        Log.i("CrashTest", "top onCreate " + step);
        // Started without a result: no caller to answer, as on Android (getCallingActivity, getCallingPackage), though the
        // app launched it (Android 14's getLaunchedFromPackage and getLaunchedFromUid).
        String launched;
        try {
            launched = android.app.Activity.class.getMethod("getLaunchedFromPackage").invoke(this) + " uid="
                    + ((Integer) android.app.Activity.class.getMethod("getLaunchedFromUid").invoke(this) == android.os.Process.myUid() ? "mine" : "other");
        } catch (ReflectiveOperationException e) {
            launched = e.toString();
        }
        Log.i("CrashTest", "top caller=" + getCallingActivity() + " package=" + getCallingPackage() + " launched=" + launched);
        // Who started it (Activity.getReferrer): the app, and the second time what First gave it (onProvideReferrer).
        Log.i("CrashTest", "top referrer " + step + " " + getReferrer());
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (asked) return;
        asked = true;
        new Handler().post(() -> {
            boolean single = "single".equals(step);
            Intent back = new Intent(this, First.class).putExtra("step", single ? "third" : "again");
            back.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | (single ? Intent.FLAG_ACTIVITY_SINGLE_TOP : 0));
            startActivity(back);
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "top onDestroy " + step);
    }
}
