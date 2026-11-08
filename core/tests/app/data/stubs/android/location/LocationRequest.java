package android.location;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step location-request calls of Android 12's class. */
public final class LocationRequest implements android.os.Parcelable {
    public static final long PASSIVE_INTERVAL = Long.MAX_VALUE;
    public static final int QUALITY_BALANCED_POWER_ACCURACY = 102;
    public static final android.os.Parcelable.Creator<LocationRequest> CREATOR = null;
    public long getIntervalMillis() { return 0; }
    public long getMinUpdateIntervalMillis() { return 0; }
    public int getMaxUpdates() { return 0; }
    public int getQuality() { return 0; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel parcel, int flags) {}
    public static final class Builder {
        public Builder(long intervalMillis) {}
        public Builder setMaxUpdates(int maxUpdates) { return this; }
        public Builder setMinUpdateDistanceMeters(float meters) { return this; }
        public LocationRequest build() { return null; }
    }
}
