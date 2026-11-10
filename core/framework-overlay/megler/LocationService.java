// Megler: new in the overlay. What is behind android.location.LocationManager (framework-overlay/android/location): the
// app's requests for the phone's location, kept here, and the phone's fixes, from the shell (megler/LocationNative.java),
// delivered to them.
//
// One thread of the layer's, "megler-location", runs while an app wants the location. It asks whether the app may have it --
// the first time, Windows asks the user, which this thread waits for, never the app's, and only for an app that holds a
// location permission (megler.Permissions; mayAsk) -- then has the shell follow it at
// the shortest interval asked for, and four times a second hands each request a fix newer than its last one, once its
// minimum time has gone by and the phone has moved its minimum distance: to a listener on its Looper or Executor, to a
// PendingIntent as KEY_LOCATION_CHANGED, or once to getCurrentLocation's consumer (null after 30 seconds without one). A
// fix older than a minute is not an update. When the location is switched off or refused, a listener is told its provider
// is disabled, and enabled again once it is back. With no request left the shell stops following it.
//
// All four providers -- gps, network, passive, fused -- are the same fixes, each named as asked for (passive: as found): Windows',
// or Google Play services' network location's where it runs in the app and has a better one (the end of the file says when,
// and what Google Play services' own requests are handed).
// getLastKnownLocation is the newest fix the shell has; when there is none, the location is followed for half a minute so
// that the next call finds one, as on a phone something has always just asked. Until the user has been asked, the location
// counts as on, so that an app that first checks isProviderEnabled("gps") goes on to ask for it; nothing is asked then.
// The location counts as on too where Windows refuses it to this app (the user's no in Settings > Privacy > Location): on
// Android the location's switch is the phone's, and an app without the permission is told so by its permission; it is off
// only where the phone's location is switched off, or nothing finds the phone.
//
// A request of the passive interval (LocationRequest.PASSIVE_INTERVAL, Long.MAX_VALUE) is handed every new fix and does not
// set how often the location is followed. An app whose permission is granted after its first request (the worker asked
// nothing then) has Windows asked once it is, and so has one whose location is undecided again (the phone's location
// switched on after it was off, the app not yet asked about).
//
// The log says each request as it comes, whose it is (the listener's class, the PendingIntent's package, the consumer's class)
// and how often, and that the app holds no location permission when it does not (Android would have refused the request), and
// the first fix each is handed: whether an app that finds no location asked at all, and whether a fix reached what it asked
// through (Google Play services' fused provider asks here for the app). At most 40 lines a run. And why no fix is handed while
// there are requests, each time the reason changes: the phone's location switched off, Windows' no for this app, no location
// device, no permission (Windows not asked), Windows not answering yet; and when 30 seconds of following have brought none.
package megler;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

public final class LocationService {
    private static final int FASTEST_MS = 1000;
    private static final long CURRENT_TIMEOUT_MS = 30000;
    private static final long WARM_MS = 30000;
    private static final long STALE_MS = 60000;

    private static final class Request {
        String provider;
        long minTimeMs;
        float minDistanceM;
        LocationListener listener;
        Executor executor;
        PendingIntent pending;
        Context context;
        Consumer<Location> consumer;
        int remaining;          // updates still to deliver; 0: any number
        long expiresAt;         // elapsedRealtime; 0: never (getCurrentLocation's waits 30 s)
        long deliveredAt = Long.MIN_VALUE / 2;
        long fixTime;
        Location delivered;
        boolean toldDisabled;
        /** Of the passive interval: handed every new fix, and no say in how often the location is followed. */
        boolean passive;
        /** Google Play services' own (its fused provider's, its network location's), not the app's: ofGoogle. */
        boolean google;
    }

    /** A request's minimum time that is the passive interval (LocationRequest.PASSIVE_INTERVAL, and what its deprecated factories make of it). */
    private static boolean passiveInterval(long minTimeMs) { return minTimeMs >= Long.MAX_VALUE - 1; }

