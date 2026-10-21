package com.google.android.gms.common;

import android.os.Parcel;
import android.os.Parcelable;

/**
 * The client library's Feature, as an app has its own copy of it: a class of the same name as microG's, written in the
 * SafeParcelable layout both read (field 1 the name, field 3 the version). microG's broker must read an array of these as
 * its own Feature objects.
 */
public class Feature implements Parcelable {
    public final String name;
    public final long version;
    public Feature(String name, long version) { this.name = name; this.version = version; }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(Parcel p, int flags) {
        p.writeInt(0xffff4f45);
        int sizeAt = p.dataPosition();
        p.writeInt(0);
        int start = p.dataPosition();
        p.writeInt(0xffff0001);
        int at = p.dataPosition();
        p.writeInt(0);
        int from = p.dataPosition();
        p.writeString(name);
        int to = p.dataPosition();
        p.setDataPosition(at); p.writeInt(to - from); p.setDataPosition(to);
        p.writeInt(0x00080003);
        p.writeLong(version);
        int end = p.dataPosition();
        p.setDataPosition(sizeAt); p.writeInt(end - start); p.setDataPosition(end);
    }
    public static final Creator<Feature> CREATOR = new Creator<Feature>() {
        public Feature createFromParcel(Parcel p) { throw new UnsupportedOperationException("the test writes Features only"); }
        public Feature[] newArray(int n) { return new Feature[n]; }
    };
}
