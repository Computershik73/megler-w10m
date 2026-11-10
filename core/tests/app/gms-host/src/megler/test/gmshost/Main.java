package megler.test.gmshost;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.widget.TextView;

/**
 * Starts the one service of the stand-in for Google's own Play services (gms-own), which says what process it is told it runs
 * in; and binds their location API, as an app's FusedLocationProviderClient does, which has Megler settle Google Location
 * Accuracy (asked of the test's shell, and written into the stand-in's Google settings). Once it is bound -- their network
 * location has handed a fix of the Wi-Fi's to their fused provider by then -- asks LocationManager as an app of it does: the
 * last "network" fix, and the first "gps" fix of a request, each said with how accurate it is and its provider. Then starts
 * an activity of theirs, which runs here, as Play Store starts their account settings ("Manage your Google Account").
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
                public void onServiceConnected(ComponentName name, IBinder binder) {
                    Log.i("GmsHostTest", "location api connected " + (binder != null));
                    askLocationManager();
                }

                @Override
                public void onServiceDisconnected(ComponentName name) {}
            }, Context.BIND_AUTO_CREATE);
        Log.i("GmsHostTest", "location api bind " + bound);
    }

    private static String said(Location l) { return l == null ? "none" : Math.round(l.getAccuracy()) + " m " + l.getProvider(); }

    private void askLocationManager() {
        final LocationManager lm = (LocationManager) getSystemService(LOCATION_SERVICE);
        try {
            Log.i("GmsHostTest", "last network fix " + said(lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)));
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, new LocationListener() {
                @Override
                public void onLocationChanged(Location l) {
                    Log.i("GmsHostTest", "first gps fix " + said(l));
                    lm.removeUpdates(this);
                    // Then an activity of Google Play services', which runs here (their stand-in's Lockup says what it read).
                    startActivity(new Intent("megler.test.gmsown.LOCKUP").setPackage("com.google.android.gms"));
                }

                @Override
                public void onStatusChanged(String provider, int status, Bundle extras) {}

                @Override
                public void onProviderEnabled(String provider) {}

                @Override
                public void onProviderDisabled(String provider) {}
            }, Looper.getMainLooper());
        } catch (SecurityException e) {
            Log.i("GmsHostTest", "location refused: " + e);
        }
    }
}
