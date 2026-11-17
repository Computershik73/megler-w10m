package android.hardware.location;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step geofence-provider calls of Android's system API. */
public final class GeofenceHardwareRequest {
    public GeofenceHardwareRequest() {}
    public static GeofenceHardwareRequest createCircularGeofence(double latitude, double longitude, double radius) { return null; }
    public void setLastTransition(int lastTransition) {}
    public void setUnknownTimer(int unknownTimer) {}
    public void setMonitorTransitions(int monitorTransitions) {}
    public void setNotificationResponsiveness(int notificationResponsiveness) {}
    public void setSourceTechnologies(int sourceTechnologies) {}
    public double getLatitude() { return 0; }
    public double getLongitude() { return 0; }
    public double getRadius() { return 0; }
    public int getMonitorTransitions() { return 0; }
    public int getUnknownTimer() { return 0; }
    public int getNotificationResponsiveness() { return 0; }
    public int getLastTransition() { return 0; }
    public int getSourceTechnologies() { return 0; }
}
