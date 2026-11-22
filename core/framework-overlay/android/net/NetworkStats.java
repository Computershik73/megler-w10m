// Overlay of core/framework/gen-src/android/net/NetworkStats.java (tools/build-framework.sh), which was an empty abstract class.
// This is Android's hidden table of network use by interface, uid, set and tag (Android 9's, the parts its own classes use):
// entries added, combined and read back, their totals, a parcel of them. A Megler phone counts no network use, so the tables
// the platform's code makes stay empty or hold what it puts in them.
//
// Why. Android's own Settings (AOSP 9, which Megler's package carries) shows the battery's summary on its main screen through
// Android 9's BatteryStatsImpl, whose constructor makes NetworkStats tables ("new NetworkStats(elapsedRealtime, 50)"): with
// the abstract class that was an InstantiationError, and Settings ended at its start.
package android.net;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.ArrayList;
import java.util.Iterator;

@SuppressWarnings({"unchecked", "rawtypes"})
public class NetworkStats implements Parcelable, Iterable {
    public static final String IFACE_ALL = null;
    public static final int UID_ALL = -1, TAG_ALL = -1, TAG_NONE = 0, SET_ALL = -1, SET_DEFAULT = 0, SET_FOREGROUND = 1,
            SET_DEBUG_START = 1000, SET_DBG_VPN_IN = 1001, SET_DBG_VPN_OUT = 1002, METERED_ALL = -1, METERED_NO = 0,
            METERED_YES = 1, ROAMING_ALL = -1, ROAMING_NO = 0, ROAMING_YES = 1, DEFAULT_NETWORK_ALL = -1,
            DEFAULT_NETWORK_NO = 0, DEFAULT_NETWORK_YES = 1;
    public static final int STATS_PER_IFACE = 0, STATS_PER_UID = 1;

    /** One row: what an interface, uid, set and tag sent and received. */
    public static class Entry {
        public String iface;
        public int uid, set, tag, metered, roaming, defaultNetwork;
        public long rxBytes, rxPackets, txBytes, txPackets, operations;
        public Entry() { this(IFACE_ALL, UID_ALL, SET_DEFAULT, TAG_NONE, 0L, 0L, 0L, 0L, 0L); }
        public Entry(long rxBytes, long rxPackets, long txBytes, long txPackets, long operations) {
            this(IFACE_ALL, UID_ALL, SET_DEFAULT, TAG_NONE, rxBytes, rxPackets, txBytes, txPackets, operations);
        }
        public Entry(String iface, int uid, int set, int tag, long rxBytes, long rxPackets, long txBytes, long txPackets, long operations) {
            this(iface, uid, set, tag, METERED_NO, ROAMING_NO, DEFAULT_NETWORK_NO, rxBytes, rxPackets, txBytes, txPackets, operations);
        }
        public Entry(String iface, int uid, int set, int tag, int metered, int roaming, int defaultNetwork, long rxBytes,
                long rxPackets, long txBytes, long txPackets, long operations) {
            this.iface = iface; this.uid = uid; this.set = set; this.tag = tag; this.metered = metered; this.roaming = roaming;
            this.defaultNetwork = defaultNetwork; this.rxBytes = rxBytes; this.rxPackets = rxPackets; this.txBytes = txBytes;
            this.txPackets = txPackets; this.operations = operations;
        }
        public boolean isNegative() { return rxBytes < 0 || rxPackets < 0 || txBytes < 0 || txPackets < 0 || operations < 0; }
        public boolean isEmpty() { return rxBytes == 0 && rxPackets == 0 && txBytes == 0 && txPackets == 0 && operations == 0; }
        public void add(Entry e) {
            rxBytes += e.rxBytes; rxPackets += e.rxPackets; txBytes += e.txBytes; txPackets += e.txPackets; operations += e.operations;
        }
        Entry copy() { return new Entry(iface, uid, set, tag, metered, roaming, defaultNetwork, rxBytes, rxPackets, txBytes, txPackets, operations); }
        boolean sameKey(Entry e) {
            return uid == e.uid && set == e.set && tag == e.tag && metered == e.metered && roaming == e.roaming
                    && defaultNetwork == e.defaultNetwork && (iface == null ? e.iface == null : iface.equals(e.iface));
        }
        @Override public String toString() {
            return "iface=" + iface + " uid=" + uid + " set=" + set + " tag=" + tag + " rxBytes=" + rxBytes + " txBytes=" + txBytes;
        }
    }

