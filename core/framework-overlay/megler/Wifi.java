// Megler: new in the overlay. The phone's Wi-Fi for android.net.wifi.WifiManager (framework-overlay/android/net/wifi): its
// scans, their networks as Android's ScanResults, the broadcast that says they are there, and the network the phone is
// connected to -- over the shell's (megler/WifiNative.java, core/src/drivers/mg_java_wifi.c; the shell's player/WifiHost.cpp,
// Windows.Devices.WiFi).
//
// Why. WifiManager answered startScan false and getScanResults with an empty list, so Google Play services in an app had no
// Wi-Fi to find the phone from (its network location: Wi-Fi networks sent to Google's location server, a fix of tens of
// metres), and the app had Windows' cell fixes alone, of 300 to 660 m (Transit, 0.1.0.679: "location is still kinda awful").
// Google Play services 26 scans through WifiManager where there is no WifiScanner (gezt: startScan; its CallbackRunner, gexg,
// listens for SCAN_RESULTS_AVAILABLE_ACTION only then, and reads getScanResults), and getSystemService("wifiscanner") is null
// here.
//
// Scans. startScan asks for one and returns at once; one thread of the layer's, "megler-wifi", makes it -- Windows' scan takes
// a few seconds -- keeps its networks as the last results (getScanResults gives copies of them until the next scan), sends
// SCAN_RESULTS_AVAILABLE_ACTION to the process's receivers with EXTRA_RESULTS_UPDATED true -- false when no scan could be made,
// the results then the last ones, as Android sends it -- and calls the ScanResultsCallbacks (Android 11's). A request that
// comes while a scan is made is answered by it: its report is made after the request. A scan begins no sooner than three
// seconds after the one before began (MIN_GAP_MS); a request inside that waits for it. Google Play services asks for a scan
// and waits five seconds for it ("swslm"), and drops a result older than its request (geyw: its time against the request's),
// so a scan made before cannot answer it: each request has a scan of its own, or shares one that ends after it.
//
// The times. A result's timestamp is microseconds of elapsedRealtime at Windows' report (its Timestamp moved onto that
// clock), as Android's, which Google Play services trusts once six scans had plausible ones (gezy).
//
// Android's rules (WifiServiceImpl's enforceCanAccessScanResults). A scan and its results are the app's only while it may
// have the location: the location on (LocationService.enabled: the phone's switch) and the permission its target wants --
// ACCESS_FINE_LOCATION from Android 10, FINE or COARSE before, or Android 13's NEARBY_WIFI_DEVICES. Else startScan answers
// false and getScanResults an empty list, without a SecurityException, as there. Google Play services in the app scans for
// it: the app's permission is what counts, as Google Play services would give an app without it no location from them
// either. The Wi-Fi's own permissions (ACCESS_WIFI_STATE, CHANGE_WIFI_STATE) are not asked of the app: Google Play services
// in it calls these, and an app that asks it for the location needs none of its own (Transit names neither). A phone whose
// Wi-Fi is off, or has none, or which Windows does not let Megler have: startScan false, and the broadcast with no update.
//
// The connection. getConnectionInfo's network has its SSID, BSSID, signal and channel where the app may have the location,
// as on Android 8.1 and later, else as before ("<unknown ssid>", 02:00:00:00:00:00); the shell is asked at most every two
// seconds.
//
// The log ("MeglerWifi", 40 lines at most a run) says who asked for the first scans and what each found, a refusal and why
// (once each), and to whom results were first handed; the core says the first scans' numbers and then each minute's
// (core/src/drivers/mg_java_wifi.c).
package megler;

import android.content.Context;
import android.content.Intent;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiSsid;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

public final class Wifi {
    private static final String TAG = "MeglerWifi";
    /** The least time between two scans' beginnings: Windows' own takes two to four seconds. */
    public static final long MIN_GAP_MS = 3000;
    private static final long IDLE_MS = 60000, CONNECTION_MS = 2000, STATE_MS = 2000;
    private static final int MAX_SAID = 40;

