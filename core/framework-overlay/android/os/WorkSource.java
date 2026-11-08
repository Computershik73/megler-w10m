// Overlay of core/framework/gen-src/android/os/WorkSource.java (tools/build-framework.sh).
// It differs in being Android's: the generated stub kept nothing (no uids, no names, no work chains), wrote nothing into a
// Parcel and had a null CREATOR. This is Android 14's WorkSource -- its uids and package names, kept sorted, its work chains,
// set, add, remove, diff and their kin, equals and hashCode -- and it goes into a Parcel and comes out of one in Android's
// layout (writeToParcel, CREATOR): the count, the uids, the names, then the chains or -1.
//
// Why. Google Play services' location requests carry one. The Play services client library in the app writes the
// request's WorkSource into the transaction with its writeToParcel (SafeParcelWriter: a header, then what writeToParcel
// wrote, its size patched in), and Google Play services' location service reads it back with the CREATOR, giving null for a
// field that wrote nothing. Every FusedLocationProviderClient.requestLocationUpdates and getCurrentLocation of an app then
// ended in Google Play services with a NullPointerException in ImmutableWorkSource's constructor
// (RegisterLocationCallback, RegisterLocationListener: Transit, and the Google Maps "my location" layer), and the app never
// had a fix while Megler's own LocationManager had one (0.1.0.671).
package android.os;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

@SuppressWarnings({"unchecked", "rawtypes"})
public class WorkSource implements Parcelable {
    int mNum;
    int[] mUids = new int[0];
    String[] mNames;
    private ArrayList<WorkChain> mChains;

    static final WorkSource sTmpWorkSource = new WorkSource(0);
    static WorkSource sNewbWork;
    static WorkSource sGoneWork;

    public WorkSource() {
        mNum = 0;
        mChains = null;
    }

    public WorkSource(WorkSource orig) {
        if (orig == null) {
            mNum = 0;
            mChains = null;
            return;
        }
        mNum = orig.mNum;
        mUids = orig.mUids != null ? orig.mUids.clone() : new int[0];
        mNames = orig.mNames != null ? orig.mNames.clone() : null;
        if (orig.mChains != null) {
            mChains = new ArrayList<>(orig.mChains.size());
            for (WorkChain chain : orig.mChains) mChains.add(new WorkChain(chain));
        } else {
            mChains = null;
        }
    }

    public WorkSource(int uid) {
        mNum = 1;
        mUids = new int[] { uid, 0 };
        mNames = null;
        mChains = null;
    }

    public WorkSource(int uid, String packageName) {
        if (packageName == null) throw new NullPointerException("packageName can't be null");
        mNum = 1;
        mUids = new int[] { uid, 0 };
        mNames = new String[] { packageName, null };
        mChains = null;
    }

    WorkSource(Parcel in) {
        mNum = in.readInt();
        int[] uids = in.createIntArray();
        mUids = uids != null ? uids : new int[0];
        mNames = in.createStringArray();
        int numChains = in.readInt();
        if (numChains >= 0) {
            mChains = new ArrayList<>(numChains);
            in.readParcelableList(mChains, WorkChain.class.getClassLoader(), WorkChain.class);
        } else {
            mChains = null;
        }
    }

    /** Whether the global setting chained_battery_attribution_enabled is 1; it is not set here, as on most phones. */
    public static boolean isChainedBatteryAttributionEnabled(android.content.Context context) {
        try {
            return android.provider.Settings.Global.getInt(context.getContentResolver(), "chained_battery_attribution_enabled", 0) == 1;
        } catch (Exception e) {
            return false;
        }
    }
    public static boolean isChainedBatteryAttributionEnabled$ravenwood(android.content.Context context) { return false; }

    public int size() { return mNum; }
    @Deprecated
    public int get(int index) { return getUid(index); }
    public int getUid(int index) { return mUids[index]; }
    public int getAttributionUid() {
        if (isEmpty()) return -1;
        return mNum > 0 ? mUids[0] : mChains.get(0).getAttributionUid();
    }
    @Deprecated
    public String getName(int index) { return getPackageName(index); }
    public String getPackageName(int index) { return mNames != null ? mNames[index] : null; }

