// Overlay of core/framework/gen-src/android/security/net/config/ApplicationConfig.java (tools/build-framework.sh), a
// signature alone there: Android's, the app's network security configuration for each host (NetworkSecurityConfigProvider.java
// says why this package is here).
package android.security.net.config;

import android.util.Pair;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import javax.net.ssl.X509TrustManager;

/** An app's network security configuration: its base configuration, and those of the domains it names. Read at its first use. */
public final class ApplicationConfig {
    private static ApplicationConfig sInstance;
    private static final Object sLock = new Object();

    private Set<Pair<Domain, NetworkSecurityConfig>> mConfigs;
    private NetworkSecurityConfig mDefaultConfig;
    private X509TrustManager mTrustManager;
    private ConfigSource mConfigSource;
    private boolean mInitialized;
    private final Object mLock = new Object();

    public ApplicationConfig(ConfigSource configSource) {
        mConfigSource = configSource;
    }

    /** Whether a <domain-config> names any host. */
    public boolean hasPerDomainConfigs() {
        ensureInitialized();
        return mConfigs != null && !mConfigs.isEmpty();
    }

    /**
     * The configuration for a host: the one whose <domain> is the host itself, else the one of the longest domain that has the
     * host among its subdomains (includeSubdomains), else the base one. Names are compared in lower case, a final dot dropped.
     */
    public NetworkSecurityConfig getConfigForHostname(String hostname) {
        ensureInitialized();
        if (hostname == null || hostname.isEmpty() || mConfigs == null) return mDefaultConfig;
        if (hostname.charAt(0) == '.') throw new IllegalArgumentException("hostname must not begin with a .");
        hostname = hostname.toLowerCase(Locale.US);
        if (hostname.charAt(hostname.length() - 1) == '.') hostname = hostname.substring(0, hostname.length() - 1);
        Pair<Domain, NetworkSecurityConfig> best = null;
        for (Pair<Domain, NetworkSecurityConfig> entry : mConfigs) {
            Domain domain = entry.first;
            if (domain.hostname.equals(hostname)) return entry.second;
            if (domain.subdomainsIncluded && hostname.endsWith(domain.hostname)
                    && hostname.charAt(hostname.length() - domain.hostname.length() - 1) == '.'
                    && (best == null || domain.hostname.length() > best.first.hostname.length())) best = entry;
        }
        return best != null ? best.second : mDefaultConfig;
    }

    /** The trust manager that chooses the host's configuration for each check (RootTrustManager). */
    public X509TrustManager getTrustManager() {
        ensureInitialized();
        return mTrustManager;
    }

    /** Whether cleartext is permitted to every host: only when every configuration permits it, as Android answers. */
    public boolean isCleartextTrafficPermitted() {
        ensureInitialized();
        if (mConfigs != null) {
            for (Pair<Domain, NetworkSecurityConfig> entry : mConfigs) if (!entry.second.isCleartextTrafficPermitted()) return false;
        }
        return mDefaultConfig.isCleartextTrafficPermitted();
    }

    public boolean isCleartextTrafficPermitted(String hostname) {
        return getConfigForHostname(hostname).isCleartextTrafficPermitted();
    }

    public void handleTrustStorageUpdate() {
        synchronized (mLock) {
            if (!mInitialized) return;
            mDefaultConfig.handleTrustStorageUpdate();
            if (mConfigs != null) {
                Set<NetworkSecurityConfig> updated = new HashSet<>();
                for (Pair<Domain, NetworkSecurityConfig> entry : mConfigs) if (updated.add(entry.second)) entry.second.handleTrustStorageUpdate();
            }
        }
    }

    private void ensureInitialized() {
        synchronized (mLock) {
            if (mInitialized) return;
            mConfigs = mConfigSource.getPerDomainConfigs();
            mDefaultConfig = mConfigSource.getDefaultConfig();
            mConfigSource = null;
            mTrustManager = new RootTrustManager(this);
            mInitialized = true;
        }
    }

    public static void setDefaultInstance(ApplicationConfig config) {
        synchronized (sLock) { sInstance = config; }
    }

    /** The app's own configuration, once NetworkSecurityConfigProvider.install has made it; null before. */
    public static ApplicationConfig getDefaultInstance() {
        synchronized (sLock) { return sInstance; }
    }
}
