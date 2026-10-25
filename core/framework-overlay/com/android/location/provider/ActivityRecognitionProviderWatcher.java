package com.android.location.provider;

/** Android's older way to the activity recognition hardware: on a Megler phone it never has a provider. */
@Deprecated
public class ActivityRecognitionProviderWatcher {
    private static final ActivityRecognitionProviderWatcher sWatcher = new ActivityRecognitionProviderWatcher();
    private final android.os.IBinder mBinder = new android.os.Binder();
    private ActivityRecognitionProviderWatcher() {}
    public static ActivityRecognitionProviderWatcher getInstance() { return sWatcher; }
    public android.os.IBinder getBinder() { return mBinder; }
    public ActivityRecognitionProvider getActivityRecognitionProvider() { return null; }
}
