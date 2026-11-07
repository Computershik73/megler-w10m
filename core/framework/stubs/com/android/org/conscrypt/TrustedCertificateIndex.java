package com.android.org.conscrypt;

import java.security.cert.TrustAnchor;
import java.security.cert.X509Certificate;
import java.util.Set;

/** Android 9's conscrypt (conscrypt.dex): compiled against, not put in the dex. Certificates indexed by subject, found by
 *  subject and public key or by issuer and signature. */
public final class TrustedCertificateIndex {
    public TrustedCertificateIndex() {}
    public TrustAnchor index(X509Certificate cert) { return null; }
    public TrustAnchor findBySubjectAndPublicKey(X509Certificate cert) { return null; }
    public TrustAnchor findByIssuerAndSignature(X509Certificate cert) { return null; }
    public Set<TrustAnchor> findAllByIssuerAndSignature(X509Certificate cert) { return null; }
}
