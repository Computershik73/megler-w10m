// Overlay of core/framework/gen-src/android/content/SyncStats.java (tools/build-framework.sh): Android's, with a clear that
// clears, the parcel read and written, and its text.
package android.content;

import android.os.Parcel;
import android.os.Parcelable;

public class SyncStats implements Parcelable {
    public long numAuthExceptions;
    public long numIoExceptions;
    public long numParseExceptions;
    public long numConflictDetectedExceptions;
    public long numInserts;
    public long numUpdates;
    public long numDeletes;
    public long numEntries;
    public long numSkippedEntries;

    public SyncStats() {}

    public SyncStats(Parcel in) {
        numAuthExceptions = in.readLong();
        numIoExceptions = in.readLong();
        numParseExceptions = in.readLong();
        numConflictDetectedExceptions = in.readLong();
        numInserts = in.readLong();
        numUpdates = in.readLong();
        numDeletes = in.readLong();
        numEntries = in.readLong();
        numSkippedEntries = in.readLong();
    }

    public void clear() {
        numAuthExceptions = numIoExceptions = numParseExceptions = numConflictDetectedExceptions = 0;
        numInserts = numUpdates = numDeletes = numEntries = numSkippedEntries = 0;
    }

    @Override
    public String toString() {
        StringBuilder sb = new StringBuilder(" stats [");
        if (numAuthExceptions > 0) sb.append(" numAuthExceptions: ").append(numAuthExceptions);
        if (numIoExceptions > 0) sb.append(" numIoExceptions: ").append(numIoExceptions);
        if (numParseExceptions > 0) sb.append(" numParseExceptions: ").append(numParseExceptions);
        if (numConflictDetectedExceptions > 0) sb.append(" numConflictDetectedExceptions: ").append(numConflictDetectedExceptions);
        if (numInserts > 0) sb.append(" numInserts: ").append(numInserts);
        if (numUpdates > 0) sb.append(" numUpdates: ").append(numUpdates);
        if (numDeletes > 0) sb.append(" numDeletes: ").append(numDeletes);
        if (numEntries > 0) sb.append(" numEntries: ").append(numEntries);
        if (numSkippedEntries > 0) sb.append(" numSkippedEntries: ").append(numSkippedEntries);
        return sb.append("]").toString();
    }

    public static final Creator<SyncStats> CREATOR = new Creator<SyncStats>() {
        public SyncStats createFromParcel(Parcel in) { return new SyncStats(in); }
        public SyncStats[] newArray(int size) { return new SyncStats[size]; }
    };

    public int describeContents() { return 0; }

    public void writeToParcel(Parcel dest, int flags) {
        dest.writeLong(numAuthExceptions);
        dest.writeLong(numIoExceptions);
        dest.writeLong(numParseExceptions);
        dest.writeLong(numConflictDetectedExceptions);
        dest.writeLong(numInserts);
        dest.writeLong(numUpdates);
        dest.writeLong(numDeletes);
        dest.writeLong(numEntries);
        dest.writeLong(numSkippedEntries);
    }
}
