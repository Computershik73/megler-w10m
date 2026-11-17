// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): how connecting to a HotspotNetwork goes -- the hotspot being
// turned on, or why it failed. A plain value with Android's Builder and Android's check.
package android.net.wifi.sharedconnectivity.app;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

public final class HotspotNetworkConnectionStatus implements Parcelable {
    public static final int CONNECTION_STATUS_UNKNOWN = 0, CONNECTION_STATUS_ENABLING_HOTSPOT = 1, CONNECTION_STATUS_UNKNOWN_ERROR = 2,
                            CONNECTION_STATUS_PROVISIONING_FAILED = 3, CONNECTION_STATUS_TETHERING_TIMEOUT = 4, CONNECTION_STATUS_TETHERING_UNSUPPORTED = 5,
                            CONNECTION_STATUS_NO_CELL_DATA = 6, CONNECTION_STATUS_ENABLING_HOTSPOT_FAILED = 7,
                            CONNECTION_STATUS_ENABLING_HOTSPOT_TIMEOUT = 8, CONNECTION_STATUS_CONNECT_TO_HOTSPOT_FAILED = 9;

    private final int mStatus;
    private final HotspotNetwork mHotspotNetwork;
    private final Bundle mExtras;

    public static final class Builder {
        private int mStatus;
        private HotspotNetwork mHotspotNetwork;
        private Bundle mExtras = Bundle.EMPTY;

        public Builder() {}
        public Builder setStatus(int status) { mStatus = status; return this; }
        public Builder setHotspotNetwork(HotspotNetwork network) { mHotspotNetwork = network; return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public HotspotNetworkConnectionStatus build() { return new HotspotNetworkConnectionStatus(mStatus, mHotspotNetwork, mExtras); }
    }

    private HotspotNetworkConnectionStatus(int status, HotspotNetwork network, Bundle extras) {
        if (status < CONNECTION_STATUS_UNKNOWN || status > CONNECTION_STATUS_CONNECT_TO_HOTSPOT_FAILED) {
            throw new IllegalArgumentException("Illegal connection status");
        }
        mStatus = status;
        mHotspotNetwork = network;
        mExtras = extras;
    }

    public int getStatus() { return mStatus; }
    public HotspotNetwork getHotspotNetwork() { return mHotspotNetwork; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof HotspotNetworkConnectionStatus)) return false;
        HotspotNetworkConnectionStatus other = (HotspotNetworkConnectionStatus) obj;
        return mStatus == other.mStatus && Objects.equals(mHotspotNetwork, other.mHotspotNetwork);
    }
    @Override public int hashCode() { return Objects.hash(mStatus, mHotspotNetwork); }
    @Override public String toString() {
        return "HotspotNetworkConnectionStatus[status=" + mStatus + ", hotspot network=" + mHotspotNetwork + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mStatus);
        mHotspotNetwork.writeToParcel(dest, flags);
        dest.writeBundle(mExtras);
    }
    public static final Parcelable.Creator<HotspotNetworkConnectionStatus> CREATOR = new Parcelable.Creator<HotspotNetworkConnectionStatus>() {
        @Override public HotspotNetworkConnectionStatus createFromParcel(Parcel in) {
            return new HotspotNetworkConnectionStatus(in.readInt(), HotspotNetwork.readFromParcel(in), in.readBundle());
        }
        @Override public HotspotNetworkConnectionStatus[] newArray(int size) { return new HotspotNetworkConnectionStatus[size]; }
    };
}
