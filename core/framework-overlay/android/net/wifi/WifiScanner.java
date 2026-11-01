// Megler: Android's WifiScanner (a system API: the scanner Google Play services' location asks for), which the framework did
// not have: the class and the types its users name, and no scanner. The phone gives an app no Wi-Fi scans, so
// getSystemService("wifiscanner") answers null, as on a device without one; what a scan would be asked of is a no-op.
//
// Why. Google Play services 26's location service names WifiScanner.ScanListener in a class it makes as it starts; without
// the class its onCreate ended in NoClassDefFoundError, and its clients asked for it again at once: 2500 times in 43
// seconds while Play Store started.
package android.net.wifi;

import android.os.Parcel;
import android.os.Parcelable;

public class WifiScanner {
    public static final int WIFI_BAND_UNSPECIFIED = 0;
    public static final int WIFI_BAND_24_GHZ = 1;
    public static final int WIFI_BAND_5_GHZ = 2;
    public static final int WIFI_BAND_5_GHZ_DFS_ONLY = 4;
    public static final int WIFI_BAND_5_GHZ_WITH_DFS = 6;
    public static final int WIFI_BAND_BOTH = 3;
    public static final int WIFI_BAND_BOTH_WITH_DFS = 7;
    public static final int WIFI_BAND_6_GHZ = 8;
    public static final int WIFI_BAND_60_GHZ = 16;
    public static final int REASON_SUCCEEDED = 0;
    public static final int REASON_UNSPECIFIED = -1;
    public static final int REASON_INVALID_LISTENER = -2;
    public static final int REASON_INVALID_REQUEST = -3;
    public static final int REASON_NOT_AUTHORIZED = -4;
    public static final int REASON_DUPLICATE_REQEUST = -5;
    public static final int REPORT_EVENT_AFTER_BUFFER_FULL = 0;
    public static final int REPORT_EVENT_AFTER_EACH_SCAN = 1;
    public static final int REPORT_EVENT_FULL_SCAN_RESULT = 2;
    public static final int REPORT_EVENT_NO_BATCH = 4;
    public static final int SCAN_TYPE_LOW_LATENCY = 0;
    public static final int SCAN_TYPE_LOW_POWER = 1;
    public static final int SCAN_TYPE_HIGH_ACCURACY = 2;

    WifiScanner() {}

    public interface ActionListener {
        void onSuccess();
        void onFailure(int reason, String description);
    }
    public interface ScanListener extends ActionListener {
        void onPeriodChanged(int periodInMs);
        void onResults(ScanData[] results);
        void onFullResult(ScanResult fullScanResult);
    }

    public static class ScanSettings implements Parcelable {
        public int band;
        public int periodInMs;
        public int reportEvents;
        public int numBssidsPerScan;
        public int maxScansToCache;
        public int maxPeriodInMs;
        public int stepCount;
        public boolean isPnoScan;
        public int type;
        public boolean ignoreLocationSettings;
        public boolean hideFromAppOps;
        public ScanSettings() {}
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(band);
            dest.writeInt(periodInMs);
            dest.writeInt(reportEvents);
            dest.writeInt(type);
        }
        public static final Parcelable.Creator<ScanSettings> CREATOR = new Parcelable.Creator<ScanSettings>() {
            @Override public ScanSettings createFromParcel(Parcel in) {
                ScanSettings s = new ScanSettings();
                s.band = in.readInt();
                s.periodInMs = in.readInt();
                s.reportEvents = in.readInt();
                s.type = in.readInt();
                return s;
            }
            @Override public ScanSettings[] newArray(int size) { return new ScanSettings[size]; }
        };
    }

    public static class ScanData implements Parcelable {
        private final int mId, mFlags, mBucketsScanned, mScannedBands;
        private final ScanResult[] mResults;
        public ScanData(int id, int flags, ScanResult[] results) { this(id, flags, 0, WIFI_BAND_UNSPECIFIED, results); }
        public ScanData(int id, int flags, int bucketsScanned, int bandsScanned, ScanResult[] results) {
            mId = id;
            mFlags = flags;
            mBucketsScanned = bucketsScanned;
            mScannedBands = bandsScanned;
            mResults = results == null ? new ScanResult[0] : results;
        }
        public ScanData(ScanData s) { this(s.mId, s.mFlags, s.mBucketsScanned, s.mScannedBands, s.mResults.clone()); }
        public int getId() { return mId; }
        public int getFlags() { return mFlags; }
        public int getBucketsScanned() { return mBucketsScanned; }
        public int getScannedBands() { return mScannedBands; }
        public ScanResult[] getResults() { return mResults; }
        @Override public int describeContents() { return 0; }
        @Override public void writeToParcel(Parcel dest, int flags) {
            dest.writeInt(mId);
            dest.writeInt(mFlags);
            dest.writeInt(mBucketsScanned);
            dest.writeInt(mScannedBands);
            // (no results: there are never any here, and the framework's ScanResult cannot be made from a parcel)
        }
        public static final Parcelable.Creator<ScanData> CREATOR = new Parcelable.Creator<ScanData>() {
            @Override public ScanData createFromParcel(Parcel in) {
                int id = in.readInt(), flags = in.readInt(), buckets = in.readInt(), bands = in.readInt();
                return new ScanData(id, flags, buckets, bands, new ScanResult[0]);
            }
            @Override public ScanData[] newArray(int size) { return new ScanData[size]; }
        };
    }

    public java.util.List<Integer> getAvailableChannels(int band) { return new java.util.ArrayList<>(); }
    public boolean isScanning() { return false; }
    public void registerScanListener(java.util.concurrent.Executor executor, ScanListener listener) {}
    public void registerScanListener(ScanListener listener) {}
    public void unregisterScanListener(ScanListener listener) {}
    public void startBackgroundScan(ScanSettings settings, ScanListener listener) {}
    public void stopBackgroundScan(ScanListener listener) {}
    public void startScan(ScanSettings settings, ScanListener listener) {}
    public void startScan(ScanSettings settings, java.util.concurrent.Executor executor, ScanListener listener) {}
    public void stopScan(ScanListener listener) {}
    public boolean getScanResults() { return false; }
    public java.util.List<ScanResult> getSingleScanResults() { return new java.util.ArrayList<>(); }
}
