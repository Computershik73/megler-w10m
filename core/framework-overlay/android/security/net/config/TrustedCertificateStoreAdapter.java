// Megler: Android's, a configuration's anchors where conscrypt's TrustManagerImpl looks for its trust anchors
// (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import com.android.org.conscrypt.TrustedCertificateStore;
import java.io.File;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Date;
import java.util.Set;

/** conscrypt's store of system and user authorities, its lookups answered by a configuration; the rest is not used for a check. */
public class TrustedCertificateStoreAdapter extends TrustedCertificateStore {
    private final NetworkSecurityConfig mConfig;

    public TrustedCertificateStoreAdapter(NetworkSecurityConfig config) {
        mConfig = config;
    }

    @Override public X509Certificate findIssuer(X509Certificate cert) {
        TrustAnchor anchor = mConfig.findTrustAnchorByIssuerAndSignature(cert);
        return anchor == null ? null : anchor.certificate;
    }

    @Override public Set<X509Certificate> findAllIssuers(X509Certificate cert) {
        return mConfig.findAllCertificatesByIssuerAndSignature(cert);
    }

    @Override public X509Certificate getTrustAnchor(X509Certificate cert) {
        TrustAnchor anchor = mConfig.findTrustAnchorBySubjectAndPublicKey(cert);
        return anchor == null ? null : anchor.certificate;
    }

    /** Asked for pinning alone: an anchor that overrides pins. */
    @Override public boolean isUserAddedCertificate(X509Certificate cert) {
        TrustAnchor anchor = mConfig.findTrustAnchorBySubjectAndPublicKey(cert);
        return anchor != null && anchor.overridesPins;
    }

    @Override public File getCertificateFile(File dir, X509Certificate x) { throw new UnsupportedOperationException(); }
    @Override public Certificate getCertificate(String alias) { throw new UnsupportedOperationException(); }
    @Override public Certificate getCertificate(String alias, boolean includeDeletedSystem) { throw new UnsupportedOperationException(); }
    @Override public Date getCreationDate(String alias) { throw new UnsupportedOperationException(); }
    @Override public Set<String> aliases() { throw new UnsupportedOperationException(); }
    @Override public Set<String> userAliases() { throw new UnsupportedOperationException(); }
    @Override public Set<String> allSystemAliases() { throw new UnsupportedOperationException(); }
    @Override public boolean containsAlias(String alias) { throw new UnsupportedOperationException(); }
    @Override public String getCertificateAlias(Certificate cert) { throw new UnsupportedOperationException(); }
    @Override public String getCertificateAlias(Certificate cert, boolean includeDeletedSystem) { throw new UnsupportedOperationException(); }
}
