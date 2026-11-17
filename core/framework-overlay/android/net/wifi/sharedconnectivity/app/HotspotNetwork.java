// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): a hotspot that another device of the user's can turn on for this
// one ("instant hotspot") -- the device, how it reaches the internet itself, the hotspot's name and security. A plain value
// with Android's Builder and Android's checks; Google Play services' tethering module builds them of the phones it finds.
package android.net.wifi.sharedconnectivity.app;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;
import android.util.ArraySet;

import java.util.Objects;
import java.util.Set;

public final class HotspotNetwork implements Parcelable {
    /** How the device that offers the hotspot reaches the internet. */
    public static final int NETWORK_TYPE_UNKNOWN = 0, NETWORK_TYPE_CELLULAR = 1, NETWORK_TYPE_WIFI = 2, NETWORK_TYPE_ETHERNET = 3;

    private final long mDeviceId;
    private final NetworkProviderInfo mNetworkProviderInfo;
    private final int mHostNetworkType;
    private final String mNetworkName;
    private final String mHotspotSsid;
    private final String mHotspotBssid;
    private final ArraySet<Integer> mHotspotSecurityTypes;
    private final Bundle mExtras;

    public static final class Builder {
        private long mDeviceId = -1;
        private NetworkProviderInfo mNetworkProviderInfo;
        private int mHostNetworkType;
        private String mNetworkName;
        private String mHotspotSsid;
        private String mHotspotBssid;
        private final ArraySet<Integer> mHotspotSecurityTypes = new ArraySet<>();
        private Bundle mExtras = Bundle.EMPTY;

        public Builder() {}
        public Builder setDeviceId(long deviceId) { mDeviceId = deviceId; return this; }
        public Builder setNetworkProviderInfo(NetworkProviderInfo networkProviderInfo) { mNetworkProviderInfo = networkProviderInfo; return this; }
        public Builder setHostNetworkType(int networkType) { mHostNetworkType = networkType; return this; }
        public Builder setNetworkName(String networkName) { mNetworkName = networkName; return this; }
        public Builder setHotspotSsid(String hotspotSsid) { mHotspotSsid = hotspotSsid; return this; }
        public Builder setHotspotBssid(String hotspotBssid) { mHotspotBssid = hotspotBssid; return this; }
        /** One of WifiInfo's SECURITY_TYPE_ values. */
        public Builder addHotspotSecurityType(int hotspotSecurityType) { mHotspotSecurityTypes.add(hotspotSecurityType); return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public HotspotNetwork build() {
            return new HotspotNetwork(mDeviceId, mNetworkProviderInfo, mHostNetworkType, mNetworkName, mHotspotSsid, mHotspotBssid, mHotspotSecurityTypes, mExtras);
        }
    }

    private HotspotNetwork(long deviceId, NetworkProviderInfo networkProviderInfo, int hostNetworkType, String networkName, String hotspotSsid, String hotspotBssid,
                           ArraySet<Integer> hotspotSecurityTypes, Bundle extras) {
        if (deviceId < 0) throw new IllegalArgumentException("DeviceId must be set");
        if (networkProviderInfo == null) throw new IllegalArgumentException("NetworkProviderInfo must be set");
        if (hostNetworkType < NETWORK_TYPE_UNKNOWN || hostNetworkType > NETWORK_TYPE_ETHERNET) throw new IllegalArgumentException("Illegal network type");
        if (networkName == null) throw new IllegalArgumentException("NetworkName must be set");
        mDeviceId = deviceId;
        mNetworkProviderInfo = networkProviderInfo;
        mHostNetworkType = hostNetworkType;
        mNetworkName = networkName;
        mHotspotSsid = hotspotSsid;
        mHotspotBssid = hotspotBssid;
        mHotspotSecurityTypes = new ArraySet<>(hotspotSecurityTypes);
        mExtras = extras;
    }

    public long getDeviceId() { return mDeviceId; }
    public NetworkProviderInfo getNetworkProviderInfo() { return mNetworkProviderInfo; }
    public int getHostNetworkType() { return mHostNetworkType; }
    public String getNetworkName() { return mNetworkName; }
    public String getHotspotSsid() { return mHotspotSsid; }
    public String getHotspotBssid() { return mHotspotBssid; }
    public Set<Integer> getHotspotSecurityTypes() { return mHotspotSecurityTypes; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof HotspotNetwork)) return false;
        HotspotNetwork other = (HotspotNetwork) obj;
        return mDeviceId == other.mDeviceId && Objects.equals(mNetworkProviderInfo, other.mNetworkProviderInfo) && mHostNetworkType == other.mHostNetworkType
               && Objects.equals(mNetworkName, other.mNetworkName) && Objects.equals(mHotspotSsid, other.mHotspotSsid)
               && Objects.equals(mHotspotBssid, other.mHotspotBssid) && Objects.equals(mHotspotSecurityTypes, other.mHotspotSecurityTypes);
    }
    @Override public int hashCode() {
        return Objects.hash(mDeviceId, mNetworkProviderInfo, mNetworkName, mHotspotSsid, mHotspotBssid, mHotspotSecurityTypes);
    }
    @Override public String toString() {
        return "HotspotNetwork[deviceId=" + mDeviceId + ", networkType=" + mHostNetworkType + ", networkProviderInfo=" + mNetworkProviderInfo + ", networkName="
               + mNetworkName + ", hotspotSsid=" + mHotspotSsid + ", hotspotBssid=" + mHotspotBssid + ", hotspotSecurityTypes=" + mHotspotSecurityTypes
               + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeLong(mDeviceId);
        mNetworkProviderInfo.writeToParcel(dest, flags);
        dest.writeInt(mHostNetworkType);
        dest.writeString(mNetworkName);
        dest.writeString(mHotspotSsid);
        dest.writeString(mHotspotBssid);
        dest.writeArraySet(mHotspotSecurityTypes);
        dest.writeBundle(mExtras);
    }
    /** One written by writeToParcel (hidden in Android; HotspotNetworkConnectionStatus reads its network so). */
    @SuppressWarnings("unchecked")
    public static HotspotNetwork readFromParcel(Parcel in) {
        return new HotspotNetwork(in.readLong(), NetworkProviderInfo.readFromParcel(in), in.readInt(), in.readString(), in.readString(), in.readString(),
                                  (ArraySet<Integer>) in.readArraySet(null), in.readBundle());
    }
    public static final Parcelable.Creator<HotspotNetwork> CREATOR = new Parcelable.Creator<HotspotNetwork>() {
        @Override public HotspotNetwork createFromParcel(Parcel in) { return readFromParcel(in); }
        @Override public HotspotNetwork[] newArray(int size) { return new HotspotNetwork[size]; }
    };
}
