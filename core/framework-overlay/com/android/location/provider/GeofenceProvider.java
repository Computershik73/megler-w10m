package com.android.location.provider;

import android.hardware.location.GeofenceHardware;
import android.os.Binder;
import android.os.IBinder;

/**
 * A geofence provider of an unbundled service, of Android's com.android.location.provider library: the service's onBind
 * gives getBinder, and the system, where the phone has geofencing hardware, hands that binder the hardware, which the
 * provider hears as onGeofenceHardwareChange. Google Play services' location service makes one when it is created
 * (GmsCore's GoogleLocationManagerService) and does not carry the class: without it the service could not be made
 * (NoClassDefFoundError; "Binding com.google.android.location.internal.GoogleLocationManagerService failed"). A Megler
 * phone has no geofencing hardware: nobody binds the provider, so it is never given any.
 */
public abstract class GeofenceProvider {
    private final IBinder mProvider = new Binder();

    public GeofenceProvider() {}

    /** The provider's binder, for its service's onBind. */
    public IBinder getBinder() { return mProvider; }

    /** The geofencing hardware, when the system gives or changes it; null when it is gone. Not called here. */
    public abstract void onGeofenceHardwareChange(GeofenceHardware geofenceHardware);
}
