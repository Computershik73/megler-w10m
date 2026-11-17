package android.net.wifi.sharedconnectivity.service;

import android.net.wifi.sharedconnectivity.app.HotspotNetwork;
import android.net.wifi.sharedconnectivity.app.HotspotNetworkConnectionStatus;
import android.net.wifi.sharedconnectivity.app.KnownNetwork;
import android.net.wifi.sharedconnectivity.app.KnownNetworkConnectionStatus;
import android.net.wifi.sharedconnectivity.app.SharedConnectivitySettingsState;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step shared-connectivity calls of Android 14's system API. */
public abstract class SharedConnectivityService extends android.app.Service {
    public final android.os.IBinder onBind(android.content.Intent intent) { return null; }
    public final void setHotspotNetworks(java.util.List<HotspotNetwork> networks) {}
    public final void setKnownNetworks(java.util.List<KnownNetwork> networks) {}
    public final void setSettingsState(SharedConnectivitySettingsState settingsState) {}
    public final void updateHotspotNetworkConnectionStatus(HotspotNetworkConnectionStatus status) {}
    public final void updateKnownNetworkConnectionStatus(KnownNetworkConnectionStatus status) {}
    public abstract void onConnectHotspotNetwork(HotspotNetwork network);
    public abstract void onDisconnectHotspotNetwork(HotspotNetwork network);
    public abstract void onConnectKnownNetwork(KnownNetwork network);
    public abstract void onForgetKnownNetwork(KnownNetwork network);
    public static boolean areHotspotNetworksEnabledForService(android.content.Context context) { return false; }
    public static boolean areKnownNetworksEnabledForService(android.content.Context context) { return false; }
}