    private long elapsedRealtime;
    private final ArrayList<Entry> entries;

    public NetworkStats(long elapsedRealtime, int initialSize) {
        this.elapsedRealtime = elapsedRealtime;
        entries = new ArrayList<>(Math.max(0, initialSize));
    }
    protected NetworkStats() { this(android.os.SystemClock.elapsedRealtime(), 10); }
    public NetworkStats(Parcel in) {
        this(in.readLong(), 0);
        int n = in.readInt();
        for (int i = 0; i < n; i++) {
            entries.add(new Entry(in.readString(), in.readInt(), in.readInt(), in.readInt(), in.readInt(), in.readInt(), in.readInt(),
                    in.readLong(), in.readLong(), in.readLong(), in.readLong(), in.readLong()));
        }
    }

    public NetworkStats clone() {
        NetworkStats c = new NetworkStats(elapsedRealtime, entries.size());
        for (Entry e : entries) c.entries.add(e.copy());
        return c;
    }
    public void clear() { entries.clear(); }
    public int size() { return entries.size(); }
    public int internalSize() { return entries.size(); }
    public long getElapsedRealtime() { return elapsedRealtime; }
    public void setElapsedRealtime(long t) { elapsedRealtime = t; }
    public long getElapsedRealtimeAge() { return android.os.SystemClock.elapsedRealtime() - elapsedRealtime; }

