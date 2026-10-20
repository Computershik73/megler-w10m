// Overlay of core/framework/gen-src/android/location/Location.java (tools/build-framework.sh).
// It differs in being a location: the original is the generated stub, whose copy constructor, set(), has*(), distances and
// parcel did nothing, so a fix handed to an app (meglet/LocationService.java) lost all but what a getter could read back. Here,
// as Android's: each field with whether it is there, copied, compared, parcelled; distanceBetween, distanceTo and bearingTo
// on the WGS 84 ellipsoid (Vincenty's inverse formula, as Android computes them); convert() both ways. Every member of the
// stub is here, with Android's behaviour.
package android.location;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class Location implements android.os.Parcelable {
    public static final java.lang.String EXTRA_NO_GPS_LOCATION = "noGPSLocation";
    public static final int FORMAT_DEGREES = 0;
    public static final int FORMAT_MINUTES = 1;
    public static final int FORMAT_SECONDS = 2;

    private static final int HAS_ALTITUDE = 1, HAS_SPEED = 2, HAS_BEARING = 4, HAS_ACCURACY = 8, HAS_VERTICAL_ACCURACY = 16,
            HAS_SPEED_ACCURACY = 32, HAS_BEARING_ACCURACY = 64, HAS_ELAPSED_UNCERTAINTY = 128, HAS_MSL_ALTITUDE = 256,
            HAS_MSL_ALTITUDE_ACCURACY = 512, MOCK = 1024;

    private java.lang.String mProvider;
    private long mTime;
    private long mElapsedRealtimeNanos;
    private double mElapsedRealtimeUncertaintyNanos;
    private int mFields;
    private double mLatitude, mLongitude, mAltitude, mMslAltitude;
    private float mSpeed, mBearing, mAccuracy, mVerticalAccuracy, mSpeedAccuracy, mBearingAccuracy, mMslAltitudeAccuracy;
    private android.os.Bundle mExtras;

    public static final android.os.Parcelable.Creator<Location> CREATOR = new android.os.Parcelable.Creator<Location>() {
        @Override
        public Location createFromParcel(android.os.Parcel in) {
            Location l = new Location(in.readString());
            l.mFields = in.readInt();
            l.mTime = in.readLong();
            l.mElapsedRealtimeNanos = in.readLong();
            l.mElapsedRealtimeUncertaintyNanos = in.readDouble();
            l.mLatitude = in.readDouble();
            l.mLongitude = in.readDouble();
            l.mAltitude = in.readDouble();
            l.mMslAltitude = in.readDouble();
            l.mSpeed = in.readFloat();
            l.mBearing = in.readFloat();
            l.mAccuracy = in.readFloat();
            l.mVerticalAccuracy = in.readFloat();
            l.mSpeedAccuracy = in.readFloat();
            l.mBearingAccuracy = in.readFloat();
            l.mMslAltitudeAccuracy = in.readFloat();
            l.mExtras = in.readBundle();
            return l;
        }

        @Override
        public Location[] newArray(int size) { return new Location[size]; }
    };

    public Location(android.location.Location l) { set(l); }
    public Location(java.lang.String provider) { mProvider = provider; }
    Location() { }

    // ------------------------------------------------------------ conversions

    public static double convert(java.lang.String coordinate) {
        if (coordinate == null) throw new NullPointerException("coordinate");
        boolean negative = false;
        if (coordinate.charAt(0) == '-') {
            coordinate = coordinate.substring(1);
            negative = true;
        }
        java.util.StringTokenizer st = new java.util.StringTokenizer(coordinate, ":");
        int tokens = st.countTokens();
        if (tokens < 1) throw new IllegalArgumentException("coordinate=" + coordinate);
        try {
            java.lang.String degrees = st.nextToken();
            double val;
            if (tokens == 1) {
                val = Double.parseDouble(degrees);
                return negative ? -val : val;
            }
            java.lang.String minutes = st.nextToken();
            int deg = Integer.parseInt(degrees);
            double min, sec = 0.0;
            boolean secPresent = false;
            if (st.hasMoreTokens()) {
                min = Integer.parseInt(minutes);
                sec = Double.parseDouble(st.nextToken());
                secPresent = true;
            } else {
                min = Double.parseDouble(minutes);
            }
            boolean isNegative180 = negative && deg == 180 && min == 0 && sec == 0;
            if (deg < 0.0 || (deg > 179 && !isNegative180)) throw new IllegalArgumentException("coordinate=" + coordinate);
            if (min < 0 || min >= 60 || (secPresent && min > 59)) throw new IllegalArgumentException("coordinate=" + coordinate);
            if (sec < 0 || sec >= 60) throw new IllegalArgumentException("coordinate=" + coordinate);
            val = deg * 3600.0 + min * 60.0 + sec;
            val /= 3600.0;
            return negative ? -val : val;
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("coordinate=" + coordinate + ": " + e);
        }
    }

    public static java.lang.String convert(double coordinate, int outputType) {
        if (coordinate < -180.0 || coordinate > 180.0 || Double.isNaN(coordinate)) throw new IllegalArgumentException("coordinate=" + coordinate);
        if (outputType != FORMAT_DEGREES && outputType != FORMAT_MINUTES && outputType != FORMAT_SECONDS)
            throw new IllegalArgumentException("outputType=" + outputType);
        StringBuilder sb = new StringBuilder();
        if (coordinate < 0) {
            sb.append('-');
            coordinate = -coordinate;
        }
        java.text.DecimalFormat df = new java.text.DecimalFormat("###.#####", java.text.DecimalFormatSymbols.getInstance(java.util.Locale.US));
        if (outputType == FORMAT_MINUTES || outputType == FORMAT_SECONDS) {
            int degrees = (int) Math.floor(coordinate);
            sb.append(degrees).append(':');
            coordinate = (coordinate - degrees) * 60.0;
            if (outputType == FORMAT_SECONDS) {
                int minutes = (int) Math.floor(coordinate);
                sb.append(minutes).append(':');
                coordinate = (coordinate - minutes) * 60.0;
            }
        }
        sb.append(df.format(coordinate));
        return sb.toString();
    }

    // ------------------------------------------------- distances and bearings

    /* Vincenty's inverse formula on WGS 84, as Android's Location computes it: distance, then the initial and final bearings. */
    private static void computeDistanceAndBearing(double lat1, double lon1, double lat2, double lon2, float[] results) {
        final int maxIters = 20;
        lat1 *= Math.PI / 180.0;
        lat2 *= Math.PI / 180.0;
        lon1 *= Math.PI / 180.0;
        lon2 *= Math.PI / 180.0;
        double a = 6378137.0, b = 6356752.3142, f = (a - b) / a;
        double aSqMinusBSqOverBSq = (a * a - b * b) / (b * b);
        double l = lon2 - lon1;
        double aA = 0.0;
        double u1 = Math.atan((1.0 - f) * Math.tan(lat1)), u2 = Math.atan((1.0 - f) * Math.tan(lat2));
        double cosU1 = Math.cos(u1), cosU2 = Math.cos(u2), sinU1 = Math.sin(u1), sinU2 = Math.sin(u2);
        double cosU1cosU2 = cosU1 * cosU2, sinU1sinU2 = sinU1 * sinU2;
        double sigma = 0.0, deltaSigma = 0.0, cosSqAlpha, cos2SM, cosSigma, sinSigma, cosLambda = 0.0, sinLambda = 0.0;
        double lambda = l;
        for (int iter = 0; iter < maxIters; iter++) {
            double lambdaOrig = lambda;
            cosLambda = Math.cos(lambda);
            sinLambda = Math.sin(lambda);
            double t1 = cosU2 * sinLambda;
            double t2 = cosU1 * sinU2 - sinU1 * cosU2 * cosLambda;
            double sinSqSigma = t1 * t1 + t2 * t2;
            sinSigma = Math.sqrt(sinSqSigma);
            cosSigma = sinU1sinU2 + cosU1cosU2 * cosLambda;
            sigma = Math.atan2(sinSigma, cosSigma);
            double sinAlpha = sinSigma == 0 ? 0.0 : cosU1cosU2 * sinLambda / sinSigma;
            cosSqAlpha = 1.0 - sinAlpha * sinAlpha;
            cos2SM = cosSqAlpha == 0 ? 0.0 : cosSigma - 2.0 * sinU1sinU2 / cosSqAlpha;
            double uSquared = cosSqAlpha * aSqMinusBSqOverBSq;
            aA = 1 + (uSquared / 16384.0) * (4096.0 + uSquared * (-768 + uSquared * (320.0 - 175.0 * uSquared)));
            double bB = (uSquared / 1024.0) * (256.0 + uSquared * (-128.0 + uSquared * (74.0 - 47.0 * uSquared)));
            double cC = (f / 16.0) * cosSqAlpha * (4.0 + f * (4.0 - 3.0 * cosSqAlpha));
            double cos2SMSq = cos2SM * cos2SM;
            deltaSigma = bB * sinSigma * (cos2SM + (bB / 4.0) * (cosSigma * (-1.0 + 2.0 * cos2SMSq)
                    - (bB / 6.0) * cos2SM * (-3.0 + 4.0 * sinSigma * sinSigma) * (-3.0 + 4.0 * cos2SMSq)));
            lambda = l + (1.0 - cC) * f * sinAlpha * (sigma + cC * sinSigma * (cos2SM + cC * cosSigma * (-1.0 + 2.0 * cos2SM * cos2SM)));
            double delta = (lambda - lambdaOrig) / lambda;
            if (Math.abs(delta) < 1.0e-12) break;
        }
        results[0] = (float) (b * aA * (sigma - deltaSigma));
        if (results.length > 1) {
            results[1] = (float) (Math.atan2(cosU2 * sinLambda, cosU1 * sinU2 - sinU1 * cosU2 * cosLambda) * 180.0 / Math.PI);
            if (results.length > 2)
                results[2] = (float) (Math.atan2(cosU1 * sinLambda, -sinU1 * cosU2 + cosU1 * sinU2 * cosLambda) * 180.0 / Math.PI);
        }
    }

    public static void distanceBetween(double startLatitude, double startLongitude, double endLatitude, double endLongitude, float[] results) {
        if (results == null || results.length < 1) throw new IllegalArgumentException("results is null or has length < 1");
        computeDistanceAndBearing(startLatitude, startLongitude, endLatitude, endLongitude, results);
    }

    public float distanceTo(android.location.Location dest) {
        float[] r = new float[1];
        computeDistanceAndBearing(mLatitude, mLongitude, dest.mLatitude, dest.mLongitude, r);
        return r[0];
    }

    public float bearingTo(android.location.Location dest) {
        float[] r = new float[2];
        computeDistanceAndBearing(mLatitude, mLongitude, dest.mLatitude, dest.mLongitude, r);
        return r[1];
    }

    // ------------------------------------------------------------- the fields

    public int describeContents() { return 0; }
    public void dump(android.util.Printer pw, java.lang.String prefix) { pw.println(prefix + this); }

    public java.lang.String getProvider() { return mProvider; }
    public void setProvider(java.lang.String p) { mProvider = p; }
    public long getTime() { return mTime; }
    public void setTime(long t) { mTime = t; }
    public long getElapsedRealtimeNanos() { return mElapsedRealtimeNanos; }
    public long getElapsedRealtimeMillis() { return mElapsedRealtimeNanos / 1000000L; }
    public void setElapsedRealtimeNanos(long n) { mElapsedRealtimeNanos = n; }
    public long getElapsedRealtimeAgeMillis() { return getElapsedRealtimeAgeMillis(android.os.SystemClock.elapsedRealtime()); }
    public long getElapsedRealtimeAgeMillis(long referenceRealtimeMs) { return referenceRealtimeMs - getElapsedRealtimeMillis(); }
    public double getElapsedRealtimeUncertaintyNanos() { return mElapsedRealtimeUncertaintyNanos; }
    public void setElapsedRealtimeUncertaintyNanos(double n) { mElapsedRealtimeUncertaintyNanos = n; mFields |= HAS_ELAPSED_UNCERTAINTY; }
    public boolean hasElapsedRealtimeUncertaintyNanos() { return (mFields & HAS_ELAPSED_UNCERTAINTY) != 0; }
    public void removeElapsedRealtimeUncertaintyNanos() { mElapsedRealtimeUncertaintyNanos = 0; mFields &= ~HAS_ELAPSED_UNCERTAINTY; }

    public double getLatitude() { return mLatitude; }
    public void setLatitude(double v) { mLatitude = v; }
    public double getLongitude() { return mLongitude; }
    public void setLongitude(double v) { mLongitude = v; }

    public double getAltitude() { return mAltitude; }
    public void setAltitude(double v) { mAltitude = v; mFields |= HAS_ALTITUDE; }
    public boolean hasAltitude() { return (mFields & HAS_ALTITUDE) != 0; }
    public void removeAltitude() { mAltitude = 0; mFields &= ~HAS_ALTITUDE; }

    public float getSpeed() { return mSpeed; }
    public void setSpeed(float v) { mSpeed = v; mFields |= HAS_SPEED; }
    public boolean hasSpeed() { return (mFields & HAS_SPEED) != 0; }
    public void removeSpeed() { mSpeed = 0; mFields &= ~HAS_SPEED; }

    public float getBearing() { return mBearing; }
    public void setBearing(float v) {
        while (v < 0.0f) v += 360.0f;
        while (v >= 360.0f) v -= 360.0f;
        mBearing = v;
        mFields |= HAS_BEARING;
    }
    public boolean hasBearing() { return (mFields & HAS_BEARING) != 0; }
    public void removeBearing() { mBearing = 0; mFields &= ~HAS_BEARING; }

    public float getAccuracy() { return mAccuracy; }
    public void setAccuracy(float v) { mAccuracy = v; mFields |= HAS_ACCURACY; }
    public boolean hasAccuracy() { return (mFields & HAS_ACCURACY) != 0; }
    public void removeAccuracy() { mAccuracy = 0; mFields &= ~HAS_ACCURACY; }

    public float getVerticalAccuracyMeters() { return mVerticalAccuracy; }
    public void setVerticalAccuracyMeters(float v) { mVerticalAccuracy = v; mFields |= HAS_VERTICAL_ACCURACY; }
    public boolean hasVerticalAccuracy() { return (mFields & HAS_VERTICAL_ACCURACY) != 0; }
    public void removeVerticalAccuracy() { mVerticalAccuracy = 0; mFields &= ~HAS_VERTICAL_ACCURACY; }

    public float getSpeedAccuracyMetersPerSecond() { return mSpeedAccuracy; }
    public void setSpeedAccuracyMetersPerSecond(float v) { mSpeedAccuracy = v; mFields |= HAS_SPEED_ACCURACY; }
    public boolean hasSpeedAccuracy() { return (mFields & HAS_SPEED_ACCURACY) != 0; }
    public void removeSpeedAccuracy() { mSpeedAccuracy = 0; mFields &= ~HAS_SPEED_ACCURACY; }

    public float getBearingAccuracyDegrees() { return mBearingAccuracy; }
    public void setBearingAccuracyDegrees(float v) { mBearingAccuracy = v; mFields |= HAS_BEARING_ACCURACY; }
    public boolean hasBearingAccuracy() { return (mFields & HAS_BEARING_ACCURACY) != 0; }
    public void removeBearingAccuracy() { mBearingAccuracy = 0; mFields &= ~HAS_BEARING_ACCURACY; }

    public double getMslAltitudeMeters() { return mMslAltitude; }
    public void setMslAltitudeMeters(double v) { mMslAltitude = v; mFields |= HAS_MSL_ALTITUDE; }
    public boolean hasMslAltitude() { return (mFields & HAS_MSL_ALTITUDE) != 0; }
    public void removeMslAltitude() { mMslAltitude = 0; mFields &= ~HAS_MSL_ALTITUDE; }

    public float getMslAltitudeAccuracyMeters() { return mMslAltitudeAccuracy; }
    public void setMslAltitudeAccuracyMeters(float v) { mMslAltitudeAccuracy = v; mFields |= HAS_MSL_ALTITUDE_ACCURACY; }
    public boolean hasMslAltitudeAccuracy() { return (mFields & HAS_MSL_ALTITUDE_ACCURACY) != 0; }
    public void removeMslAltitudeAccuracy() { mMslAltitudeAccuracy = 0; mFields &= ~HAS_MSL_ALTITUDE_ACCURACY; }

    public android.os.Bundle getExtras() { return mExtras; }
    public void setExtras(android.os.Bundle extras) { mExtras = extras == null ? null : new android.os.Bundle(extras); }

    public boolean isMock() { return (mFields & MOCK) != 0; }
    public boolean isFromMockProvider() { return isMock(); }
    public void setMock(boolean mock) { if (mock) mFields |= MOCK; else mFields &= ~MOCK; }
    public void setIsFromMockProvider(boolean mock) { setMock(mock); }

    public boolean isComplete() { return mProvider != null && hasAccuracy() && mTime != 0 && mElapsedRealtimeNanos != 0; }
    public void makeComplete() {
        if (mProvider == null) mProvider = "";
        if (!hasAccuracy()) setAccuracy(100.0f);
        if (mTime == 0) mTime = System.currentTimeMillis();
        if (mElapsedRealtimeNanos == 0) mElapsedRealtimeNanos = android.os.SystemClock.elapsedRealtimeNanos();
    }

    public void reset() {
        mProvider = null;
        mTime = 0;
        mElapsedRealtimeNanos = 0;
        mElapsedRealtimeUncertaintyNanos = 0;
        mFields = 0;
        mLatitude = mLongitude = mAltitude = mMslAltitude = 0;
        mSpeed = mBearing = mAccuracy = mVerticalAccuracy = mSpeedAccuracy = mBearingAccuracy = mMslAltitudeAccuracy = 0;
        mExtras = null;
    }

    public void set(android.location.Location l) {
        if (l == null) {
            reset();
            return;
        }
        mProvider = l.mProvider;
        mTime = l.mTime;
        mElapsedRealtimeNanos = l.mElapsedRealtimeNanos;
        mElapsedRealtimeUncertaintyNanos = l.mElapsedRealtimeUncertaintyNanos;
        mFields = l.mFields;
        mLatitude = l.mLatitude;
        mLongitude = l.mLongitude;
        mAltitude = l.mAltitude;
        mMslAltitude = l.mMslAltitude;
        mSpeed = l.mSpeed;
        mBearing = l.mBearing;
        mAccuracy = l.mAccuracy;
        mVerticalAccuracy = l.mVerticalAccuracy;
        mSpeedAccuracy = l.mSpeedAccuracy;
        mBearingAccuracy = l.mBearingAccuracy;
        mMslAltitudeAccuracy = l.mMslAltitudeAccuracy;
        mExtras = l.mExtras == null ? null : new android.os.Bundle(l.mExtras);
    }

    public void writeToParcel(android.os.Parcel out, int flags) {
        out.writeString(mProvider);
        out.writeInt(mFields);
        out.writeLong(mTime);
        out.writeLong(mElapsedRealtimeNanos);
        out.writeDouble(mElapsedRealtimeUncertaintyNanos);
        out.writeDouble(mLatitude);
        out.writeDouble(mLongitude);
        out.writeDouble(mAltitude);
        out.writeDouble(mMslAltitude);
        out.writeFloat(mSpeed);
        out.writeFloat(mBearing);
        out.writeFloat(mAccuracy);
        out.writeFloat(mVerticalAccuracy);
        out.writeFloat(mSpeedAccuracy);
        out.writeFloat(mBearingAccuracy);
        out.writeFloat(mMslAltitudeAccuracy);
        out.writeBundle(mExtras);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof Location)) return false;
        Location l = (Location) o;
        return mTime == l.mTime && mElapsedRealtimeNanos == l.mElapsedRealtimeNanos && mFields == l.mFields && Double.compare(l.mLatitude, mLatitude) == 0
                && Double.compare(l.mLongitude, mLongitude) == 0 && Double.compare(l.mAltitude, mAltitude) == 0 && Float.compare(l.mSpeed, mSpeed) == 0
                && Float.compare(l.mBearing, mBearing) == 0 && Float.compare(l.mAccuracy, mAccuracy) == 0 && java.util.Objects.equals(mProvider, l.mProvider);
    }

    @Override
    public int hashCode() { return java.util.Objects.hash(mProvider, mElapsedRealtimeNanos, mLatitude, mLongitude); }

    @Override
    public java.lang.String toString() {
        StringBuilder s = new StringBuilder("Location[");
        s.append(mProvider).append(' ').append(String.format(java.util.Locale.ROOT, "%.6f,%.6f", mLatitude, mLongitude));
        if (hasAccuracy()) s.append(String.format(java.util.Locale.ROOT, " hAcc=%.0f", mAccuracy));
        s.append(" et=").append(mElapsedRealtimeNanos / 1000000L).append("ms");
        if (hasAltitude()) s.append(" alt=").append(mAltitude);
        if (hasSpeed()) s.append(" vel=").append(mSpeed);
        if (hasBearing()) s.append(" bear=").append(mBearing);
        if (isMock()) s.append(" mock");
        return s.append(']').toString();
    }
}
