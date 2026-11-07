// Megler: replaces the original's, which read CoreMotion's samples by polling. The phone's sensors as Android's
// (core/src/ndk/mg_sensors.c over the shell's megler_sensor_host, core/src/drivers/mg_java_sensors.c): what sensors there
// are, and the framework's one queue of their events, which android.hardware.SensorManager's thread waits on and hands to
// the app's listeners.
package megler;

public final class Sensors {
    /* describe()'s numbers, by index. */
    public static final int TYPE = 0, HANDLE = 1, MAX_RANGE = 2, RESOLUTION = 3, POWER = 4, MIN_DELAY = 5, MAX_DELAY = 6, REPORTING_MODE = 7, WAKE_UP = 8,
            VALUES = 9, VERSION = 10, NUMBERS = 11;
    /* text()'s */
    public static final int NAME = 0, VENDOR = 1, STRING_TYPE = 2;
    /* next()'s meta, by index: when it was read, its accuracy, the flush it ends (0: a reading), how many values it has. */
    public static final int TIMESTAMP = 0, ACCURACY = 1, FLUSH = 2, COUNT = 3, META = 4;

    private Sensors() {}

    /** How many sensors the device has. */
    public static native int count();
    /** The index-th sensor's numbers into `numbers` (NUMBERS of them); false when there is no such sensor. */
    public static native boolean describe(int index, float[] numbers);
    public static native String text(int index, int what);
    /** The sensor's events into the queue at about every periodUs, or at that period from now on; false: no such sensor. */
    public static native boolean enable(int handle, int periodUs);
    public static native void disable(int handle);
    /** After the events waiting, an event that ends a flush of the sensor, with that token. */
    public static native boolean flush(int handle, int token);
    /** The next event's handle, its values (16 floats) and meta (META longs); 0 when none came within timeoutMs. */
    public static native int next(float[] values, long[] meta, int timeoutMs);
}