    /** The names dropped, and with them the uids that only they told apart. */
    public void clearNames() {
        if (mNames != null) {
            mNames = null;
            int destIndex = 1;
            int newNum = mNum;
            for (int sourceIndex = 1; sourceIndex < mNum; sourceIndex++) {
                if (mUids[sourceIndex] == mUids[sourceIndex - 1]) {
                    newNum--;
                } else {
                    mUids[destIndex] = mUids[sourceIndex];
                    destIndex++;
                }
            }
            mNum = newNum;
        }
    }

    public void clear() {
        mNum = 0;
        if (mChains != null) mChains.clear();
    }

    @Override
    public boolean equals(Object o) {
        if (o instanceof WorkSource) {
            WorkSource other = (WorkSource) o;
            if (diff(other)) return false;
            if (mChains != null && !mChains.isEmpty()) return mChains.equals(other.mChains);
            return other.mChains == null || other.mChains.isEmpty();
        }
        return false;
    }

    @Override
    public int hashCode() {
        int result = 0;
        for (int i = 0; i < mNum; i++) result = ((result << 4) | (result >>> 28)) ^ mUids[i];
        if (mNames != null) {
            for (int i = 0; i < mNum; i++) result = ((result << 4) | (result >>> 28)) ^ mNames[i].hashCode();
        }
        if (mChains != null) result = ((result << 4) | (result >>> 28)) ^ mChains.hashCode();
        return result;
    }

    /** Whether the uids and names differ (the work chains are not compared). */
    public boolean diff(WorkSource other) {
        int n = mNum;
        if (n != other.mNum) return true;
        final int[] uids1 = mUids, uids2 = other.mUids;
        final String[] names1 = mNames, names2 = other.mNames;
        for (int i = 0; i < n; i++) {
            if (uids1[i] != uids2[i]) return true;
            if (names1 != null && names2 != null && !names1[i].equals(names2[i])) return true;
        }
        return false;
    }

    public void set(WorkSource other) {
        if (other == null) {
            clear();
            return;
        }
        mNum = other.mNum;
        if (other.mUids != null) {
            if (mUids != null && mUids.length >= mNum) System.arraycopy(other.mUids, 0, mUids, 0, mNum);
            else mUids = other.mUids.clone();
            if (other.mNames != null) {
                if (mNames != null && mNames.length >= mNum) System.arraycopy(other.mNames, 0, mNames, 0, mNum);
                else mNames = other.mNames.clone();
            } else {
                mNames = null;
            }
        } else {
            mUids = null;
            mNames = null;
        }
        if (other.mChains != null) {
            if (mChains != null) mChains.clear();
            else mChains = new ArrayList<>(other.mChains.size());
            for (WorkChain chain : other.mChains) mChains.add(new WorkChain(chain));
        }
    }

    public void set(int uid) {
        mNum = 1;
        if (mUids == null || mUids.length == 0) mUids = new int[2];
        mUids[0] = uid;
        mNames = null;
        if (mChains != null) mChains.clear();
    }

    public void set(int uid, String name) {
        if (name == null) throw new NullPointerException("Name can't be null");
        mNum = 1;
        if (mUids == null || mUids.length == 0) mUids = new int[2];
        if (mNames == null || mNames.length == 0) mNames = new String[2];
        mUids[0] = uid;
        mNames[0] = name;
        if (mChains != null) mChains.clear();
    }

    @Deprecated
    public WorkSource[] setReturningDiffs(WorkSource other) {
        synchronized (sTmpWorkSource) {
            sNewbWork = null;
            sGoneWork = null;
            updateLocked(other, true, true);
            if (sNewbWork != null || sGoneWork != null) return new WorkSource[] { sNewbWork, sGoneWork };
            return null;
        }
    }

    public boolean add(WorkSource other) {
        synchronized (sTmpWorkSource) {
            boolean uidAdded = updateLocked(other, false, false);
            boolean chainAdded = false;
            if (other.mChains != null) {
                if (mChains == null) mChains = new ArrayList<>(other.mChains.size());
                for (WorkChain wc : other.mChains) {
                    if (!mChains.contains(wc)) {
                        mChains.add(new WorkChain(wc));
                        chainAdded = true;
                    }
                }
            }
            return uidAdded || chainAdded;
        }
    }

