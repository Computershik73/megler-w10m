// Megler: replaces the original's, which polled CoreMotion's samples every few milliseconds. The phone's sensors, as
// Android's SystemSensorManager hands them out: the list of what the device has (megler.Sensors over
// core/src/ndk/mg_sensors.c, which makes them of Windows' sensors), and for each listener its sensors' events on its
// Handler's thread -- the main thread's unless it gave another -- at about the rate it asked for.
//
// Each sensor runs in the core's queue of the framework's at the shortest period any listener wants of it; one thread of the
// framework's, "SensorEventThread", waits on that queue and posts each event to the listeners of its sensor whose own period
// has gone by since their last (four fifths of it, so that a sensor's jitter does not halve the rate), as Android's own
// SensorEventQueue would see them -- no more than a hundred of a sensor's waiting on a listener's thread that does not take
// them, as Android's socket to it holds no more. A listener's SensorEvent is one object, filled anew for each event; onAccuracyChanged
// comes before an event whose accuracy is not the one before (the first included), as Android's dispatchSensorEvent calls
// it. While the app is paused the core keeps the phone's sensors stopped: no events, as Android sends an app in the
// background none. flush() ends with onFlushCompleted after the events already in the queue. The old SensorListener of
// Android 1.x is served as Android's LegacySensorManager serves it (its values turned to the old signs), for a display
// that is not turned. The static math is Android's.
package android.hardware;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;

public class SensorManager {
    public static final int SENSOR_DELAY_FASTEST = 0, SENSOR_DELAY_GAME = 1, SENSOR_DELAY_UI = 2, SENSOR_DELAY_NORMAL = 3;
    public static final int SENSOR_STATUS_NO_CONTACT = -1, SENSOR_STATUS_UNRELIABLE = 0, SENSOR_STATUS_ACCURACY_LOW = 1, SENSOR_STATUS_ACCURACY_MEDIUM = 2, SENSOR_STATUS_ACCURACY_HIGH = 3;
    public static final float STANDARD_GRAVITY = 9.80665f, GRAVITY_EARTH = 9.80665f, GRAVITY_SUN = 275.0f, GRAVITY_MOON = 1.6f, MAGNETIC_FIELD_EARTH_MAX = 60.0f, MAGNETIC_FIELD_EARTH_MIN = 30.0f,
        PRESSURE_STANDARD_ATMOSPHERE = 1013.25f, LIGHT_SUNLIGHT_MAX = 120000.0f, LIGHT_SUNLIGHT = 110000.0f, LIGHT_SHADE = 20000.0f, LIGHT_OVERCAST = 10000.0f, LIGHT_SUNRISE = 400.0f, LIGHT_CLOUDY = 100.0f, LIGHT_FULLMOON = 0.25f, LIGHT_NO_MOON = 0.001f;
    public static final int AXIS_X = 1, AXIS_Y = 2, AXIS_Z = 3, AXIS_MINUS_X = AXIS_X | 0x80, AXIS_MINUS_Y = AXIS_Y | 0x80, AXIS_MINUS_Z = AXIS_Z | 0x80;
    @Deprecated public static final int SENSOR_ACCELEROMETER = 2, SENSOR_MAGNETIC_FIELD = 8, SENSOR_ORIENTATION = 1, SENSOR_ORIENTATION_RAW = 128, SENSOR_LIGHT = 16, SENSOR_PROXIMITY = 32, SENSOR_TEMPERATURE = 4, SENSOR_TRICORDER = 64, SENSOR_ALL = 127, SENSOR_MIN = SENSOR_ORIENTATION, SENSOR_MAX = ((SENSOR_ALL + 1) >> 1);
    @Deprecated public static final int DATA_X = 0, DATA_Y = 1, DATA_Z = 2, RAW_DATA_INDEX = 3, RAW_DATA_X = 3, RAW_DATA_Y = 4, RAW_DATA_Z = 5;
    public static final int DATA_INJECTION = 1, REPLAY_DATA_INJECTION = 3, HAL_BYPASS_REPLAY_DATA_INJECTION = 4;
    public static final float GRAVITY_DEATH_STAR_I = 3.5303614254189597e-07f, GRAVITY_JUPITER = 23.1200008392334f, GRAVITY_MARS = 3.7100000381469727f,
        GRAVITY_MERCURY = 3.700000047683716f, GRAVITY_NEPTUNE = 11.0f, GRAVITY_PLUTO = 0.6000000238418579f, GRAVITY_SATURN = 8.960000038146973f,
        GRAVITY_THE_ISLAND = 4.815162181854248f, GRAVITY_URANUS = 8.6899995803833f, GRAVITY_VENUS = 8.869999885559082f;
    protected static final String TAG = "SensorManager";

