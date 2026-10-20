// Megler: new in the overlay. What is behind android.location.LocationManager (framework-overlay/android/location): the
// app's requests for the phone's location, kept here, and the phone's fixes, from the shell (megler/LocationNative.java),
// delivered to them.
//
// One thread of the layer's, "megler-location", runs while an app wants the location. It asks whether the app may have it --
// the first time, Windows asks the user, which this thread waits for, never the app's -- then has the shell follow it at
// the shortest interval asked for, and four times a second hands each request a fix newer than its last one, once its
// minimum time has gone by and the phone has moved its minimum distance: to a listener on its Looper or Executor, to a
// PendingIntent as KEY_LOCATION_CHANGED, or once to getCurrentLocation's consumer (null after 30 seconds without one). A
// fix older than a minute is not an update. When the location is switched off or refused, a listener is told its provider
// is disabled, and enabled again once it is back. With no request left the shell stops following it.
//
// All four providers -- gps, network, passive, fused -- are the same fixes, each named as asked for (passive: as found).
// getLastKnownLocation is the newest fix the shell has; when there is none, the location is followed for half a minute so
// that the next call finds one, as on a phone something has always just asked. Until the user has been asked, the location
// counts as on (they are asked in the background then), so that an app that first checks isProviderEnabled("gps") goes on
// to ask for it.
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
    }

    private static final Object LOCK = new Object();
    private static final ArrayList<Request> REQUESTS = new ArrayList<>();
    private static Thread sWorker;
    private static long sWarmUntil;
    private static boolean sAsking;

    private LocationService() {}

    public static final String[] PROVIDERS = { LocationManager.PASSIVE_PROVIDER, LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
            LocationManager.FUSED_PROVIDER };

    public static boolean known(String provider) {
        for (String p : PROVIDERS)
            if (p.equals(provider)) return true;
        return false;
    }

    /** Whether the location is on for the app: on, or not asked yet (asked now, in the background). */
    public static boolean enabled() {
        int a = LocationNative.access(false);
        if (a == LocationNative.UNKNOWN) {
            askInBackground();
            return true;
        }
        return a == LocationNative.ON;
    }

    private static void askInBackground() {
        synchronized (LOCK) {
            if (sAsking) return;
            sAsking = true;
        }
        Thread t = new Thread(() -> {
            try {
                LocationNative.access(true);
            } finally {
                synchronized (LOCK) { sAsking = false; }
            }
        }, "megler-location-ask");
        t.setDaemon(true);
        t.start();
    }

    /** The newest fix, named for `provider`; null when there is none (then the location is followed a while) or it is off. */
    public static Location last(String provider) {
        int a = LocationNative.access(false);
        if (a != LocationNative.ON && a != LocationNative.UNKNOWN) return null;
        double[] f = LocationNative.last();
        if (f == null) {
            synchronized (LOCK) {
                sWarmUntil = Math.max(sWarmUntil, SystemClock.elapsedRealtime() + WARM_MS);
                ensureWorker();
            }
            return null;
        }
        return toLocation(f, provider);
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
        r.minTimeMs = Math.max(0, minTimeMs);
        r.minDistanceM = Math.max(0, minDistanceM);
        r.listener = listener;
        r.executor = executor != null ? executor : executorFor(null);
        r.remaining = Math.max(0, maxUpdates);
        add(r, listener, null);
    }

    /** Updates sent to a PendingIntent (any earlier request of the same one replaced). */
    public static void request(String provider, long minTimeMs, float minDistanceM, PendingIntent pending, Context context, int maxUpdates) {
        if (pending == null) throw new IllegalArgumentException("invalid null pending intent");
        Request r = new Request();
        r.provider = provider;
        r.minTimeMs = Math.max(0, minTimeMs);
        r.minDistanceM = Math.max(0, minDistanceM);
        r.pending = pending;
        r.context = context;
        r.remaining = Math.max(0, maxUpdates);
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
        r.expiresAt = SystemClock.elapsedRealtime() + CURRENT_TIMEOUT_MS;
        int a = LocationNative.access(false);
        if (a != LocationNative.ON && a != LocationNative.UNKNOWN) {
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

    private static void add(Request r, LocationListener sameListener, PendingIntent samePending) {
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

    private static int interval(ArrayList<Request> now) {
        long ms = Long.MAX_VALUE;
        for (Request r : now) ms = Math.min(ms, r.minTimeMs);
        if (ms == Long.MAX_VALUE) ms = FASTEST_MS;
        return (int) Math.max(FASTEST_MS, Math.min(ms, 60000));
    }

    private static void follow() {
        int started = -1;
        int access = LocationNative.access(true);
        long accessAt = SystemClock.elapsedRealtime();
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
                        for (Request r : expired) r.executor.execute(() -> r.consumer.accept(null));
                        return;
                    }
                    now = new ArrayList<>(REQUESTS);
                }
                for (Request r : expired) r.executor.execute(() -> r.consumer.accept(null));
                if (t - accessAt >= 2000) {
                    access = LocationNative.access(false);
                    accessAt = t;
                }
                if (access != LocationNative.ON) {
                    if (started >= 0) {
                        LocationNative.stop();
                        started = -1;
                    }
                    for (Request r : now) {
                        if (r.toldDisabled) continue;
                        r.toldDisabled = true;
                        disabled(r);
                    }
                    pause(500);
                    continue;
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
                    started = interval;
                }
                double[] f = LocationNative.last();
                if (f != null) deliver(now, f, t);
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

    private static void deliver(ArrayList<Request> now, double[] f, long t) {
        long fixTime = (long) f[7];
        if (System.currentTimeMillis() - fixTime > STALE_MS) return;
        for (Request r : now) {
            if (fixTime <= r.fixTime) continue;
            if (t - r.deliveredAt < r.minTimeMs - 100) continue;
            Location loc = toLocation(f, r.provider);
            if (r.minDistanceM > 0 && r.delivered != null && loc.distanceTo(r.delivered) < r.minDistanceM) continue;
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
}
