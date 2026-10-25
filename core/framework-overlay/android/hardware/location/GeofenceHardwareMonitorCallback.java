package android.hardware.location;

/** The callback of the geofencing hardware's state, as Android's: its methods do nothing (GeofenceHardwareCallback). */
public abstract class GeofenceHardwareMonitorCallback {
    @Deprecated
    public void onMonitoringSystemChange(int monitoringType, boolean available, android.location.Location location) {}
}