    public static abstract class DynamicSensorCallback { public void onDynamicSensorConnected(Sensor s) {} public void onDynamicSensorDisconnected(Sensor s) {} }

    /**
     * A sensor of a listener's: the period it asked for, its last event's time, the accuracy it was last told, its event object,
     * and how many of its events wait on the listener's thread.
     */
    private static final class Sub {
        final Sensor sensor;
        final long periodNs;
        final SensorEvent event;
        long lastNs;
        int accuracy, waiting;

        Sub(Sensor sensor, int periodUs) {
            this.sensor = sensor;
            this.periodNs = periodUs * 1000L;
            event = new SensorEvent(Sensor.getMaxLengthValuesArray(sensor, megler.Manifest.targetSdk));
            event.sensor = sensor;
        }
    }

    /** A listener: where its events go, and its sensors (Android's SensorEventQueue of one listener). */
    private static final class Listener {
        final SensorEventListener listener;
        final Handler handler;
        final ArrayList<Sub> subs = new ArrayList<>();

        Listener(SensorEventListener listener, Handler handler) {
            this.listener = listener;
            this.handler = handler;
        }
    }

    private static final Object LOCK = new Object();
    /** The events of one sensor that may wait on a listener's thread (Android's sensor socket holds about a hundred). */
    private static final int MAX_WAITING = 100;
    private static List<Sensor> sAll;
    private static final HashMap<Integer, List<Sensor>> sByType = new HashMap<>();
    private static final HashMap<SensorEventListener, Listener> sListeners = new HashMap<>();
    /* A flush's token: the listener and the sensor whose onFlushCompleted it ends with. */
    private static final HashMap<Integer, Object[]> sFlushes = new HashMap<>();
    private static int sFlushToken;
    private static Thread sThread;
    private static final HashMap<SensorListener, LegacyListener> sLegacy = new HashMap<>();

    public SensorManager() {}

    /** Every sensor the device has, asked of the core once. */
    private static List<Sensor> all() {
        synchronized (LOCK) {
            if (sAll == null) {
                ArrayList<Sensor> list = new ArrayList<>();
                float[] k = new float[megler.Sensors.NUMBERS];
                int n = megler.Sensors.count();
                for (int i = 0; i < n; i++) {
                    if (!megler.Sensors.describe(i, k)) continue;
                    list.add(new Sensor(megler.Sensors.text(i, megler.Sensors.NAME), megler.Sensors.text(i, megler.Sensors.VENDOR),
                            megler.Sensors.text(i, megler.Sensors.STRING_TYPE), k));
                }
                sAll = Collections.unmodifiableList(list);
            }
            return sAll;
        }
    }

    public List<Sensor> getSensorList(int type) {
        List<Sensor> full = all();
        synchronized (LOCK) {
            List<Sensor> list = sByType.get(type);
            if (list == null) {
                if (type == Sensor.TYPE_ALL) {
                    list = full;
                } else {
                    ArrayList<Sensor> some = new ArrayList<>();
                    for (Sensor s : full)
                        if (s.getType() == type) some.add(s);
                    list = Collections.unmodifiableList(some);
                }
                sByType.put(type, list);
            }
            return list;
        }
    }

    protected List<Sensor> getFullSensorList() { return all(); }
    public List<Sensor> getDynamicSensorList(int type) { return Collections.emptyList(); }
    protected List<Sensor> getFullDynamicSensorList() { return Collections.emptyList(); }

    /** As Android's: of the types that wake the device by default, the waking sensor; of the rest, one that does not. */
    public Sensor getDefaultSensor(int type) {
        boolean wakeUp = type == Sensor.TYPE_PROXIMITY || type == Sensor.TYPE_SIGNIFICANT_MOTION || type == Sensor.TYPE_TILT_DETECTOR
                || type == Sensor.TYPE_WAKE_GESTURE || type == Sensor.TYPE_GLANCE_GESTURE || type == Sensor.TYPE_PICK_UP_GESTURE
                || type == Sensor.TYPE_WRIST_TILT_GESTURE || type == Sensor.TYPE_DYNAMIC_SENSOR_META || type == Sensor.TYPE_HINGE_ANGLE;
        return getDefaultSensor(type, wakeUp);
    }

