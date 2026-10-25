package com.android.location.provider;

/** The activities hardware recognition reports at once, as Android's (ActivityRecognitionProvider). */
public class ActivityChangedEvent {
    private final java.util.List<ActivityRecognitionEvent> mActivityRecognitionEvents;
    public ActivityChangedEvent(java.util.List<ActivityRecognitionEvent> activityRecognitionEvents) {
        if (activityRecognitionEvents == null)
            throw new java.security.InvalidParameterException("Parameter 'activityRecognitionEvents' must not be null.");
        mActivityRecognitionEvents = activityRecognitionEvents;
    }
    public Iterable<ActivityRecognitionEvent> getActivityRecognitionEvents() { return mActivityRecognitionEvents; }
    @Override public String toString() { return "[ ActivityChangedEvent: " + mActivityRecognitionEvents + " ]"; }
}
