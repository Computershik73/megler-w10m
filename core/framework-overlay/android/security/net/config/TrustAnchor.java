// Megler: Android's, a configuration's trust anchor (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import java.security.cert.X509Certificate;

/** A certificate a configuration trusts, and whether a chain that ends in it is exempt from the configuration's pins. */
public final class TrustAnchor {
    public final X509Certificate certificate;
    public final boolean overridesPins;

    public TrustAnchor(X509Certificate certificate, boolean overridesPins) {
        if (certificate == null) throw new NullPointerException("certificate");
        this.certificate = certificate;
        this.overridesPins = overridesPins;
    }
}