    public Sensor getDefaultSensor(int type, boolean wakeUp) {
        for (Sensor s : getSensorList(type))
            if (s.isWakeUpSensor() == wakeUp) return s;
        return null;
    }

    public Sensor getSensorByHandle(int handle) {
        for (Sensor s : all())
            if (s.getHandle() == handle) return s;
        return null;
    }

    public boolean isDynamicSensorDiscoverySupported() { return false; }
    public void registerDynamicSensorCallback(DynamicSensorCallback cb) {}
    public void registerDynamicSensorCallback(DynamicSensorCallback cb, Handler h) {}
    public void unregisterDynamicSensorCallback(DynamicSensorCallback cb) {}
    protected void registerDynamicSensorCallbackImpl(DynamicSensorCallback cb, Handler h) {}
    protected void unregisterDynamicSensorCallbackImpl(DynamicSensorCallback cb) {}

    /** SENSOR_DELAY_* as microseconds; anything else is microseconds already. */
    private static int delayUs(int rate) {
        switch (rate) {
        case SENSOR_DELAY_FASTEST: return 0;
        case SENSOR_DELAY_GAME: return 20000;
        case SENSOR_DELAY_UI: return 66667;
        case SENSOR_DELAY_NORMAL: return 200000;
        default: return rate;
        }
    }

    public boolean registerListener(SensorEventListener l, Sensor s, int samplingPeriodUs) { return registerListenerImpl(l, s, delayUs(samplingPeriodUs), null, 0, 0); }
    public boolean registerListener(SensorEventListener l, Sensor s, int samplingPeriodUs, int maxReportLatencyUs) {
        return registerListenerImpl(l, s, delayUs(samplingPeriodUs), null, maxReportLatencyUs, 0);
    }
    public boolean registerListener(SensorEventListener l, Sensor s, int samplingPeriodUs, Handler h) { return registerListenerImpl(l, s, delayUs(samplingPeriodUs), h, 0, 0); }
    public boolean registerListener(SensorEventListener l, Sensor s, int samplingPeriodUs, int maxReportLatencyUs, Handler h) {
        return registerListenerImpl(l, s, delayUs(samplingPeriodUs), h, maxReportLatencyUs, 0);
    }

    /** As Android's: a listener's handler is the one it first registered with; a sensor it has already is refused. */
    protected boolean registerListenerImpl(SensorEventListener listener, Sensor sensor, int delayUs, Handler handler, int maxBatchReportLatencyUs, int reservedFlags) {
        if (listener == null || sensor == null) {
            Log.e(TAG, "sensor or listener is null");
            return false;
        }
        if (sensor.getReportingMode() == Sensor.REPORTING_MODE_ONE_SHOT) {
            Log.e(TAG, "Trigger Sensors should use the requestTriggerSensor.");
            return false;
        }
        if (maxBatchReportLatencyUs < 0 || delayUs < 0) {
            Log.e(TAG, "delayUs and maxBatchReportLatencyUs should be non-negative");
            return false;
        }
        synchronized (LOCK) {
            Listener r = sListeners.get(listener);
            boolean made = r == null;
            if (made) {
                r = new Listener(listener, handler != null ? handler : new Handler(Looper.getMainLooper()));
                sListeners.put(listener, r);
            } else {
                for (Sub x : r.subs)
                    if (x.sensor == sensor) return false;
            }
            Sub sub = new Sub(sensor, delayUs);
            r.subs.add(sub);
            if (!runLocked(sensor)) {
                r.subs.remove(sub);
                if (made) sListeners.remove(listener);
                return false;
            }
            if (sThread == null) {
                sThread = new Thread(SensorManager::loop, "SensorEventThread");
                sThread.setDaemon(true);
                sThread.start();
            }
        }
        return true;
    }

    /** The sensor in the core's queue at the shortest period its listeners want, or out of it when none wants it. */
    private static boolean runLocked(Sensor sensor) {
        long shortest = -1;
        for (Listener r : sListeners.values())
            for (Sub s : r.subs)
                if (s.sensor == sensor && (shortest < 0 || s.periodNs < shortest)) shortest = s.periodNs;
        if (shortest < 0) {
            megler.Sensors.disable(sensor.getHandle());
            return true;
        }
        return megler.Sensors.enable(sensor.getHandle(), (int) Math.min(Integer.MAX_VALUE, shortest / 1000));
    }