    @Deprecated
    public WorkSource addReturningNewbs(WorkSource other) {
        synchronized (sTmpWorkSource) {
            sNewbWork = null;
            updateLocked(other, false, true);
            return sNewbWork;
        }
    }

    public boolean add(int uid) {
        if (mNum <= 0) {
            mNames = null;
            insert(0, uid);
            return true;
        }
        if (mNames != null) throw new IllegalArgumentException("Adding without name to named " + this);
        int i = Arrays.binarySearch(mUids, 0, mNum, uid);
        if (i >= 0) return false;
        insert(-i - 1, uid);
        return true;
    }

    public boolean add(int uid, String name) {
        if (mNum <= 0) {
            insert(0, uid, name);
            return true;
        }
        if (mNames == null) throw new IllegalArgumentException("Adding name to unnamed " + this);
        int i;
        for (i = 0; i < mNum; i++) {
            if (mUids[i] > uid) break;
            if (mUids[i] == uid) {
                int diff = mNames[i].compareTo(name);
                if (diff > 0) break;
                if (diff == 0) return false;
            }
        }
        insert(i, uid, name);
        return true;
    }

    public boolean remove(WorkSource other) {
        if (isEmpty() || other.isEmpty()) return false;
        boolean uidRemoved;
        if (mNames == null && other.mNames == null) {
            uidRemoved = removeUids(other);
        } else {
            if (mNames == null) throw new IllegalArgumentException("Other " + other + " has names, but target " + this + " does not");
            if (other.mNames == null) throw new IllegalArgumentException("Target " + this + " has names, but other " + other + " does not");
            uidRemoved = removeUidsAndNames(other);
        }
        boolean chainRemoved = false;
        if (other.mChains != null && mChains != null) chainRemoved = mChains.removeAll(other.mChains);
        return uidRemoved || chainRemoved;
    }

    public WorkChain createWorkChain() {
        if (mChains == null) mChains = new ArrayList<>(4);
        final WorkChain wc = new WorkChain();
        mChains.add(wc);
        return wc;
    }

    public boolean isEmpty() { return mNum == 0 && (mChains == null || mChains.isEmpty()); }

    public List<WorkChain> getWorkChains() { return mChains; }

    public void transferWorkChains(WorkSource other) {
        if (mChains != null) mChains.clear();
        if (other.mChains == null || other.mChains.isEmpty()) return;
        if (mChains == null) mChains = new ArrayList<>(4);
        mChains.addAll(other.mChains);
        other.mChains.clear();
    }

    /** A copy without the names. */
    public WorkSource withoutNames() {
        final WorkSource copy = new WorkSource(this);
        copy.clearNames();
        return copy;
    }

    private boolean removeUids(WorkSource other) {
        int n1 = mNum;
        final int[] uids1 = mUids;
        final int n2 = other.mNum;
        final int[] uids2 = other.mUids;
        boolean changed = false;
        int i1 = 0, i2 = 0;
        while (i1 < n1 && i2 < n2) {
            if (uids2[i2] == uids1[i1]) {
                n1--;
                changed = true;
                if (i1 < n1) System.arraycopy(uids1, i1 + 1, uids1, i1, n1 - i1);
                i2++;
            } else if (uids2[i2] > uids1[i1]) {
                i1++;
            } else {
                i2++;
            }
        }
        mNum = n1;
        return changed;
    }

    private boolean removeUidsAndNames(WorkSource other) {
        int n1 = mNum;
        final int[] uids1 = mUids;
        final String[] names1 = mNames;
        final int n2 = other.mNum;
        final int[] uids2 = other.mUids;
        final String[] names2 = other.mNames;
        boolean changed = false;
        int i1 = 0, i2 = 0;
        while (i1 < n1 && i2 < n2) {
            if (uids2[i2] == uids1[i1] && names2[i2].equals(names1[i1])) {
                n1--;
                changed = true;
                if (i1 < n1) {
                    System.arraycopy(uids1, i1 + 1, uids1, i1, n1 - i1);
                    System.arraycopy(names1, i1 + 1, names1, i1, n1 - i1);
                }
                i2++;
            } else if (uids2[i2] > uids1[i1] || (uids2[i2] == uids1[i1] && names2[i2].compareTo(names1[i1]) > 0)) {
                i1++;
            } else {
                i2++;
            }
        }
        mNum = n1;
        return changed;
    }