    private static final Object LOCK = new Object();
    private static final ArrayList<Request> REQUESTS = new ArrayList<>();
    private static Thread sWorker;
    private static long sWarmUntil;
    private static int sSaid;

    private LocationService() {}

    public static final String[] PROVIDERS = { LocationManager.PASSIVE_PROVIDER, LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
            LocationManager.FUSED_PROVIDER };

    public static boolean known(String provider) {
        for (String p : PROVIDERS)
            if (p.equals(provider)) return true;
        return false;
    }

    /**
     * Whether the location is on for the app: on, or not decided yet, so that an app that first checks isProviderEnabled("gps")
     * goes on to ask for it, or refused to this app by the user, which is the app's permission's to say on Android (see the
     * top); off where the phone's location is switched off or nothing finds the phone. Nothing is asked here. It was, in the
     * background: Google Play services in an app asks this of every provider, and Windows asked the user for the location for
     * apps that never wanted it.
     */
    public static boolean enabled() {
        int a = LocationNative.access(false);
        return a == LocationNative.ON || a == LocationNative.UNKNOWN || a == LocationNative.DENIED;
    }

    /**
     * Whether Windows may ask the user for the location as it is wanted: only for an app that holds a location permission
     * (megler.Permissions: granted, or with the install), whoever in it wants the location. Google Play services in the app,
     * which follows the location for its own ends too, never has the user asked for an app that did not ask; an app that
     * did is asked when it requests the permission.
     */
    private static boolean mayAsk() {
        return Permissions.check("android.permission.ACCESS_FINE_LOCATION") == Permissions.GRANTED
            || Permissions.check("android.permission.ACCESS_COARSE_LOCATION") == Permissions.GRANTED;
    }

    /**
     * The newest fix -- the better of Windows' and Google's network location's (better) -- named for `provider`; null when
     * there is none (then the location is followed a while) or it is off. Asked by Google Play services' own code (the
     * closest class loader up the stack is theirs), it is what their own requests are handed (deliver): never their network
     * location's fix, and Windows' from the satellites alone while that is fresh.
     */
    public static Location last(String provider) {
        int a = LocationNative.access(false);
        if (a != LocationNative.ON && a != LocationNative.UNKNOWN) return null;
        double[] f = LocationNative.last();
        long t = SystemClock.elapsedRealtime();
        boolean theirs = byGoogle();
        Location g = google(t);
        if (f == null) {
            synchronized (LOCK) {
                sWarmUntil = Math.max(sWarmUntil, t + WARM_MS);
                ensureWorker();
            }
            return g == null || theirs ? null : named(g, provider);
        }
        Location w = toLocation(f, null);
        if (theirs) return g == null || (int) f[9] != LocationNative.NETWORK ? named(w, provider) : null;
        return named(better(w, g, t), provider);
    }

    public static Executor executorFor(Looper looper) {
        if (looper == null) looper = Looper.myLooper();
        if (looper == null) looper = Looper.getMainLooper();
        final Handler h = new Handler(looper);
        return h::post;
    }

    /** Updates for a listener (any earlier request of the same listener replaced). maxUpdates 0: any number. */
    public static void request(String provider, long minTimeMs, float minDistanceM, LocationListener listener, Executor executor, int maxUpdates) {
        if (listener == null) throw new IllegalArgumentException("invalid null listener");
        Request r = new Request();
        r.provider = provider;
        r.passive = passiveInterval(minTimeMs);
        r.minTimeMs = r.passive ? 0 : Math.max(0, minTimeMs);
        r.minDistanceM = Math.max(0, minDistanceM);
        r.listener = listener;
        r.executor = executor != null ? executor : executorFor(null);
        r.remaining = Math.max(0, maxUpdates);
        r.google = ofGoogle(listener) || byGoogle();
        add(r, listener, null);
    }

