// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// VolumeInfo: one volume as the storage daemon knows it, which StorageManager.getVolumes lists. Files by Google reads the
// list by reflection -- the fields path, fsUuid, disk and state, and getMountUserId -- to find an SD card and a USB drive;
// the list was a generated empty one of nothing and android.os.storage.DiskInfo was missing, so each look ended in
// "Unable to obtain StorageManager volumes" and "Error in getting USB volume!".
package android.os.storage;

import android.os.Parcel;
import android.os.Parcelable;
import android.os.UserHandle;
import java.io.File;

/**
 * A storage volume as vold describes it (hidden API): fields that anyone reads, as Android's are. StorageManager.getVolumes
 * makes the two a phone without a removable medium has, the private one at /data and the emulated one whose folder of a
 * user is the shared storage StorageVolume describes.
 */
public class VolumeInfo implements Parcelable {
    public static final String ACTION_VOLUME_STATE_CHANGED = "android.os.storage.action.VOLUME_STATE_CHANGED";
    public static final String EXTRA_VOLUME_ID = "android.os.storage.extra.VOLUME_ID";
    public static final String EXTRA_VOLUME_STATE = "android.os.storage.extra.VOLUME_STATE";

    /** The internal private storage (/data). */
    public static final String ID_PRIVATE_INTERNAL = "private";
    /** The internal emulated storage: its volume is "emulated;<user>". */
    public static final String ID_EMULATED_INTERNAL = "emulated";

    public static final int TYPE_PUBLIC = 0, TYPE_PRIVATE = 1, TYPE_EMULATED = 2, TYPE_ASEC = 3, TYPE_OBB = 4, TYPE_STUB = 5;

    public static final int STATE_UNMOUNTED = 0, STATE_CHECKING = 1, STATE_MOUNTED = 2, STATE_MOUNTED_READ_ONLY = 3, STATE_FORMATTING = 4,
                            STATE_EJECTING = 5, STATE_UNMOUNTABLE = 6, STATE_REMOVED = 7, STATE_BAD_REMOVAL = 8;

    public static final int MOUNT_FLAG_PRIMARY = 1 << 0;
    public static final int MOUNT_FLAG_VISIBLE_FOR_READ = 1 << 1;
    public static final int MOUNT_FLAG_VISIBLE_FOR_WRITE = 1 << 2;

    public final String id;
    public final int type;
    /** The medium the volume is on; null for the phone's own storage. */
    public final DiskInfo disk;
    public final String partGuid;
    public int mountFlags = 0;
    public int mountUserId = UserHandle.USER_NULL;
    public int state = STATE_UNMOUNTED;
    public String fsType;
    public String fsUuid;
    public String fsLabel;
    public String path;
    public String internalPath;

    public VolumeInfo(String id, int type, DiskInfo disk, String partGuid) {
        if (id == null) throw new NullPointerException();
        this.id = id;
        this.type = type;
        this.disk = disk;
        this.partGuid = partGuid;
    }
    public VolumeInfo(VolumeInfo v) {
        this(v.id, v.type, v.disk, v.partGuid);
        mountFlags = v.mountFlags;
        mountUserId = v.mountUserId;
        state = v.state;
        fsType = v.fsType;
        fsUuid = v.fsUuid;
        fsLabel = v.fsLabel;
        path = v.path;
        internalPath = v.internalPath;
    }
    public VolumeInfo(Parcel parcel) {
        id = parcel.readString();
        type = parcel.readInt();
        disk = parcel.readInt() != 0 ? DiskInfo.CREATOR.createFromParcel(parcel) : null;
        partGuid = parcel.readString();
        mountFlags = parcel.readInt();
        mountUserId = parcel.readInt();
        state = parcel.readInt();
        fsType = parcel.readString();
        fsUuid = parcel.readString();
        fsLabel = parcel.readString();
        path = parcel.readString();
        internalPath = parcel.readString();
    }

