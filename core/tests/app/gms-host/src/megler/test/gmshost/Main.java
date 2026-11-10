package megler.test.gmshost;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.location.Address;
import android.location.Geocoder;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.util.Log;
import android.widget.TextView;
import java.io.IOException;
import java.util.List;
import java.util.Locale;

/**
 * Starts the one service of the stand-in for Google's own Play services (gms-own), which says what process it is told it runs
 * in; and binds their location API, as an app's FusedLocationProviderClient does, which has Megler settle Google Location
 * Accuracy (asked of the test's shell, and written into the stand-in's Google settings). Once it is bound -- their network
 * location has handed a fix of the Wi-Fi's to their fused provider by then -- asks LocationManager as an app of it does: the
 * last "network" fix, and the first "gps" fix of a request, each said with how accurate it is and its provider. Then asks
 * Android's Geocoder, which their geocode provider answers, and starts an activity of theirs, which runs here, as Play Store
 * starts their account settings ("Manage your Google Account"), and one whose onCreate fails, which the app outlives.
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

    private int resumed;

    @Override
    protected void onResume() {
        super.onResume();
        Log.i("GmsHostTest", "resumed " + ++resumed);
    }

    private static String said(Location l) { return l == null ? "none" : Math.round(l.getAccuracy()) + " m " + l.getProvider(); }

    /** Addresses as the log says them: how many, and each one's lines, locality, country, place and locale. */
    private static String said(List<Address> list) {
        if (list == null) return "null";
        StringBuilder b = new StringBuilder(list.size() + " address(es)");
        for (Address a : list)
            b.append(" [").append(a.getAddressLine(0)).append(" | ").append(a.getAddressLine(1)).append(" | ").append(a.getLocality()).append(' ')
                .append(a.getCountryCode()).append(' ').append(a.hasLatitude() ? a.getLatitude() + "," + a.getLongitude() : "nowhere").append(' ')
                .append(a.getLocale()).append(']');
        return b.toString();
    }

    private void geocodeOnMain() {
        try {
            Log.i("GmsHostTest", "geocoder on the main thread: " + said(new Geocoder(this, Locale.US).getFromLocation(55.7539, 37.6208, 1)));
        } catch (IOException e) {
            Log.i("GmsHostTest", "geocoder on the main thread FAILED " + e);
        }
    }

    /*
     * Android's Geocoder, on a thread of the app's own: there is a provider (their stand-in's); a place at a point, one of a
     * name, the error of a name it has no place for (an IOException), and Android 13's listener forms, answered on a thread
     * that is not the main one, the provider's error to onError.
     */
    private void geocode() {
        try {
            Log.i("GmsHostTest", "geocoder present " + Geocoder.isPresent());
            Geocoder g = new Geocoder(this, new Locale("ru", "RU"));
            Log.i("GmsHostTest", "geocoder at a point: " + said(g.getFromLocation(55.7539, 37.6208, 2)));
            Log.i("GmsHostTest", "geocoder of a name: " + said(g.getFromLocationName("Red Square", 1)));
            try {
                g.getFromLocationName("Atlantis", 1, 10, 20, 30, 40);
                Log.i("GmsHostTest", "geocoder of no place: no error");
            } catch (IOException e) {
                Log.i("GmsHostTest", "geocoder of no place: " + e.getMessage());
            }
            g.getFromLocation(55.7539, 37.6208, 1, new Geocoder.GeocodeListener() {
                @Override
                public void onGeocode(List<Address> addresses) {
                    Log.i("GmsHostTest", "geocoder listener: " + said(addresses) + " off the main thread " + (Looper.myLooper() != Looper.getMainLooper()));
                }

                @Override
                public void onError(String error) {
                    Log.i("GmsHostTest", "geocoder listener FAILED " + error);
                }
            });
            g.getFromLocation(-33.8568, 151.2153, 1, new Geocoder.GeocodeListener() {
                @Override
                public void onGeocode(List<Address> addresses) {
                    Log.i("GmsHostTest", "geocoder listener of an error FAILED " + said(addresses));
                }

                @Override
                public void onError(String error) {
                    Log.i("GmsHostTest", "geocoder listener of an error: " + error);
                }
            });
        } catch (Throwable t) {
            Log.i("GmsHostTest", "geocoder FAILED " + t);
        }
    }

    private void askLocationManager() {
        final LocationManager lm = (LocationManager) getSystemService(LOCATION_SERVICE);
        try {
            Log.i("GmsHostTest", "last network fix " + said(lm.getLastKnownLocation(LocationManager.NETWORK_PROVIDER)));
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, new LocationListener() {
                @Override
                public void onLocationChanged(Location l) {
                    Log.i("GmsHostTest", "first gps fix " + said(l));
                    lm.removeUpdates(this);
                    // Then the Geocoder, the first time on the main thread, synchronously, as old apps ask it (their provider is
                    // made on the main thread, which then waits for its answer), the rest on a thread of the app's own.
                    geocodeOnMain();
                    new Thread(Main.this::geocode, "gms-host-geocoder").start();
                    // Then an activity of Google Play services', which runs here (their stand-in's Lockup says what it read), and
                    // one whose onCreate fails (Broken), which the app outlives, this one resumed after.
                    startActivity(new Intent("megler.test.gmsown.LOCKUP").setPackage("com.google.android.gms"));
                    startActivity(new Intent("megler.test.gmsown.BROKEN").setPackage("com.google.android.gms"));
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
