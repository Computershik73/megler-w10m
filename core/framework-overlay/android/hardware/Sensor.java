// Megler: replaces the original's (CoreMotion's sensors, every one called Apple's). A sensor of the phone's as Android
// describes it: what core/src/ndk/mg_sensors.c makes of what Windows has (megler.Sensors), with its name, vendor, range,
// resolution, delays, reporting mode and whether it wakes the device -- the proximity, as Android's own default does.
package android.hardware;

public final class Sensor {
    public static final int TYPE_ACCELEROMETER = 1, TYPE_MAGNETIC_FIELD = 2, TYPE_ORIENTATION = 3, TYPE_GYROSCOPE = 4, TYPE_LIGHT = 5, TYPE_PRESSURE = 6,
            TYPE_TEMPERATURE = 7, TYPE_PROXIMITY = 8, TYPE_GRAVITY = 9, TYPE_LINEAR_ACCELERATION = 10, TYPE_ROTATION_VECTOR = 11, TYPE_RELATIVE_HUMIDITY = 12,
            TYPE_AMBIENT_TEMPERATURE = 13, TYPE_MAGNETIC_FIELD_UNCALIBRATED = 14, TYPE_GAME_ROTATION_VECTOR = 15, TYPE_GYROSCOPE_UNCALIBRATED = 16,
            TYPE_SIGNIFICANT_MOTION = 17, TYPE_STEP_DETECTOR = 18, TYPE_STEP_COUNTER = 19, TYPE_GEOMAGNETIC_ROTATION_VECTOR = 20, TYPE_HEART_RATE = 21,
            TYPE_TILT_DETECTOR = 22, TYPE_WAKE_GESTURE = 23, TYPE_GLANCE_GESTURE = 24, TYPE_PICK_UP_GESTURE = 25, TYPE_WRIST_TILT_GESTURE = 26,
            TYPE_DEVICE_ORIENTATION = 27, TYPE_POSE_6DOF = 28, TYPE_STATIONARY_DETECT = 29, TYPE_MOTION_DETECT = 30, TYPE_HEART_BEAT = 31,
            TYPE_DYNAMIC_SENSOR_META = 32, TYPE_LOW_LATENCY_OFFBODY_DETECT = 34, TYPE_ACCELEROMETER_UNCALIBRATED = 35, TYPE_HINGE_ANGLE = 36,
            TYPE_HEAD_TRACKER = 37, TYPE_ACCELEROMETER_LIMITED_AXES = 38, TYPE_GYROSCOPE_LIMITED_AXES = 39, TYPE_ACCELEROMETER_LIMITED_AXES_UNCALIBRATED = 40,
            TYPE_GYROSCOPE_LIMITED_AXES_UNCALIBRATED = 41, TYPE_HEADING = 42, TYPE_MOISTURE_INTRUSION = 43, TYPE_ALL = -1, TYPE_DEVICE_PRIVATE_BASE = 0x10000;
    public static final String STRING_TYPE_ACCELEROMETER = "android.sensor.accelerometer", STRING_TYPE_MAGNETIC_FIELD = "android.sensor.magnetic_field",
            STRING_TYPE_ORIENTATION = "android.sensor.orientation", STRING_TYPE_GYROSCOPE = "android.sensor.gyroscope", STRING_TYPE_LIGHT = "android.sensor.light",
            STRING_TYPE_PRESSURE = "android.sensor.pressure", STRING_TYPE_TEMPERATURE = "android.sensor.temperature",
            STRING_TYPE_PROXIMITY = "android.sensor.proximity", STRING_TYPE_GRAVITY = "android.sensor.gravity",
            STRING_TYPE_LINEAR_ACCELERATION = "android.sensor.linear_acceleration", STRING_TYPE_ROTATION_VECTOR = "android.sensor.rotation_vector",
            STRING_TYPE_RELATIVE_HUMIDITY = "android.sensor.relative_humidity", STRING_TYPE_AMBIENT_TEMPERATURE = "android.sensor.ambient_temperature",
            STRING_TYPE_MAGNETIC_FIELD_UNCALIBRATED = "android.sensor.magnetic_field_uncalibrated",
            STRING_TYPE_GAME_ROTATION_VECTOR = "android.sensor.game_rotation_vector", STRING_TYPE_GYROSCOPE_UNCALIBRATED = "android.sensor.gyroscope_uncalibrated",
            STRING_TYPE_SIGNIFICANT_MOTION = "android.sensor.significant_motion", STRING_TYPE_STEP_DETECTOR = "android.sensor.step_detector",
            STRING_TYPE_STEP_COUNTER = "android.sensor.step_counter", STRING_TYPE_GEOMAGNETIC_ROTATION_VECTOR = "android.sensor.geomagnetic_rotation_vector",
            STRING_TYPE_HEART_RATE = "android.sensor.heart_rate", SENSOR_STRING_TYPE_TILT_DETECTOR = "android.sensor.tilt_detector",
            STRING_TYPE_WAKE_GESTURE = "android.sensor.wake_gesture", STRING_TYPE_GLANCE_GESTURE = "android.sensor.glance_gesture",
            STRING_TYPE_PICK_UP_GESTURE = "android.sensor.pick_up_gesture", STRING_TYPE_WRIST_TILT_GESTURE = "android.sensor.wrist_tilt_gesture",
            STRING_TYPE_DEVICE_ORIENTATION = "android.sensor.device_orientation", STRING_TYPE_POSE_6DOF = "android.sensor.pose_6dof",
            STRING_TYPE_STATIONARY_DETECT = "android.sensor.stationary_detect", STRING_TYPE_MOTION_DETECT = "android.sensor.motion_detect",
            STRING_TYPE_HEART_BEAT = "android.sensor.heart_beat", STRING_TYPE_DYNAMIC_SENSOR_META = "android.sensor.dynamic_sensor_meta",
            STRING_TYPE_LOW_LATENCY_OFFBODY_DETECT = "android.sensor.low_latency_offbody_detect",
            STRING_TYPE_ACCELEROMETER_UNCALIBRATED = "android.sensor.accelerometer_uncalibrated", STRING_TYPE_HINGE_ANGLE = "android.sensor.hinge_angle",
            STRING_TYPE_HEAD_TRACKER = "android.sensor.head_tracker", STRING_TYPE_ACCELEROMETER_LIMITED_AXES = "android.sensor.accelerometer_limited_axes",
            STRING_TYPE_GYROSCOPE_LIMITED_AXES = "android.sensor.gyroscope_limited_axes",
            STRING_TYPE_ACCELEROMETER_LIMITED_AXES_UNCALIBRATED = "android.sensor.accelerometer_limited_axes_uncalibrated",
            STRING_TYPE_GYROSCOPE_LIMITED_AXES_UNCALIBRATED = "android.sensor.gyroscope_limited_axes_uncalibrated", STRING_TYPE_HEADING = "android.sensor.heading",
            STRING_TYPE_MOISTURE_INTRUSION = "android.sensor.moisture_intrusion";
    public static final int REPORTING_MODE_CONTINUOUS = 0, REPORTING_MODE_ON_CHANGE = 1, REPORTING_MODE_ONE_SHOT = 2, REPORTING_MODE_SPECIAL_TRIGGER = 3;

