package com.android.location.provider;

/**
 * Hardware activity recognition, of Android's com.android.location.provider library, which GmsCore's manifest asks for
 * without requiring it. A Megler phone has no such hardware: no instance is handed out (ActivityRecognitionProviderWatcher
 * has none, and a client is never told of one), and one would support nothing. GmsCore's location service makes a Sink
 * when it starts, so the class has to be there.
 */
public final class ActivityRecognitionProvider {
    public static final String ACTIVITY_IN_VEHICLE = "android.activity_recognition.in_vehicle";
    public static final String ACTIVITY_ON_BICYCLE = "android.activity_recognition.on_bicycle";
    public static final String ACTIVITY_WALKING = "android.activity_recognition.walking";
    public static final String ACTIVITY_RUNNING = "android.activity_recognition.running";
    public static final String ACTIVITY_STILL = "android.activity_recognition.still";
    public static final String ACTIVITY_TILTING = "android.activity_recognition.tilting";
    public static final int EVENT_TYPE_FLUSH_COMPLETE = 0, EVENT_TYPE_ENTER = 1, EVENT_TYPE_EXIT = 2;

    public interface Sink {
        void onActivityChanged(ActivityChangedEvent event);
    }

    private ActivityRecognitionProvider() {}
    public String[] getSupportedActivities() { return new String[0]; }
    public boolean isActivitySupported(String activity) { return false; }
    public void registerSink(Sink sink) {}
    public void unregisterSink(Sink sink) {}
    public boolean enableActivityEvent(String activity, int eventType, long reportLatencyNs) { return false; }
    public boolean disableActivityEvent(String activity, int eventType) { return false; }
    public boolean flush() { return false; }
}
