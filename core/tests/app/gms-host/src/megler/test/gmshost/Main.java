package megler.test.gmshost;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/** Starts the one service of the stand-in for Google's own Play services (gms-own), which says what process it is told it runs in. */
public class Main extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Starting a service of Google Play services' stand-in");
        setContentView(text);
        ComponentName started = startService(new Intent("megler.test.gmsown.PROBE").setPackage("com.google.android.gms"));
        Log.i("GmsHostTest", "started " + started);
    }
}