    private static final Object LOCK = new Object();
    private static ScanResult[] sLast = new ScanResult[0];
    private static long sLastAt = -1;
    private static boolean sWanted, sScanning;
    private static long sBeganAt = Long.MIN_VALUE / 2;
    private static int sScans, sAsked, sHanded;
    private static Thread sWorker;
    private static final ArrayList<Object[]> sCallbacks = new ArrayList<>();
    private static int sState = -1;
    private static long sStateAt;
    private static WifiInfo sConnection;
    private static long sConnectionAt = Long.MIN_VALUE / 2;
    private static int sSaid;
    private static final java.util.HashSet<String> sSaidOnce = new java.util.HashSet<>();
    /* Who asked for the last scan, and Google Play services' own requests (their network location's): how many, the last when. */
    private static String sLastCaller = "?";
    private static int sGoogleAsked, sIdleSaid;
    private static long sGoogleAskedAt = -1;

    private Wifi() {}

    private static void say(String what) {
        synchronized (sSaidOnce) {
            if (sSaid >= MAX_SAID) return;
            sSaid++;
        }
        android.util.Log.i(TAG, what);
    }

    private static void sayOnce(String what) {
        synchronized (sSaidOnce) {
            if (!sSaidOnce.add(what)) return;
        }
        say(what);
    }

    /** The Wi-Fi as the shell has it (WifiNative's NONE, ON, OFF, DENIED, UNKNOWN), asked at most every two seconds. */
    public static int state() {
        long now = SystemClock.elapsedRealtime();
        synchronized (LOCK) {
            if (sState >= 0 && now - sStateAt < STATE_MS) return sState;
        }
        int s = WifiNative.state(false);
        synchronized (LOCK) {
            sState = s;
            sStateAt = now;
        }
        return s;
    }

    /** Why the app may not have a scan's results now (WifiServiceImpl's rule), or null when it may. */
    public static String refusal() {
        if (!LocationService.enabled()) return "the phone's location is switched off";
        if (Permissions.check("android.permission.ACCESS_FINE_LOCATION") == Permissions.GRANTED) return null;
        if (Manifest.targetSdk < 29 && Permissions.check("android.permission.ACCESS_COARSE_LOCATION") == Permissions.GRANTED) return null;
        if (Manifest.targetSdk >= 33 && Permissions.check("android.permission.NEARBY_WIFI_DEVICES") == Permissions.GRANTED) return null;
        return Manifest.targetSdk >= 29 ? "the app holds no ACCESS_FINE_LOCATION, which Android 10 and later want for a scan's results"
                                        : "the app holds no location permission (ACCESS_FINE_LOCATION, ACCESS_COARSE_LOCATION)";
    }

    /** The class of the code that called into WifiManager: the first frame of neither the framework's Wi-Fi nor this. */
    private static String caller() {
        for (StackTraceElement e : new Throwable().getStackTrace()) {
            String c = e.getClassName();
            if (c.equals("megler.Wifi") || c.startsWith("megler.Wifi$") || c.startsWith("android.net.wifi.") || c.startsWith("java.") || c.startsWith("dalvik."))
                continue;
            return c;
        }
        return "?";
    }

    /** WifiManager.startScan: a scan asked for (true), or refused by Android's rules or for want of the Wi-Fi (false). */
    public static boolean startScan() {
        String why = refusal();
        if (why != null) {
            sayOnce("a scan refused: " + why + " (WifiManager.startScan answers false, as on Android)");
            return false;
        }
        int s = state();
        if (s == WifiNative.NONE || s == WifiNative.OFF || s == WifiNative.DENIED) {
            sayOnce("a scan refused: " + (s == WifiNative.NONE ? "the phone has no Wi-Fi adapter" : s == WifiNative.OFF ? "the phone's Wi-Fi is switched off"
                                                                : "Windows does not let Megler have the Wi-Fi") + "; the results are the last ones");
            broadcast(false);
            return false;
        }
        boolean said, google = LocationService.byGoogle();
        String who = caller();
        synchronized (LOCK) {
            said = sAsked++ < 6;
            sLastCaller = who;
            if (google) {
                sGoogleAsked++;
                sGoogleAskedAt = SystemClock.elapsedRealtime();
            }
            if (!sScanning) sWanted = true;
            if (sWorker == null) {
                sWorker = new Thread(Wifi::work, "megler-wifi");
                sWorker.setDaemon(true);
                sWorker.start();
            }
            LOCK.notifyAll();
        }
        if (said) say("a scan asked for by " + who);
        return true;
    }