    public void unregisterListener(SensorEventListener l) { unregisterListenerImpl(l, null); }
    public void unregisterListener(SensorEventListener l, Sensor s) { unregisterListenerImpl(l, s); }

    protected void unregisterListenerImpl(SensorEventListener listener, Sensor sensor) {
        if (listener == null) return;
        synchronized (LOCK) {
            Listener r = sListeners.get(listener);
            if (r == null) return;
            ArrayList<Sensor> gone = new ArrayList<>();
            for (int i = r.subs.size() - 1; i >= 0; i--) {
                Sub s = r.subs.get(i);
                if (sensor == null || s.sensor == sensor) {
                    r.subs.remove(i);
                    gone.add(s.sensor);
                }
            }
            if (r.subs.isEmpty()) sListeners.remove(listener);
            for (Sensor s : gone) runLocked(s);
        }
    }

    public boolean flush(SensorEventListener listener) { return flushImpl(listener); }

    protected boolean flushImpl(SensorEventListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener cannot be null");
        synchronized (LOCK) {
            Listener r = sListeners.get(listener);
            if (r == null) return false;
            for (Sub s : r.subs) {
                int token = ++sFlushToken;
                if (token == 0) token = ++sFlushToken;
                sFlushes.put(token, new Object[] { r, s.sensor });
                megler.Sensors.flush(s.sensor.getHandle(), token);
            }
            return true;
        }
    }

    /** No sensor of the device's triggers once (Windows has none of significant motion). */
    public boolean requestTriggerSensor(TriggerEventListener l, Sensor s) { return requestTriggerSensorImpl(l, s); }
    public boolean cancelTriggerSensor(TriggerEventListener l, Sensor s) { return cancelTriggerSensorImpl(l, s, true); }
    protected boolean requestTriggerSensorImpl(TriggerEventListener l, Sensor s) {
        if (s == null) throw new IllegalArgumentException("sensor cannot be null");
        if (l == null) throw new IllegalArgumentException("listener cannot be null");
        return false;
    }
    protected boolean cancelTriggerSensorImpl(TriggerEventListener l, Sensor s, boolean disable) { return false; }

    /** The thread that waits on the core's queue and posts each event to the listeners it is for. */
    private static void loop() {
        float[] values = new float[16];
        long[] meta = new long[megler.Sensors.META];
        for (;;) {
            int handle = megler.Sensors.next(values, meta, 1000);
            if (handle == 0) continue;
            try {
                dispatch(handle, values, meta);
            } catch (Throwable t) {
                Log.w(TAG, "an event could not be handed on", t);
            }
        }
    }

    private static void dispatch(int handle, float[] values, long[] meta) {
        final long ns = meta[megler.Sensors.TIMESTAMP];
        final int accuracy = (int) meta[megler.Sensors.ACCURACY], flush = (int) meta[megler.Sensors.FLUSH];
        synchronized (LOCK) {
            if (flush != 0) {
                final Object[] f = sFlushes.remove(flush);
                if (f == null) return;
                final Listener r = (Listener) f[0];
                if (r.listener instanceof SensorEventListener2) r.handler.post(() -> ((SensorEventListener2) r.listener).onFlushCompleted((Sensor) f[1]));
                return;
            }
            for (final Listener r : sListeners.values()) {
                for (final Sub s : r.subs) {
                    if (s.sensor.getHandle() != handle) continue;
                    if (s.sensor.getReportingMode() == Sensor.REPORTING_MODE_CONTINUOUS && s.lastNs != 0 && ns - s.lastNs < s.periodNs * 4 / 5) continue;
                    // A listener whose thread does not take its events has no more sent it than Android's socket would
                    // hold for it: the newest are what is lost, as there.
                    if (s.waiting >= MAX_WAITING) continue;
                    s.waiting++;
                    s.lastNs = ns;
                    final float[] v = java.util.Arrays.copyOf(values, s.event.values.length);
                    r.handler.post(() -> deliver(r, s, v, ns, accuracy));
                }
            }
        }
    }

