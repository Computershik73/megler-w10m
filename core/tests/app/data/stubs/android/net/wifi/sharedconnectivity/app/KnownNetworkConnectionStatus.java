package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class KnownNetworkConnectionStatus implements android.os.Parcelable {
    public static final class Builder {
        public Builder() {}
        public Builder setStatus(int status) { return this; }
        public Builder setKnownNetwork(KnownNetwork network) { return this; }
        public KnownNetworkConnectionStatus build() { return null; }
    }
    private KnownNetworkConnectionStatus() {}
    public int getStatus() { return 0; }
    public KnownNetwork getKnownNetwork() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<KnownNetworkConnectionStatus> CREATOR = null;
}