    /** Updates sent to a PendingIntent (any earlier request of the same one replaced). */
    public static void request(String provider, long minTimeMs, float minDistanceM, PendingIntent pending, Context context, int maxUpdates) {
        if (pending == null) throw new IllegalArgumentException("invalid null pending intent");
        Request r = new Request();
        r.provider = provider;
        r.passive = passiveInterval(minTimeMs);
        r.minTimeMs = r.passive ? 0 : Math.max(0, minTimeMs);
        r.minDistanceM = Math.max(0, minDistanceM);
        r.pending = pending;
        r.context = context;
        r.remaining = Math.max(0, maxUpdates);
        r.google = GooglePackages.GMS.equals(pending.getCreatorPackage()) || byGoogle();
        add(r, null, pending);
    }

    /** One fix soon, or null after 30 seconds, when cancelled before, or at once when the location is off. */
    public static void current(String provider, CancellationSignal cancel, Executor executor, Consumer<Location> consumer) {
        if (consumer == null) throw new IllegalArgumentException("invalid null consumer");
        final Request r = new Request();
        r.provider = provider;
        r.consumer = consumer;
        r.executor = executor != null ? executor : executorFor(null);
        r.remaining = 1;
        r.google = ofGoogle(consumer) || byGoogle();
        r.expiresAt = SystemClock.elapsedRealtime() + CURRENT_TIMEOUT_MS;
        int a = LocationNative.access(false);
        if (a != LocationNative.ON && a != LocationNative.UNKNOWN) {
            say("one fix (getCurrentLocation) of " + provider + " for " + whose(r) + ": " + why(a, mayAsk()) + "; it is handed null at once");
            r.executor.execute(() -> consumer.accept(null));
            return;
        }
        if (cancel != null) cancel.setOnCancelListener(() -> { synchronized (LOCK) { REQUESTS.remove(r); } });
        add(r, null, null);
    }

    public static void remove(LocationListener listener) {
        synchronized (LOCK) {
            for (int i = REQUESTS.size() - 1; i >= 0; i--)
                if (REQUESTS.get(i).listener == listener) REQUESTS.remove(i);
        }
    }

    public static void remove(PendingIntent pending) {
        synchronized (LOCK) {
            for (int i = REQUESTS.size() - 1; i >= 0; i--)
                if (REQUESTS.get(i).pending != null && REQUESTS.get(i).pending.equals(pending)) REQUESTS.remove(i);
        }
    }

    /** requestFlush: nothing is batched, so the flush is done at once. */
    public static void flush(LocationListener listener, int requestCode) {
        Executor e = null;
        synchronized (LOCK) {
            for (Request r : REQUESTS)
                if (r.listener == listener) e = r.executor;
        }
        if (e == null) throw new IllegalArgumentException("unregistered listener cannot be flushed");
        e.execute(() -> listener.onFlushComplete(requestCode));
    }

    private static String whose(Request r) {
        if (r.listener != null) return r.listener.getClass().getName();
        if (r.pending != null) return "a PendingIntent of " + r.pending.getCreatorPackage();
        return r.consumer != null ? r.consumer.getClass().getName() : "?";
    }

    private static void say(String what) {
        synchronized (LOCK) {
            if (sSaid >= 40) return;
            sSaid++;
        }
        android.util.Log.i("MeglerLocation", what);
    }

    private static void add(Request r, LocationListener sameListener, PendingIntent samePending) {
        say((r.consumer != null ? "one fix (getCurrentLocation) of " + r.provider + " for " + whose(r)
                : r.provider + (r.passive ? " at the passive interval (every fix found for others)" : " every " + r.minTimeMs + " ms")
                  + (r.minDistanceM > 0 ? " and " + r.minDistanceM + " m" : "") + (r.remaining > 0 ? ", " + r.remaining + " updates," : "") + " for " + whose(r))
            + (mayAsk() ? "" : " -- the app holds no location permission (ACCESS_FINE_LOCATION, ACCESS_COARSE_LOCATION): Android would refuse this"
                               + " request with a SecurityException, and Windows is not asked for the location on its behalf"));
        synchronized (LOCK) {
            for (int i = REQUESTS.size() - 1; i >= 0; i--) {
                Request o = REQUESTS.get(i);
                if ((sameListener != null && o.listener == sameListener) || (samePending != null && samePending.equals(o.pending))) REQUESTS.remove(i);
            }
            REQUESTS.add(r);
            ensureWorker();
        }
    }

