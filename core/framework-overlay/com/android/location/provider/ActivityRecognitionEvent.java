package com.android.location.provider;

/** One activity hardware recognition reports, as Android's (ActivityRecognitionProvider). */
public class ActivityRecognitionEvent {
    private final String mActivity;
    private final int mEventType;
    private final long mTimestampNs;
    public ActivityRecognitionEvent(String activity, int eventType, long timestampNs) {
        mActivity = activity;
        mEventType = eventType;
        mTimestampNs = timestampNs;
    }
    public String getActivity() { return mActivity; }
    public int getEventType() { return mEventType; }
    public long getTimestampNs() { return mTimestampNs; }
    @Override public String toString() { return "Activity='" + mActivity + "', EventType=" + mEventType + ", TimestampNs=" + mTimestampNs; }
}
