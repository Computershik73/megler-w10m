package megler.test.wifi;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Looper;
import android.os.Parcel;
import android.os.SystemClock;
import android.util.Log;
import android.widget.TextView;

import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * The phone's Wi-Fi as an app -- and Google Play services' network location in it -- uses it, on the test's stand-in for the
 * shell's Wi-Fi (core/tests/mg_wifi_standin.c: four networks, the first a little weaker at each scan, the phone connected to
 * the first). A thread of the app's goes through the steps and says how each went; the broadcasts and the callback come on
 * the main thread into a queue that the steps read.
 */
public class Main extends Activity {
    private static final String T = "WifiTest";
    private int ok, failed;
    private String detail = "";
    private WifiManager wifi;
    /* What was heard, in order: "results <updated> <main|other>", "callback <main|other>". */
    private final BlockingQueue<String> heard = new LinkedBlockingQueue<>();
    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            heard.add("results " + intent.getBooleanExtra(WifiManager.EXTRA_RESULTS_UPDATED, false) + " " + (onMain() ? "main" : "other"));
        }
    };

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Wi-Fi");
        setContentView(v);
        wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        registerReceiver(receiver, new IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION));
        new Thread(this::steps, "steps").start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        unregisterReceiver(receiver);
        Log.i(T, "onDestroy");
    }

    private interface Step { boolean run() throws Exception; }

    private void step(String name, Step s) {
        boolean good;
        detail = "";
        try {
            good = s.run();
        } catch (Throwable e) {
            Log.e(T, name + " threw", e);
            good = false;
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED " + detail));
    }

    private static boolean onMain() { return Looper.myLooper() == Looper.getMainLooper(); }

    /** The next thing heard that begins with `prefix`, within `seconds`; "" for none (what came before it is said and dropped). */
    private String await(String prefix, int seconds) throws InterruptedException {
        long until = SystemClock.elapsedRealtime() + seconds * 1000L;
        for (;;) {
            long left = until - SystemClock.elapsedRealtime();
            String s = left > 0 ? heard.poll(left, TimeUnit.MILLISECONDS) : null;
            if (s == null) {
                detail += " (nothing like \"" + prefix + "\" heard)";
                return "";
            }
            if (s.startsWith(prefix)) return s;
            detail += " [" + s + "]";
        }
    }

    private static Object call(Object o, String method) throws Exception { return o.getClass().getMethod(method).invoke(o); }

    private static String describe(ScanResult r) {
        return r.BSSID + " \"" + r.SSID + "\" " + r.level + " dBm " + r.frequency + " MHz " + r.capabilities + " w" + r.channelWidth + " t" + r.timestamp;
    }

    private long scanAskedAt;

    private void steps() {
        step("permission", () -> {
            detail = "fine=" + checkSelfPermission("android.permission.ACCESS_FINE_LOCATION") + " always=" + wifi.isScanAlwaysAvailable() + " enabled="
                     + wifi.isWifiEnabled();
            return checkSelfPermission("android.permission.ACCESS_FINE_LOCATION") == PackageManager.PERMISSION_GRANTED && !wifi.isScanAlwaysAvailable()
                && wifi.isWifiEnabled();
        });
        /* A scan asked for: true at once, then the broadcast on the main thread, the results updated. */
        step("scan", () -> {
            scanAskedAt = SystemClock.elapsedRealtime();
            boolean asked = wifi.startScan();
            String got = await("results", 60);
            detail += " asked=" + asked + " got=" + got;
            return asked && got.equals("results true main");
        });
        /*
         * Its networks, as the stand-in's shell found them: Android's BSSIDs, the names as text (none for a hidden one), the
         * signals, the channels, Android's capabilities of Windows' security, the widths, and the time each was heard, on
         * elapsedRealtime's clock in microseconds, after the scan was asked for and not after now.
         */
        step("results", () -> {
            List<ScanResult> r = wifi.getScanResults();
            long now = SystemClock.elapsedRealtime();
            detail = "n=" + r.size();
            if (r.size() != 4) return false;
            for (ScanResult x : r) detail += " | " + describe(x);
            ScanResult a = r.get(0), b = r.get(1), c = r.get(2), d = r.get(3);
            Object ssid = call(a, "getWifiSsid"), standard = call(a, "getWifiStandard");
            detail += " wifiSsid=" + ssid + " standard=" + standard;
            return "00:11:22:33:44:55".equals(a.BSSID) && "Megler Test".equals(a.SSID) && a.level == -46 && a.frequency == 2437
                && "[WPA2-PSK-CCMP][RSN-PSK-CCMP][ESS]".equals(a.capabilities) && a.channelWidth == ScanResult.CHANNEL_WIDTH_20MHZ && a.centerFreq0 == 2437
                && a.timestamp >= scanAskedAt * 1000 && a.timestamp <= now * 1000 && "\"Megler Test\"".equals(String.valueOf(ssid))
                && Integer.valueOf(4).equals(standard)
                && "0a:bb:cc:dd:ee:ff".equals(b.BSSID) && "Кафе".equals(b.SSID) && b.level == -67 && b.frequency == 5180
                && "[ESS]".equals(b.capabilities) && b.channelWidth == ScanResult.CHANNEL_WIDTH_80MHZ
                && "02:12:34:56:78:ab".equals(c.BSSID) && "".equals(c.SSID) && "[RSN-SAE-CCMP][ESS]".equals(c.capabilities) && c.level == -80
                && "66:55:44:33:22:11".equals(d.BSSID) && "Printer_nomap".equals(d.SSID) && "[WPA-PSK-TKIP][IBSS]".equals(d.capabilities)
                && a.timestamp == d.timestamp;
        });
        /* What getScanResults gives is the caller's own: a change to it is not the next caller's. */
        step("copies", () -> {
            wifi.getScanResults().get(0).level = 0;
            int level = wifi.getScanResults().get(0).level;
            detail = "level=" + level;
            return level == -46;
        });
        /* A second scan asked for at once: made no sooner than three seconds after the first began, its results the new ones. */
        step("second-scan", () -> {
            boolean asked = wifi.startScan();
            String got = await("results", 60);
            long since = SystemClock.elapsedRealtime() - scanAskedAt;
            int level = wifi.getScanResults().get(0).level;
            detail = "asked=" + asked + " got=" + got + " since=" + since + " level=" + level;
            return asked && got.equals("results true main") && since >= 2900 && level == -47;
        });
        /* Two requests at once are answered by one scan, and its callback (Android 11's) is called on the executor given. */
        step("joined", () -> {
            final BlockingQueue<String> callbacks = new LinkedBlockingQueue<>();
            WifiManager.ScanResultsCallback cb = new WifiManager.ScanResultsCallback() {
                @Override
                public void onScanResultsAvailable() { callbacks.add("callback " + (onMain() ? "main" : "other")); }
            };
            wifi.registerScanResultsCallback(getMainExecutor(), cb);
            boolean one = wifi.startScan(), two = wifi.startScan();
            String got = await("results", 60);
            String again = heard.poll(5, TimeUnit.SECONDS);
            String called = callbacks.poll(10, TimeUnit.SECONDS);
            wifi.unregisterScanResultsCallback(cb);
            int level = wifi.getScanResults().get(0).level;
            detail = "one=" + one + " two=" + two + " got=" + got + " again=" + again + " called=" + called + " level=" + level;
            return one && two && got.equals("results true main") && again == null && "callback main".equals(called) && level == -48;
        });
        /* The network the phone is connected to, as an app that may have the location is told of it. */
        step("connection", () -> {
            WifiInfo w = wifi.getConnectionInfo();
            detail = w == null ? "null" : w.getSSID() + " " + w.getBSSID() + " " + w.getRssi() + " " + w.getFrequency();
            return w != null && "\"Megler Test\"".equals(w.getSSID()) && "00:11:22:33:44:55".equals(w.getBSSID()) && w.getRssi() == -45
                && w.getFrequency() == 2437;
        });
        /* A ScanResult through a Parcel, as an app hands it to another or keeps it. */
        step("parcel", () -> {
            ScanResult a = wifi.getScanResults().get(1);
            Parcel p = Parcel.obtain();
            a.writeToParcel(p, 0);
            p.setDataPosition(0);
            // (Its CREATOR by reflection: public from Android 11, and the SDK the app is built with is 29's.)
            ScanResult back = (ScanResult) ((android.os.Parcelable.Creator<?>) ScanResult.class.getField("CREATOR").get(null)).createFromParcel(p);
            p.recycle();
            if (back == a) return false;
            detail = describe(back) + " wifiSsid=" + call(back, "getWifiSsid");
            return a.BSSID.equals(back.BSSID) && a.SSID.equals(back.SSID) && a.level == back.level && a.frequency == back.frequency
                && a.timestamp == back.timestamp && a.capabilities.equals(back.capabilities) && a.channelWidth == back.channelWidth
                && "\"Кафе\"".equals(String.valueOf(call(back, "getWifiSsid")));
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }
}