    private static boolean active(Request r) {
        synchronized (LOCK) { return REQUESTS.contains(r); }
    }

    /* With LOCK held. */
    private static void ensureWorker() {
        if (sWorker != null) return;
        sWorker = new Thread(LocationService::follow, "megler-location");
        sWorker.setDaemon(true);
        sWorker.start();
    }

    /** How often the location is followed: the shortest minimum time asked for, the passive requests' aside (60 s for those alone). */
    private static int interval(ArrayList<Request> now) {
        long ms = Long.MAX_VALUE;
        boolean any = false;
        for (Request r : now) {
            if (r.passive) continue;
            ms = Math.min(ms, r.minTimeMs);
            any = true;
        }
        if (!any) ms = now.isEmpty() ? FASTEST_MS : 60000;
        return (int) Math.max(FASTEST_MS, Math.min(ms, 60000));
    }

    /** Why no fix is handed while the location is not on for the app, as the log says it. */
    private static String why(int access, boolean permitted) {
        switch (access) {
        case LocationNative.OFF: return "the phone's location is switched off (Windows' Settings > Privacy > Location): the app is told its providers are disabled";
        case LocationNative.DENIED: return "Windows does not let this app have the location (Settings > Privacy > Location)";
        case LocationNative.NONE: return "nothing finds where the phone is: Windows has no location device or service";
        default: return permitted ? "Windows has not answered whether the app may have the location"
                                  : "the app holds no location permission (ACCESS_FINE_LOCATION, ACCESS_COARSE_LOCATION), so Windows is not asked";
        }
    }

    private static void follow() {
        int started = -1;
        boolean asked = mayAsk();
        int access = LocationNative.access(asked);
        long accessAt = SystemClock.elapsedRealtime();
        long startedAt = 0;
        boolean saidNoFix = false;
        String saidWhy = null;
        try {
            for (;;) {
                ArrayList<Request> now;
                ArrayList<Request> expired = new ArrayList<>();
                long t = SystemClock.elapsedRealtime();
                synchronized (LOCK) {
                    for (int i = REQUESTS.size() - 1; i >= 0; i--) {
                        Request r = REQUESTS.get(i);
                        if (r.expiresAt != 0 && t >= r.expiresAt) {
                            REQUESTS.remove(i);
                            expired.add(r);
                        }
                    }
                    if (REQUESTS.isEmpty() && t >= sWarmUntil) {
                        if (started >= 0) LocationNative.stop();
                        started = -1;
                        sWorker = null;
                        for (Request r : expired) expire(r, access);
                        return;
                    }
                    now = new ArrayList<>(REQUESTS);
                }
                for (Request r : expired) expire(r, access);
                if (t - accessAt >= 2000) {
                    int before = access;
                    access = LocationNative.access(false);
                    // (Not decided again -- the phone's location switched on after it was off -- or an app granted its permission
                    // since the worker started without asking: Windows is asked now, once each time.)
                    if (access == LocationNative.UNKNOWN && before != LocationNative.UNKNOWN) asked = false;
                    if (access == LocationNative.UNKNOWN && !asked && mayAsk()) {
                        asked = true;
                        access = LocationNative.access(true);
                    }
                    accessAt = t;
                }
                if (access != LocationNative.ON) {
                    if (started >= 0) {
                        LocationNative.stop();
                        started = -1;
                    }
                    String why = why(access, asked || mayAsk());
                    if (!why.equals(saidWhy) && !now.isEmpty()) {
                        saidWhy = why;
                        android.util.Log.i("MeglerLocation", "no fix for the app's " + now.size() + " request" + (now.size() == 1 ? "" : "s") + ": " + why);
                    }
                    for (Request r : now) {
                        if (r.toldDisabled) continue;
                        r.toldDisabled = true;
                        disabled(r);
                    }
                    pause(500);
                    continue;
                }
                if (saidWhy != null) {
                    saidWhy = null;
                    android.util.Log.i("MeglerLocation", "the location is on for the app again");
                }
                for (Request r : now) {
                    if (!r.toldDisabled) continue;
                    r.toldDisabled = false;
                    enabled(r);
                }
                int interval = interval(now);
                if (started != interval) {
                    if (started >= 0) LocationNative.stop();
                    LocationNative.start(interval);
                    if (started < 0) {
                        startedAt = t;
                        saidNoFix = false;
                    }
                    started = interval;
                }
                double[] f = LocationNative.last();
                boolean fresh = f != null && System.currentTimeMillis() - (long) f[7] <= STALE_MS;
                Location g = google(t);
                watchGoogle(t);
                if (f != null || g != null) deliver(now, f, g, t);
                if (fresh || g != null) {
                    saidNoFix = true;
                } else if (!saidNoFix && t - startedAt >= CURRENT_TIMEOUT_MS) {
                    saidNoFix = true;
                    android.util.Log.i("MeglerLocation", "no fix yet, " + (t - startedAt) / 1000 + " s after the location was followed: Windows' Geolocator has"
                            + " found none (indoors with no Wi-Fi or cell network position, or the satellites not found yet); requests wait for one");
                }
                pause(250);
            }
        } catch (Throwable e) {
            android.util.Log.w("MeglerLocation", "following the location ended: " + e);
            synchronized (LOCK) {
                if (started >= 0) LocationNative.stop();
                if (sWorker == Thread.currentThread()) sWorker = null;
            }
        }
    }

