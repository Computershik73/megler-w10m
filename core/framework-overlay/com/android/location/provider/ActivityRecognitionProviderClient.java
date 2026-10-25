package com.android.location.provider;

/**
 * A client the system tells of the activity recognition hardware, as Android's. On a Megler phone there is none, and
 * onProviderChanged is never called; its binder is one nobody calls.
 */
public abstract class ActivityRecognitionProviderClient {
    private final android.os.IBinder mBinder = new android.os.Binder();
    protected ActivityRecognitionProviderClient() {}
    public android.os.IBinder getBinder() { return mBinder; }
    public abstract void onProviderChanged(boolean isSupported, ActivityRecognitionProvider instance);
}
