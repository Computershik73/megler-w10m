// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): whether a KnownNetwork was saved on this device. A plain value
// with Android's Builder and Android's check.
package android.net.wifi.sharedconnectivity.app;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

public final class KnownNetworkConnectionStatus implements Parcelable {
    public static final int CONNECTION_STATUS_UNKNOWN = 0, CONNECTION_STATUS_SAVED = 1, CONNECTION_STATUS_SAVE_FAILED = 2;

    private final int mStatus;
    private final KnownNetwork mKnownNetwork;
    private final Bundle mExtras;

    public static final class Builder {
        private int mStatus;
        private KnownNetwork mKnownNetwork;
        private Bundle mExtras = Bundle.EMPTY;

        public Builder() {}
        public Builder setStatus(int status) { mStatus = status; return this; }
        public Builder setKnownNetwork(KnownNetwork network) { mKnownNetwork = network; return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public KnownNetworkConnectionStatus build() { return new KnownNetworkConnectionStatus(mStatus, mKnownNetwork, mExtras); }
    }

    private KnownNetworkConnectionStatus(int status, KnownNetwork network, Bundle extras) {
        if (status < CONNECTION_STATUS_UNKNOWN || status > CONNECTION_STATUS_SAVE_FAILED) throw new IllegalArgumentException("Illegal connection status");
        mStatus = status;
        mKnownNetwork = network;
        mExtras = extras;
    }

    public int getStatus() { return mStatus; }
    public KnownNetwork getKnownNetwork() { return mKnownNetwork; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof KnownNetworkConnectionStatus)) return false;
        KnownNetworkConnectionStatus other = (KnownNetworkConnectionStatus) obj;
        return mStatus == other.mStatus && Objects.equals(mKnownNetwork, other.mKnownNetwork);
    }
    @Override public int hashCode() { return Objects.hash(mStatus, mKnownNetwork); }
    @Override public String toString() {
        return "KnownNetworkConnectionStatus[status=" + mStatus + ", known network=" + mKnownNetwork + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mStatus);
        mKnownNetwork.writeToParcel(dest, flags);
        dest.writeBundle(mExtras);
    }
    public static final Parcelable.Creator<KnownNetworkConnectionStatus> CREATOR = new Parcelable.Creator<KnownNetworkConnectionStatus>() {
        @Override public KnownNetworkConnectionStatus createFromParcel(Parcel in) {
            return new KnownNetworkConnectionStatus(in.readInt(), KnownNetwork.readFromParcel(in), in.readBundle());
        }
        @Override public KnownNetworkConnectionStatus[] newArray(int size) { return new KnownNetworkConnectionStatus[size]; }
    };
}
