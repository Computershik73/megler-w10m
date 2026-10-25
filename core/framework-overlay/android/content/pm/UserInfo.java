package android.content.pm;

/** The one unlocked owner profile on a Megler phone. */
public class UserInfo implements android.os.Parcelable {
    public int id;
    public String name = "Owner";
    public int flags;
    public UserInfo() { this(0); }
    public UserInfo(int id) { this.id = id; }
    public boolean isManagedProfile() { return false; }
    public boolean isGuest() { return false; }
    public boolean isPrimary() { return true; }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel out, int parcelFlags) { out.writeInt(id); out.writeString(name); out.writeInt(flags); }
    public static final Creator<UserInfo> CREATOR = new Creator<UserInfo>() {
        public UserInfo createFromParcel(android.os.Parcel in) { UserInfo u = new UserInfo(in.readInt()); u.name = in.readString(); u.flags = in.readInt(); return u; }
        public UserInfo[] newArray(int size) { return new UserInfo[size]; }
    };
}
