package android.metrics;

/**
 * One metrics event, as Android's MetricsReader hands them out. A Megler phone has none to read (MetricsReader); this holds
 * what an app sets on one it makes itself.
 */
public class LogMaker {
    private int mCategory, mCounterValue;
    private long mTimestamp, mCounterBucket;
    private boolean mLongCounterBucket;
    private String mCounterName;
    public LogMaker(int category) { mCategory = category; }
    public LogMaker setCategory(int category) { mCategory = category; return this; }
    public int getCategory() { return mCategory; }
    public long getTimestamp() { return mTimestamp; }
    public String getCounterName() { return mCounterName; }
    public long getCounterBucket() { return mCounterBucket; }
    public boolean isLongCounterBucket() { return mLongCounterBucket; }
    public int getCounterValue() { return mCounterValue; }
    public Object[] serialize() { return new Object[0]; }
}
