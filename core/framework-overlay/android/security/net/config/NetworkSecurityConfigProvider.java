// Megler: Android's network security configuration of an app (its manifest's android:networkSecurityConfig) for TLS. This
// package is new to the original's framework, which had its ApplicationConfig as a signature alone (tools/build-framework.sh).
//
// Why. Russian apps trust certificate authorities Android 9 does not have: the Russian Trusted Root CA, whose Sub CA issued
// api.ozon.ru's certificate, and authorities of their own. They say so in the XML resource their manifest names: under
// <trust-anchors>, <certificates src="@raw/..."/> beside src="system", for every host (<base-config>) or for some
// (<domain-config>). The framework read that file for cleartext alone (android.security.NetworkSecurityPolicy), and TLS
// trusted Android 9's authorities and nothing else. Ozon (ru.ozon.app.android) failed its requests to api.ozon.ru with
// "Trust anchor for certification path not found"; VTB's bank (ru.vtb24.mobilebanking.android) names the same root.
//
// How, as Android does it. Before the app's code runs (megler.AppRunner.run, where Android 9's ActivityThread calls
// install), the app's configuration becomes the default ApplicationConfig, read from the manifest and the XML at its first
// use (ManifestConfigSource, XmlConfigSource). This provider goes first in the list of security providers. Its
// TrustManagerFactory "PKIX" (alias "X509") gives a RootTrustManager over that configuration: conscrypt's default context,
// Android's HttpsURLConnection, and an app's TrustManagerFactory.getInstance(getDefaultAlgorithm()).init(null) (OkHttp's,
// Cronet's, the WebView's) all get it. For each connection it takes the host -- the handshake session's peer, or the host
// given to checkServerTrusted(chain, authType, host), which X509TrustManagerExtensions calls -- and the configuration whose
// <domain> names it most closely, else the base one. That configuration's trust manager (NetworkSecurityTrustManager) is
// conscrypt's TrustManagerImpl over the configuration's anchors, then its pins (<pin-set>, with its expiration, and
// anchors that override pins). The anchors: "system", the Android root's etc/security/cacerts; "user", none (Megler has no
// store a user adds certificates to); "@raw/...", the app's files, PEM or DER, one certificate or several. A KeyStore
// given to init is a configuration of its own whose only anchors are that store's certificates (KeyStoreConfigSource).
// <debug-overrides> count only for an app whose manifest says android:debuggable.
//
// Where Megler differs. A configuration that cannot be read is said in the log and the platform's default is used (Android's
// authorities) where Android would end the app; a certificate file that cannot be read counts as empty. The provider is not
// put in where TLS is not conscrypt's (its native library missing: the runtime's stand-in has no TLS, and its trust manager
// says so). libcore's NetworkSecurityPolicy, which Android also sets here, is left as it was, so HttpURLConnection is not
// refused cleartext; android.security.NetworkSecurityPolicy answers from this configuration.
package android.security.net.config;

import android.content.Context;
import android.util.Log;
import java.security.Provider;
import java.security.Security;

/** The provider of the app's TrustManagerFactory (see the top of the file). */
public final class NetworkSecurityConfigProvider extends Provider {
    private static final String TAG = "NetworkSecurityConfig";
    private static final String PREFIX = "android.security.net.config.";

    public NetworkSecurityConfigProvider() {
        super("AndroidNSSP", 1.0, "Android Network Security Policy Provider");
        put("TrustManagerFactory.PKIX", PREFIX + "RootTrustManagerFactorySpi");
        put("Alg.Alias.TrustManagerFactory.X509", "PKIX");
    }

    /** The app's configuration made the default, and the provider put first (see the top of the file). */
    public static void install(Context context) {
        try {
            ApplicationConfig config = new ApplicationConfig(new ManifestConfigSource(context));
            ApplicationConfig.setDefaultInstance(config);
            Provider tls = Security.getProvider("AndroidOpenSSL");
            if (tls == null || !"com.android.org.conscrypt.OpenSSLProvider".equals(tls.getClass().getName())) {
                Log.i(TAG, "TLS is not conscrypt's here: the network security configuration answers for cleartext alone");
                return;
            }
            int pos = Security.insertProviderAt(new NetworkSecurityConfigProvider(), 1);
            if (pos != 1) Log.w(TAG, "the provider was put at " + pos + ", not first: TLS does not go by the app's network security configuration");
        } catch (Throwable t) {
            // Android would end the app here; the app starts, its TLS trusting Android's authorities alone
            Log.w(TAG, "the network security configuration was not put in place", t);
        }
    }
}
