package android.hardware.location;

/**
 * What a hardware geofence is to be, Android's system API: a circle, which transitions to tell of, how soon, and by which
 * technologies. A plain value with Android's defaults; GeofenceHardware.addGeofence, which would take it, adds nothing here.
 */
public final class GeofenceHardwareRequest {
    static final int GEOFENCE_TYPE_CIRCLE = 0;
    private int mType;
    private double mLatitude, mLongitude, mRadius;
    private int mLastTransition = GeofenceHardware.GEOFENCE_UNCERTAIN;
    private int mUnknownTimer = 30000;
    private int mMonitorTransitions = GeofenceHardware.GEOFENCE_UNCERTAIN | GeofenceHardware.GEOFENCE_ENTERED | GeofenceHardware.GEOFENCE_EXITED;
    private int mNotificationResponsiveness = 5000;
    private int mSourceTechnologies = GeofenceHardware.SOURCE_TECHNOLOGY_GNSS;

    public GeofenceHardwareRequest() {}

    /** A circle of `radius` metres round a point. */
    public static GeofenceHardwareRequest createCircularGeofence(double latitude, double longitude, double radius) {
        GeofenceHardwareRequest r = new GeofenceHardwareRequest();
        r.mType = GEOFENCE_TYPE_CIRCLE;
        r.mLatitude = latitude;
        r.mLongitude = longitude;
        r.mRadius = radius;
        return r;
    }

    public void setLastTransition(int lastTransition) { mLastTransition = lastTransition; }
    /** How long, in milliseconds, the place may be unknown before GEOFENCE_UNCERTAIN is told. */
    public void setUnknownTimer(int unknownTimer) { mUnknownTimer = unknownTimer; }
    public void setMonitorTransitions(int monitorTransitions) { mMonitorTransitions = monitorTransitions; }
    public void setNotificationResponsiveness(int notificationResponsiveness) { mNotificationResponsiveness = notificationResponsiveness; }
    public void setSourceTechnologies(int sourceTechnologies) {
        int known = sourceTechnologies & (GeofenceHardware.SOURCE_TECHNOLOGY_GNSS | GeofenceHardware.SOURCE_TECHNOLOGY_WIFI | GeofenceHardware.SOURCE_TECHNOLOGY_SENSORS
                                          | GeofenceHardware.SOURCE_TECHNOLOGY_CELL | GeofenceHardware.SOURCE_TECHNOLOGY_BLUETOOTH);
        if (known == 0) throw new IllegalArgumentException("At least one valid source technology must be set.");
        mSourceTechnologies = known;
    }

    public double getLatitude() { return mLatitude; }
    public double getLongitude() { return mLongitude; }
    public double getRadius() { return mRadius; }
    public int getMonitorTransitions() { return mMonitorTransitions; }
    public int getUnknownTimer() { return mUnknownTimer; }
    public int getNotificationResponsiveness() { return mNotificationResponsiveness; }
    public int getLastTransition() { return mLastTransition; }
    public int getSourceTechnologies() { return mSourceTechnologies; }
    int getType() { return mType; }
}