    private boolean updateLocked(WorkSource other, boolean set, boolean returnNewbs) {
        if (mNames == null && other.mNames == null) return updateUidsLocked(other, set, returnNewbs);
        if (mNum > 0 && mNames == null) throw new IllegalArgumentException("Other " + other + " has names, but target " + this + " does not");
        if (other.mNum > 0 && other.mNames == null) throw new IllegalArgumentException("Target " + this + " has names, but other " + other + " does not");
        return updateUidsAndNamesLocked(other, set, returnNewbs);
    }

    private static WorkSource addWork(WorkSource cur, int newUid) {
        if (cur == null) return new WorkSource(newUid);
        cur.insert(cur.mNum, newUid);
        return cur;
    }

    private boolean updateUidsLocked(WorkSource other, boolean set, boolean returnNewbs) {
        int n1 = mNum;
        int[] uids1 = mUids;
        final int n2 = other.mNum;
        final int[] uids2 = other.mUids;
        boolean changed = false;
        int i1 = 0, i2 = 0;
        while (i1 < n1 || i2 < n2) {
            if (i1 >= n1 || (i2 < n2 && uids2[i2] < uids1[i1])) {
                // A uid to insert.
                changed = true;
                if (uids1 == null || uids1.length == 0) {
                    uids1 = new int[4];
                    uids1[0] = uids2[i2];
                } else if (n1 >= uids1.length) {
                    int[] newuids = new int[Math.max(4, (uids1.length * 3) / 2)];
                    if (i1 > 0) System.arraycopy(uids1, 0, newuids, 0, i1);
                    if (i1 < n1) System.arraycopy(uids1, i1, newuids, i1 + 1, n1 - i1);
                    uids1 = newuids;
                    uids1[i1] = uids2[i2];
                } else {
                    if (i1 < n1) System.arraycopy(uids1, i1, uids1, i1 + 1, n1 - i1);
                    uids1[i1] = uids2[i2];
                }
                if (returnNewbs) sNewbWork = addWork(sNewbWork, uids2[i2]);
                n1++;
                i1++;
                i2++;
            } else if (!set) {
                // Uids already there, or not in the other, are passed over.
                if (i2 < n2 && uids2[i2] == uids1[i1]) i2++;
                i1++;
            } else {
                // Those not in the other are taken out.
                int start = i1;
                while (i1 < n1 && (i2 >= n2 || uids2[i2] > uids1[i1])) {
                    sGoneWork = addWork(sGoneWork, uids1[i1]);
                    i1++;
                }
                if (start < i1) {
                    System.arraycopy(uids1, i1, uids1, start, n1 - i1);
                    n1 -= i1 - start;
                    i1 = start;
                }
                if (i1 < n1 && i2 < n2 && uids2[i2] == uids1[i1]) {
                    i1++;
                    i2++;
                }
            }
        }
        mNum = n1;
        mUids = uids1;
        return changed;
    }

    /** 0 if equal, negative if this one's i1 is before the other's i2, positive if after. */
    private int compare(WorkSource other, int i1, int i2) {
        final int diff = mUids[i1] - other.mUids[i2];
        if (diff != 0) return diff;
        return mNames[i1].compareTo(other.mNames[i2]);
    }

    private static WorkSource addWork(WorkSource cur, int newUid, String newName) {
        if (cur == null) return new WorkSource(newUid, newName);
        cur.insert(cur.mNum, newUid, newName);
        return cur;
    }