    private static void work() {
        long[] meta = new long[WifiNative.M_SIZE];
        try {
            for (;;) {
                synchronized (LOCK) {
                    long idleFrom = SystemClock.elapsedRealtime();
                    for (;;) {
                        long now = SystemClock.elapsedRealtime();
                        if (!sWanted) {
                            if (now - idleFrom >= IDLE_MS) {
                                sWorker = null;
                                // (Said: a network location that scanned and then stopped -- its alarms, its fix -- is seen so.)
                                if (sIdleSaid++ < 5)
                                    say("no scan asked for in the last " + IDLE_MS / 1000 + " s: " + sAsked + " asked for so far, " + sScans + " made, the last asked for by "
                                        + sLastCaller + (sGoogleAsked > 0 ? "; Google Play services' network location asked for " + sGoogleAsked : ""));
                                return;
                            }
                            LOCK.wait(IDLE_MS - (now - idleFrom));
                            continue;
                        }
                        long wait = sBeganAt + MIN_GAP_MS - now;
                        if (wait <= 0) break;
                        LOCK.wait(wait);
                    }
                    sWanted = false;
                    sScanning = true;
                    sBeganAt = SystemClock.elapsedRealtime();
                }
                long t0 = SystemClock.elapsedRealtime();
                Object[] nets = WifiNative.scan(meta);
                ScanResult[] results = nets == null ? null : results(nets, (int) meta[WifiNative.M_COUNT], meta[WifiNative.M_TIME]);
                int scan;
                synchronized (LOCK) {
                    sScanning = false;
                    scan = ++sScans;
                    if (results != null) {
                        sLast = results;
                        sLastAt = SystemClock.elapsedRealtime();
                    }
                }
                if (scan <= 6) {
                    if (results == null) say("scan " + scan + " could not be made: the last results stand");
                    else say("scan " + scan + " found " + results.length + " network" + (results.length == 1 ? "" : "s") + " in "
                             + (SystemClock.elapsedRealtime() - t0) + " ms" + (results.length > 0 ? ", the strongest " + strongest(results) : ""));
                }
                broadcast(results != null);
            }
        } catch (Throwable e) {
            android.util.Log.w(TAG, "the Wi-Fi's scans ended: " + e);
            synchronized (LOCK) {
                sScanning = false;
                if (sWorker == Thread.currentThread()) sWorker = null;
            }
        }
    }

    private static String strongest(ScanResult[] r) {
        ScanResult best = r[0];
        for (ScanResult x : r)
            if (x.level > best.level) best = x;
        return "\"" + best.SSID + "\" (" + best.BSSID + ") at " + best.level + " dBm, " + best.frequency + " MHz";
    }

