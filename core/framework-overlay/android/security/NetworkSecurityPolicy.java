// Overlay of core/framework/gen-src/android/security/NetworkSecurityPolicy.java (tools/build-framework.sh).
// The original's is a generated signature: cleartext traffic was never permitted, to any host. This one answers as Android
// does, from the app's network security configuration, the one TLS goes by too (android.security.net.config.ApplicationConfig,
// made by NetworkSecurityConfigProvider at the app's start):
//  - with android:networkSecurityConfig, that XML resource decides: for a host, the most specific <domain-config> whose
//    <domain> names it (includeSubdomains for its subdomains), nested ones taking what they do not say from the one around
//    them, then from the <base-config>; what none says is the default below. Without a host, cleartext is permitted only
//    when every configuration permits it;
//  - without one, android:usesCleartextTraffic;
//  - without either, the default: permitted for an app written for Android 8.1 or earlier (targetSdkVersion 27 or less),
//    not for a later one.
// (Android ignores usesCleartextTraffic when there is a configuration; so does this.)
//
// Why. Android's WebView asks this class for every http:// request (Chromium's AwContentBrowserClient, by host) and refuses
// the request with net::ERR_CLEARTEXT_NOT_PERMITTED when it says no. Every http page failed to load, in every app, whatever
// its manifest said.
package android.security;

import android.content.Context;
import android.security.net.config.ApplicationConfig;
import android.security.net.config.ManifestConfigSource;

public class NetworkSecurityPolicy {
    private static final NetworkSecurityPolicy sInstance = new NetworkSecurityPolicy();
    /** The app's configuration made here, when this is asked before the app's start has made it. */
    private static ApplicationConfig sOwn;
    /** What setCleartextTrafficPermitted said, for every host (null: the configuration answers). */
    private volatile Boolean mPermitted;

    protected NetworkSecurityPolicy() {}

    public static NetworkSecurityPolicy getInstance() {
        return sInstance;
    }

    public boolean isCleartextTrafficPermitted() {
        Boolean said = mPermitted;
        return said != null ? said : config().isCleartextTrafficPermitted();
    }

    public boolean isCleartextTrafficPermitted(String hostname) {
        Boolean said = mPermitted;
        return said != null ? said : config().isCleartextTrafficPermitted(hostname);
    }

    /** Android's, hidden: cleartext permitted, or not, to every host from now on. */
    public void setCleartextTrafficPermitted(boolean permitted) { mPermitted = permitted; }

    public boolean isCertificateTransparencyVerificationRequired(String hostname) { return false; }

    public void handleTrustStorageUpdate() {
        ApplicationConfig config = ApplicationConfig.getDefaultInstance();
        if (config != null) config.handleTrustStorageUpdate();
    }

    /** The configuration of the app's own package (no other app's manifest is read here). */
    public static ApplicationConfig getApplicationConfigForPackage(Context context, String packageName) {
        if (context == null || packageName == null || !packageName.equals(context.getPackageName())) return null;
        return new ApplicationConfig(new ManifestConfigSource(context));
    }

    private static ApplicationConfig config() {
        ApplicationConfig config = ApplicationConfig.getDefaultInstance();
        if (config != null) return config;
        synchronized (NetworkSecurityPolicy.class) {
            if (sOwn == null) sOwn = new ApplicationConfig(new ManifestConfigSource(megler.ContextImpl.app()));
            return sOwn;
        }
    }
}
