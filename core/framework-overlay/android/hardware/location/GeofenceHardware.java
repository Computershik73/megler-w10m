package android.hardware.location;

/**
 * Geofences kept by the phone's hardware (the GNSS chip, the sensor hub), Android's system API: what a
 * com.android.location.provider.GeofenceProvider is given, and what GmsCore's geofencer would use in place of its own
 * watching of the location. A Megler phone has no such hardware: no instance is handed out (the constructor is private,
 * Context.getSystemService gives none, a GeofenceProvider is never told of one), and one would monitor nothing.
 */
public final class GeofenceHardware {
    public static final int MONITORING_TYPE_GPS_HARDWARE = 0, MONITORING_TYPE_FUSED_HARDWARE = 1;
    public static final int MONITOR_CURRENTLY_AVAILABLE = 0, MONITOR_CURRENTLY_UNAVAILABLE = 1, MONITOR_UNSUPPORTED = 2;
    public static final int GEOFENCE_ENTERED = 1 << 0, GEOFENCE_EXITED = 1 << 1, GEOFENCE_UNCERTAIN = 1 << 2;
    public static final int GEOFENCE_SUCCESS = 0, GEOFENCE_ERROR_TOO_MANY_GEOFENCES = 1, GEOFENCE_ERROR_ID_EXISTS = 2, GEOFENCE_ERROR_ID_UNKNOWN = 3,
                            GEOFENCE_ERROR_INVALID_TRANSITION = 4, GEOFENCE_FAILURE = 5, GEOFENCE_ERROR_INSUFFICIENT_MEMORY = 6;
    public static final int SOURCE_TECHNOLOGY_GNSS = 1 << 0, SOURCE_TECHNOLOGY_WIFI = 1 << 1, SOURCE_TECHNOLOGY_SENSORS = 1 << 2,
                            SOURCE_TECHNOLOGY_CELL = 1 << 3, SOURCE_TECHNOLOGY_BLUETOOTH = 1 << 4;

    private GeofenceHardware() {}
    public int[] getMonitoringTypes() { return new int[0]; }
    public int getStatusOfMonitoringType(int monitoringType) { return MONITOR_UNSUPPORTED; }
    public boolean addGeofence(int geofenceId, int monitoringType, GeofenceHardwareRequest geofenceRequest, GeofenceHardwareCallback callback) { return false; }
    public boolean removeGeofence(int geofenceId, int monitoringType) { return false; }
    public boolean pauseGeofence(int geofenceId, int monitoringType) { return false; }
    public boolean resumeGeofence(int geofenceId, int monitoringType, int monitorTransition) { return false; }
    public boolean registerForMonitorStateChangeCallback(int monitoringType, GeofenceHardwareMonitorCallback callback) { return false; }
    public boolean unregisterForMonitorStateChangeCallback(int monitoringType, GeofenceHardwareMonitorCallback callback) { return false; }
}
