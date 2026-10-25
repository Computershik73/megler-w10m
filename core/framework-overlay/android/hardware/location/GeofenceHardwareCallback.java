package android.hardware.location;

/**
 * The callback of a hardware geofence, as Android's: an abstract class whose methods do nothing. A Megler phone has no
 * geofencing hardware (Context.getSystemService gives no GeofenceHardware), so nothing calls it; GmsCore's geofencer makes
 * one all the same when its location service starts.
 */
public abstract class GeofenceHardwareCallback {
    public void onGeofenceTransition(int geofenceId, int transition, android.location.Location location, long timestamp, int monitoringType) {}
    public void onGeofenceAdd(int geofenceId, int status) {}
    public void onGeofenceRemove(int geofenceId, int status) {}
    public void onGeofencePause(int geofenceId, int status) {}
    public void onGeofenceResume(int geofenceId, int status) {}
}
