// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// DiskInfo: a physical medium the storage daemon knows of -- an SD card, a USB drive -- that the volumes on it name
// (VolumeInfo.disk). Files by Google looks the class up by name and asks each volume's disk isAdoptable, isSd and isUsb to
// find a card or a drive; without the class it had ClassNotFoundException and logged that it could not list the volumes.
package android.os.storage;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * A removable medium (hidden API). Megler hands none out: the volumes StorageManager.getVolumes lists are the phone's own
 * storage, which is on no disk, and StorageManager.getDisks is empty. The class says of one what Android's says.
 */
public class DiskInfo implements Parcelable {
    public static final String ACTION_DISK_SCANNED = "android.os.storage.action.DISK_SCANNED";
    public static final String EXTRA_DISK_ID = "android.os.storage.extra.DISK_ID";
    public static final String EXTRA_VOLUME_COUNT = "android.os.storage.extra.VOLUME_COUNT";

    public static final int FLAG_ADOPTABLE = 1 << 0;
    public static final int FLAG_DEFAULT_PRIMARY = 1 << 1;
    public static final int FLAG_SD = 1 << 2;
    public static final int FLAG_USB = 1 << 3;
    public static final int FLAG_STUB_VISIBLE = 1 << 6;

    public final String id;
    public final int flags;
    public long size;
    public String label;
    /** How many volumes the last scan of the disk found. */
    public int volumeCount;
    public String sysPath;

    public DiskInfo(String id, int flags) {
        if (id == null) throw new NullPointerException();
        this.id = id;
        this.flags = flags;
    }
    public DiskInfo(Parcel parcel) {
        id = parcel.readString();
        flags = parcel.readInt();
        size = parcel.readLong();
        label = parcel.readString();
        volumeCount = parcel.readInt();
        sysPath = parcel.readString();
    }

    public String getId() { return id; }

    /** The medium's kind with its label where it has one ("SD card", "Kingston USB drive"); null for a disk of neither kind. */
    public String getDescription() {
        String name = label == null || label.trim().isEmpty() ? null : label.trim();
        if ((flags & FLAG_SD) != 0) return name != null ? name + " SD card" : "SD card";
        if ((flags & FLAG_USB) != 0) return name != null ? name + " USB drive" : "USB drive";
        return null;
    }
    public String getShortDescription() {
        if ((flags & FLAG_SD) != 0) return "SD card";
        if ((flags & FLAG_USB) != 0) return "USB drive";
        return null;
    }

    public boolean isAdoptable() { return (flags & FLAG_ADOPTABLE) != 0; }
    public boolean isDefaultPrimary() { return (flags & FLAG_DEFAULT_PRIMARY) != 0; }
    public boolean isSd() { return (flags & FLAG_SD) != 0; }
    public boolean isUsb() { return (flags & FLAG_USB) != 0; }
    public boolean isStubVisible() { return (flags & FLAG_STUB_VISIBLE) != 0; }

    @Override public String toString() { return "DiskInfo{" + id + "}: flags=" + flags + " size=" + size + " label=" + label + " volumeCount=" + volumeCount + " sysPath=" + sysPath; }
    @Override public DiskInfo clone() {
        DiskInfo d = new DiskInfo(id, flags);
        d.size = size;
        d.label = label;
        d.volumeCount = volumeCount;
        d.sysPath = sysPath;
        return d;
    }
    @Override public boolean equals(Object o) { return o instanceof DiskInfo && id.equals(((DiskInfo) o).id); }
    @Override public int hashCode() { return id.hashCode(); }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel parcel, int parcelFlags) {
        parcel.writeString(id);
        parcel.writeInt(flags);
        parcel.writeLong(size);
        parcel.writeString(label);
        parcel.writeInt(volumeCount);
        parcel.writeString(sysPath);
    }
    public static final Parcelable.Creator<DiskInfo> CREATOR = new Parcelable.Creator<DiskInfo>() {
        @Override public DiskInfo createFromParcel(Parcel in) { return new DiskInfo(in); }
        @Override public DiskInfo[] newArray(int size) { return new DiskInfo[size]; }
    };
}