    private static void expire(Request r, int access) {
        say("no fix within " + CURRENT_TIMEOUT_MS / 1000 + " s for " + whose(r)
            + (access == LocationNative.ON ? ": Windows' Geolocator found none in that time" : ": " + why(access, mayAsk())) + "; it is handed null");
        r.executor.execute(() -> r.consumer.accept(null));
    }

    /**
     * Each request handed its fix, where it has a newer one than its last and its minimum time and distance are past: the
     * better of Windows' newest and Google's network location's newest (better), or for Google Play services' own requests
     * Windows' alone -- and, while Google's network location finds the phone, Windows' from the satellites alone: its cell
     * fixes would come to Google Play services' fused provider every second as "gps" ones (ofGoogle says why).
     */
    private static void deliver(ArrayList<Request> now, double[] f, Location g, long t) {
        Location w = f != null && System.currentTimeMillis() - (long) f[7] <= STALE_MS ? toLocation(f, null) : null;
        boolean satellites = f != null && (int) f[9] != LocationNative.NETWORK;
        for (Request r : now) {
            Location c = r.google ? (g == null || satellites ? w : null) : better(w, g, t);
            if (c == null) continue;
            long fixTime = c.getTime();
            if (fixTime <= r.fixTime) continue;
            if (t - r.deliveredAt < r.minTimeMs - 100) continue;
            Location loc = named(c, r.provider);
            if (r.minDistanceM > 0 && r.delivered != null && loc.distanceTo(r.delivered) < r.minDistanceM) continue;
            if (r.delivered == null)
                say("the first fix for " + whose(r) + ", " + Math.round(loc.getAccuracy()) + " m accurate, " + (System.currentTimeMillis() - fixTime) + " ms old"
                    + (c == g ? ", Google's network location's" : ""));
            r.fixTime = fixTime;
            r.deliveredAt = t;
            r.delivered = loc;
            boolean lastOne = r.remaining > 0 && --r.remaining == 0;
            if (lastOne) {
                synchronized (LOCK) { REQUESTS.remove(r); }
            }
            send(r, loc, lastOne);
        }
    }

    private static void send(Request r, Location loc, boolean lastOne) {
        if (r.consumer != null) {
            r.executor.execute(() -> r.consumer.accept(loc));
        } else if (r.listener != null) {
            // (Dropped when removeUpdates came between: a listener is not called after it has been removed -- but the last of a
            // single update is its own, removed as it was sent.)
            r.executor.execute(() -> {
                if (lastOne || active(r)) r.listener.onLocationChanged(loc);
            });
        } else if (r.pending != null) {
            Intent fill = new Intent();
            fill.putExtra(LocationManager.KEY_LOCATION_CHANGED, loc);
            sendPending(r, fill);
        }
    }

