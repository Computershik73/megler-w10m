package megler.test.crossapp;

import android.os.Parcel;
import android.os.Parcelable;

/** A Parcelable this app alone has: the app it is sent to cannot read it, and gets the rest of the Intent without it. */
public final class Only implements Parcelable {
    public final int n;

    public Only(int n) { this.n = n; }

    @Override
    public int describeContents() { return 0; }

    @Override
    public void writeToParcel(Parcel p, int flags) { p.writeInt(n); }

    public static final Creator<Only> CREATOR = new Creator<Only>() {
        @Override
        public Only createFromParcel(Parcel p) { return new Only(p.readInt()); }

        @Override
        public Only[] newArray(int n) { return new Only[n]; }
    };
}
