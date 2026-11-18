package megler.test.playstore;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * The stand-in Play Store's screen: nothing but that the app is up, which is when Megler takes up the request it was
 * started for (megler.PlaySplits). The work is the service's (Splits).
 */
public class Main extends Activity {
    static final String T = "PlayStoreTest";

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Play Store");
        setContentView(v);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }
}
