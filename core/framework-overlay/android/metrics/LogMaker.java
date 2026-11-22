package android.metrics;

import android.content.ComponentName;
import android.util.SparseArray;

/**
 * One metrics event, as Android's MetricsReader hands them out and as the platform's own apps build them for MetricsLogger.
 * A Megler phone has none to read (MetricsReader) and logs none anywhere; this holds what an app sets on one it makes.
 *
 * Android's hidden setters and getters are here too (setType, setSubtype, setPackageName, addTaggedData...): Android's own
 * Settings (AOSP 9, which Megler's package carries) builds one for every screen it shows (EventLogWriter.visible), and ended
 * at its first screen on NoSuchMethodError: LogMaker.setType. The tags are Android's (MetricsEvent's RESERVED_FOR_LOGBUILDER_*),
 * so serialize() and the Object[] constructor read each other as Android's do.
 */
public class LogMaker {
    // MetricsEvent's tags for the fields every event has.
    private static final int CATEGORY = 757, TYPE = 758, SUBTYPE = 759, NAME = 799, LATENCY = 798, COUNTER_NAME = 803,
            COUNTER_BUCKET = 801, COUNTER_VALUE = 802, TIMESTAMP = 805, PID = 865, UID = 943, PACKAGE_NAME = 806,
            CLASS_NAME = 871;
    /** Android's limit on a string's length in an event. */
    public static final int MAX_SERIALIZED_SIZE = 4000;

    private final SparseArray<Object> entries = new SparseArray<>();

    public LogMaker(int category) { setCategory(category); }
    public LogMaker(Object[] items) { if (items != null) deserialize(items); else setCategory(0); }

    public LogMaker setCategory(int category) { entries.put(CATEGORY, category); return this; }
    public LogMaker clearCategory() { entries.remove(CATEGORY); return this; }
    public LogMaker setType(int type) { entries.put(TYPE, type); return this; }
    public LogMaker clearType() { entries.remove(TYPE); return this; }
    public LogMaker setSubtype(int subtype) { entries.put(SUBTYPE, subtype); return this; }
    public LogMaker clearSubtype() { entries.remove(SUBTYPE); return this; }
    public LogMaker setLatency(long milliseconds) { entries.put(LATENCY, milliseconds); return this; }
    public LogMaker setTimestamp(long timestamp) { entries.put(TIMESTAMP, timestamp); return this; }
    public LogMaker clearTimestamp() { entries.remove(TIMESTAMP); return this; }
    public LogMaker setPackageName(String packageName) { entries.put(PACKAGE_NAME, packageName); return this; }
    public LogMaker setComponentName(ComponentName component) {
        entries.put(PACKAGE_NAME, component.getPackageName());
        entries.put(CLASS_NAME, component.getClassName());
        return this;
    }
    public LogMaker clearPackageName() { entries.remove(PACKAGE_NAME); return this; }
    public LogMaker setProcessId(int pid) { entries.put(PID, pid); return this; }
    public LogMaker clearProcessId() { entries.remove(PID); return this; }
    public LogMaker setUid(int uid) { entries.put(UID, uid); return this; }
    public LogMaker clearUid() { entries.remove(UID); return this; }
    public LogMaker setCounterName(String name) { entries.put(COUNTER_NAME, name); return this; }
    public LogMaker setCounterBucket(int bucket) { entries.put(COUNTER_BUCKET, bucket); return this; }
    public LogMaker setCounterBucket(long bucket) { entries.put(COUNTER_BUCKET, bucket); return this; }
    public LogMaker setCounterValue(int value) { entries.put(COUNTER_VALUE, value); return this; }
    public LogMaker addTaggedData(int tag, Object value) {
        if (value == null) return clearTaggedData(tag);
        if (!isValidValue(value)) throw new IllegalArgumentException("Value must be loggable type - int, long, float, String");
        if (value instanceof String && ((String) value).length() > MAX_SERIALIZED_SIZE) value = ((String) value).substring(0, MAX_SERIALIZED_SIZE);
        entries.put(tag, value);
        return this;
    }
    public LogMaker clearTaggedData(int tag) { entries.delete(tag); return this; }
    public boolean isValidValue(Object value) {
        return value instanceof Integer || value instanceof String || value instanceof Long || value instanceof Float;
    }
    public Object getTaggedData(int tag) { return entries.get(tag); }

    private int intOf(int tag) { Object o = entries.get(tag); return o instanceof Integer ? (Integer) o : 0; }
    public int getCategory() { return intOf(CATEGORY); }
    public int getType() { return intOf(TYPE); }
    public int getSubtype() { return intOf(SUBTYPE); }
    public long getTimestamp() { Object o = entries.get(TIMESTAMP); return o instanceof Long ? (Long) o : 0; }
    public String getPackageName() { Object o = entries.get(PACKAGE_NAME); return o instanceof String ? (String) o : null; }
    public String getComponentName() { return getPackageName() + "/" + entries.get(CLASS_NAME); }
    public int getProcessId() { Object o = entries.get(PID); return o instanceof Integer ? (Integer) o : -1; }
    public int getUid() { Object o = entries.get(UID); return o instanceof Integer ? (Integer) o : -1; }
    public String getCounterName() { Object o = entries.get(COUNTER_NAME); return o instanceof String ? (String) o : null; }
    public long getCounterBucket() { Object o = entries.get(COUNTER_BUCKET); return o instanceof Number ? ((Number) o).longValue() : 0; }
    public boolean isLongCounterBucket() { return entries.get(COUNTER_BUCKET) instanceof Long; }
    public int getCounterValue() { return intOf(COUNTER_VALUE); }

    /** The event as Android writes it to the event log: tag, value, tag, value... */
    public Object[] serialize() {
        Object[] out = new Object[entries.size() * 2];
        for (int i = 0; i < entries.size(); i++) {
            out[i * 2] = entries.keyAt(i);
            out[i * 2 + 1] = entries.valueAt(i);
        }
        return out;
    }
    public void deserialize(Object[] items) {
        for (int i = 0; items != null && i + 1 < items.length; i += 2) {
            if (items[i] instanceof Integer) entries.put((Integer) items[i], items[i + 1]);
        }
    }
    public SparseArray<Object> getEntries() { return entries; }
    /** Whether every tag of this one is in `that` with the same value. */
    public boolean isSubsetOf(LogMaker that) {
        if (that == null) return false;
        for (int i = 0; i < entries.size(); i++) {
            Object mine = entries.valueAt(i), theirs = that.entries.get(entries.keyAt(i));
            if (mine == null ? theirs != null : !mine.equals(theirs)) return false;
        }
        return true;
    }
}
