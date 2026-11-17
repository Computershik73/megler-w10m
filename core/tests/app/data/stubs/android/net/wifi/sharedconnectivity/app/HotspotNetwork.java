package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class HotspotNetwork implements android.os.Parcelable {
    public static final class Builder {
        public Builder() {}
        public Builder setDeviceId(long deviceId) { return this; }
        public Builder setNetworkProviderInfo(NetworkProviderInfo networkProviderInfo) { return this; }
        public Builder setHostNetworkType(int networkType) { return this; }
        public Builder setNetworkName(String networkName) { return this; }
        public Builder setHotspotSsid(String hotspotSsid) { return this; }
        public Builder setHotspotBssid(String hotspotBssid) { return this; }
        public Builder addHotspotSecurityType(int hotspotSecurityType) { return this; }
        public Builder setExtras(android.os.Bundle extras) { return this; }
        public HotspotNetwork build() { return null; }
    }
    private HotspotNetwork() {}
    public long getDeviceId() { return 0; }
    public NetworkProviderInfo getNetworkProviderInfo() { return null; }
    public int getHostNetworkType() { return 0; }
    public String getNetworkName() { return null; }
    public String getHotspotSsid() { return null; }
    public String getHotspotBssid() { return null; }
    public java.util.Set<Integer> getHotspotSecurityTypes() { return null; }
    public android.os.Bundle getExtras() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<HotspotNetwork> CREATOR = null;
}
