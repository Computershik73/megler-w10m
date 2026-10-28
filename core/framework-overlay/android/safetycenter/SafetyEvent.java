// Megler: Android 13's SafetyEvent, which the framework did not have: what a safety source says happened when it hands Safety
// Center its data. The phone has no Safety Center (SafetyCenterManager.java), so nothing here is sent anywhere; the class is
// there for code that builds one before it asks for the manager.
//
// Why. Google Play services 26's password manager module, as it starts, builds a SafetyEvent of the type "device rebooted"
// (new SafetyEvent.Builder(600).build()) before it looks for Safety Center; without the class its thread ended in
// NoClassDefFoundError: android/safetycenter/SafetyEvent$Builder.
package android.safetycenter;

import android.os.Parcel;
import android.os.Parcelable;

public final class SafetyEvent implements Parcelable {
    public static final int SAFETY_EVENT_TYPE_SOURCE_STATE_CHANGED = 100;
    public static final int SAFETY_EVENT_TYPE_REFRESH_REQUESTED = 200;
    public static final int SAFETY_EVENT_TYPE_RESOLVING_ACTION_SUCCEEDED = 300;
    public static final int SAFETY_EVENT_TYPE_RESOLVING_ACTION_FAILED = 400;
    public static final int SAFETY_EVENT_TYPE_DEVICE_LOCALE_CHANGED = 500;
    public static final int SAFETY_EVENT_TYPE_DEVICE_REBOOTED = 600;

    private final int mType;
    private final String mRefreshBroadcastId, mSafetySourceIssueId, mSafetySourceIssueActionId;

    private SafetyEvent(int type, String refreshBroadcastId, String issueId, String issueActionId) {
        mType = type;
        mRefreshBroadcastId = refreshBroadcastId;
        mSafetySourceIssueId = issueId;
        mSafetySourceIssueActionId = issueActionId;
    }

    public int getType() { return mType; }
    public String getRefreshBroadcastId() { return mRefreshBroadcastId; }
    public String getSafetySourceIssueId() { return mSafetySourceIssueId; }
    public String getSafetySourceIssueActionId() { return mSafetySourceIssueActionId; }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mType);
        dest.writeString(mRefreshBroadcastId);
        dest.writeString(mSafetySourceIssueId);
        dest.writeString(mSafetySourceIssueActionId);
    }
    public static final Parcelable.Creator<SafetyEvent> CREATOR = new Parcelable.Creator<SafetyEvent>() {
        @Override public SafetyEvent createFromParcel(Parcel in) { return new SafetyEvent(in.readInt(), in.readString(), in.readString(), in.readString()); }
        @Override public SafetyEvent[] newArray(int size) { return new SafetyEvent[size]; }
    };

    @Override public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof SafetyEvent)) return false;
        SafetyEvent e = (SafetyEvent) o;
        return mType == e.mType && java.util.Objects.equals(mRefreshBroadcastId, e.mRefreshBroadcastId)
            && java.util.Objects.equals(mSafetySourceIssueId, e.mSafetySourceIssueId) && java.util.Objects.equals(mSafetySourceIssueActionId, e.mSafetySourceIssueActionId);
    }
    @Override public int hashCode() { return java.util.Objects.hash(mType, mRefreshBroadcastId, mSafetySourceIssueId, mSafetySourceIssueActionId); }
    @Override public String toString() {
        return "SafetyEvent{mType=" + mType + ", mRefreshBroadcastId='" + mRefreshBroadcastId + "', mSafetySourceIssueId='" + mSafetySourceIssueId
            + "', mSafetySourceIssueActionId='" + mSafetySourceIssueActionId + "'}";
    }

    public static final class Builder {
        private int mType;
        private String mRefreshBroadcastId, mSafetySourceIssueId, mSafetySourceIssueActionId;
        public Builder(int type) { mType = type; }
        public Builder(SafetyEvent e) {
            mType = e.mType;
            mRefreshBroadcastId = e.mRefreshBroadcastId;
            mSafetySourceIssueId = e.mSafetySourceIssueId;
            mSafetySourceIssueActionId = e.mSafetySourceIssueActionId;
        }
        public Builder setType(int type) { mType = type; return this; }
        public Builder setRefreshBroadcastId(String id) { mRefreshBroadcastId = id; return this; }
        public Builder setSafetySourceIssueId(String id) { mSafetySourceIssueId = id; return this; }
        public Builder setSafetySourceIssueActionId(String id) { mSafetySourceIssueActionId = id; return this; }
        public SafetyEvent build() { return new SafetyEvent(mType, mRefreshBroadcastId, mSafetySourceIssueId, mSafetySourceIssueActionId); }
    }
}