    private final String mName, mVendor, mStringType;
    private final int mType, mHandle, mVersion, mMinDelay, mMaxDelay, mReportingMode, mValues;
    private final float mMaxRange, mResolution, mPower;
    private final boolean mWakeUp;

    /** From megler.Sensors.describe's numbers. */
    Sensor(String name, String vendor, String stringType, float[] k) {
        mName = name;
        mVendor = vendor;
        mStringType = stringType;
        mType = (int) k[megler.Sensors.TYPE];
        mHandle = (int) k[megler.Sensors.HANDLE];
        mMaxRange = k[megler.Sensors.MAX_RANGE];
        mResolution = k[megler.Sensors.RESOLUTION];
        mPower = k[megler.Sensors.POWER];
        mMinDelay = (int) k[megler.Sensors.MIN_DELAY];
        mMaxDelay = (int) k[megler.Sensors.MAX_DELAY];
        mReportingMode = (int) k[megler.Sensors.REPORTING_MODE];
        mWakeUp = k[megler.Sensors.WAKE_UP] != 0;
        mValues = (int) k[megler.Sensors.VALUES];
        mVersion = (int) k[megler.Sensors.VERSION];
    }

    public int getType() { return mType; }
    public String getName() { return mName; }
    public String getVendor() { return mVendor; }
    public int getVersion() { return mVersion; }
    public String getStringType() { return mStringType; }
    public float getMaximumRange() { return mMaxRange; }
    public float getResolution() { return mResolution; }
    public float getPower() { return mPower; }
    public int getMinDelay() { return mMinDelay; }
    public int getMaxDelay() { return mMaxDelay; }
    public int getFifoReservedEventCount() { return 0; }
    public int getFifoMaxEventCount() { return 0; }
    public int getReportingMode() { return mReportingMode; }
    /** 0: none of these sensors is told apart from another of its kind (Android gives an id to dynamic sensors). */
    public int getId() { return 0; }
    public boolean isWakeUpSensor() { return mWakeUp; }
    public boolean isDynamicSensor() { return false; }
    public boolean isAdditionalInfoSupported() { return false; }
    public int getHighestDirectReportRateLevel() { return 0; }
    public boolean isDirectChannelTypeSupported(int sharedMemType) { return false; }
    /** (Hidden on Android: what its events and the NDK's ASensor call it.) */
    public int getHandle() { return mHandle; }
    public String getRequiredPermission() { return null; }
    public java.util.UUID getUuid() { return null; }
    public boolean isDataInjectionSupported() { return false; }
    /** How many values an event of the sensor has (hidden on Android): the rotation vector three for an app made for Android 4.2 or before. */
    public static int getMaxLengthValuesArray(Sensor sensor, int sdkLevel) {
        if (sensor.mType == TYPE_ROTATION_VECTOR && sdkLevel <= 17) return 3;
        return sensor.mValues > 0 ? sensor.mValues : 16;
    }
    int valueCount() { return mValues; }

    @Override
    public String toString() {
        return "{Sensor name=\"" + mName + "\", vendor=\"" + mVendor + "\", version=" + mVersion + ", type=" + mType + ", maxRange=" + mMaxRange
                + ", resolution=" + mResolution + ", power=" + mPower + ", minDelay=" + mMinDelay + "}";
    }
}
