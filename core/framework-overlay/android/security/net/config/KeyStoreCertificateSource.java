// Megler: Android's, the certificates of a KeyStore an app gives TrustManagerFactory.init (NetworkSecurityConfigProvider.java
// says why this package is here).
package android.security.net.config;

import com.android.org.conscrypt.TrustedCertificateIndex;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.Set;

/** The X.509 certificates of a KeyStore's entries, read when first needed. */
class KeyStoreCertificateSource implements CertificateSource {
    private final Object mLock = new Object();
    private final KeyStore mKeyStore;
    private TrustedCertificateIndex mIndex;
    private Set<X509Certificate> mCertificates;

    KeyStoreCertificateSource(KeyStore ks) {
        mKeyStore = ks;
    }

    private void ensureInitialized() {
        synchronized (mLock) {
            if (mCertificates != null) return;
            try {
                TrustedCertificateIndex index = new TrustedCertificateIndex();
                Set<X509Certificate> certificates = new HashSet<>();
                for (Enumeration<String> en = mKeyStore.aliases(); en.hasMoreElements();) {
                    Certificate cert = mKeyStore.getCertificate(en.nextElement());
                    if (cert instanceof X509Certificate) {
                        certificates.add((X509Certificate) cert);
                        index.index((X509Certificate) cert);
                    }
                }
                mIndex = index;
                mCertificates = certificates;
            } catch (KeyStoreException e) {
                throw new RuntimeException("Failed to load certificates from KeyStore", e);
            }
        }
    }

    @Override public Set<X509Certificate> getCertificates() {
        ensureInitialized();
        return mCertificates;
    }

    @Override public X509Certificate findBySubjectAndPublicKey(X509Certificate cert) {
        ensureInitialized();
        java.security.cert.TrustAnchor anchor = mIndex.findBySubjectAndPublicKey(cert);
        return anchor == null ? null : anchor.getTrustedCert();
    }

    @Override public X509Certificate findByIssuerAndSignature(X509Certificate cert) {
        ensureInitialized();
        java.security.cert.TrustAnchor anchor = mIndex.findByIssuerAndSignature(cert);
        return anchor == null ? null : anchor.getTrustedCert();
    }

    @Override public Set<X509Certificate> findAllByIssuerAndSignature(X509Certificate cert) {
        ensureInitialized();
        Set<java.security.cert.TrustAnchor> anchors = mIndex.findAllByIssuerAndSignature(cert);
        if (anchors.isEmpty()) return Collections.<X509Certificate>emptySet();
        Set<X509Certificate> certs = new HashSet<>();
        for (java.security.cert.TrustAnchor anchor : anchors) certs.add(anchor.getTrustedCert());
        return certs;
    }

    @Override public void handleTrustStorageUpdate() {}

    @Override public String toString() { return "a KeyStore's"; }
}