    private boolean updateUidsAndNamesLocked(WorkSource other, boolean set, boolean returnNewbs) {
        final int n2 = other.mNum;
        final int[] uids2 = other.mUids;
        String[] names2 = other.mNames;
        boolean changed = false;
        int i1 = 0, i2 = 0;
        while (i1 < mNum || i2 < n2) {
            int diff = -1;
            if (i1 >= mNum || (i2 < n2 && (diff = compare(other, i1, i2)) > 0)) {
                changed = true;
                insert(i1, uids2[i2], names2[i2]);
                if (returnNewbs) sNewbWork = addWork(sNewbWork, uids2[i2], names2[i2]);
                i1++;
                i2++;
            } else if (!set) {
                if (i2 < n2 && diff == 0) i2++;
                i1++;
            } else {
                int start = i1;
                while (diff < 0) {
                    sGoneWork = addWork(sGoneWork, mUids[i1], mNames[i1]);
                    i1++;
                    if (i1 >= mNum) break;
                    diff = i2 < n2 ? compare(other, i1, i2) : -1;
                }
                if (start < i1) {
                    System.arraycopy(mUids, i1, mUids, start, mNum - i1);
                    System.arraycopy(mNames, i1, mNames, start, mNum - i1);
                    mNum -= i1 - start;
                    i1 = start;
                }
                if (i1 < mNum && diff == 0) {
                    i1++;
                    i2++;
                }
            }
        }
        return changed;
    }

    private void insert(int index, int uid) {
        if (mUids == null || mUids.length == 0) {
            mUids = new int[4];
            mUids[0] = uid;
            mNum = 1;
        } else if (mNum >= mUids.length) {
            int[] newuids = new int[Math.max(4, (mNum * 3) / 2)];
            if (index > 0) System.arraycopy(mUids, 0, newuids, 0, index);
            if (index < mNum) System.arraycopy(mUids, index, newuids, index + 1, mNum - index);
            mUids = newuids;
            mUids[index] = uid;
            mNum++;
        } else {
            if (index < mNum) System.arraycopy(mUids, index, mUids, index + 1, mNum - index);
            mUids[index] = uid;
            mNum++;
        }
    }

    private void insert(int index, int uid, String name) {
        if (mNum == 0) {
            mUids = new int[4];
            mUids[0] = uid;
            mNames = new String[4];
            mNames[0] = name;
            mNum = 1;
        } else if (mNum >= mUids.length) {
            int[] newuids = new int[Math.max(4, (mNum * 3) / 2)];
            String[] newnames = new String[newuids.length];
            if (index > 0) {
                System.arraycopy(mUids, 0, newuids, 0, index);
                System.arraycopy(mNames, 0, newnames, 0, index);
            }
            if (index < mNum) {
                System.arraycopy(mUids, index, newuids, index + 1, mNum - index);
                System.arraycopy(mNames, index, newnames, index + 1, mNum - index);
            }
            mUids = newuids;
            mNames = newnames;
            mUids[index] = uid;
            mNames[index] = name;
            mNum++;
        } else {
            if (index < mNum) {
                System.arraycopy(mUids, index, mUids, index + 1, mNum - index);
                System.arraycopy(mNames, index, mNames, index + 1, mNum - index);
            }
            mUids[index] = uid;
            mNames[index] = name;
            mNum++;
        }
    }

    /** A chain of the uids (and tags) a piece of work was done for, the first the one it is attributed to. */
    public static final class WorkChain implements Parcelable {
        private int mSize;
        private int[] mUids;
        private String[] mTags;

        public WorkChain() {
            mSize = 0;
            mUids = new int[4];
            mTags = new String[4];
        }

        public WorkChain(WorkChain other) {
            mSize = other.mSize;
            mUids = other.mUids.clone();
            mTags = other.mTags.clone();
        }

        private WorkChain(Parcel in) {
            mSize = in.readInt();
            int[] uids = in.createIntArray();
            String[] tags = in.createStringArray();
            mUids = uids != null ? uids : new int[0];
            mTags = tags != null ? tags : new String[0];
        }

        public WorkChain addNode(int uid, String tag) {
            if (mSize == mUids.length) resizeArrays();
            mUids[mSize] = uid;
            mTags[mSize] = tag;
            mSize++;
            return this;
        }

