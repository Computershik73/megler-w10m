// Megler: Android's, where an ApplicationConfig's configurations come from (NetworkSecurityConfigProvider.java says why this
// package is here).
package android.security.net.config;

import android.util.Pair;
import java.util.Set;

/** The configurations of an app: its base one, and each <domain> named with the configuration that names it (null: none). */
public interface ConfigSource {
    Set<Pair<Domain, NetworkSecurityConfig>> getPerDomainConfigs();
    NetworkSecurityConfig getDefaultConfig();
}