    public NetworkStats addValues(Entry e) { return addEntry(e); }
    public NetworkStats addEntry(Entry e) { entries.add(e.copy()); return this; }
    public NetworkStats addIfaceValues(String iface, long rxBytes, long rxPackets, long txBytes, long txPackets) {
        return addValues(new Entry(iface, UID_ALL, SET_DEFAULT, TAG_NONE, rxBytes, rxPackets, txBytes, txPackets, 0L));
    }
    public NetworkStats addValues(String iface, int uid, int set, int tag, long rxBytes, long rxPackets, long txBytes, long txPackets, long operations) {
        return addValues(new Entry(iface, uid, set, tag, rxBytes, rxPackets, txBytes, txPackets, operations));
    }
    /** That row's values, in `recycle` when one is given. */
    public Entry getValues(int i, Entry recycle) {
        Entry e = entries.get(i), out = recycle != null ? recycle : new Entry();
        out.iface = e.iface; out.uid = e.uid; out.set = e.set; out.tag = e.tag; out.metered = e.metered; out.roaming = e.roaming;
        out.defaultNetwork = e.defaultNetwork; out.rxBytes = e.rxBytes; out.rxPackets = e.rxPackets; out.txBytes = e.txBytes;
        out.txPackets = e.txPackets; out.operations = e.operations;
        return out;
    }
    public int findIndex(String iface, int uid, int set, int tag, int metered, int roaming, int defaultNetwork) {
        Entry key = new Entry(iface, uid, set, tag, metered, roaming, defaultNetwork, 0, 0, 0, 0, 0);
        for (int i = 0; i < entries.size(); i++) if (entries.get(i).sameKey(key)) return i;
        return -1;
    }
    /** The row of the same key added to, or a new one. */
    public NetworkStats combineValues(Entry e) {
        for (Entry mine : entries) if (mine.sameKey(e)) { mine.add(e); return this; }
        return addEntry(e);
    }
    public NetworkStats combineValues(String iface, int uid, int set, int tag, long rxBytes, long rxPackets, long txBytes, long txPackets, long operations) {
        return combineValues(new Entry(iface, uid, set, tag, rxBytes, rxPackets, txBytes, txPackets, operations));
    }
    public void combineAllValues(NetworkStats another) { for (Entry e : another.entries) combineValues(e); }
    public long getTotalBytes() { Entry t = getTotal(null); return t.rxBytes + t.txBytes; }
    public long getTotalPackets() { Entry t = getTotal(null); return t.rxPackets + t.txPackets; }
    public Entry getTotal(Entry recycle) {
        Entry t = recycle != null ? recycle : new Entry();
        t.iface = IFACE_ALL; t.uid = UID_ALL; t.set = SET_ALL; t.tag = TAG_NONE;
        t.rxBytes = t.rxPackets = t.txBytes = t.txPackets = t.operations = 0;
        for (Entry e : entries) if (e.tag == TAG_NONE) t.add(e);
        return t;
    }
    public String[] getUniqueIfaces() {
        java.util.LinkedHashSet<String> s = new java.util.LinkedHashSet<>();
        for (Entry e : entries) if (e.iface != null) s.add(e.iface);
        return s.toArray(new String[0]);
    }
    public int[] getUniqueUids() {
        java.util.TreeSet<Integer> s = new java.util.TreeSet<>();
        for (Entry e : entries) s.add(e.uid);
        int[] out = new int[s.size()];
        int i = 0;
        for (int u : s) out[i++] = u;
        return out;
    }
    /** `left` less `right`, row by row of the same key (rows of `left` alone kept as they are). */
    public static NetworkStats subtract(NetworkStats left, NetworkStats right, Object observer, Object cookie) {
        return subtract(left, right, observer, cookie, null);
    }
    public static NetworkStats subtract(NetworkStats left, NetworkStats right, Object observer, Object cookie, NetworkStats recycle) {
        NetworkStats out = recycle != null ? recycle : new NetworkStats(left.elapsedRealtime, left.size());
        out.entries.clear();
        out.elapsedRealtime = left.elapsedRealtime - right.elapsedRealtime;
        for (Entry l : left.entries) {
            Entry d = l.copy();
            for (Entry r : right.entries) {
                if (r.sameKey(l)) {
                    d.rxBytes -= r.rxBytes; d.rxPackets -= r.rxPackets; d.txBytes -= r.txBytes; d.txPackets -= r.txPackets; d.operations -= r.operations;
                    break;
                }
            }
            out.entries.add(d);
        }
        return out;
    }
    public NetworkStats subtract(NetworkStats right) { return subtract(this, right, null, null); }
    public NetworkStats groupedByIface() {
        NetworkStats out = new NetworkStats(elapsedRealtime, 10);
        for (Entry e : entries) {
            if (e.tag != TAG_NONE) continue;
            out.combineValues(new Entry(e.iface, UID_ALL, SET_ALL, TAG_NONE, e.rxBytes, e.rxPackets, e.txBytes, e.txPackets, e.operations));
        }
        return out;
    }
    public NetworkStats groupedByUid() {
        NetworkStats out = new NetworkStats(elapsedRealtime, 10);
        for (Entry e : entries) {
            if (e.tag != TAG_NONE) continue;
            out.combineValues(new Entry(IFACE_ALL, e.uid, SET_ALL, TAG_NONE, e.rxBytes, e.rxPackets, e.txBytes, e.txPackets, e.operations));
        }
        return out;
    }
    public Iterator iterator() { return new ArrayList<>(entries).iterator(); }

    public int describeContents() { return 0; }
    public void writeToParcel(Parcel out, int flags) {
        out.writeLong(elapsedRealtime);
        out.writeInt(entries.size());
        for (Entry e : entries) {
            out.writeString(e.iface); out.writeInt(e.uid); out.writeInt(e.set); out.writeInt(e.tag); out.writeInt(e.metered);
            out.writeInt(e.roaming); out.writeInt(e.defaultNetwork); out.writeLong(e.rxBytes); out.writeLong(e.rxPackets);
            out.writeLong(e.txBytes); out.writeLong(e.txPackets); out.writeLong(e.operations);
        }
    }
    public static final Creator<NetworkStats> CREATOR = new Creator<NetworkStats>() {
        public NetworkStats createFromParcel(Parcel in) { return new NetworkStats(in); }
        public NetworkStats[] newArray(int size) { return new NetworkStats[size]; }
    };
}