    /** On the listener's thread: nothing for a sensor it no longer has, as on Android. */
    private static void deliver(Listener r, Sub s, float[] v, long ns, int accuracy) {
        synchronized (LOCK) {
            s.waiting--;
            if (!r.subs.contains(s)) return;
        }
        SensorEvent e = s.event;
        System.arraycopy(v, 0, e.values, 0, e.values.length);
        e.timestamp = ns;
        e.accuracy = accuracy;
        e.sensor = s.sensor;
        if (accuracy >= 0 && accuracy != s.accuracy) {
            s.accuracy = accuracy;
            r.listener.onAccuracyChanged(s.sensor, accuracy);
        }
        r.listener.onSensorChanged(e);
    }

    // ---- Android 1.x's SensorListener, as Android's LegacySensorManager serves it

    @Deprecated
    public int getSensors() {
        int result = 0;
        for (Sensor s : all()) {
            switch (s.getType()) {
            case Sensor.TYPE_ACCELEROMETER: result |= SENSOR_ACCELEROMETER; break;
            case Sensor.TYPE_MAGNETIC_FIELD: result |= SENSOR_MAGNETIC_FIELD; break;
            case Sensor.TYPE_ORIENTATION: result |= SENSOR_ORIENTATION | SENSOR_ORIENTATION_RAW; break;
            }
        }
        return result;
    }

    @Deprecated public boolean registerListener(SensorListener l, int sensors) { return registerListener(l, sensors, SENSOR_DELAY_NORMAL); }

    @Deprecated
    public boolean registerListener(SensorListener l, int sensors, int rate) {
        if (l == null) return false;
        boolean any = false;
        synchronized (sLegacy) {
            LegacyListener legacy = sLegacy.get(l);
            if (legacy == null) sLegacy.put(l, legacy = new LegacyListener(l));
            legacy.sensors |= sensors;
            if ((sensors & SENSOR_ACCELEROMETER) != 0) any |= registerLegacy(legacy, Sensor.TYPE_ACCELEROMETER, rate);
            if ((sensors & SENSOR_MAGNETIC_FIELD) != 0) any |= registerLegacy(legacy, Sensor.TYPE_MAGNETIC_FIELD, rate);
            if ((sensors & (SENSOR_ORIENTATION | SENSOR_ORIENTATION_RAW)) != 0) any |= registerLegacy(legacy, Sensor.TYPE_ORIENTATION, rate);
            if ((sensors & SENSOR_TEMPERATURE) != 0) any |= registerLegacy(legacy, Sensor.TYPE_TEMPERATURE, rate);
        }
        return any;
    }

    private boolean registerLegacy(LegacyListener legacy, int type, int rate) {
        Sensor s = getDefaultSensor(type);
        return s != null && (registerListener(legacy, s, rate) || legacy.has(s));
    }

    @Deprecated public void unregisterListener(SensorListener l) { unregisterListener(l, SENSOR_ALL | SENSOR_ORIENTATION_RAW); }

    @Deprecated
    public void unregisterListener(SensorListener l, int sensors) {
        synchronized (sLegacy) {
            LegacyListener legacy = sLegacy.get(l);
            if (legacy == null) return;
            legacy.sensors &= ~sensors;
            if ((legacy.sensors & SENSOR_ACCELEROMETER) == 0) unregisterLegacy(legacy, Sensor.TYPE_ACCELEROMETER);
            if ((legacy.sensors & SENSOR_MAGNETIC_FIELD) == 0) unregisterLegacy(legacy, Sensor.TYPE_MAGNETIC_FIELD);
            if ((legacy.sensors & (SENSOR_ORIENTATION | SENSOR_ORIENTATION_RAW)) == 0) unregisterLegacy(legacy, Sensor.TYPE_ORIENTATION);
            if ((legacy.sensors & SENSOR_TEMPERATURE) == 0) unregisterLegacy(legacy, Sensor.TYPE_TEMPERATURE);
            if (legacy.sensors == 0) sLegacy.remove(l);
        }
    }

    private void unregisterLegacy(LegacyListener legacy, int type) {
        Sensor s = getDefaultSensor(type);
        if (s != null) unregisterListener(legacy, s);
    }

    /** An old listener's values: three, then the same three again ("raw"), in the old API's signs. */
    private static final class LegacyListener implements SensorEventListener {
        final SensorListener target;
        int sensors;
        final float[] values = new float[6];

        LegacyListener(SensorListener target) { this.target = target; }

        boolean has(Sensor s) {
            synchronized (LOCK) {
                Listener r = sListeners.get(this);
                if (r != null)
                    for (Sub x : r.subs)
                        if (x.sensor == s) return true;
                return false;
            }
        }

