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
 * And, at its start, reads assets through the NDK with their AssetManager and with its own (nativeAssets).
 * And, first of all, asks their provider from a thread whose lock the main thread then waits for (askWhileMainWaits).
 */
public class Main extends Activity {
    static { System.loadLibrary("gmshost"); }

    /* jni/gmshost.c: an asset's text and a folder's names as the NDK's AAssetManager of this AssetManager gives them. */
    private static native String readAsset(android.content.res.AssetManager assets, String name);
    private static native String listAssets(android.content.res.AssetManager assets, String folder);
    private static native boolean sameManager(android.content.res.AssetManager a, android.content.res.AssetManager b);

    /*
     * Native code given the AssetManager of Google Play services' context reads Google Play services' assets, of their base
     * and of their splits, and none of the app's; given the app's, the app's and none of theirs -- as Java reads them. The
     * map library inside Google Play services (microG's libmapbox-gl.so) reads its sprites and glyphs so, with its own
     * context's AssetManager: AAssetManager_fromJava gave the app's assets for every AssetManager, the files were looked
     * for in the app's APKs, and a map in Citymapper had no icons and no labels ("Failed to load sprite: Could not read
     * asset", "Failed to load glyph range 0-255 for font stack Roboto Medium: Could not read asset").
     */
    private void nativeAssets() {
        try {
            android.content.res.AssetManager mine = getAssets();
            android.content.res.AssetManager theirs = createPackageContext("com.google.android.gms", CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY).getAssets();
            String java;
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.InputStreamReader(theirs.open("gms-own-note.txt"), "UTF-8"))) {
                java = r.readLine();
            }
            Log.i("GmsHostTest", "native assets: theirs [" + readAsset(theirs, "gms-own-note.txt") + "] as Java reads it " + (java != null && java.equals(readAsset(theirs, "gms-own-note.txt")))
                + ", their split's [" + readAsset(theirs, "gmspack/note.txt") + "] folder [" + listAssets(theirs, "gmspack").trim() + "], the app's through theirs "
                + readAsset(theirs, "host-note.txt") + "; the app's [" + readAsset(mine, "host-note.txt") + "], theirs through the app's "
                + readAsset(mine, "gms-own-note.txt") + " " + readAsset(mine, "gmspack/note.txt") + " folder [" + listAssets(mine, "gmspack").trim() + "]; one manager each "
                + (sameManager(theirs, theirs) && sameManager(mine, getApplicationContext().getAssets()) && !sameManager(mine, theirs)));
        } catch (Throwable t) {
            Log.i("GmsHostTest", "native assets FAILED " + t);
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        askWhileMainWaits();
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
        askUnderLock();
        nativeAssets();
    }

    /*
     * Their provider asked, before their Application is attached, by a thread holding a lock that the main thread then waits to
     * enter: as Citymapper's main thread, initializing Google Maps in its Application.onCreate, waited for DynamiteModule's lock
     * while the thread that held it asked their Chimera provider. The thread is to be answered at once -- the main thread, which
     * would attach their Application, stands until the thread lets the lock go -- not after the ten seconds it would wait.
     */
    private void askWhileMainWaits() {
        final Object lock = new Object();
        final java.util.concurrent.CountDownLatch held = new java.util.concurrent.CountDownLatch(1);
        final boolean[] answered = new boolean[1];
        new Thread(() -> {
            synchronized (lock) {
                held.countDown();
                android.database.Cursor c = getContentResolver().query(android.net.Uri.parse("content://com.google.settings/partner"), null, "name=?",
                                                                       new String[] { "main_waits" }, null);
                answered[0] = c != null;
                if (c != null) c.close();
            }
        }, "gms-host-main-waits").start();
        try { held.await(); } catch (InterruptedException e) { }
        final long began = android.os.SystemClock.uptimeMillis();
        synchronized (lock) {
            Log.i("GmsHostTest", "the main thread entered the asking thread's lock after " + (android.os.SystemClock.uptimeMillis() - began)
                  + " ms, their provider answered " + answered[0]);
        }
    }

    /*
     * Their provider asked by a thread of the app's that holds a lock of its own, while their Application is being attached (App,
     * slowly) for a binding whose onServiceConnected wants that lock: as Smash Hit's GoogleApiHandler asked their Chimera provider
     * with the client library's GmsClientSupervisor lock held, and the main thread then delivered a binding's onServiceConnected,
     * which waits for that lock. The binding is to be delivered once the attaching is done, not after the ten seconds the thread
     * would wait for a request of its own behind it on the main thread's queue.
     */
    private void askUnderLock() {
        final Object lock = new Object();
        final long asked = android.os.SystemClock.uptimeMillis();
        getApplicationContext().bindService(
            new Intent("com.google.android.location.internal.GoogleLocationManagerService.START").setPackage("com.google.android.gms"),
            new ServiceConnection() {
                @Override
                public void onServiceConnected(ComponentName name, IBinder binder) {
                    synchronized (lock) {
                        Log.i("GmsHostTest", "bound under the provider's lock after " + (android.os.SystemClock.uptimeMillis() - asked) + " ms");
                    }
                }

                @Override
                public void onServiceDisconnected(ComponentName name) {}
            }, Context.BIND_AUTO_CREATE);
        new Thread(() -> {
            synchronized (lock) {
                android.database.Cursor c = getContentResolver().query(android.net.Uri.parse("content://com.google.settings/partner"), null, "name=?",
                                                                       new String[] { "under_lock" }, null);
                Log.i("GmsHostTest", "provider asked under a lock " + (c != null) + " after " + (android.os.SystemClock.uptimeMillis() - asked) + " ms");
                if (c != null) c.close();
            }
        }, "gms-host-under-lock").start();
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
