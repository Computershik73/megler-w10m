// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): a Wi-Fi network another device of the user's knows and this one
// may be given -- where the knowledge comes from, the SSID, its security, the device. A plain value with Android's Builder
// and Android's checks.
package android.net.wifi.sharedconnectivity.app;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.ArraySet;

import java.util.Objects;
import java.util.Set;

public final class KnownNetwork implements Parcelable {
    /** Where the network is known from: a device of the user's nearby, or the user's account. */
    public static final int NETWORK_SOURCE_UNKNOWN = 0, NETWORK_SOURCE_NEARBY_SELF = 1, NETWORK_SOURCE_CLOUD_SELF = 2;

    private final int mNetworkSource;
    private final String mSsid;
    private final ArraySet<Integer> mSecurityTypes;
    private final NetworkProviderInfo mNetworkProviderInfo;
    private final Bundle mExtras;

    public static final class Builder {
        private int mNetworkSource = -1;
        private String mSsid;
        private final ArraySet<Integer> mSecurityTypes = new ArraySet<>();
        private NetworkProviderInfo mNetworkProviderInfo;
        private Bundle mExtras = Bundle.EMPTY;

        public Builder() {}
        public Builder setNetworkSource(int networkSource) { mNetworkSource = networkSource; return this; }
        public Builder setSsid(String ssid) { mSsid = ssid; return this; }
        /** One of WifiInfo's SECURITY_TYPE_ values. */
        public Builder addSecurityType(int securityType) { mSecurityTypes.add(securityType); return this; }
        /** The device; it may be null unless the source is NETWORK_SOURCE_NEARBY_SELF. */
        public Builder setNetworkProviderInfo(NetworkProviderInfo networkProviderInfo) { mNetworkProviderInfo = networkProviderInfo; return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public KnownNetwork build() { return new KnownNetwork(mNetworkSource, mSsid, mSecurityTypes, mNetworkProviderInfo, mExtras); }
    }

    private KnownNetwork(int networkSource, String ssid, ArraySet<Integer> securityTypes, NetworkProviderInfo networkProviderInfo, Bundle extras) {
        if (networkSource < NETWORK_SOURCE_UNKNOWN || networkSource > NETWORK_SOURCE_CLOUD_SELF) throw new IllegalArgumentException("Illegal network source");
        if (ssid == null || ssid.isEmpty()) throw new IllegalArgumentException("SSID must be set");
        if (securityTypes.isEmpty()) throw new IllegalArgumentException("SecurityTypes must be set");
        if (networkSource == NETWORK_SOURCE_NEARBY_SELF && networkProviderInfo == null) {
            throw new IllegalArgumentException("Device info must be provided when network source is NETWORK_SOURCE_NEARBY_SELF");
        }
        mNetworkSource = networkSource;
        mSsid = ssid;
        mSecurityTypes = new ArraySet<>(securityTypes);
        mNetworkProviderInfo = networkProviderInfo;
        mExtras = extras;
    }

    public int getNetworkSource() { return mNetworkSource; }
    public String getSsid() { return mSsid; }
    public Set<Integer> getSecurityTypes() { return mSecurityTypes; }
    public NetworkProviderInfo getNetworkProviderInfo() { return mNetworkProviderInfo; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof KnownNetwork)) return false;
        KnownNetwork other = (KnownNetwork) obj;
        return mNetworkSource == other.mNetworkSource && Objects.equals(mSsid, other.mSsid) && Objects.equals(mSecurityTypes, other.mSecurityTypes)
               && Objects.equals(mNetworkProviderInfo, other.mNetworkProviderInfo);
    }
    @Override public int hashCode() { return Objects.hash(mNetworkSource, mSsid, mSecurityTypes, mNetworkProviderInfo); }
    @Override public String toString() {
        return "KnownNetwork[NetworkSource=" + mNetworkSource + ", ssid=" + mSsid + ", securityTypes=" + mSecurityTypes + ", networkProviderInfo="
               + mNetworkProviderInfo + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mNetworkSource);
        dest.writeString(mSsid);
        dest.writeArraySet(mSecurityTypes);
        dest.writeBoolean(mNetworkProviderInfo != null);
        if (mNetworkProviderInfo != null) mNetworkProviderInfo.writeToParcel(dest, flags);
        dest.writeBundle(mExtras);
    }
    /** One written by writeToParcel (hidden in Android; KnownNetworkConnectionStatus reads its network so). */
    @SuppressWarnings("unchecked")
    public static KnownNetwork readFromParcel(Parcel in) {
        int networkSource = in.readInt();
        String ssid = in.readString();
        ArraySet<Integer> securityTypes = (ArraySet<Integer>) in.readArraySet(null);
        NetworkProviderInfo networkProviderInfo = in.readBoolean() ? NetworkProviderInfo.readFromParcel(in) : null;
        return new KnownNetwork(networkSource, ssid, securityTypes, networkProviderInfo, in.readBundle());
    }
    public static final Parcelable.Creator<KnownNetwork> CREATOR = new Parcelable.Creator<KnownNetwork>() {
        @Override public KnownNetwork createFromParcel(Parcel in) { return readFromParcel(in); }
        @Override public KnownNetwork[] newArray(int size) { return new KnownNetwork[size]; }
    };
}
