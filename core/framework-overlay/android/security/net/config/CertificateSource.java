// Megler: Android's, where a <certificates> element's certificates come from (NetworkSecurityConfigProvider.java says why this
// package is here): the system's (SystemCertificateSource), the user's (UserCertificateSource), an app's file
// (ResourceCertificateSource), a KeyStore's (KeyStoreCertificateSource).
package android.security.net.config;

import java.security.cert.X509Certificate;
import java.util.Set;

/** A set of certificates, and the lookups conscrypt's TrustManagerImpl makes in it as it builds a chain. */
public interface CertificateSource {
    Set<X509Certificate> getCertificates();
    /** One of these with the certificate's subject and public key: the certificate itself, as a trust anchor. */
    X509Certificate findBySubjectAndPublicKey(X509Certificate cert);
    /** One of these that issued the certificate (its subject the certificate's issuer, its key the signature's). */
    X509Certificate findByIssuerAndSignature(X509Certificate cert);
    Set<X509Certificate> findAllByIssuerAndSignature(X509Certificate cert);
    void handleTrustStorageUpdate();
}
