package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class NetworkProviderInfo implements android.os.Parcelable {
    public static final class Builder {
        public Builder(String deviceName, String modelName) {}
        public Builder setDeviceType(int deviceType) { return this; }
        public Builder setBatteryPercentage(int batteryPercentage) { return this; }
        public Builder setBatteryCharging(boolean isBatteryCharging) { return this; }
        public Builder setConnectionStrength(int connectionStrength) { return this; }
        public NetworkProviderInfo build() { return null; }
    }
    private NetworkProviderInfo() {}
    public int getDeviceType() { return 0; }
    public String getDeviceName() { return null; }
    public String getModelName() { return null; }
    public int getBatteryPercentage() { return 0; }
    public boolean isBatteryCharging() { return false; }
    public int getConnectionStrength() { return 0; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<NetworkProviderInfo> CREATOR = null;
}
