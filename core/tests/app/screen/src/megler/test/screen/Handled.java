package megler.test.screen;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Bundle;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.LinearLayout;

/**
 * The activity that takes every change of the screen itself (its manifest's android:configChanges): it hears
 * onConfigurationChanged and is never made again. Its top half green, its bottom half yellow. Its manifest asks for the
 * screen held upright (sensorPortrait); a tap asks for landscape.
 */
public class Handled extends Activity {
    static final String TAG = App.TAG;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(TAG, "Handled.onCreate " + App.describe(getResources().getConfiguration()) + " requested=" + getRequestedOrientation());
        LinearLayout halves = new LinearLayout(this);
        halves.setOrientation(LinearLayout.VERTICAL);
        halves.addView(new Main.Half(this, 0xFF00C000, null), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        halves.addView(new Main.Half(this, 0xFFFFFF00, "Handled.bottom"), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        halves.setOnClickListener(v -> {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            Log.i(TAG, "Handled.asked landscape requested=" + getRequestedOrientation());
        });
        setContentView(halves);
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        Log.i(TAG, "Handled.onConfigurationChanged " + Main.screen(this));
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "Handled.onResume");
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.i(TAG, "Handled.onPause");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(TAG, "Handled.onDestroy changing=" + isChangingConfigurations());
    }
}
