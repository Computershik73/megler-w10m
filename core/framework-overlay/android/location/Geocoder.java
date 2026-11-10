// Overlay of core/framework/gen-src/android/location/Geocoder.java (tools/build-framework.sh), whose class was signatures:
// isPresent() false and every request an empty list, so Transit showed a place it was asked about as its coordinates
// ("Map location" and its latitude and longitude) where Android shows its address.
//
// On Android an app's Geocoder asks the location manager, which asks the geocode provider of the phone's -- the service of
// Google Play services' for the action com.android.location.service.GeocodeProvider (GmsCore's GeocodeService, not exported:
// the system binds it) -- and answers the app on a binder thread. Here Google Play services run in the app (megler.MicroG):
// their geocode service is bound as the system binds it (MicroG.bindAsSystemNow) once, and kept; its binder, a
// com.android.location.provider.GeocodeProvider's, is local and called in place, on a thread of the Geocoder's own, as a
// binder thread of the provider's process would run it. Where there is no such provider -- no Google Play services, or none
// that declares one -- isPresent() is false and a request has no addresses and no error, as Android's location manager
// answers it then. An error of the provider's is the IOException of the synchronous forms and GeocodeListener.onError of
// Android 13's; the synchronous forms wait a minute at most ("Service not Available"), as Android's do. The arguments are
// checked as Android 13 checks them (IllegalArgumentException). The log says each request's outcome, not its place.
//
// One thing differs from a phone: there the provider has a process, and a main thread, of its own. Here its service is made
// and bound on the app's main thread, so a synchronous request asked on the main thread binds it there first, before it
// waits; what the provider then wants of the main thread while it answers waits until the answer, or the minute, is over.
package android.location;

import android.content.Context;
import android.content.Intent;
import android.os.IBinder;
import java.io.IOException;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Addresses of a place and places of an address, from the geocode provider of the Google Play services the app has. */
public final class Geocoder {
    /** Android 13's answer to one request: the addresses (none when there are none), or why there are none. */
    public interface GeocodeListener {
        void onGeocode(List<Address> addresses);
        default void onError(String errorMessage) {}
    }

    /** The action of the service a geocode provider is (Android's location manager binds the one Google Play services declare). */
    private static final String ACTION_GEOCODE_PROVIDER = "com.android.location.service.GeocodeProvider";
    private static final long TIMEOUT_MS = 60000;

    private final GeocoderParams mParams;

    public Geocoder(Context context) { this(context, Locale.getDefault()); }
    public Geocoder(Context context, Locale locale) {
        if (locale == null) throw new NullPointerException("locale == null");
        mParams = new GeocoderParams(context, locale);
    }

    /** Whether there is a geocode provider: the Google Play services the app has declare one (megler.MicroG.geocoderPresent). */
    public static boolean isPresent() { return megler.MicroG.geocoderPresent(ACTION_GEOCODE_PROVIDER); }

    @Deprecated
    public List<Address> getFromLocation(double latitude, double longitude, int maxResults) throws IOException {
        Waiting w = new Waiting();
        getFromLocation(latitude, longitude, maxResults, w);
        return w.results();
    }
    public void getFromLocation(final double latitude, final double longitude, final int maxResults, GeocodeListener listener) {
        inRange(latitude, -90, 90, "latitude");
        inRange(longitude, -180, 180, "longitude");
        if (listener == null) throw new NullPointerException("listener == null");
        final GeocoderParams params = mParams;
        ask("from its coordinates", listener, (p, l) -> p.getFromLocation(latitude, longitude, maxResults, params, l));
    }

    @Deprecated
    public List<Address> getFromLocationName(String locationName, int maxResults) throws IOException {
        return getFromLocationName(locationName, maxResults, 0, 0, 0, 0);
    }
    public void getFromLocationName(String locationName, int maxResults, GeocodeListener listener) {
        getFromLocationName(locationName, maxResults, 0, 0, 0, 0, listener);
    }
    @Deprecated
    public List<Address> getFromLocationName(String locationName, int maxResults, double lowerLeftLatitude, double lowerLeftLongitude,
                                             double upperRightLatitude, double upperRightLongitude) throws IOException {
        Waiting w = new Waiting();
        getFromLocationName(locationName, maxResults, lowerLeftLatitude, lowerLeftLongitude, upperRightLatitude, upperRightLongitude, w);
        return w.results();
    }
    public void getFromLocationName(final String locationName, final int maxResults, final double lowerLeftLatitude, final double lowerLeftLongitude,
                                    final double upperRightLatitude, final double upperRightLongitude, GeocodeListener listener) {
        if (locationName == null) throw new IllegalArgumentException("locationName == null");
        inRange(lowerLeftLatitude, -90, 90, "lowerLeftLatitude");
        inRange(lowerLeftLongitude, -180, 180, "lowerLeftLongitude");
        inRange(upperRightLatitude, -90, 90, "upperRightLatitude");
        inRange(upperRightLongitude, -180, 180, "upperRightLongitude");
        if (listener == null) throw new NullPointerException("listener == null");
        final GeocoderParams params = mParams;
        ask("from its name", listener, (p, l) -> p.getFromLocationName(locationName, lowerLeftLatitude, lowerLeftLongitude, upperRightLatitude,
                                                                      upperRightLongitude, maxResults, params, l));
    }

