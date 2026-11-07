// Megler: Android's, a <certificates> element of <trust-anchors> (NetworkSecurityConfigProvider.java says why this package is
// here).
package android.security.net.config;

import java.security.cert.X509Certificate;
import java.util.HashSet;
import java.util.Set;

/** A source of certificates, and whether the anchors it gives override the configuration's pins (overridePins). */
public final class CertificatesEntryRef {
    private final CertificateSource mSource;
    private final boolean mOverridesPins;

    public CertificatesEntryRef(CertificateSource source, boolean overridesPins) {
        mSource = source;
        mOverridesPins = overridesPins;
    }

    boolean overridesPins() { return mOverridesPins; }

    public Set<TrustAnchor> getTrustAnchors() {
        Set<TrustAnchor> anchors = new HashSet<>();
        for (X509Certificate cert : mSource.getCertificates()) anchors.add(new TrustAnchor(cert, mOverridesPins));
        return anchors;
    }

    public TrustAnchor findBySubjectAndPublicKey(X509Certificate cert) {
        X509Certificate found = mSource.findBySubjectAndPublicKey(cert);
        return found == null ? null : new TrustAnchor(found, mOverridesPins);
    }

    public TrustAnchor findByIssuerAndSignature(X509Certificate cert) {
        X509Certificate found = mSource.findByIssuerAndSignature(cert);
        return found == null ? null : new TrustAnchor(found, mOverridesPins);
    }

    public Set<X509Certificate> findAllCertificatesByIssuerAndSignature(X509Certificate cert) {
        return mSource.findAllByIssuerAndSignature(cert);
    }

    public void handleTrustStorageUpdate() {
        mSource.handleTrustStorageUpdate();
    }

    /** For the log: the source ("system", "@raw/name"), and whether it overrides pins. */
    String describe() {
        return mSource + (mOverridesPins ? " (overriding pins)" : "");
    }
}
