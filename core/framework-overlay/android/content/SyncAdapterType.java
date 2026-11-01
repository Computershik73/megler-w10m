// Overlay of core/framework/gen-src/android/content/SyncAdapterType.java (tools/build-framework.sh): Android's, whose
// constructors keep what they are given (the stub's kept nothing: ContentResolver.getSyncAdapterTypes said nothing true).
package android.content;

import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;

public class SyncAdapterType implements Parcelable {
    public final String authority;
    public final String accountType;
    public final boolean isKey;
    private final boolean userVisible, supportsUploading, isAlwaysSyncable, allowParallelSyncs;
    private final String settingsActivity, packageName;

    public SyncAdapterType(String authority, String accountType, boolean userVisible, boolean supportsUploading) {
        this(authority, accountType, userVisible, supportsUploading, false, false, null, null);
    }

    public SyncAdapterType(String authority, String accountType, boolean userVisible, boolean supportsUploading, boolean isAlwaysSyncable,
                           boolean allowParallelSyncs, String settingsActivity, String packageName) {
        if (TextUtils.isEmpty(authority)) throw new IllegalArgumentException("the authority must not be empty: " + authority);
        if (TextUtils.isEmpty(accountType)) throw new IllegalArgumentException("the accountType must not be empty: " + accountType);
        this.authority = authority;
        this.accountType = accountType;
        this.userVisible = userVisible;
        this.supportsUploading = supportsUploading;
        this.isAlwaysSyncable = isAlwaysSyncable;
        this.allowParallelSyncs = allowParallelSyncs;
        this.settingsActivity = settingsActivity;
        this.packageName = packageName;
        this.isKey = false;
    }

    private SyncAdapterType(String authority, String accountType) {
        this.authority = authority;
        this.accountType = accountType;
        this.userVisible = true;
        this.supportsUploading = true;
        this.isAlwaysSyncable = false;
        this.allowParallelSyncs = false;
        this.settingsActivity = null;
        this.packageName = null;
        this.isKey = true;
    }

    public SyncAdapterType(Parcel source) {
        this(source.readString(), source.readString(), source.readInt() != 0, source.readInt() != 0, source.readInt() != 0, source.readInt() != 0,
             source.readString(), source.readString());
    }

    public static SyncAdapterType newKey(String authority, String accountType) { return new SyncAdapterType(authority, accountType); }

    public boolean supportsUploading() {
        if (isKey) throw new IllegalStateException("this method is not allowed to be called when this is a key");
        return supportsUploading;
    }

    public boolean isUserVisible() {
        if (isKey) throw new IllegalStateException("this method is not allowed to be called when this is a key");
        return userVisible;
    }

    public boolean allowParallelSyncs() {
        if (isKey) throw new IllegalStateException("this method is not allowed to be called when this is a key");
        return allowParallelSyncs;
    }

    public boolean isAlwaysSyncable() {
        if (isKey) throw new IllegalStateException("this method is not allowed to be called when this is a key");
        return isAlwaysSyncable;
    }

    public String getSettingsActivity() {
        if (isKey) throw new IllegalStateException("this method is not allowed to be called when this is a key");
        return settingsActivity;
    }

    public String getPackageName() { return packageName; }

    @Override
    public boolean equals(Object o) {
        if (o == this) return true;
        if (!(o instanceof SyncAdapterType)) return false;
        final SyncAdapterType other = (SyncAdapterType) o;
        return authority.equals(other.authority) && accountType.equals(other.accountType);
    }

    @Override
    public int hashCode() { return 31 * (527 + authority.hashCode()) + accountType.hashCode(); }

    @Override
    public String toString() {
        if (isKey) return "SyncAdapterType Key {name=" + authority + ", type=" + accountType + "}";
        return "SyncAdapterType {name=" + authority + ", type=" + accountType + ", userVisible=" + userVisible + ", supportsUploading=" + supportsUploading
               + ", isAlwaysSyncable=" + isAlwaysSyncable + ", allowParallelSyncs=" + allowParallelSyncs + ", settingsActivity=" + settingsActivity
               + ", packageName=" + packageName + "}";
    }

    public int describeContents() { return 0; }

    public void writeToParcel(Parcel dest, int flags) {
        if (isKey) throw new IllegalStateException("keys aren't parcelable");
        dest.writeString(authority);
        dest.writeString(accountType);
        dest.writeInt(userVisible ? 1 : 0);
        dest.writeInt(supportsUploading ? 1 : 0);
        dest.writeInt(isAlwaysSyncable ? 1 : 0);
        dest.writeInt(allowParallelSyncs ? 1 : 0);
        dest.writeString(settingsActivity);
        dest.writeString(packageName);
    }

    public static final Creator<SyncAdapterType> CREATOR = new Creator<SyncAdapterType>() {
        public SyncAdapterType createFromParcel(Parcel source) { return new SyncAdapterType(source); }
        public SyncAdapterType[] newArray(int size) { return new SyncAdapterType[size]; }
    };
}
