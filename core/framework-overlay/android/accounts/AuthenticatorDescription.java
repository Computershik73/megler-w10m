// Overlay of core/framework/gen-src/android/accounts/AuthenticatorDescription.java (tools/build-framework.sh).
// It differs in being Android's: the generated stub's constructors kept nothing, so a description of an authenticator
// (AccountManager.getAuthenticatorTypes: microG's for com.google) had no type, package, label or icon.
package android.accounts;

import android.os.Parcel;
import android.os.Parcelable;

public class AuthenticatorDescription implements Parcelable {
    public final String type;
    public final int labelId;
    public final int iconId;
    public final int smallIconId;
    public final int accountPreferencesId;
    public final String packageName;
    public final boolean customTokens;

    public AuthenticatorDescription(String type, String packageName, int labelId, int iconId, int smallIconId, int prefId, boolean customTokens) {
        if (type == null) throw new IllegalArgumentException("type cannot be null");
        if (packageName == null) throw new IllegalArgumentException("packageName cannot be null");
        this.type = type;
        this.packageName = packageName;
        this.labelId = labelId;
        this.iconId = iconId;
        this.smallIconId = smallIconId;
        this.accountPreferencesId = prefId;
        this.customTokens = customTokens;
    }
    public AuthenticatorDescription(String type, String packageName, int labelId, int iconId, int smallIconId, int prefId) {
        this(type, packageName, labelId, iconId, smallIconId, prefId, false);
    }
    public static AuthenticatorDescription newKey(String type) {
        if (type == null) throw new IllegalArgumentException("type cannot be null");
        return new AuthenticatorDescription(type);
    }
    private AuthenticatorDescription(String type) {
        this.type = type;
        this.packageName = null;
        this.labelId = 0;
        this.iconId = 0;
        this.smallIconId = 0;
        this.accountPreferencesId = 0;
        this.customTokens = false;
    }
    private AuthenticatorDescription(Parcel source) {
        this.type = source.readString();
        this.packageName = source.readString();
        this.labelId = source.readInt();
        this.iconId = source.readInt();
        this.smallIconId = source.readInt();
        this.accountPreferencesId = source.readInt();
        this.customTokens = source.readByte() == 1;
    }
    public int describeContents() { return 0; }
    @Override public int hashCode() { return type.hashCode(); }
    @Override public boolean equals(Object o) {
        if (o == this) return true;
        if (!(o instanceof AuthenticatorDescription)) return false;
        return type.equals(((AuthenticatorDescription) o).type);
    }
    @Override public String toString() { return "AuthenticatorDescription {type=" + type + "}"; }
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(type);
        dest.writeString(packageName);
        dest.writeInt(labelId);
        dest.writeInt(iconId);
        dest.writeInt(smallIconId);
        dest.writeInt(accountPreferencesId);
        dest.writeByte((byte) (customTokens ? 1 : 0));
    }
    public static final Creator<AuthenticatorDescription> CREATOR = new Creator<AuthenticatorDescription>() {
        public AuthenticatorDescription createFromParcel(Parcel source) { return new AuthenticatorDescription(source); }
        public AuthenticatorDescription[] newArray(int size) { return new AuthenticatorDescription[size]; }
    };
}
