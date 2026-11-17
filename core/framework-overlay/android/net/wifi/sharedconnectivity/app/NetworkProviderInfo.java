// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API beside SharedConnectivityService (see there): the device that offers a network -- its kind, its name and
// model, its battery, how strong its connection is. A plain value with Android's Builder and Android's checks.
package android.net.wifi.sharedconnectivity.app;

import android.os.Bundle;
import android.os.Parcel;
import android.os.Parcelable;

import java.util.Objects;

public final class NetworkProviderInfo implements Parcelable {
    public static final int DEVICE_TYPE_UNKNOWN = 0, DEVICE_TYPE_PHONE = 1, DEVICE_TYPE_TABLET = 2, DEVICE_TYPE_LAPTOP = 3, DEVICE_TYPE_WATCH = 4,
                            DEVICE_TYPE_AUTO = 5;

    private final int mDeviceType;
    private final String mDeviceName;
    private final String mModelName;
    private final int mBatteryPercentage;
    private final boolean mIsBatteryCharging;
    private final int mConnectionStrength;
    private final Bundle mExtras;

    public static final class Builder {
        private int mDeviceType;
        private String mDeviceName;
        private String mModelName;
        private int mBatteryPercentage;
        private boolean mIsBatteryCharging;
        private int mConnectionStrength;
        private Bundle mExtras = Bundle.EMPTY;

        public Builder(String deviceName, String modelName) {
            mDeviceName = Objects.requireNonNull(deviceName);
            mModelName = Objects.requireNonNull(modelName);
        }

        public Builder setDeviceType(int deviceType) { mDeviceType = deviceType; return this; }
        public Builder setDeviceName(String deviceName) { mDeviceName = Objects.requireNonNull(deviceName); return this; }
        public Builder setModelName(String modelName) { mModelName = Objects.requireNonNull(modelName); return this; }
        /** 0 to 100. */
        public Builder setBatteryPercentage(int batteryPercentage) { mBatteryPercentage = batteryPercentage; return this; }
        public Builder setBatteryCharging(boolean isBatteryCharging) { mIsBatteryCharging = isBatteryCharging; return this; }
        /** 0 to 4. */
        public Builder setConnectionStrength(int connectionStrength) { mConnectionStrength = connectionStrength; return this; }
        public Builder setExtras(Bundle extras) { mExtras = extras; return this; }
        public NetworkProviderInfo build() {
            return new NetworkProviderInfo(mDeviceType, mDeviceName, mModelName, mBatteryPercentage, mIsBatteryCharging, mConnectionStrength, mExtras);
        }
    }

    private NetworkProviderInfo(int deviceType, String deviceName, String modelName, int batteryPercentage, boolean isBatteryCharging, int connectionStrength,
                                Bundle extras) {
        if (deviceType < DEVICE_TYPE_UNKNOWN || deviceType > DEVICE_TYPE_AUTO) throw new IllegalArgumentException("Illegal device type");
        if (batteryPercentage < 0 || batteryPercentage > 100) throw new IllegalArgumentException("BatteryPercentage must be in range 0-100");
        if (connectionStrength < 0 || connectionStrength > 4) throw new IllegalArgumentException("ConnectionStrength must be in range 0-4");
        mDeviceType = deviceType;
        mDeviceName = deviceName;
        mModelName = modelName;
        mBatteryPercentage = batteryPercentage;
        mIsBatteryCharging = isBatteryCharging;
        mConnectionStrength = connectionStrength;
        mExtras = extras;
    }

    public int getDeviceType() { return mDeviceType; }
    public String getDeviceName() { return mDeviceName; }
    public String getModelName() { return mModelName; }
    public int getBatteryPercentage() { return mBatteryPercentage; }
    public boolean isBatteryCharging() { return mIsBatteryCharging; }
    public int getConnectionStrength() { return mConnectionStrength; }
    public Bundle getExtras() { return mExtras; }

    @Override public boolean equals(Object obj) {
        if (!(obj instanceof NetworkProviderInfo)) return false;
        NetworkProviderInfo other = (NetworkProviderInfo) obj;
        return mDeviceType == other.mDeviceType && Objects.equals(mDeviceName, other.mDeviceName) && Objects.equals(mModelName, other.mModelName)
               && mBatteryPercentage == other.mBatteryPercentage && mIsBatteryCharging == other.mIsBatteryCharging
               && mConnectionStrength == other.mConnectionStrength;
    }
    @Override public int hashCode() { return Objects.hash(mDeviceType, mDeviceName, mModelName, mBatteryPercentage, mIsBatteryCharging, mConnectionStrength); }
    @Override public String toString() {
        return "NetworkProviderInfo[deviceType=" + mDeviceType + ", deviceName=" + mDeviceName + ", modelName=" + mModelName + ", batteryPercentage="
               + mBatteryPercentage + ", isBatteryCharging=" + mIsBatteryCharging + ", connectionStrength=" + mConnectionStrength + ", extras=" + mExtras + "]";
    }

    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mDeviceType);
        dest.writeString(mDeviceName);
        dest.writeString(mModelName);
        dest.writeInt(mBatteryPercentage);
        dest.writeBoolean(mIsBatteryCharging);
        dest.writeInt(mConnectionStrength);
        dest.writeBundle(mExtras);
    }
    /** One written by writeToParcel (hidden in Android; the classes that hold one read it so). */
    public static NetworkProviderInfo readFromParcel(Parcel in) {
        return new NetworkProviderInfo(in.readInt(), in.readString(), in.readString(), in.readInt(), in.readBoolean(), in.readInt(), in.readBundle());
    }
    public static final Parcelable.Creator<NetworkProviderInfo> CREATOR = new Parcelable.Creator<NetworkProviderInfo>() {
        @Override public NetworkProviderInfo createFromParcel(Parcel in) { return readFromParcel(in); }
        @Override public NetworkProviderInfo[] newArray(int size) { return new NetworkProviderInfo[size]; }
    };
}
