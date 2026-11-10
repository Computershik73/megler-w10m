package megler.test.gmsown;

import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

/**
 * Google Play services' location API, as an app's FusedLocationProviderClient binds it: a binder, and a line when it is bound.
 * And, as it is bound, what their network location and their fused provider do in the app's process: the network location
 * hands a fix of the Wi-Fi's, 5 m accurate, to the fused provider as theirs does (GoogleLocationChimeraService: a
 * PendingIntent of their own, sent with an OnFinished, its intent marked with the module's EXTRA_RELEASE_VERSION); and the
 * fused provider asks LocationManager for "gps" as theirs does, and says each fix it is handed -- "too soon" for one that
 * comes while that Wi-Fi fix is fresh (under 50 s old), which it must not be, the test's Windows finding the phone by the
 * cell network alone.
 */
public class LocationApi extends Service {
    private static final String RELEASE = "com.google.android.location.internal.EXTRA_RELEASE_VERSION";
    private long sentAt;

    @Override
    public IBinder onBind(Intent intent) {
        Log.i("GmsOwnTest", "location api bound");
        sendFix();
        LocationManager lm = (LocationManager) getSystemService(LOCATION_SERVICE);
        try {
            lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, new LocationListener() {
                @Override
                public void onLocationChanged(Location l) {
                    boolean soon = SystemClock.elapsedRealtime() - sentAt < 50000;
                    Log.i("GmsOwnTest", "own gps fix " + (soon ? "too soon " : "") + Math.round(l.getAccuracy()) + " m");
                }

                @Override
                public void onStatusChanged(String provider, int status, Bundle extras) {}

                @Override
                public void onProviderEnabled(String provider) {}

                @Override
                public void onProviderDisabled(String provider) {}
            }, Looper.getMainLooper());
        } catch (SecurityException e) {
            Log.i("GmsOwnTest", "own gps refused: " + e);
        }
        return new Binder();
    }

    private void sendFix() {
        Location l = new Location(LocationManager.NETWORK_PROVIDER);
        l.setLatitude(55.7525);
        l.setLongitude(37.6231);
        l.setAccuracy(5f);
        l.setTime(System.currentTimeMillis());
        l.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos());
        PendingIntent p = PendingIntent.getBroadcast(this, 0, new Intent("megler.test.gmsown.FLP_RESULT").setPackage(getPackageName()),
                                                     PendingIntent.FLAG_IMMUTABLE);
        Intent fill = new Intent().putExtra(LocationManager.KEY_LOCATION_CHANGED, l).putExtra(RELEASE, 2641);
        sentAt = SystemClock.elapsedRealtime();
        try {
            p.send(this, 0, fill, (pi, sent, code, data, extras) -> Log.i("GmsOwnTest", "network location fix sent " + sent.hasExtra(RELEASE)), null, null, null);
        } catch (PendingIntent.CanceledException e) {
            Log.i("GmsOwnTest", "network location fix not sent: " + e);
        }
    }
}
