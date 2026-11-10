package megler.test.gmshost;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.util.Log;
import android.widget.TextView;

/**
 * Starts the one service of the stand-in for Google's own Play services (gms-own), which says what process it is told it runs
 * in; and binds their location API, as an app's FusedLocationProviderClient does, which has Megler settle Google Location
 * Accuracy (asked of the test's shell, and written into the stand-in's Google settings).
 */
public class Main extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this);
        text.setText("Starting a service of Google Play services' stand-in");
        setContentView(text);
        ComponentName started = startService(new Intent("megler.test.gmsown.PROBE").setPackage("com.google.android.gms"));
        Log.i("GmsHostTest", "started " + started);
        boolean bound = getApplicationContext().bindService(
            new Intent("com.google.android.location.internal.GoogleLocationManagerService.START").setPackage("com.google.android.gms"),
            new ServiceConnection() {
                @Override
                public void onServiceConnected(ComponentName name, IBinder binder) { Log.i("GmsHostTest", "location api connected " + (binder != null)); }

                @Override
                public void onServiceDisconnected(ComponentName name) {}
            }, Context.BIND_AUTO_CREATE);
        Log.i("GmsHostTest", "location api bind " + bound);
    }
}
