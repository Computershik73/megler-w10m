// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android 14's
// system API: the service through which an app of the system's -- Google Play services' tethering module on a phone with
// Google apps -- tells the Wi-Fi settings of the networks the user's other devices can share: hotspots they can turn on,
// Wi-Fi networks they know. The settings bind the service the system's configuration names, hear what it sets, and ask it
// to connect. Play services' TetherService makes one in its onCreate and does not carry the class: here that was
// NoClassDefFoundError, "Starting com.google.android.gms.magictether.service.TetherService failed" in the logs of the apps
// that host Play services.
package android.net.wifi.sharedconnectivity.service;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.sharedconnectivity.app.HotspotNetwork;
import android.net.wifi.sharedconnectivity.app.HotspotNetworkConnectionStatus;
import android.net.wifi.sharedconnectivity.app.KnownNetwork;
import android.net.wifi.sharedconnectivity.app.KnownNetworkConnectionStatus;
import android.net.wifi.sharedconnectivity.app.SharedConnectivitySettingsState;
import android.os.Binder;
import android.os.IBinder;

import java.util.List;

/**
 * As on an Android whose configuration names no shared connectivity service (AOSP's own, where
 * config_sharedConnectivityServicePackage is empty), nothing binds this service here: Megler has no Wi-Fi settings that
 * would show another device's networks. So what the service sets has nobody to be told to, and its on... methods are
 * never called. The binder onBind gives stands for the settings' interface and answers none of its calls.
 */
public abstract class SharedConnectivityService extends Service {
    private static final String DESCRIPTOR = "android.net.wifi.sharedconnectivity.service.ISharedConnectivityService";

    @Override public final IBinder onBind(Intent intent) {
        Binder binder = new Binder();
        binder.attachInterface(null, DESCRIPTOR);
        onBind();
        return binder;
    }

    /** Told of each onBind (Android's, for its tests). */
    public void onBind() {}

    /** The hotspots the user's other devices offer now. */
    public final void setHotspotNetworks(List<HotspotNetwork> networks) {}
    /** The Wi-Fi networks the user's other devices know now. */
    public final void setKnownNetworks(List<KnownNetwork> networks) {}
    /** The service's own settings. */
    public final void setSettingsState(SharedConnectivitySettingsState settingsState) {}
    /** How the connection the settings asked for (onConnectHotspotNetwork) goes. */
    public final void updateHotspotNetworkConnectionStatus(HotspotNetworkConnectionStatus status) {}
    /** How the saving the settings asked for (onConnectKnownNetwork) went. */
    public final void updateKnownNetworkConnectionStatus(KnownNetworkConnectionStatus status) {}

    /** The settings ask to turn the hotspot on and connect to it. */
    public abstract void onConnectHotspotNetwork(HotspotNetwork network);
    /** The settings ask to leave the hotspot. */
    public abstract void onDisconnectHotspotNetwork(HotspotNetwork network);
    /** The settings ask to save the network on this device. */
    public abstract void onConnectKnownNetwork(KnownNetwork network);
    /** The settings ask to forget the network. */
    public abstract void onForgetKnownNetwork(KnownNetwork network);

    /** Whether the system shows this app's hotspots: it names no shared connectivity service, so no. */
    public static boolean areHotspotNetworksEnabledForService(Context context) { return false; }
    /** Whether the system shows this app's known networks: it names no shared connectivity service, so no. */
    public static boolean areKnownNetworksEnabledForService(Context context) { return false; }
}
