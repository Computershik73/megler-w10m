package android.content.pm;

/**
 * A user of the phone, as Android's hidden UserInfo: on a Megler phone the one unlocked owner, user 0 (owner()), primary,
 * admin, full and initialized. Android's fields and flags are all here, as the platform's own apps read them: Android's own
 * Settings (AOSP 9, which Megler's package carries) asks a user whether it is the admin, enabled, restricted, its profile group
 * and whether it may be switched to, and ended on NoSuchMethodError where this had only isPrimary, isGuest and
 * isManagedProfile.
 */
public class UserInfo implements android.os.Parcelable {
    public static final int FLAG_MASK_USER_TYPE = 0x0000FFFF;
    public static final int FLAG_PRIMARY = 0x00000001, FLAG_ADMIN = 0x00000002, FLAG_GUEST = 0x00000004,
            FLAG_RESTRICTED = 0x00000008, FLAG_INITIALIZED = 0x00000010, FLAG_MANAGED_PROFILE = 0x00000020,
            FLAG_DISABLED = 0x00000040, FLAG_QUIET_MODE = 0x00000080, FLAG_EPHEMERAL = 0x00000100, FLAG_DEMO = 0x00000200,
            FLAG_FULL = 0x00000400, FLAG_SYSTEM = 0x00000800, FLAG_PROFILE = 0x00001000, FLAG_MAIN = 0x00004000;
    public static final int NO_PROFILE_GROUP_ID = -10000;

    public int id;
    public int serialNumber;
    public String name;
    public String iconPath;
    public int flags;
    public long creationTime;
    public long lastLoggedInTime;
    public String lastLoggedInFingerprint;
    public boolean partial;
    public boolean guestToRemove;
    public int profileGroupId = NO_PROFILE_GROUP_ID;
    public int restrictedProfileParentId = NO_PROFILE_GROUP_ID;
    public int profileBadge;
    public String userType = "android.os.usertype.full.SYSTEM";

    /** The phone's one user, the owner. */
    public static UserInfo owner() {
        UserInfo u = new UserInfo(0, "Owner", FLAG_PRIMARY | FLAG_ADMIN | FLAG_INITIALIZED | FLAG_FULL | FLAG_SYSTEM | FLAG_MAIN);
        u.profileGroupId = 0;
        return u;
    }

    public UserInfo() { this(0); }
    public UserInfo(int id) { this(id, id == 0 ? "Owner" : null, id == 0 ? FLAG_PRIMARY | FLAG_ADMIN | FLAG_INITIALIZED | FLAG_FULL | FLAG_SYSTEM | FLAG_MAIN : FLAG_INITIALIZED | FLAG_FULL); }
    public UserInfo(int id, String name, int flags) { this(id, name, null, flags); }
    public UserInfo(int id, String name, String iconPath, int flags) {
        this.id = id;
        this.name = name;
        this.iconPath = iconPath;
        this.flags = flags;
        this.serialNumber = id;
    }
    public UserInfo(UserInfo o) {
        id = o.id; serialNumber = o.serialNumber; name = o.name; iconPath = o.iconPath; flags = o.flags;
        creationTime = o.creationTime; lastLoggedInTime = o.lastLoggedInTime; lastLoggedInFingerprint = o.lastLoggedInFingerprint;
        partial = o.partial; guestToRemove = o.guestToRemove; profileGroupId = o.profileGroupId;
        restrictedProfileParentId = o.restrictedProfileParentId; profileBadge = o.profileBadge; userType = o.userType;
    }

    public boolean isPrimary() { return (flags & FLAG_PRIMARY) != 0; }
    public boolean isAdmin() { return (flags & FLAG_ADMIN) != 0; }
    public boolean isGuest() { return (flags & FLAG_GUEST) != 0; }
    public boolean isRestricted() { return (flags & FLAG_RESTRICTED) != 0; }
    public boolean isManagedProfile() { return (flags & FLAG_MANAGED_PROFILE) != 0; }
    public boolean isProfile() { return (flags & (FLAG_MANAGED_PROFILE | FLAG_PROFILE)) != 0; }
    public boolean isEnabled() { return (flags & FLAG_DISABLED) == 0; }
    public boolean isQuietModeEnabled() { return (flags & FLAG_QUIET_MODE) != 0; }
    public boolean isEphemeral() { return (flags & FLAG_EPHEMERAL) != 0; }
    public boolean isInitialized() { return (flags & FLAG_INITIALIZED) != 0; }
    public boolean isDemo() { return (flags & FLAG_DEMO) != 0; }
    public boolean isFull() { return (flags & FLAG_FULL) != 0; }
    public boolean isMain() { return (flags & FLAG_MAIN) != 0; }
    public boolean isSystemOnly() { return false; }
    public boolean isCloneProfile() { return false; }
    public boolean isCommunalProfile() { return false; }
    public boolean isPrivateProfile() { return false; }
    public boolean supportsSwitchTo() { return !partial && isEnabled() && !isProfile(); }
    public boolean supportsSwitchToByUser() { return supportsSwitchTo(); }
    public boolean canHaveProfile() { return false; }
    public android.os.UserHandle getUserHandle() { return android.os.UserHandle.of(id); }
    @Override public String toString() { return "UserInfo{" + id + ":" + name + ":" + Integer.toHexString(flags) + "}"; }

    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel out, int parcelFlags) {
        out.writeInt(id); out.writeString(name); out.writeString(iconPath); out.writeInt(flags); out.writeInt(serialNumber);
        out.writeLong(creationTime); out.writeLong(lastLoggedInTime); out.writeString(lastLoggedInFingerprint);
        out.writeInt(partial ? 1 : 0); out.writeInt(guestToRemove ? 1 : 0); out.writeInt(profileGroupId);
        out.writeInt(restrictedProfileParentId); out.writeInt(profileBadge); out.writeString(userType);
    }
    public static final Creator<UserInfo> CREATOR = new Creator<UserInfo>() {
        public UserInfo createFromParcel(android.os.Parcel in) {
            UserInfo u = new UserInfo(in.readInt(), in.readString(), in.readString(), in.readInt());
            u.serialNumber = in.readInt(); u.creationTime = in.readLong(); u.lastLoggedInTime = in.readLong();
            u.lastLoggedInFingerprint = in.readString(); u.partial = in.readInt() != 0; u.guestToRemove = in.readInt() != 0;
            u.profileGroupId = in.readInt(); u.restrictedProfileParentId = in.readInt(); u.profileBadge = in.readInt();
            u.userType = in.readString();
            return u;
        }
        public UserInfo[] newArray(int size) { return new UserInfo[size]; }
    };
}