        private static int legacyType(int type) {
            switch (type) {
            case Sensor.TYPE_ACCELEROMETER: return SENSOR_ACCELEROMETER;
            case Sensor.TYPE_MAGNETIC_FIELD: return SENSOR_MAGNETIC_FIELD;
            case Sensor.TYPE_ORIENTATION: return SENSOR_ORIENTATION_RAW;
            case Sensor.TYPE_TEMPERATURE: return SENSOR_TEMPERATURE;
            default: return 0;
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
            int type = legacyType(sensor.getType());
            if (type == SENSOR_ORIENTATION_RAW) type = (sensors & SENSOR_ORIENTATION) != 0 ? SENSOR_ORIENTATION : SENSOR_ORIENTATION_RAW;
            if (type != 0) target.onAccuracyChanged(type, accuracy);
        }

        @Override
        public void onSensorChanged(SensorEvent event) {
            int type = legacyType(event.sensor.getType());
            if (type == 0) return;
            float x = event.values[0], y = event.values.length > 1 ? event.values[1] : 0, z = event.values.length > 2 ? event.values[2] : 0;
            // (LegacySensorManager.mapSensorDataToWindow, for a display that is not turned.)
            if (type == SENSOR_ORIENTATION_RAW) {
                z = -z;
            } else if (type == SENSOR_ACCELEROMETER) {
                x = -x;
                y = -y;
                z = -z;
            } else if (type == SENSOR_MAGNETIC_FIELD) {
                x = -x;
                y = -y;
            }
            values[0] = values[3] = x;
            values[1] = values[4] = y;
            values[2] = values[5] = z;
            if (type == SENSOR_ORIENTATION_RAW) {
                if ((sensors & SENSOR_ORIENTATION_RAW) != 0) target.onSensorChanged(SENSOR_ORIENTATION_RAW, values);
                if ((sensors & SENSOR_ORIENTATION) != 0) target.onSensorChanged(SENSOR_ORIENTATION, values);
            } else {
                target.onSensorChanged(type, values);
            }
        }
    }

    // ---- what Android has and no sensor here offers: direct channels, data injection, additional info

    protected int configureDirectChannelImpl(SensorDirectChannel channel, Sensor sensor, int rate) { return 0; }
    public SensorDirectChannel createDirectChannel(HardwareBuffer buffer) { return null; }
    public SensorDirectChannel createDirectChannel(android.os.MemoryFile file) { return null; }
    protected SensorDirectChannel createDirectChannelImpl(android.os.MemoryFile file, HardwareBuffer buffer) { return null; }
    protected void destroyDirectChannelImpl(SensorDirectChannel channel) {}
    public boolean initDataInjection(boolean enable) { return false; }
    public boolean initDataInjection(boolean enable, int mode) { return false; }
    protected boolean initDataInjectionImpl(boolean enable, int mode) { return false; }
    public boolean injectSensorData(Sensor sensor, float[] values, int accuracy, long timestamp) { return false; }
    protected boolean injectSensorDataImpl(Sensor sensor, float[] values, int accuracy, long timestamp) { return false; }
    public boolean setOperationParameter(SensorAdditionalInfo parameter) { return false; }
    protected boolean setOperationParameterImpl(SensorAdditionalInfo parameter) { return false; }

