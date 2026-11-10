package android.net.wifi;

/** What the test app calls of Android 11's WifiManager, which the SDK it is built with (29) has not got all of. */
public class WifiManager {
    public static final String SCAN_RESULTS_AVAILABLE_ACTION = "android.net.wifi.SCAN_RESULTS";
    public static final String EXTRA_RESULTS_UPDATED = "resultsUpdated";
    public boolean startScan() { throw new RuntimeException("Stub!"); }
    public java.util.List<ScanResult> getScanResults() { throw new RuntimeException("Stub!"); }
    public WifiInfo getConnectionInfo() { throw new RuntimeException("Stub!"); }
    public boolean isScanAlwaysAvailable() { throw new RuntimeException("Stub!"); }
    public boolean isWifiEnabled() { throw new RuntimeException("Stub!"); }
    public void registerScanResultsCallback(java.util.concurrent.Executor executor, ScanResultsCallback callback) { throw new RuntimeException("Stub!"); }
    public void unregisterScanResultsCallback(ScanResultsCallback callback) { throw new RuntimeException("Stub!"); }
    public abstract static class ScanResultsCallback {
        public ScanResultsCallback() { throw new RuntimeException("Stub!"); }
        public abstract void onScanResultsAvailable();
    }
}
