// Overlay of core/framework/src/android/accounts/Account.java (tools/build-framework.sh).
// It differs in being Parcelable in fact, as Android's: the original's CREATOR was null and writeToParcel missing, so an
// account in a Bundle that was parcelled (an intent's extras, a saved state, an authenticator's answer) could not be read
// back. And it says what it is (toString), checks its name and type, and has the access id Android 9 has.
package android.accounts;

import android.os.Parcel;
import android.os.Parcelable;
import android.text.TextUtils;

public class Account implements Parcelable {
    public final String name, type;
    private final String accessId;

    public Account(String name, String type) { this(name, type, null); }
    /** @hide */
    public Account(Account other, String accessId) { this(other.name, other.type, accessId); }
    private Account(String name, String type, String accessId) {
        if (TextUtils.isEmpty(name)) throw new IllegalArgumentException("the name must not be empty: " + name);
        if (TextUtils.isEmpty(type)) throw new IllegalArgumentException("the type must not be empty: " + type);
        this.name = name;
        this.type = type;
        this.accessId = accessId;
    }
    public Account(Parcel in) {
        this.name = in.readString();
        this.type = in.readString();
        this.accessId = in.readString();
    }
    public String getAccessId() { return accessId; }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeString(name);
        dest.writeString(type);
        dest.writeString(accessId);
    }
    @Override public boolean equals(Object o) { return o instanceof Account && name.equals(((Account) o).name) && type.equals(((Account) o).type); }
    @Override public int hashCode() { return name.hashCode() * 31 + type.hashCode(); }
    @Override public String toString() { return "Account {name=" + name + ", type=" + type + "}"; }
    public String toSafeString() { return "Account {name=" + toSafeName(name, 'x') + ", type=" + type + "}"; }
    public static String toSafeName(String name, char replacement) {
        StringBuilder builder = new StringBuilder(64);
        int len = name.length();
        for (int i = 0; i < len; i++) {
            char c = name.charAt(i);
            builder.append(Character.isLetterOrDigit(c) ? replacement : c);
        }
        return builder.toString();
    }
    public static final Creator<Account> CREATOR = new Creator<Account>() {
        public Account createFromParcel(Parcel source) { return new Account(source); }
        public Account[] newArray(int size) { return new Account[size]; }
    };
}
