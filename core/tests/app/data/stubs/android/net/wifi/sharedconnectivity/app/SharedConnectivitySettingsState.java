package android.net.wifi.sharedconnectivity.app;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public final class SharedConnectivitySettingsState implements android.os.Parcelable {
    public static final class Builder {
        public Builder() {}
        public Builder setInstantTetherEnabled(boolean instantTetherEnabled) { return this; }
        public Builder setInstantTetherSettingsPendingIntent(android.app.PendingIntent pendingIntent) { return this; }
        public SharedConnectivitySettingsState build() { return null; }
    }
    private SharedConnectivitySettingsState() {}
    public boolean isInstantTetherEnabled() { return false; }
    public android.app.PendingIntent getInstantTetherSettingsPendingIntent() { return null; }
    public android.os.Bundle getExtras() { return null; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {}
    public static final android.os.Parcelable.Creator<SharedConnectivitySettingsState> CREATOR = null;
}
