// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): what the service's own settings say -- whether "instant
// tethering" is on, and the screen that changes it. A plain value with Android's Builder. Google Play services' tethering
// service builds one in its onCreate (instant tethering off, its settings screen) and gives it to the service.
package android.net.wifi.sharedconnectivity.app;

import android.app.PendingIntent;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

public final class SharedConnectivitySettingsState implements Parcelable {
    private final boolean mInstantTetherEnabled;
    private final PendingIntent mInstantTetherSettingsPendingIntent;
    private final Bundle mExtras;

    public static final class Builder {
        private boolean mInstantTetherEnabled;
        private PendingIntent mInstantTetherSettingsPendingIntent;
        private Bundle mExtras = Bundle.EMPTY;

        public Builder() {}
        public Builder setInstantTetherEnabled(boolean instantTetherEnabled) { mInstantTetherEnabled = instantTetherEnabled; return this; }
        public Builder setInstantTetherSettingsPendingIntent(PendingIntent pendingIntent) { mInstantTetherSettingsPendingIntent = pendingIntent; return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public SharedConnectivitySettingsState build() {
            return new SharedConnectivitySettingsState(mInstantTetherEnabled, mInstantTetherSettingsPendingIntent, mExtras);
        }
    }

    private SharedConnectivitySettingsState(boolean instantTetherEnabled, PendingIntent pendingIntent, Bundle extras) {
        mInstantTetherEnabled = instantTetherEnabled;
        mInstantTetherSettingsPendingIntent = pendingIntent;
        mExtras = extras;
    }

    public boolean isInstantTetherEnabled() { return mInstantTetherEnabled; }
    /** The settings screen of instant tethering; null when none was given. */
    public PendingIntent getInstantTetherSettingsPendingIntent() { return mInstantTetherSettingsPendingIntent; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof SharedConnectivitySettingsState)) return false;
        SharedConnectivitySettingsState other = (SharedConnectivitySettingsState) obj;
        return mInstantTetherEnabled == other.mInstantTetherEnabled
               && Objects.equals(mInstantTetherSettingsPendingIntent, other.mInstantTetherSettingsPendingIntent);
    }
    @Override public int hashCode() { return Objects.hash(mInstantTetherEnabled, mInstantTetherSettingsPendingIntent); }
    @Override public String toString() {
        return "SharedConnectivitySettingsState[instantTetherEnabled=" + mInstantTetherEnabled + ", PendingIntent=" + mInstantTetherSettingsPendingIntent
               + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        PendingIntent.writePendingIntentOrNullToParcel(mInstantTetherSettingsPendingIntent, dest);
        dest.writeBoolean(mInstantTetherEnabled);
        dest.writeBundle(mExtras);
    }
    public static final Parcelable.Creator<SharedConnectivitySettingsState> CREATOR = new Parcelable.Creator<SharedConnectivitySettingsState>() {
        @Override public SharedConnectivitySettingsState createFromParcel(Parcel in) {
            PendingIntent pendingIntent = PendingIntent.readPendingIntentOrNullFromParcel(in);
            boolean instantTetherEnabled = in.readBoolean();
            return new SharedConnectivitySettingsState(instantTetherEnabled, pendingIntent, in.readBundle());
        }
        @Override public SharedConnectivitySettingsState[] newArray(int size) { return new SharedConnectivitySettingsState[size]; }
    };
}