    // ---- Android's math
    public static boolean getRotationMatrix(float[] R, float[] I, float[] gravity, float[] geomagnetic) {
        float Ax = gravity[0], Ay = gravity[1], Az = gravity[2];
        final float normsqA = (Ax * Ax + Ay * Ay + Az * Az), g = 9.81f, freeFallGravitySquared = 0.01f * g * g;
        if (normsqA < freeFallGravitySquared) return false;
        final float Ex = geomagnetic[0], Ey = geomagnetic[1], Ez = geomagnetic[2];
        float Hx = Ey * Az - Ez * Ay, Hy = Ez * Ax - Ex * Az, Hz = Ex * Ay - Ey * Ax;
        final float normH = (float) Math.sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
        if (normH < 0.1f) return false;
        final float invH = 1.0f / normH;
        Hx *= invH; Hy *= invH; Hz *= invH;
        final float invA = 1.0f / (float) Math.sqrt(Ax * Ax + Ay * Ay + Az * Az);
        Ax *= invA; Ay *= invA; Az *= invA;
        final float Mx = Ay * Hz - Az * Hy, My = Az * Hx - Ax * Hz, Mz = Ax * Hy - Ay * Hx;
        if (R != null) {
            if (R.length == 9) { R[0] = Hx; R[1] = Hy; R[2] = Hz; R[3] = Mx; R[4] = My; R[5] = Mz; R[6] = Ax; R[7] = Ay; R[8] = Az; }
            else if (R.length == 16) { R[0] = Hx; R[1] = Hy; R[2] = Hz; R[3] = 0; R[4] = Mx; R[5] = My; R[6] = Mz; R[7] = 0; R[8] = Ax; R[9] = Ay; R[10] = Az; R[11] = 0; R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 1; }
        }
        if (I != null) {
            final float invE = 1.0f / (float) Math.sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
            final float c = (Ex * Mx + Ey * My + Ez * Mz) * invE, s = (Ex * Ax + Ey * Ay + Ez * Az) * invE;
            if (I.length == 9) { I[0] = 1; I[1] = 0; I[2] = 0; I[3] = 0; I[4] = c; I[5] = s; I[6] = 0; I[7] = -s; I[8] = c; }
            else if (I.length == 16) { I[0] = 1; I[1] = 0; I[2] = 0; I[4] = 0; I[5] = c; I[6] = s; I[8] = 0; I[9] = -s; I[10] = c; I[3] = I[7] = I[11] = I[12] = I[13] = I[14] = 0; I[15] = 1; }
        }
        return true;
    }
    public static float getInclination(float[] I) { if (I.length == 9) return (float) Math.atan2(I[5], I[4]); return (float) Math.atan2(I[6], I[5]); }
    public static boolean remapCoordinateSystem(float[] inR, int X, int Y, float[] outR) {
        if (inR == outR) { final float[] temp = new float[16]; synchronized (temp) { if (remapCoordinateSystemImpl(inR, X, Y, temp)) { System.arraycopy(temp, 0, outR, 0, outR.length); return true; } } }
        return remapCoordinateSystemImpl(inR, X, Y, outR);
    }
    private static boolean remapCoordinateSystemImpl(float[] inR, int X, int Y, float[] outR) {
        final int length = outR.length;
        if (inR.length != length) return false;
        if ((X & 0x7C) != 0 || (Y & 0x7C) != 0) return false;
        if (((X & 0x3) == 0) || ((Y & 0x3) == 0)) return false;
        if ((X & 0x3) == (Y & 0x3)) return false;
        int Z = X ^ Y;
        final int x = (X & 0x3) - 1, y = (Y & 0x3) - 1, z = (Z & 0x3) - 1;
        final int axis_y = (z + 1) % 3, axis_z = (z + 2) % 3;
        if (((x ^ axis_y) | (y ^ axis_z)) != 0) Z ^= 0x80;
        final boolean sx = (X >= 0x80), sy = (Y >= 0x80), sz = (Z >= 0x80);
        final int rowLength = ((length == 16) ? 4 : 3);
        for (int j = 0; j < 3; j++) {
            final int offset = j * rowLength;
            for (int i = 0; i < 3; i++) {
                if (x == i) outR[offset + i] = sx ? -inR[offset + 0] : inR[offset + 0];
                if (y == i) outR[offset + i] = sy ? -inR[offset + 1] : inR[offset + 1];
                if (z == i) outR[offset + i] = sz ? -inR[offset + 2] : inR[offset + 2];
            }
        }
        if (length == 16) { outR[3] = outR[7] = outR[11] = outR[12] = outR[13] = outR[14] = 0; outR[15] = 1; }
        return true;
    }
    public static float[] getOrientation(float[] R, float[] values) {
        if (R.length == 9) { values[0] = (float) Math.atan2(R[1], R[4]); values[1] = (float) Math.asin(-R[7]); values[2] = (float) Math.atan2(-R[6], R[8]); }
        else { values[0] = (float) Math.atan2(R[1], R[5]); values[1] = (float) Math.asin(-R[9]); values[2] = (float) Math.atan2(-R[8], R[10]); }
        return values;
    }
    public static float getAltitude(float p0, float p) { final float coef = 1.0f / 5.255f; return 44330.0f * (1.0f - (float) Math.pow(p / p0, coef)); }
    public static void getAngleChange(float[] angleChange, float[] R, float[] prevR) {
        float rd1, rd4, rd6, rd7, rd8, ri0, ri1, ri2, ri3, ri4, ri5, ri6, ri7, ri8, pri0, pri1, pri2, pri3, pri4, pri5, pri6, pri7, pri8;
        if (R.length == 9) { ri0 = R[0]; ri1 = R[1]; ri2 = R[2]; ri3 = R[3]; ri4 = R[4]; ri5 = R[5]; ri6 = R[6]; ri7 = R[7]; ri8 = R[8]; }
        else { ri0 = R[0]; ri1 = R[1]; ri2 = R[2]; ri3 = R[4]; ri4 = R[5]; ri5 = R[6]; ri6 = R[8]; ri7 = R[9]; ri8 = R[10]; }
        if (prevR.length == 9) { pri0 = prevR[0]; pri1 = prevR[1]; pri2 = prevR[2]; pri3 = prevR[3]; pri4 = prevR[4]; pri5 = prevR[5]; pri6 = prevR[6]; pri7 = prevR[7]; pri8 = prevR[8]; }
        else { pri0 = prevR[0]; pri1 = prevR[1]; pri2 = prevR[2]; pri3 = prevR[4]; pri4 = prevR[5]; pri5 = prevR[6]; pri6 = prevR[8]; pri7 = prevR[9]; pri8 = prevR[10]; }
        rd1 = pri0 * ri1 + pri3 * ri4 + pri6 * ri7; rd4 = pri1 * ri1 + pri4 * ri4 + pri7 * ri7; rd6 = pri2 * ri0 + pri5 * ri3 + pri8 * ri6; rd7 = pri2 * ri1 + pri5 * ri4 + pri8 * ri7; rd8 = pri2 * ri2 + pri5 * ri5 + pri8 * ri8;
        angleChange[0] = (float) Math.atan2(rd1, rd4); angleChange[1] = (float) Math.asin(-rd7); angleChange[2] = (float) Math.atan2(-rd6, rd8);
    }
    public static void getRotationMatrixFromVector(float[] R, float[] rotationVector) {
        float q0, q1 = rotationVector[0], q2 = rotationVector[1], q3 = rotationVector[2];
        if (rotationVector.length >= 4) q0 = rotationVector[3];
        else { q0 = 1 - q1 * q1 - q2 * q2 - q3 * q3; q0 = (q0 > 0) ? (float) Math.sqrt(q0) : 0; }
        float sq_q1 = 2 * q1 * q1, sq_q2 = 2 * q2 * q2, sq_q3 = 2 * q3 * q3, q1_q2 = 2 * q1 * q2, q3_q0 = 2 * q3 * q0, q1_q3 = 2 * q1 * q3, q2_q0 = 2 * q2 * q0, q2_q3 = 2 * q2 * q3, q1_q0 = 2 * q1 * q0;
        if (R.length == 9) {
            R[0] = 1 - sq_q2 - sq_q3; R[1] = q1_q2 - q3_q0; R[2] = q1_q3 + q2_q0;
            R[3] = q1_q2 + q3_q0; R[4] = 1 - sq_q1 - sq_q3; R[5] = q2_q3 - q1_q0;
            R[6] = q1_q3 - q2_q0; R[7] = q2_q3 + q1_q0; R[8] = 1 - sq_q1 - sq_q2;
        } else if (R.length == 16) {
            R[0] = 1 - sq_q2 - sq_q3; R[1] = q1_q2 - q3_q0; R[2] = q1_q3 + q2_q0; R[3] = 0.0f;
            R[4] = q1_q2 + q3_q0; R[5] = 1 - sq_q1 - sq_q3; R[6] = q2_q3 - q1_q0; R[7] = 0.0f;
            R[8] = q1_q3 - q2_q0; R[9] = q2_q3 + q1_q0; R[10] = 1 - sq_q1 - sq_q2; R[11] = 0.0f;
            R[12] = R[13] = R[14] = 0.0f; R[15] = 1.0f;
        }
    }
    public static void getQuaternionFromVector(float[] Q, float[] rv) {
        if (rv.length >= 4) Q[0] = rv[3];
        else { Q[0] = 1 - rv[0] * rv[0] - rv[1] * rv[1] - rv[2] * rv[2]; Q[0] = (Q[0] > 0) ? (float) Math.sqrt(Q[0]) : 0; }
        Q[1] = rv[0]; Q[2] = rv[1]; Q[3] = rv[2];
    }
}
