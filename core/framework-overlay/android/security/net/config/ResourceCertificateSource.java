// Megler: Android's, the certificates of <certificates src="@raw/..."/> (NetworkSecurityConfigProvider.java says why this
// package is here). It differs where the file cannot be read: Android throws, which ends the app at its first connection;
// here it is said in the log and the file gives no anchors.
package android.security.net.config;

import android.content.Context;
import android.util.Log;
import com.android.org.conscrypt.TrustedCertificateIndex;
import java.io.InputStream;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;

/**
 * The certificates of one of the app's resources, read when first needed: as the X.509 certificate factory reads a stream, a
 * DER certificate or any number of PEM ones (or a PKCS#7 bundle).
 */
public class ResourceCertificateSource implements CertificateSource {
    private static final String TAG = "NetworkSecurityConfig";
    private final Object mLock = new Object();
    private final int mResourceId;
    private final String mName;
    private Set<X509Certificate> mCertificates;
    private Context mContext;
    private TrustedCertificateIndex mIndex;

    public ResourceCertificateSource(int resourceId, Context context) {
        mResourceId = resourceId;
        mContext = context;
        mName = "@" + XmlConfigSource.name(context, resourceId);
    }

    private void ensureInitialized() {
        synchronized (mLock) {
            if (mCertificates != null) return;
            Set<X509Certificate> certificates = new HashSet<>();
            TrustedCertificateIndex index = new TrustedCertificateIndex();
            try (InputStream in = mContext.getResources().openRawResource(mResourceId)) {
                Collection<? extends Certificate> certs = CertificateFactory.getInstance("X.509").generateCertificates(in);
                for (Certificate c : certs) {
                    if (!(c instanceof X509Certificate)) continue;
                    certificates.add((X509Certificate) c);
                    index.index((X509Certificate) c);
                }
            } catch (Exception e) {
                Log.w(TAG, "the trust anchors of " + mName + " cannot be read; it gives none", e);
            }
            mCertificates = certificates;
            mIndex = index;
            mContext = null;
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

    @Override public String toString() { return mName; }
}