    /** SCAN_RESULTS_AVAILABLE_ACTION to the process's receivers, and the ScanResultsCallbacks after a scan that was made. */
    private static void broadcast(boolean updated) {
        Intent i = new Intent(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
        i.putExtra(WifiManager.EXTRA_RESULTS_UPDATED, updated);
        Context app = AppRunner.application();
        if (app != null) Broadcasts.send(app, i);
        if (!updated) return;
        ArrayList<Object[]> now;
        synchronized (sCallbacks) { now = new ArrayList<>(sCallbacks); }
        for (Object[] c : now) {
            final WifiManager.ScanResultsCallback cb = (WifiManager.ScanResultsCallback) c[1];
            ((Executor) c[0]).execute(cb::onScanResultsAvailable);
        }
    }

    /** WifiManager.registerScanResultsCallback and unregisterScanResultsCallback. */
    public static void register(Executor executor, WifiManager.ScanResultsCallback callback) {
        if (executor == null || callback == null) throw new IllegalArgumentException("executor and callback must not be null");
        synchronized (sCallbacks) {
            unregister(callback);
            sCallbacks.add(new Object[] { executor, callback });
        }
    }

    public static void unregister(WifiManager.ScanResultsCallback callback) {
        synchronized (sCallbacks) {
            for (int i = sCallbacks.size() - 1; i >= 0; i--)
                if (sCallbacks.get(i)[1] == callback) sCallbacks.remove(i);
        }
    }

    /** WifiManager.getScanResults: copies of the last scan's results, or none where the app may not have them. */
    public static List<ScanResult> results() {
        String why = refusal();
        ArrayList<ScanResult> out = new ArrayList<>();
        if (why != null) {
            sayOnce("scan results withheld: " + why + " (getScanResults answers an empty list, as on Android)");
            return out;
        }
        boolean said;
        long age;
        synchronized (LOCK) {
            for (ScanResult r : sLast) out.add(new ScanResult(r));
            said = sLastAt >= 0 && sHanded++ < 4;
            age = sLastAt < 0 ? -1 : SystemClock.elapsedRealtime() - sLastAt;
        }
        if (said) say(out.size() + " result" + (out.size() == 1 ? "" : "s") + " of a scan " + age + " ms old handed to " + caller());
        return out;
    }

    /** "aa:bb:cc:dd:ee:ff", as Android writes a BSSID, of a 48-bit number. */
    public static String mac(long v) {
        StringBuilder b = new StringBuilder(17);
        for (int i = 5; i >= 0; i--) {
            int x = (int) (v >>> (i * 8)) & 0xff;
            b.append(Character.forDigit(x >> 4, 16)).append(Character.forDigit(x & 15, 16));
            if (i > 0) b.append(':');
        }
        return b.toString();
    }

    /** A scan's networks (WifiNative.scan's arrays) as Android's ScanResults, at the report's time. */
    static ScanResult[] results(Object[] nets, int n, long reportNs) {
        long[] bssids = (long[]) nets[0];
        byte[] ssids = (byte[]) nets[1];
        int[] v = (int[]) nets[2];
        // (Windows' report time moved onto elapsedRealtime's clock: as long before now as it was; never after now.)
        long nowWallMs = System.currentTimeMillis(), nowUs = SystemClock.elapsedRealtimeNanos() / 1000;
        long ageUs = reportNs > 0 ? Math.max(0, nowWallMs * 1000 - reportNs / 1000) : 0;
        long stampUs = Math.max(1, nowUs - ageUs), seenMs = reportNs > 0 ? Math.min(nowWallMs, reportNs / 1000000) : nowWallMs;
        n = Math.max(0, Math.min(n, bssids.length));
        ScanResult[] out = new ScanResult[n];
        for (int i = 0; i < n; i++) {
            int at = i * WifiNative.I_STRIDE, size = Math.max(0, Math.min(32, v[at + WifiNative.I_SSID_SIZE]));
            byte[] octets = java.util.Arrays.copyOfRange(ssids, i * 32, i * 32 + size);
            ScanResult r = new ScanResult();
            WifiSsid w = WifiSsid.fromBytes(octets);
            r.setWifiSsid(w);
            r.SSID = w.meglerText();
            r.BSSID = mac(bssids[i]);
            r.level = v[at + WifiNative.I_RSSI];
            r.frequency = v[at + WifiNative.I_FREQUENCY];
            r.capabilities = capabilities(v[at + WifiNative.I_SECURITY], v[at + WifiNative.I_ADHOC] != 0);
            r.setWifiStandard(standard(v[at + WifiNative.I_PHY]));
            r.channelWidth = width(v[at + WifiNative.I_PHY], r.frequency);
            r.centerFreq0 = r.frequency;
            r.timestamp = stampUs;
            r.seen = seenMs;
            out[i] = r;
        }
        return out;
    }

    /**
     * Android's capabilities of a network ("[WPA2-PSK-CCMP][RSN-PSK-CCMP][ESS]"), of Windows' authentication and cipher, as
     * Android's InformationElementUtil.Capabilities writes them: the key management and cipher of each protocol, then [ESS]
     * for an access point or [IBSS] for an ad hoc network.
     */
    public static String capabilities(int security, boolean adhoc) {
        int auth = security & 0xff, cipher = security >> 8 & 0xff;
        String c = cipher == WifiNative.CIPHER_TKIP ? "TKIP" : cipher == WifiNative.CIPHER_GCMP ? "GCMP-128" : "CCMP";
        String s;
        switch (auth) {
        case WifiNative.AUTH_OPEN: s = cipher == WifiNative.CIPHER_WEP ? "[WEP]" : ""; break;
        case WifiNative.AUTH_SHARED_KEY: s = "[WEP]"; break;
        case WifiNative.AUTH_WPA: s = "[WPA-EAP-" + c + "]"; break;
        case WifiNative.AUTH_WPA_PSK: s = "[WPA-PSK-" + c + "]"; break;
        case WifiNative.AUTH_RSNA: s = "[WPA2-EAP-" + c + "][RSN-EAP-" + c + "]"; break;
        case WifiNative.AUTH_RSNA_PSK: s = "[WPA2-PSK-" + c + "][RSN-PSK-" + c + "]"; break;
        case WifiNative.AUTH_WPA3: s = "[RSN-EAP_SUITE_B_192-" + (cipher == WifiNative.CIPHER_GCMP ? "GCMP-256" : c) + "]"; break;
        case WifiNative.AUTH_WPA3_SAE: s = "[RSN-SAE-" + c + "]"; break;
        case WifiNative.AUTH_OWE: s = "[RSN-OWE-" + c + "]"; break;
        default: s = cipher == WifiNative.CIPHER_NONE || cipher == WifiNative.CIPHER_UNKNOWN ? "" : "[WPA2-PSK-" + c + "][RSN-PSK-" + c + "]"; break;
        }
        return s + (adhoc ? "[IBSS]" : "[ESS]");
    }

    /** ScanResult.WIFI_STANDARD_* of Windows' physical layer. */
    public static int standard(int phy) {
        switch (phy) {
        case WifiNative.PHY_HT: return ScanResult.WIFI_STANDARD_11N;
        case WifiNative.PHY_VHT: return ScanResult.WIFI_STANDARD_11AC;
        case WifiNative.PHY_HE: return ScanResult.WIFI_STANDARD_11AX;
        case WifiNative.PHY_DMG: return ScanResult.WIFI_STANDARD_11AD;
        case WifiNative.PHY_UNKNOWN: return 0;
        default: return ScanResult.WIFI_STANDARD_LEGACY;
        }
    }

    /** A channel's width, which Windows does not say: 20 MHz, and 80 MHz for 802.11ac and ax on 5 GHz, their usual. */
    public static int width(int phy, int mhz) {
        return (phy == WifiNative.PHY_VHT || phy == WifiNative.PHY_HE) && mhz >= 4900 ? ScanResult.CHANNEL_WIDTH_80MHZ : ScanResult.CHANNEL_WIDTH_20MHZ;
    }

    /**
     * WifiManager.getConnectionInfo: the network the phone is connected to, with its name, BSSID, signal and channel where the
     * app may have the location (Android 8.1's rule), else as Android tells an app without it; `ip` its address.
     */
    public static WifiInfo connection(int ip) {
        if (refusal() != null) return WifiInfo.meglerConnected(ip);
        long now = SystemClock.elapsedRealtime();
        synchronized (LOCK) {
            if (sConnection != null && now - sConnectionAt < CONNECTION_MS && sConnection.getIpAddress() == ip) return sConnection;
        }
        long[] meta = new long[WifiNative.M_SIZE];
        Object[] c = WifiNative.connection(meta);
        WifiInfo w;
        if (c == null) {
            w = WifiInfo.meglerConnected(ip);
        } else {
            int[] v = (int[]) c[2];
            byte[] octets = java.util.Arrays.copyOfRange((byte[]) c[1], 0, Math.max(0, Math.min(32, v[WifiNative.I_SSID_SIZE])));
            long bssid = ((long[]) c[0])[0];
            w = WifiInfo.meglerConnected(ip, WifiSsid.fromBytes(octets), bssid != 0 ? mac(bssid) : "02:00:00:00:00:00", v[WifiNative.I_RSSI],
                                         v[WifiNative.I_FREQUENCY], standard(v[WifiNative.I_PHY]));
        }
        synchronized (LOCK) {
            sConnection = w;
            sConnectionAt = now;
        }
        return w;
    }

    /** For megler.LocationService: how many scans Google Play services' own code asked for, and when the last (elapsedRealtime; -1: none). */
    public static int googleScans() {
        synchronized (LOCK) { return sGoogleAsked; }
    }

    public static long googleScanAt() {
        synchronized (LOCK) { return sGoogleAskedAt; }
    }

    /** For megler.LocationService: how many networks the last scan heard, and how long ago it was (ms; -1: none yet). */
    public static int lastCount() {
        synchronized (LOCK) { return sLastAt < 0 ? -1 : sLast.length; }
    }

    public static long lastAge() {
        synchronized (LOCK) { return sLastAt < 0 ? -1 : SystemClock.elapsedRealtime() - sLastAt; }
    }
}
