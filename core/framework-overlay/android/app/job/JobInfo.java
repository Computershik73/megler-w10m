// Overlay of core/framework/src/android/app/job/JobInfo.java (tools/build-framework.sh).
// It differs in what a job's info keeps: everything its Builder is told that Megler's job scheduler goes by or an app asks
// back -- its service, its transient extras, its network type, its minimum latency and deadline, whether and how often it
// repeats, its backoff, whether it is persisted, its other constraints and its priority -- where the original kept its id and
// extras alone, and getService() was null.
//
// Why. Jobs were kept and never run, and a job's info kept its id and extras only: Play Store's download service has its
// downloads done by jobs of its DownloadJobService (needing a network), and asks the pending job's service by its class
// (getPendingJob(...).getService(), which was null). Megler runs a job as Android's job scheduler does once its constraints
// hold, network ones taken to hold while the phone has a network: its service made as the app's others are (megler.Services:
// in its process's scope, megler.Processes), on the main thread, after its minimum latency.
//
// Everything else is the original's, unchanged.
package android.app.job;
public class JobInfo implements android.os.Parcelable {
    public static final int NETWORK_TYPE_NONE = 0, NETWORK_TYPE_ANY = 1, NETWORK_TYPE_UNMETERED = 2, NETWORK_TYPE_NOT_ROAMING = 3, NETWORK_TYPE_CELLULAR = 4, BACKOFF_POLICY_LINEAR = 0, BACKOFF_POLICY_EXPONENTIAL = 1, PRIORITY_DEFAULT = 300;
    public static final long DEFAULT_INITIAL_BACKOFF_MILLIS = 30000, MAX_BACKOFF_DELAY_MILLIS = 18000000;
    private final int mId; private final android.os.PersistableBundle mExtras;
    private final android.content.ComponentName mService; private final android.os.Bundle mTransientExtras;
    private final int mNetworkType, mBackoffPolicy, mPriority;
    private final long mMinLatency, mMaxDelay, mInterval, mFlex, mInitialBackoff;
    private final boolean mPeriodic, mPersisted, mCharging, mIdle, mBatteryNotLow, mStorageNotLow, mExpedited, mImportantWhileForeground, mPrefetch, mUserInitiated;
    JobInfo(Builder b) {
        mId = b.id; mExtras = b.extras; mService = b.service; mTransientExtras = b.transientExtras; mNetworkType = b.networkType;
        mBackoffPolicy = b.backoffPolicy; mPriority = b.priority; mMinLatency = b.minLatency; mMaxDelay = b.maxDelay; mInterval = b.interval;
        mFlex = b.flex; mInitialBackoff = b.initialBackoff; mPeriodic = b.periodic; mPersisted = b.persisted; mCharging = b.charging; mIdle = b.idle;
        mBatteryNotLow = b.batteryNotLow; mStorageNotLow = b.storageNotLow; mExpedited = b.expedited; mImportantWhileForeground = b.importantWhileForeground;
        mPrefetch = b.prefetch; mUserInitiated = b.userInitiated;
    }
    public int getId() { return mId; } public android.os.PersistableBundle getExtras() { return mExtras; } public android.content.ComponentName getService() { return mService; }
    public android.os.Bundle getTransientExtras() { return mTransientExtras; }
    public int getNetworkType() { return mNetworkType; }
    public long getMinLatencyMillis() { return mMinLatency; }
    public long getMaxExecutionDelayMillis() { return mMaxDelay; }
    public boolean isPeriodic() { return mPeriodic; }
    public long getIntervalMillis() { return mInterval; }
    public long getFlexMillis() { return mFlex; }
    public long getInitialBackoffMillis() { return mInitialBackoff; }
    public int getBackoffPolicy() { return mBackoffPolicy; }
    public boolean isPersisted() { return mPersisted; }
    public boolean isRequireCharging() { return mCharging; }
    public boolean isRequireDeviceIdle() { return mIdle; }
    public boolean isRequireBatteryNotLow() { return mBatteryNotLow; }
    public boolean isRequireStorageNotLow() { return mStorageNotLow; }
    public boolean isExpedited() { return mExpedited; }
    public boolean isImportantWhileForeground() { return mImportantWhileForeground; }
    public boolean isPrefetch() { return mPrefetch; }
    public boolean isUserInitiated() { return mUserInitiated; }
    public int getPriority() { return mPriority; }
    public boolean hasEarlyConstraint() { return mMinLatency > 0; }
    public boolean hasLateConstraint() { return mMaxDelay > 0 || mPeriodic; }
    @Override public String toString() { return "(job:" + mId + "/" + (mService != null ? mService.flattenToShortString() : "?") + ")"; }
    public static long getMinPeriodMillis() { return 900000; } public static long getMinFlexMillis() { return 300000; }
    public int describeContents() { return 0; }
    public static final class Builder {
        private final int id; private final android.content.ComponentName service;
        private android.os.PersistableBundle extras = new android.os.PersistableBundle(); private android.os.Bundle transientExtras = android.os.Bundle.EMPTY;
        private int networkType = NETWORK_TYPE_NONE, backoffPolicy = BACKOFF_POLICY_EXPONENTIAL, priority = PRIORITY_DEFAULT;
        private long minLatency, maxDelay, interval, flex, initialBackoff = DEFAULT_INITIAL_BACKOFF_MILLIS;
        private boolean periodic, persisted, charging, idle, batteryNotLow, storageNotLow, expedited, importantWhileForeground, prefetch, userInitiated;
        public Builder(int id, android.content.ComponentName service) { this.id = id; this.service = service; }
        public Builder setExtras(android.os.PersistableBundle e) { extras = e; return this; } public Builder setTransientExtras(android.os.Bundle e) { transientExtras = e; return this; } public Builder setRequiredNetworkType(int t) { networkType = t; return this; }
        public Builder setRequiredNetwork(android.net.NetworkRequest r) { networkType = r != null ? NETWORK_TYPE_ANY : NETWORK_TYPE_NONE; return this; } public Builder setRequiresCharging(boolean b) { charging = b; return this; } public Builder setRequiresDeviceIdle(boolean b) { idle = b; return this; }
        public Builder setRequiresBatteryNotLow(boolean b) { batteryNotLow = b; return this; } public Builder setRequiresStorageNotLow(boolean b) { storageNotLow = b; return this; } public Builder addTriggerContentUri(TriggerContentUri u) { return this; }
        public Builder setTriggerContentUpdateDelay(long d) { return this; } public Builder setTriggerContentMaxDelay(long d) { return this; }
        public Builder setPeriodic(long i) { return setPeriodic(i, i); }
        public Builder setPeriodic(long i, long f) { periodic = true; interval = Math.max(i, getMinPeriodMillis()); flex = Math.max(Math.min(f, interval), getMinFlexMillis()); return this; }
        public Builder setMinimumLatency(long l) { minLatency = l; return this; } public Builder setOverrideDeadline(long d) { maxDelay = d; return this; }
        public Builder setBackoffCriteria(long b, int p) { initialBackoff = b; backoffPolicy = p; return this; } public Builder setImportantWhileForeground(boolean b) { importantWhileForeground = b; return this; }
        public Builder setPrefetch(boolean b) { prefetch = b; return this; } public Builder setPersisted(boolean b) { persisted = b; return this; } public Builder setExpedited(boolean b) { expedited = b; return this; } public Builder setEstimatedNetworkBytes(long d, long u) { return this; }
        public Builder setUserInitiated(boolean b) { userInitiated = b; return this; } public Builder setPriority(int p) { priority = p; return this; }
        public JobInfo build() { return new JobInfo(this); }
        // ---- generated by tools/compat/fillmembers.py (Builder): the platform's members this class does not write (signatures only)
        public android.app.job.JobInfo.Builder addDebugTag(java.lang.String p0) { return this; }
        public android.app.job.JobInfo.Builder addDebugTags(java.util.Set p0) { return this; }
        public android.app.job.JobInfo build(boolean p0, boolean p1, boolean p2, boolean p3) { return null; }
        public android.app.job.JobInfo.Builder removeDebugTag(java.lang.String p0) { return this; }
        public android.app.job.JobInfo.Builder setBias(int p0) { return this; }
        public android.app.job.JobInfo.Builder setClipData(android.content.ClipData p0, int p1) { return this; }
        public android.app.job.JobInfo.Builder setFlags(int p0) { return this; }
        public android.app.job.JobInfo.Builder setMinimumNetworkChunkBytes(long p0) { return this; }
        public android.app.job.JobInfo.Builder setTraceTag(java.lang.String p0) { return this; }
        public java.lang.String summarize() { return null; }
        // ---- end of generated members (Builder)
    }
    // ---- platform API stubs (tools/compat/genstubs.py)
    public static final class TriggerContentUri implements android.os.Parcelable {
        public static final int FLAG_NOTIFY_FOR_DESCENDANTS = 1;
        private final android.net.Uri mUri; private final int mFlags;
        public TriggerContentUri(android.net.Uri uri, int flags) { mUri = uri; mFlags = flags; }
        public android.net.Uri getUri() { return mUri; }
        public int getFlags() { return mFlags; }
        public int describeContents() { return 0; }
        public void writeToParcel(android.os.Parcel p, int f) { p.writeParcelable(mUri, f); p.writeInt(mFlags); }
        public static final Creator<TriggerContentUri> CREATOR = new Creator<TriggerContentUri>() { public TriggerContentUri createFromParcel(android.os.Parcel p) { return new TriggerContentUri(p.readParcelable(null), p.readInt()); } public TriggerContentUri[] newArray(int n) { return new TriggerContentUri[n]; } };
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final int BIAS_ADJ_ALWAYS_RUNNING = -80;
    public static final int BIAS_ADJ_OFTEN_RUNNING = -40;
    public static final int BIAS_BOUND_FOREGROUND_SERVICE = 30;
    public static final int BIAS_DEFAULT = 0;
    public static final int BIAS_FOREGROUND_SERVICE = 35;
    public static final int BIAS_SYNC_EXPEDITED = 10;
    public static final int BIAS_SYNC_INITIALIZATION = 20;
    public static final int BIAS_TOP_APP = 40;
    public static final int CONSTRAINT_FLAG_BATTERY_NOT_LOW = 2;
    public static final int CONSTRAINT_FLAG_CHARGING = 1;
    public static final int CONSTRAINT_FLAG_DEVICE_IDLE = 4;
    public static final int CONSTRAINT_FLAG_STORAGE_NOT_LOW = 8;
    public static final int DEFAULT_BACKOFF_POLICY = 1;
    public static final long DISALLOW_DEADLINES_FOR_PREFETCH_JOBS = 194532703L;
    public static final long ENFORCE_MINIMUM_TIME_WINDOWS = 311402873L;
    public static final int FLAG_EXEMPT_FROM_APP_STANDBY = 8;
    public static final int FLAG_EXPEDITED = 16;
    public static final int FLAG_IMPORTANT_WHILE_FOREGROUND = 2;
    public static final int FLAG_PREFETCH = 4;
    public static final int FLAG_USER_INITIATED = 32;
    public static final int FLAG_WILL_BE_FOREGROUND = 1;
    public static final int MAX_DEBUG_TAG_LENGTH = 127;
    public static final int MAX_NUM_DEBUG_TAGS = 32;
    public static final int MAX_TRACE_TAG_LENGTH = 127;
    public static final long MIN_BACKOFF_MILLIS = 10000L;
    public static final int NETWORK_BYTES_UNKNOWN = -1;
    public static final int NETWORK_TYPE_METERED = 4;
    public static final int PRIORITY_FOREGROUND_APP = 30;
    public static final int PRIORITY_FOREGROUND_SERVICE = 35;
    public static final int PRIORITY_HIGH = 400;
    public static final int PRIORITY_LOW = 200;
    public static final int PRIORITY_MAX = 500;
    public static final int PRIORITY_MIN = 100;
    public static final long REJECT_NEGATIVE_DELAYS_AND_DEADLINES = 323349338L;
    public static final long REJECT_NEGATIVE_NETWORK_ESTIMATES = 253665015L;
    public static final long THROW_ON_INVALID_PRIORITY_VALUE = 140852299L;
    public static java.lang.String getBiasString(int p0) { return null; }
    public static long getMinBackoffMillis() { return 0L; }
    public static java.lang.String getPriorityString(int p0) { return null; }
    public static java.lang.String validateDebugTag(java.lang.String p0) { return null; }
    public static java.lang.String validateTraceTag(java.lang.String p0) { return null; }
    public void enforceValidity(boolean p0, boolean p1, boolean p2, boolean p3) {}
    public int getBias() { return 0; }
    public android.content.ClipData getClipData() { return null; }
    public int getClipGrantFlags() { return 0; }
    public int getConstraintFlags() { return 0; }
    public java.util.Set getDebugTags() { return new java.util.HashSet(); }
    public android.util.ArraySet getDebugTagsArraySet() { return null; }
    public long getEstimatedNetworkDownloadBytes() { return 0L; }
    public long getEstimatedNetworkUploadBytes() { return 0L; }
    public long getMinimumNetworkChunkBytes() { return 0L; }
    public android.net.NetworkRequest getRequiredNetwork() { return null; }
    public java.lang.String getTraceTag() { return null; }
    public long getTriggerContentMaxDelay() { return 0L; }
    public long getTriggerContentUpdateDelay() { return 0L; }
    public android.app.job.JobInfo.TriggerContentUri[] getTriggerContentUris() { return null; }
    public boolean isExemptedFromAppStandby() { return false; }
    // ---- end of generated members
}
