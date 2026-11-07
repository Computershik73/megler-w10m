// Megler: Android's, the configuration of a KeyStore an app gives TrustManagerFactory.init (NetworkSecurityConfigProvider.java
// says why this package is here).
package android.security.net.config;

import android.util.Pair;
import java.security.KeyStore;
import java.util.Set;

/** One configuration for every host, whose anchors are the store's certificates and nothing else, with no pins. */
class KeyStoreConfigSource implements ConfigSource {
    private final NetworkSecurityConfig mConfig;

    KeyStoreConfigSource(KeyStore ks) {
        mConfig = new NetworkSecurityConfig.Builder()
                .addCertificatesEntryRef(new CertificatesEntryRef(new KeyStoreCertificateSource(ks), false))
                .build();
    }

    @Override public Set<Pair<Domain, NetworkSecurityConfig>> getPerDomainConfigs() { return null; }

    @Override public NetworkSecurityConfig getDefaultConfig() { return mConfig; }
}
