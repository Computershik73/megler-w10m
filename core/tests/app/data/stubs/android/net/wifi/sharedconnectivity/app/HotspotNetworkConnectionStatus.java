package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class HotspotNetworkConnectionStatus implements android.os.Parcelable {
    public static final class Builder {
        public Builder() {}
        public Builder setStatus(int status) { return this; }
        public Builder setHotspotNetwork(HotspotNetwork network) { return this; }
        public HotspotNetworkConnectionStatus build() { return null; }
    }
    private HotspotNetworkConnectionStatus() {}
    public int getStatus() { return 0; }
    public HotspotNetwork getHotspotNetwork() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<HotspotNetworkConnectionStatus> CREATOR = null;
}