    private static void disabled(Request r) {
        if (r.consumer != null) {
            synchronized (LOCK) { REQUESTS.remove(r); }
            r.executor.execute(() -> r.consumer.accept(null));
        } else if (r.listener != null) {
            r.executor.execute(() -> r.listener.onProviderDisabled(r.provider));
        } else if (r.pending != null) {
            Intent fill = new Intent();
            fill.putExtra(LocationManager.KEY_PROVIDER_ENABLED, false);
            sendPending(r, fill);
        }
    }

    private static void enabled(Request r) {
        if (r.listener != null) {
            r.executor.execute(() -> r.listener.onProviderEnabled(r.provider));
        } else if (r.pending != null) {
            Intent fill = new Intent();
            fill.putExtra(LocationManager.KEY_PROVIDER_ENABLED, true);
            sendPending(r, fill);
        }
    }

    private static void sendPending(Request r, Intent fill) {
        try {
            r.pending.send(r.context, 0, fill);
        } catch (Exception e) {
            android.util.Log.w("MeglerLocation", "a location update could not be sent to its PendingIntent: " + e);
            synchronized (LOCK) { REQUESTS.remove(r); }
        }
    }

    /** A fix from LocationNative.last() as a Location named for `provider` (passive, or none: named for what found it). */
    public static Location toLocation(double[] f, String provider) {
        int has = (int) f[8], source = (int) f[9];
        String name = provider == null || LocationManager.PASSIVE_PROVIDER.equals(provider)
                ? (source == LocationNative.NETWORK ? LocationManager.NETWORK_PROVIDER : LocationManager.GPS_PROVIDER) : provider;
        Location l = new Location(name);
        l.setLatitude(f[0]);
        l.setLongitude(f[1]);
        if ((has & LocationNative.HAS_ALTITUDE) != 0) l.setAltitude(f[2]);
        l.setAccuracy((float) f[3]);
        if ((has & LocationNative.HAS_ALTITUDE_ACCURACY) != 0) l.setVerticalAccuracyMeters((float) f[4]);
        if ((has & LocationNative.HAS_SPEED) != 0) l.setSpeed((float) f[5]);
        if ((has & LocationNative.HAS_BEARING) != 0) l.setBearing((float) f[6]);
        long time = (long) f[7];
        l.setTime(time);
        long age = Math.max(0, System.currentTimeMillis() - time);
        l.setElapsedRealtimeNanos(Math.max(1, SystemClock.elapsedRealtimeNanos() - age * 1000000L));
        return l;
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            // (Nothing interrupts this thread; a spurious wake is a shorter pause.)
        }
    }

    /* ---------------------------------------------------------------- Google's network location */
    //
    // Google Play services in the app finds the phone from the Wi-Fi networks around it (its network location, with Google
    // Location Accuracy on: megler.LocationAccuracy) and hands each fix to its fused provider by a PendingIntent of its own,
    // the intent marked with its EXTRA_RELEASE_VERSION (GoogleLocationChimeraService's geup.ms, sent through gesy.d:
    // PendingIntent.send with an OnFinished and options). Each such fix is seen here as it is sent (sent), and kept while it
    // is fresh (a minute):
    //   - the app's own requests -- LocationManager's providers, getLastKnownLocation -- are handed the better of it and
    //     Windows' newest (better), as on Android, where the network provider is Google Play services' network location: an
    //     app that asks LocationManager as well as Google Play services has the Wi-Fi's fixes there too. An app that uses
    //     LocationManager alone has none: Google's network location runs only for Google Play services' fused provider,
    //     which such an app never binds, and Megler does not start it for LocationManager -- GmsCore read into the app's
    //     process, seconds and tens of megabytes on a Lumia, and the phone's Wi-Fi networks sent to Google for an app that
    //     asked nothing of Google;
    //   - Google Play services' own requests (its fused provider's "gps" and "passive", its network location's "passive":
    //     ofGoogle) are never handed it back, and while it is fresh are handed Windows' fixes from the satellites alone:
    //     Windows' cell fixes, every second, would be "gps" ones to the fused provider, fresher than the Wi-Fi's, and data
    //     its collection takes satellite positions for. Without a fix of Google's for a minute, they have Windows' fixes as
    //     before, the cell's too.
    // The log says the first fix, then once a minute while they come how many, how accurate, and Windows' best beside them;
    // and when Windows' cell fixes stop and start going to Google Play services.

    /** The marks of Google Play services' network location's result (GoogleLocationChimeraService's extras). */
    private static final String NLP_RELEASE = "com.google.android.location.internal.EXTRA_RELEASE_VERSION",
            NLP_LIST = "com.google.android.location.internal.EXTRA_LOCATION_LIST";
    private static final long GOOGLE_FRESH_MS = 60000;
    private static Location sGoogle;
    private static long sGoogleAt = Long.MIN_VALUE / 2;
    private static boolean sGoogleFresh;
    private static int sGoogleFixes;
    private static long sGoogleFrom;
    private static float sGoogleBest, sGoogleLast;

    /** Whether `o` is Google Play services' code: its class is of GmsCore's class loader, or one under it (MicroG.isGmsLoader). */
    static boolean ofGoogle(Object o) {
        return o != null && MicroG.isGmsLoader(o.getClass().getClassLoader());
    }

    /** Whether the code that called into the framework is Google Play services': the closest class loader up the stack is theirs. */
    static boolean byGoogle() {
        return MicroG.isGmsLoader(dalvik.system.VMStack.getClosestUserClassLoader());
    }

    /** A PendingIntent of Google Play services' is sent (android.app.PendingIntent): its network location's fix, if it carries one. */
    public static void sent(PendingIntent p, Intent i) {
        if (i == null || p == null || !i.hasExtra(NLP_RELEASE) || !GooglePackages.GMS.equals(p.getCreatorPackage())) return;
        Location l = null;
        try {
            Object one = i.getParcelableExtra(LocationManager.KEY_LOCATION_CHANGED);
            if (one instanceof Location) l = (Location) one;
            if (l == null) {
                ArrayList<?> list = i.getParcelableArrayListExtra(NLP_LIST);
                if (list != null && !list.isEmpty() && list.get(list.size() - 1) instanceof Location) l = (Location) list.get(list.size() - 1);
            }
        } catch (RuntimeException e) {
            return;
        }
        if (l == null || !l.hasAccuracy()) return;
        google(new Location(l));
    }

    private static void google(Location l) {
        long t = SystemClock.elapsedRealtime();
        boolean first, minute = false, began;
        int fixes = 0;
        float best = 0, last = 0;
        long span = 0;
        synchronized (LOCK) {
            first = sGoogleFixes == 0 && sGoogleFrom == 0;
            began = !sGoogleFresh;
            sGoogle = l;
            sGoogleAt = t;
            sGoogleFresh = true;
            if (sGoogleFrom == 0) sGoogleFrom = t;
            sGoogleFixes++;
            if (sGoogleFixes == 1 || l.getAccuracy() < sGoogleBest) sGoogleBest = l.getAccuracy();
            sGoogleLast = l.getAccuracy();
            if (t - sGoogleFrom >= 60000) {
                minute = true;
                fixes = sGoogleFixes;
                best = sGoogleBest;
                last = sGoogleLast;
                span = (t - sGoogleFrom) / 1000;
                sGoogleFrom = t;
                sGoogleFixes = 0;
            }
        }
        int heard = Wifi.lastCount();
        if (first)
            android.util.Log.i("MeglerLocation", "Google's network location: its first fix, " + Math.round(l.getAccuracy()) + " m accurate, "
                    + Math.max(0, (SystemClock.elapsedRealtimeNanos() - l.getElapsedRealtimeNanos()) / 1000000) + " ms old"
                    + (heard >= 0 ? ", the last Wi-Fi scan hearing " + heard + " networks" : ", no Wi-Fi scan yet"));
        if (began)
            android.util.Log.i("MeglerLocation", "Google's network location finds the phone: Google Play services' own requests are handed Windows' fixes"
                    + " from the satellites alone, the app's the better of Windows' and Google's");
        if (minute) {
            double[] f = LocationNative.last();
            android.util.Log.i("MeglerLocation", "Google's network location: " + fixes + " fix" + (fixes == 1 ? "" : "es") + " in the last " + span
                    + " s, accurate to " + Math.round(best) + " m at best, " + Math.round(last) + " m the last" + (f != null ? "; Windows' newest "
                    + Math.round(f[3]) + " m (" + ((int) f[9] == LocationNative.SATELLITES ? "the satellites" : "Wi-Fi or the cell network") + ")" : ""));
        }
    }

    /*
     * Google's network location watched while it gives no fix, so that a phone's log says why: a minute after the first Wi-Fi
     * scan Google Play services asked for, and each minute after, where no fix of its came in that minute, how many scans it
     * asked for in it -- none: it stopped (it scans again when an alarm of its goes off); some: it scanned and still found
     * nothing (the server it asks where the networks are: the network's line says whether its name was looked up). Ten lines
     * at most.
     */
    private static long sWatchAt;
    private static int sWatchScans, sWatchSaid;

    private static void watchGoogle(long t) {
        int scans = Wifi.googleScans(), asked;
        if (scans == 0) return;
        synchronized (LOCK) {
            if (sWatchAt == 0) {
                sWatchAt = t;
                sWatchScans = scans - 1;
            }
            if (t - sWatchAt < 60000) return;
            boolean fixed = sGoogle != null && sGoogleAt >= sWatchAt;
            asked = scans - sWatchScans;
            sWatchAt = t;
            sWatchScans = scans;
            if (fixed || sWatchSaid >= 10) return;
            sWatchSaid++;
        }
        long last = Wifi.googleScanAt();
        android.util.Log.i("MeglerLocation", "Google's network location: no fix in the last 60 s; it asked for " + asked + " Wi-Fi scan" + (asked == 1 ? "" : "s")
                + " in them (" + scans + " in all, the last " + (last < 0 ? "?" : (SystemClock.elapsedRealtime() - last) / 1000 + " s ago") + ")"
                + (asked == 0 ? ": it has stopped scanning" : ": it scanned, and had no fix"));
    }

    /** Google's network location's newest fix while it is fresh; null after a minute without one (said once when it ends). */
    static Location google(long t) {
        boolean ended = false;
        Location g;
        synchronized (LOCK) {
            g = sGoogle;
            if (g != null && t - sGoogleAt > GOOGLE_FRESH_MS) {
                g = null;
                ended = sGoogleFresh;
                sGoogleFresh = false;
            }
        }
        if (ended)
            android.util.Log.i("MeglerLocation", "Google's network location has given no fix for " + GOOGLE_FRESH_MS / 1000
                    + " s: Google Play services' own requests are handed Windows' fixes again, the cell's too");
        return g;
    }

    /**
     * The better of Windows' fix and Google's: the more accurate, each taken 3 m less accurate for every second of its age (a
     * walk's pace: a Wi-Fi fix of 25 m from 15 s ago is better than a cell fix of 600 m from now, not than the satellites').
     */
    static Location better(Location w, Location g, long t) {
        if (g == null) return w;
        if (w == null) return g;
        long now = SystemClock.elapsedRealtimeNanos();
        double ew = w.getAccuracy() + 3.0 * Math.max(0, now - w.getElapsedRealtimeNanos()) / 1e9;
        double eg = g.getAccuracy() + 3.0 * Math.max(0, now - g.getElapsedRealtimeNanos()) / 1e9;
        return eg < ew ? g : w;
    }

    /** A copy of a fix named for `provider` (passive, or none: as it is named), without the extras Google Play services put in its own. */
    static Location named(Location c, String provider) {
        if (c == null) return null;
        Location l = new Location(c);
        if (provider != null && !LocationManager.PASSIVE_PROVIDER.equals(provider)) l.setProvider(provider);
        l.setExtras(null);
        return l;
    }
}
