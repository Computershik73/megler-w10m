package com.android.location.provider;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step geofence-provider calls of Android's location provider library. */
public abstract class GeofenceProvider {
    public GeofenceProvider() {}
    public android.os.IBinder getBinder() { return null; }
    public abstract void onGeofenceHardwareChange(android.hardware.location.GeofenceHardware geofenceHardware);
}
