// Megler: the certificates of <certificates src="user"/> (NetworkSecurityConfigProvider.java says why this package is here).
// It differs from Android's in having none: Android's are the authorities the user installed in Settings, kept in the user's
// cacerts-added folder, and there is no such setting or folder here.
package android.security.net.config;

import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.Set;

/** The authorities the user added: none here. */
public final class UserCertificateSource implements CertificateSource {
    private static final UserCertificateSource INSTANCE = new UserCertificateSource();

    private UserCertificateSource() {}

    public static UserCertificateSource getInstance() {
        return INSTANCE;
    }

    @Override public Set<X509Certificate> getCertificates() { return Collections.emptySet(); }
    @Override public X509Certificate findBySubjectAndPublicKey(X509Certificate cert) { return null; }
    @Override public X509Certificate findByIssuerAndSignature(X509Certificate cert) { return null; }
    @Override public Set<X509Certificate> findAllByIssuerAndSignature(X509Certificate cert) { return Collections.emptySet(); }
    @Override public void handleTrustStorageUpdate() {}

    @Override public String toString() { return "user (none here)"; }
}
