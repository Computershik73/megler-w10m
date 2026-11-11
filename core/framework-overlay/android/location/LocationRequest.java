// Overlay of core/framework/gen-src/android/location/LocationRequest.java (tools/build-framework.sh).
// It differs in being Android 14's: the generated stub's Builder(intervalMillis) forgot the interval, its factories gave a
// request of nothing (createFromDeprecatedProvider's provider, interval and single shot were dropped), getMaxUpdates said 0
// where Android says "any number" (Integer.MAX_VALUE), and it wrote nothing into a Parcel. The fields and their defaults,
// the Builder, the deprecated setters and getters (an interval of 60 minutes for create(), the minimum update interval a
// sixth of the interval until one is set, the quality a provider or Criteria stands for), equals, hashCode, toString and
// Android's parcel layout are Android's.
//
// Why. Google Play services' location engine in the app asks LocationManager for the passive provider with
// createFromDeprecatedProvider("passive", ...): it came out a request of the fused provider every Long.MAX_VALUE ms ("fused
// every 9223372036854775807 ms for ggdn", 0.1.0.671), which was never handed a fix. And an app's
// LocationManager.requestLocationUpdates(provider, new LocationRequest.Builder(5000).build(), ...) asked for its updates
// every 0 ms.
//
// And the Builder refuses what Android's refuses: a request of the passive interval whose minimum update interval was never
// set ("passive location requests must have an explicit minimum update interval"). It had the check the other way round and
// refused every passive request that did set one, which is every one androidx makes (LocationRequestCompat always sets
// it): microG's network location service asks for the GPS provider's fixes so, and failed at each start of its engine
// ("D/NetworkLocation: GPS location retriever not initialized", "IllegalStateException: passive location requests must have
// an implicit minimum update interval"; Citymapper, 0.1.0.690), and so never heard the GPS provider's fixes.
package android.location;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class LocationRequest implements android.os.Parcelable {
    public static final int ACCURACY_BLOCK = 102;
    public static final int ACCURACY_CITY = 104;
    public static final int ACCURACY_FINE = 100;
    public static final long LOW_POWER_EXCEPTIONS = 168936375L;
    public static final long PASSIVE_INTERVAL = Long.MAX_VALUE;
    public static final int POWER_HIGH = 203;
    public static final int POWER_LOW = 201;
    public static final int POWER_NONE = 200;
    public static final int QUALITY_BALANCED_POWER_ACCURACY = 102;
    public static final int QUALITY_HIGH_ACCURACY = 100;
    public static final int QUALITY_LOW_POWER = 104;
    private static final long IMPLICIT_MIN_UPDATE_INTERVAL = -1;
    private static final double IMPLICIT_MIN_UPDATE_INTERVAL_FACTOR = 1D / 6D;

    private String mProvider;
    private int mQuality;
    private long mIntervalMillis;
    private long mExpireAtRealtimeMillis;
    private long mDurationMillis;
    private int mMaxUpdates;
    private long mMinUpdateIntervalMillis;
    private float mMinUpdateDistanceMeters;
    private long mMaxUpdateDelayMillis;
    private boolean mHideFromAppOps;
    private boolean mAdasGnssBypass;
    private boolean mBypass;
    private boolean mLowPower;
    private android.os.WorkSource mWorkSource;

    @Deprecated
    public static LocationRequest create() {
        // (60 minutes: the legacy default interval)
        return new Builder(60 * 60 * 1000).build();
    }

    @Deprecated
    public static LocationRequest createFromDeprecatedProvider(String provider, long intervalMillis, float minUpdateDistanceMeters, boolean singleShot) {
        if (provider == null) throw new IllegalArgumentException("invalid null provider");
        if (intervalMillis < 0) intervalMillis = 0;
        else if (intervalMillis == PASSIVE_INTERVAL) intervalMillis = Long.MAX_VALUE - 1;
        if (minUpdateDistanceMeters < 0) minUpdateDistanceMeters = 0;
        int quality;
        if (LocationManager.PASSIVE_PROVIDER.equals(provider)) quality = POWER_NONE;
        else if (LocationManager.GPS_PROVIDER.equals(provider)) quality = ACCURACY_FINE;
        else quality = POWER_LOW;
        return new LocationRequest(provider, intervalMillis, quality, Long.MAX_VALUE, Long.MAX_VALUE, singleShot ? 1 : Integer.MAX_VALUE, intervalMillis,
                minUpdateDistanceMeters, 0, false, false, false, false, new android.os.WorkSource());
    }

    @Deprecated
    public static LocationRequest createFromDeprecatedCriteria(Criteria criteria, long intervalMillis, float minUpdateDistanceMeters, boolean singleShot) {
        if (criteria == null) throw new IllegalArgumentException("invalid null criteria");
        if (intervalMillis < 0) intervalMillis = 0;
        else if (intervalMillis == PASSIVE_INTERVAL) intervalMillis = Long.MAX_VALUE - 1;
        if (minUpdateDistanceMeters < 0) minUpdateDistanceMeters = 0;
        int quality;
        switch (criteria.getAccuracy()) {
        case Criteria.ACCURACY_COARSE: quality = ACCURACY_BLOCK; break;
        case Criteria.ACCURACY_FINE: quality = ACCURACY_FINE; break;
        default: quality = criteria.getPowerRequirement() == Criteria.POWER_HIGH ? POWER_HIGH : POWER_LOW;
        }
        return new LocationRequest(null, intervalMillis, quality, Long.MAX_VALUE, Long.MAX_VALUE, singleShot ? 1 : Integer.MAX_VALUE, intervalMillis,
                minUpdateDistanceMeters, 0, false, false, false, false, new android.os.WorkSource());
    }

    private LocationRequest(String provider, long intervalMillis, int quality, long expireAtRealtimeMillis, long durationMillis, int maxUpdates,
            long minUpdateIntervalMillis, float minUpdateDistanceMeters, long maxUpdateDelayMillis, boolean hiddenFromAppOps, boolean adasGnssBypass,
            boolean locationSettingsIgnored, boolean lowPower, android.os.WorkSource workSource) {
        mProvider = provider;
        mIntervalMillis = intervalMillis;
        mQuality = quality;
        mMinUpdateIntervalMillis = minUpdateIntervalMillis;
        mExpireAtRealtimeMillis = expireAtRealtimeMillis;
        mDurationMillis = durationMillis;
        mMaxUpdates = maxUpdates;
        mMinUpdateDistanceMeters = minUpdateDistanceMeters;
        mMaxUpdateDelayMillis = maxUpdateDelayMillis;
        mHideFromAppOps = hiddenFromAppOps;
        mAdasGnssBypass = adasGnssBypass;
        mBypass = locationSettingsIgnored;
        mLowPower = lowPower;
        mWorkSource = workSource != null ? workSource : new android.os.WorkSource();
    }

    /** (Android's hides it; code that made one by reflection gets Android's defaults.) */
    protected LocationRequest() {
        this(null, 60 * 60 * 1000, QUALITY_BALANCED_POWER_ACCURACY, Long.MAX_VALUE, Long.MAX_VALUE, Integer.MAX_VALUE, IMPLICIT_MIN_UPDATE_INTERVAL,
                0, 0, false, false, false, false, new android.os.WorkSource());
    }

    @Deprecated public LocationRequest setProvider(String provider) { if (provider == null) throw new IllegalArgumentException("invalid null provider"); mProvider = provider; return this; }
    @Deprecated public String getProvider() { return mProvider != null ? mProvider : LocationManager.FUSED_PROVIDER; }
    @Deprecated public LocationRequest setQuality(int quality) { mQuality = Builder.checkQuality(quality, true); return this; }
    public int getQuality() { return mQuality; }
    @Deprecated public LocationRequest setInterval(long millis) {
        mIntervalMillis = Math.max(0, millis);
        mMinUpdateIntervalMillis = Math.min(mMinUpdateIntervalMillis, mIntervalMillis);
        return this;
    }
    @Deprecated public long getInterval() { return getIntervalMillis(); }
    public long getIntervalMillis() { return mIntervalMillis; }
    @Deprecated public LocationRequest setFastestInterval(long millis) { mMinUpdateIntervalMillis = Math.max(0, millis); return this; }
    @Deprecated public long getFastestInterval() { return getMinUpdateIntervalMillis(); }
    @Deprecated public LocationRequest setExpireAt(long millis) { mExpireAtRealtimeMillis = Math.max(millis, 0); return this; }
    @Deprecated public long getExpireAt() { return mExpireAtRealtimeMillis; }
    @Deprecated public LocationRequest setExpireIn(long millis) { mDurationMillis = millis; return this; }
    @Deprecated public long getExpireIn() { return getDurationMillis(); }
    public long getExpirationRealtimeMs(long startRealtimeMs) {
        long expirationRealtimeMs;
        // (the sum saturates rather than overflows)
        if (mDurationMillis > Long.MAX_VALUE - startRealtimeMs) expirationRealtimeMs = Long.MAX_VALUE;
        else expirationRealtimeMs = startRealtimeMs + mDurationMillis;
        return Math.min(expirationRealtimeMs, mExpireAtRealtimeMillis);
    }
    public long getDurationMillis() { return mDurationMillis; }
    @Deprecated public LocationRequest setNumUpdates(int numUpdates) {
        if (numUpdates <= 0) throw new IllegalArgumentException("invalid numUpdates: " + numUpdates);
        mMaxUpdates = numUpdates;
        return this;
    }
    @Deprecated public int getNumUpdates() { return getMaxUpdates(); }
    public int getMaxUpdates() { return mMaxUpdates; }
    public long getMinUpdateIntervalMillis() {
        if (mMinUpdateIntervalMillis == IMPLICIT_MIN_UPDATE_INTERVAL) return (long) (mIntervalMillis * IMPLICIT_MIN_UPDATE_INTERVAL_FACTOR);
        return Math.min(mMinUpdateIntervalMillis, mIntervalMillis);
    }
    @Deprecated public LocationRequest setSmallestDisplacement(float minDisplacementMeters) { mMinUpdateDistanceMeters = Math.max(0, minDisplacementMeters); return this; }
    @Deprecated public float getSmallestDisplacement() { return getMinUpdateDistanceMeters(); }
    public float getMinUpdateDistanceMeters() { return mMinUpdateDistanceMeters; }
    public long getMaxUpdateDelayMillis() { return mMaxUpdateDelayMillis; }
    @Deprecated public void setHideFromAppOps(boolean hiddenFromAppOps) { mHideFromAppOps = hiddenFromAppOps; }
    @Deprecated public boolean getHideFromAppOps() { return isHiddenFromAppOps(); }
    public boolean isHiddenFromAppOps() { return mHideFromAppOps; }
    public boolean isAdasGnssBypass() { return mAdasGnssBypass; }
    @Deprecated public LocationRequest setLocationSettingsIgnored(boolean locationSettingsIgnored) { mBypass = locationSettingsIgnored; return this; }
    public boolean isLocationSettingsIgnored() { return mBypass; }
    public boolean isBypass() { return mAdasGnssBypass || mBypass; }
    @Deprecated public LocationRequest setLowPowerMode(boolean enabled) { mLowPower = enabled; return this; }
    @Deprecated public boolean isLowPowerMode() { return isLowPower(); }
    public boolean isLowPower() { return mLowPower; }
    @Deprecated public void setWorkSource(android.os.WorkSource workSource) { mWorkSource = workSource != null ? workSource : new android.os.WorkSource(); }
    public android.os.WorkSource getWorkSource() { return mWorkSource; }

    public static final android.os.Parcelable.Creator<LocationRequest> CREATOR = new android.os.Parcelable.Creator<LocationRequest>() {
        @Override
        public LocationRequest createFromParcel(android.os.Parcel in) {
            String provider = in.readString();
            long interval = in.readLong();
            int quality = in.readInt();
            long expireAt = in.readLong();
            long duration = in.readLong();
            int maxUpdates = in.readInt();
            long minUpdateInterval = in.readLong();
            float minUpdateDistance = in.readFloat();
            long maxUpdateDelay = in.readLong();
            boolean hidden = in.readInt() != 0, adas = in.readInt() != 0, bypass = in.readInt() != 0, lowPower = in.readInt() != 0;
            android.os.WorkSource ws = in.readTypedObject(android.os.WorkSource.CREATOR);
            return new LocationRequest(provider, interval, quality, expireAt, duration, maxUpdates, minUpdateInterval, minUpdateDistance, maxUpdateDelay,
                    hidden, adas, bypass, lowPower, ws);
        }
        @Override
        public LocationRequest[] newArray(int size) { return new LocationRequest[size]; }
    };

    @Override public int describeContents() { return 0; }

    @Override
    public void writeToParcel(android.os.Parcel parcel, int flags) {
        parcel.writeString(mProvider);
        parcel.writeLong(mIntervalMillis);
        parcel.writeInt(mQuality);
        parcel.writeLong(mExpireAtRealtimeMillis);
        parcel.writeLong(mDurationMillis);
        parcel.writeInt(mMaxUpdates);
        parcel.writeLong(mMinUpdateIntervalMillis);
        parcel.writeFloat(mMinUpdateDistanceMeters);
        parcel.writeLong(mMaxUpdateDelayMillis);
        parcel.writeInt(mHideFromAppOps ? 1 : 0);
        parcel.writeInt(mAdasGnssBypass ? 1 : 0);
        parcel.writeInt(mBypass ? 1 : 0);
        parcel.writeInt(mLowPower ? 1 : 0);
        parcel.writeTypedObject(mWorkSource, 0);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (o == null || getClass() != o.getClass()) return false;
        LocationRequest that = (LocationRequest) o;
        return mQuality == that.mQuality && mIntervalMillis == that.mIntervalMillis && mExpireAtRealtimeMillis == that.mExpireAtRealtimeMillis
                && mDurationMillis == that.mDurationMillis && mMaxUpdates == that.mMaxUpdates && mMinUpdateIntervalMillis == that.mMinUpdateIntervalMillis
                && Float.compare(that.mMinUpdateDistanceMeters, mMinUpdateDistanceMeters) == 0 && mMaxUpdateDelayMillis == that.mMaxUpdateDelayMillis
                && mHideFromAppOps == that.mHideFromAppOps && mAdasGnssBypass == that.mAdasGnssBypass && mBypass == that.mBypass && mLowPower == that.mLowPower
                && java.util.Objects.equals(mProvider, that.mProvider) && java.util.Objects.equals(mWorkSource, that.mWorkSource);
    }

    @Override
    public int hashCode() { return java.util.Objects.hash(mProvider, mIntervalMillis, mWorkSource); }

    @Override
    public String toString() {
        StringBuilder s = new StringBuilder("Request[");
        if (mProvider != null) s.append(mProvider).append(' ');
        if (mIntervalMillis != PASSIVE_INTERVAL) {
            s.append(qualityName(mQuality)).append(" @+").append(mIntervalMillis).append("ms");
        } else {
            s.append("OFF");
        }
        if (getMinUpdateIntervalMillis() < mIntervalMillis) s.append(", minUpdateInterval=+").append(getMinUpdateIntervalMillis()).append("ms");
        if (mDurationMillis != Long.MAX_VALUE) s.append(", duration=").append(mDurationMillis).append("ms");
        if (mMaxUpdates != Integer.MAX_VALUE) s.append(", maxUpdates=").append(mMaxUpdates);
        if (mMinUpdateDistanceMeters > 0) s.append(", minUpdateDistance=").append(mMinUpdateDistanceMeters);
        if (mLowPower) s.append(", lowPower");
        if (mBypass) s.append(", bypass");
        return s.append(']').toString();
    }

    private static String qualityName(int quality) {
        switch (quality) {
        case QUALITY_HIGH_ACCURACY: return "HIGH_ACCURACY";
        case QUALITY_BALANCED_POWER_ACCURACY: return "BALANCED";
        case QUALITY_LOW_POWER: return "LOW_POWER";
        case POWER_NONE: return "POWER_NONE";
        case POWER_LOW: return "POWER_LOW";
        case POWER_HIGH: return "POWER_HIGH";
        default: return "???";
        }
    }

    public static final class Builder {
        private long mIntervalMillis;
        private int mQuality;
        private long mDurationMillis;
        private int mMaxUpdates;
        private long mMinUpdateIntervalMillis;
        private float mMinUpdateDistanceMeters;
        private long mMaxUpdateDelayMillis;
        private boolean mHiddenFromAppOps;
        private boolean mAdasGnssBypass;
        private boolean mBypass;
        private boolean mLowPower;
        private android.os.WorkSource mWorkSource;

        public Builder(long intervalMillis) {
            setIntervalMillis(intervalMillis);
            mQuality = QUALITY_BALANCED_POWER_ACCURACY;
            mDurationMillis = Long.MAX_VALUE;
            mMaxUpdates = Integer.MAX_VALUE;
            mMinUpdateIntervalMillis = IMPLICIT_MIN_UPDATE_INTERVAL;
            mMinUpdateDistanceMeters = 0;
            mMaxUpdateDelayMillis = 0;
        }

        public Builder(LocationRequest locationRequest) {
            mIntervalMillis = locationRequest.mIntervalMillis;
            mQuality = locationRequest.mQuality;
            mDurationMillis = locationRequest.mDurationMillis;
            mMaxUpdates = locationRequest.mMaxUpdates;
            mMinUpdateIntervalMillis = locationRequest.mMinUpdateIntervalMillis;
            mMinUpdateDistanceMeters = locationRequest.mMinUpdateDistanceMeters;
            mMaxUpdateDelayMillis = locationRequest.mMaxUpdateDelayMillis;
            mHiddenFromAppOps = locationRequest.mHideFromAppOps;
            mAdasGnssBypass = locationRequest.mAdasGnssBypass;
            mBypass = locationRequest.mBypass;
            mLowPower = locationRequest.mLowPower;
            mWorkSource = new android.os.WorkSource(locationRequest.mWorkSource);
            // (Android's builder turns the deprecated maximum-of-interval and implicit rules into explicit ones.)
            if (mMinUpdateIntervalMillis > mIntervalMillis) mMinUpdateIntervalMillis = mIntervalMillis;
        }

        Builder() { this(0L); }

        public Builder setIntervalMillis(long intervalMillis) {
            if (intervalMillis < 0) throw new IllegalArgumentException("intervalMillis must be >= 0");
            mIntervalMillis = intervalMillis;
            return this;
        }
        public Builder setQuality(int quality) { mQuality = checkQuality(quality, false); return this; }
        public Builder setQuality(Criteria criteria) {
            switch (criteria.getAccuracy()) {
            case Criteria.ACCURACY_COARSE: mQuality = QUALITY_BALANCED_POWER_ACCURACY; break;
            case Criteria.ACCURACY_FINE: mQuality = QUALITY_HIGH_ACCURACY; break;
            default: mQuality = criteria.getPowerRequirement() == Criteria.POWER_HIGH ? POWER_HIGH : POWER_LOW;
            }
            return this;
        }
        public Builder setDurationMillis(long durationMillis) {
            if (durationMillis < 1) throw new IllegalArgumentException("durationMillis must be > 0");
            mDurationMillis = durationMillis;
            return this;
        }
        public Builder setMaxUpdates(int maxUpdates) {
            if (maxUpdates < 1) throw new IllegalArgumentException("maxUpdates must be > 0");
            mMaxUpdates = maxUpdates;
            return this;
        }
        public Builder setMinUpdateIntervalMillis(long minUpdateIntervalMillis) {
            if (minUpdateIntervalMillis < 0) throw new IllegalArgumentException("minUpdateIntervalMillis must be >= 0");
            mMinUpdateIntervalMillis = minUpdateIntervalMillis;
            return this;
        }
        public Builder clearMinUpdateIntervalMillis() { mMinUpdateIntervalMillis = IMPLICIT_MIN_UPDATE_INTERVAL; return this; }
        public Builder setMinUpdateDistanceMeters(float minUpdateDistanceMeters) {
            if (minUpdateDistanceMeters < 0) throw new IllegalArgumentException("minUpdateDistanceMeters must be >= 0");
            mMinUpdateDistanceMeters = minUpdateDistanceMeters;
            return this;
        }
        public Builder setMaxUpdateDelayMillis(long maxUpdateDelayMillis) {
            if (maxUpdateDelayMillis < 0) throw new IllegalArgumentException("maxUpdateDelayMillis must be >= 0");
            mMaxUpdateDelayMillis = maxUpdateDelayMillis;
            return this;
        }
        public Builder setHiddenFromAppOps(boolean hiddenFromAppOps) { mHiddenFromAppOps = hiddenFromAppOps; return this; }
        public Builder setAdasGnssBypass(boolean adasGnssBypass) { mAdasGnssBypass = adasGnssBypass; return this; }
        public Builder setLocationSettingsIgnored(boolean locationSettingsIgnored) { mBypass = locationSettingsIgnored; return this; }
        public Builder setLowPower(boolean lowPower) { mLowPower = lowPower; return this; }
        public Builder setWorkSource(android.os.WorkSource workSource) { mWorkSource = workSource; return this; }

        public LocationRequest build() {
            if (mIntervalMillis == PASSIVE_INTERVAL && mMinUpdateIntervalMillis == IMPLICIT_MIN_UPDATE_INTERVAL)
                throw new IllegalStateException("passive location requests must have an explicit minimum update interval");
            return new LocationRequest(null, mIntervalMillis, mQuality, Long.MAX_VALUE, mDurationMillis, mMaxUpdates,
                    Math.min(mMinUpdateIntervalMillis, mIntervalMillis), mMinUpdateDistanceMeters, mMaxUpdateDelayMillis, mHiddenFromAppOps,
                    mAdasGnssBypass, mBypass, mLowPower, new android.os.WorkSource(mWorkSource));
        }

        /** A quality Android takes: the three qualities and POWER_NONE, and the deprecated powers as the qualities they mean where allowed. */
        static int checkQuality(int quality, boolean allowDeprecated) {
            switch (quality) {
            case QUALITY_HIGH_ACCURACY:
            case QUALITY_BALANCED_POWER_ACCURACY:
            case QUALITY_LOW_POWER:
            case POWER_NONE:
                return quality;
            case POWER_LOW:
                if (allowDeprecated) return QUALITY_LOW_POWER;
                break;
            case POWER_HIGH:
                if (allowDeprecated) return QUALITY_HIGH_ACCURACY;
                break;
            default:
                break;
            }
            throw new IllegalArgumentException("invalid quality: " + quality);
        }
    }
}
