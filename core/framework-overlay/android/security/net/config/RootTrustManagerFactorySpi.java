// Megler: Android's, the TrustManagerFactory "PKIX" of NetworkSecurityConfigProvider (whose file says why this package is
// here).
package android.security.net.config;

import java.security.InvalidAlgorithmParameterException;
import java.security.KeyStore;
import java.security.KeyStoreException;
import javax.net.ssl.ManagerFactoryParameters;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactorySpi;

/** init(null): the app's configuration; init(a KeyStore): a configuration whose anchors are that store's certificates alone. */
public class RootTrustManagerFactorySpi extends TrustManagerFactorySpi {
    private ApplicationConfig mApplicationConfig;

    @Override public void engineInit(ManagerFactoryParameters spec) throws InvalidAlgorithmParameterException {
        if (!(spec instanceof ApplicationConfigParameters)) {
            throw new InvalidAlgorithmParameterException("Unsupported spec: " + spec + ". Only " + ApplicationConfigParameters.class.getName() + " supported");
        }
        mApplicationConfig = ((ApplicationConfigParameters) spec).config;
    }

    @Override public void engineInit(KeyStore ks) throws KeyStoreException {
        mApplicationConfig = ks != null ? new ApplicationConfig(new KeyStoreConfigSource(ks)) : ApplicationConfig.getDefaultInstance();
    }

    @Override public TrustManager[] engineGetTrustManagers() {
        if (mApplicationConfig == null) throw new IllegalStateException("TrustManagerFactory not initialized");
        return new TrustManager[] { mApplicationConfig.getTrustManager() };
    }

    public static final class ApplicationConfigParameters implements ManagerFactoryParameters {
        public final ApplicationConfig config;

        public ApplicationConfigParameters(ApplicationConfig config) {
            this.config = config;
        }
    }
}