        public int getAttributionUid() { return mSize > 0 ? mUids[0] : -1; }
        public String getAttributionTag() { return mTags.length > 0 ? mTags[0] : null; }
        public int[] getUids() { return Arrays.copyOf(mUids, mSize); }
        public String[] getTags() { return Arrays.copyOf(mTags, mSize); }
        public int getSize() { return mSize; }

        private void resizeArrays() {
            final int newSize = Math.max(4, mSize * 2);
            mUids = Arrays.copyOf(mUids, newSize);
            mTags = Arrays.copyOf(mTags, newSize);
        }

        @Override
        public String toString() {
            StringBuilder result = new StringBuilder("WorkChain{");
            for (int i = 0; i < mSize; ++i) {
                if (i != 0) result.append(", ");
                result.append("(").append(mUids[i]);
                if (mTags[i] != null) result.append(", ").append(mTags[i]);
                result.append(")");
            }
            return result.append("}").toString();
        }

        @Override
        public int hashCode() { return (mSize + 31 * Arrays.hashCode(mUids)) * 31 + Arrays.hashCode(mTags); }

        @Override
        public boolean equals(Object o) {
            if (o instanceof WorkChain) {
                WorkChain other = (WorkChain) o;
                return mSize == other.mSize && Arrays.equals(mUids, other.mUids) && Arrays.equals(mTags, other.mTags);
            }
            return false;
        }

        @Override
        public int describeContents() { return 0; }

        @Override
        public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(mSize);
            dest.writeIntArray(mUids);
            dest.writeStringArray(mTags);
        }

        public static final Parcelable.Creator<WorkChain> CREATOR = new Parcelable.Creator<WorkChain>() {
            public WorkChain createFromParcel(Parcel in) { return new WorkChain(in); }
            public WorkChain[] newArray(int size) { return new WorkChain[size]; }
        };
    }

    /** The chains that are in newWs and not in oldWs, and those gone; null when none came or went. */
    public static ArrayList<WorkChain>[] diffChains(WorkSource oldWs, WorkSource newWs) {
        ArrayList<WorkChain> newChains = null, goneChains = null;
        if (oldWs.mChains != null) {
            for (WorkChain wc : oldWs.mChains) {
                if (newWs.mChains == null || !newWs.mChains.contains(wc)) {
                    if (goneChains == null) goneChains = new ArrayList<>(oldWs.mChains.size());
                    goneChains.add(wc);
                }
            }
        }
        if (newWs.mChains != null) {
            for (WorkChain wc : newWs.mChains) {
                if (oldWs.mChains == null || !oldWs.mChains.contains(wc)) {
                    if (newChains == null) newChains = new ArrayList<>(newWs.mChains.size());
                    newChains.add(wc);
                }
            }
        }
        if (newChains != null || goneChains != null) return new ArrayList[] { newChains, goneChains };
        return null;
    }

    @Override
    public int describeContents() { return 0; }

    @Override
    public void writeToParcel(Parcel dest, int flags) {
        dest.writeInt(mNum);
        dest.writeIntArray(mUids);
        dest.writeStringArray(mNames);
        if (mChains == null) {
            dest.writeInt(-1);
        } else {
            dest.writeInt(mChains.size());
            dest.writeParcelableList(mChains, flags);
        }
    }

    @Override
    public String toString() {
        StringBuilder result = new StringBuilder("WorkSource{");
        for (int i = 0; i < mNum; i++) {
            if (i != 0) result.append(", ");
            result.append(mUids[i]);
            if (mNames != null) result.append(" ").append(mNames[i]);
        }
        if (mChains != null) {
            result.append(" chains=");
            for (int i = 0; i < mChains.size(); ++i) {
                if (i != 0) result.append(", ");
                result.append(mChains.get(i));
            }
        }
        return result.append("}").toString();
    }

    /** (Android writes it into a proto for dumpsys; nothing asks here.) */
    public void dumpDebug(android.util.proto.ProtoOutputStream proto, long fieldId) {}

    public static final Parcelable.Creator<WorkSource> CREATOR = new Parcelable.Creator<WorkSource>() {
        public WorkSource createFromParcel(Parcel in) { return new WorkSource(in); }
        public WorkSource[] newArray(int size) { return new WorkSource[size]; }
    };
}
