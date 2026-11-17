package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class KnownNetwork implements android.os.Parcelable {
    public static final class Builder {
        public Builder() {}
        public Builder setNetworkSource(int networkSource) { return this; }
        public Builder setSsid(String ssid) { return this; }
        public Builder addSecurityType(int securityType) { return this; }
        public Builder setNetworkProviderInfo(NetworkProviderInfo networkProviderInfo) { return this; }
        public KnownNetwork build() { return null; }
    }
    private KnownNetwork() {}
    public int getNetworkSource() { return 0; }
    public String getSsid() { return null; }
    public java.util.Set<Integer> getSecurityTypes() { return null; }
    public NetworkProviderInfo getNetworkProviderInfo() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<KnownNetwork> CREATOR = null;
}