    /** Environment's word for a state ("mounted", "removed", ...), "unknown" for a number that is no state. */
    public static String getEnvironmentForState(int state) {
        switch (state) {
            case STATE_UNMOUNTED: case STATE_FORMATTING: return "unmounted";
            case STATE_CHECKING: return "checking";
            case STATE_MOUNTED: return "mounted";
            case STATE_MOUNTED_READ_ONLY: return "mounted_ro";
            case STATE_EJECTING: return "ejecting";
            case STATE_UNMOUNTABLE: return "unmountable";
            case STATE_REMOVED: return "removed";
            case STATE_BAD_REMOVAL: return "bad_removal";
            default: return "unknown";
        }
    }

    public String getId() { return id; }
    public DiskInfo getDisk() { return disk; }
    public String getDiskId() { return disk != null ? disk.id : null; }
    public int getType() { return type; }
    public int getState() { return state; }
    public String getFsUuid() { return fsUuid; }
    public String getNormalizedFsUuid() { return fsUuid != null ? fsUuid.toLowerCase(java.util.Locale.US) : null; }
    public int getMountUserId() { return mountUserId; }

    /** "Internal storage" for the phone's own two, the label a file system has otherwise, else null. */
    public String getDescription() {
        if (ID_PRIVATE_INTERNAL.equals(id) || id.startsWith(ID_EMULATED_INTERNAL)) return "Internal storage";
        if (fsLabel != null && !fsLabel.trim().isEmpty()) return fsLabel;
        return null;
    }

    public boolean isMountedReadable() { return state == STATE_MOUNTED || state == STATE_MOUNTED_READ_ONLY; }
    public boolean isMountedWritable() { return state == STATE_MOUNTED; }
    public boolean isPrimary() { return (mountFlags & MOUNT_FLAG_PRIMARY) != 0; }
    public boolean isPrimaryPhysical() { return isPrimary() && type == TYPE_PUBLIC; }
    public boolean isVisible() { return (mountFlags & (MOUNT_FLAG_VISIBLE_FOR_READ | MOUNT_FLAG_VISIBLE_FOR_WRITE)) != 0; }
    public boolean isVisibleForUser(int userId) {
        return (type == TYPE_PUBLIC || type == TYPE_STUB || type == TYPE_EMULATED) && mountUserId == userId && isVisible();
    }

    public File getPath() { return path != null ? new File(path) : null; }
    public File getInternalPath() { return internalPath != null ? new File(internalPath) : null; }
    /** Where a user's files of the volume are: an emulated volume has a folder for each user, a public one is whole. */
    public File getPathForUser(int userId) {
        if (path == null) return null;
        if (type == TYPE_PUBLIC || type == TYPE_STUB) return new File(path);
        if (type == TYPE_EMULATED) return new File(path, Integer.toString(userId));
        return null;
    }
    public File getInternalPathForUser(int userId) {
        if (path == null) return null;
        if (type == TYPE_PUBLIC || type == TYPE_STUB) return new File(path.replace("/storage/", "/mnt/media_rw/"));
        return getPathForUser(userId);
    }

    @Override public String toString() {
        return "VolumeInfo{" + id + "}: type=" + type + " diskId=" + getDiskId() + " mountFlags=" + mountFlags + " mountUserId=" + mountUserId
               + " state=" + state + " fsUuid=" + fsUuid + " path=" + path + " internalPath=" + internalPath;
    }
    @Override public VolumeInfo clone() { return new VolumeInfo(this); }
    @Override public boolean equals(Object o) { return o instanceof VolumeInfo && id.equals(((VolumeInfo) o).id); }
    @Override public int hashCode() { return id.hashCode(); }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel parcel, int flags) {
        parcel.writeString(id);
        parcel.writeInt(type);
        if (disk != null) {
            parcel.writeInt(1);
            disk.writeToParcel(parcel, flags);
        } else {
            parcel.writeInt(0);
        }
        parcel.writeString(partGuid);
        parcel.writeInt(mountFlags);
        parcel.writeInt(mountUserId);
        parcel.writeInt(state);
        parcel.writeString(fsType);
        parcel.writeString(fsUuid);
        parcel.writeString(fsLabel);
        parcel.writeString(path);
        parcel.writeString(internalPath);
    }
    public static final Parcelable.Creator<VolumeInfo> CREATOR = new Parcelable.Creator<VolumeInfo>() {
        @Override public VolumeInfo createFromParcel(Parcel in) { return new VolumeInfo(in); }
        @Override public VolumeInfo[] newArray(int size) { return new VolumeInfo[size]; }
    };
}