    private static void inRange(double value, double lower, double upper, String name) {
        if (!(value >= lower && value <= upper)) throw new IllegalArgumentException(name + " value " + value + " out of range [" + lower + ", " + upper + "]");
    }

    // ---- Megler: the provider, and the requests' threads

    private interface Request { void on(IGeocodeProvider provider, IGeocodeListener listener); }

    /*
     * The provider, bound once and kept (null until then: a binding that failed is tried again at the next request). No lock is
     * held while it is bound, which is done on the main thread (MicroG.bindAsSystemNow gives the same binder to two that ask at
     * once): a synchronous request on the main thread binds it there first (Waiting), and a request's thread that held a lock
     * waiting for the main thread would have the main thread wait for it.
     */
    private static volatile IGeocodeProvider sProvider;
    private static final java.util.concurrent.atomic.AtomicBoolean sSaid = new java.util.concurrent.atomic.AtomicBoolean();

    private static IGeocodeProvider provider() {
        IGeocodeProvider p = sProvider;
        if (p != null) return p;
        IBinder binder = megler.MicroG.bindAsSystemNow(new Intent(ACTION_GEOCODE_PROVIDER), 30000);
        p = IGeocodeProvider.Stub.asInterface(binder);
        if (p == null) {
            if (sSaid.compareAndSet(false, true))
                android.util.Log.i("Geocoder", binder == null ? "no geocode provider: the app's Google Play services have none, or none here"
                                                              : "Google Play services' geocode service gave a binder of another kind: " + binder);
            return null;
        }
        if (sProvider == null) android.util.Log.i("Geocoder", "Google Play services' geocode provider answers the app's geocoding");
        return sProvider = p;
    }

    /*
     * A request on a thread of the Geocoder's own, as on a binder thread of the provider's: answered with no addresses where there
     * is no provider, else by the provider, whose answer goes to the listener on that thread; a provider that throws is an error.
     */
    private static final java.util.concurrent.ExecutorService sThreads = java.util.concurrent.Executors.newCachedThreadPool(r -> {
        Thread t = new Thread(r, "Geocoder");
        t.setDaemon(true);
        return t;
    });

    private static void ask(final String what, final GeocodeListener listener, final Request request) {
        // A synchronous request on the main thread, which will wait for the answer: the provider bound first, there (its service
        // is made and bound on the main thread, and the request's thread would wait for it in vain).
        if (listener instanceof Waiting && android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) provider();
        sThreads.execute(() -> {
            final long began = android.os.SystemClock.uptimeMillis();
            IGeocodeProvider p = provider();
            if (p == null) {
                listener.onGeocode(Collections.<Address>emptyList());
                return;
            }
            try {
                request.on(p, new IGeocodeListener.Stub() {
                    @Override public void onResults(String error, List<Address> results) {
                        long ms = android.os.SystemClock.uptimeMillis() - began;
                        if (error != null) {
                            android.util.Log.i("Geocoder", "a place asked " + what + ": the provider's error in " + ms + " ms: " + error);
                            listener.onError(error);
                        } else {
                            List<Address> found = results != null ? results : Collections.<Address>emptyList();
                            android.util.Log.i("Geocoder", "a place asked " + what + ": " + found.size() + " address(es) in " + ms + " ms");
                            listener.onGeocode(found);
                        }
                    }
                });
            } catch (RuntimeException | LinkageError e) {
                android.util.Log.w("Geocoder", "Google Play services' geocode provider failed a place asked " + what, e);
                listener.onError(e.toString());
            }
        });
    }

    /* The synchronous forms' answer: the addresses, or the provider's error thrown; "Service not Available" after a minute. */
    private static final class Waiting implements GeocodeListener {
        private final CountDownLatch done = new CountDownLatch(1);
        private volatile List<Address> mResults = Collections.emptyList();
        private volatile String mError;
        @Override public void onGeocode(List<Address> addresses) { mResults = addresses; done.countDown(); }
        @Override public void onError(String error) { mError = error; done.countDown(); }
        List<Address> results() throws IOException {
            try {
                if (!done.await(TIMEOUT_MS, TimeUnit.MILLISECONDS)) mError = "Service not Available";
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            if (mError != null) throw new IOException(mError);
            return mResults;
        }
    }
}
