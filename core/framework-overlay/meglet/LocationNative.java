// Meglet: new in the overlay. The phone's location over the shell's meglet_location_host (core/include/meglet.h), answered by
// core/src/drivers/hk_java_location.c; meglet/LocationService.java is what calls it.
package meglet;

public final class LocationNative {
    /* What access() answers: MEGLET_LOCATION_* in meglet.h. */
    public static final int NONE = 0, ON = 1, OFF = 2, DENIED = 3, UNKNOWN = 4;
    /* last()'s source, at index 9: MEGLET_LOCATION_SATELLITES, _NETWORK, _SET_BY_HAND. */
    public static final int SATELLITES = 0, NETWORK = 1, SET_BY_HAND = 2;
    /* last()'s has, at index 8. */
    public static final int HAS_ALTITUDE = 1, HAS_ALTITUDE_ACCURACY = 2, HAS_SPEED = 4, HAS_BEARING = 8;

    private LocationNative() {}

    /** Whether the app may have the location; with `ask`, Windows asks the user first if it has not (this thread waits). */
    public static native int access(boolean ask);
    /** The phone's location followed, a fix every intervalMs or so, until stop(). */
    public static native boolean start(int intervalMs);
    public static native void stop();
    /** The newest fix: {latitude, longitude, altitude, accuracy, altitude accuracy, speed, bearing, time ms, has, source}; null for none. */
    public static native double[] last();
}
