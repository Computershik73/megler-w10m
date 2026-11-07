// Megler: Android's, the certificates of a folder laid out as Android's cacerts are (NetworkSecurityConfigProvider.java says
// why this package is here). It differs in keeping each file it has read, by name: the few that the lookups of each
// handshake read are parsed once, not at every connection.
package android.security.net.config;

import android.util.Log;
import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Set;
import javax.security.auth.x500.X500Principal;

/**
 * A folder of certificates, each in a file named for its subject as Android names them: the hash of the subject's encoding
 * (OpenSSL's old one, X509_NAME_hash_old: the first four bytes of its MD5, little-endian, in hexadecimal), a dot and a number
 * for subjects of the same hash. A lookup reads the files of the one subject.
 */
abstract class DirectoryCertificateSource implements CertificateSource {
    private static final String TAG = "DirectoryCertificateSrc";
    private final File mDir;
    private final Object mLock = new Object();
    private Set<X509Certificate> mCertificates;
    /** The files read, by name (null for one that could not be). */
    private final HashMap<String, X509Certificate> mRead = new HashMap<>();

    protected DirectoryCertificateSource(File caDir) {
        mDir = caDir;
    }

    protected abstract boolean isCertMarkedAsRemoved(String caFile);

    @Override public Set<X509Certificate> getCertificates() {
        synchronized (mLock) {
            if (mCertificates != null) return mCertificates;
            Set<X509Certificate> certs = new HashSet<>();
            String[] files = mDir.isDirectory() ? mDir.list() : null;
            if (files != null) {
                for (String caFile : files) {
                    if (isCertMarkedAsRemoved(caFile)) continue;
                    X509Certificate cert = readCertificate(caFile);
                    if (cert != null) certs.add(cert);
                }
            }
            mCertificates = certs;
            return certs;
        }
    }

    private interface CertSelector { boolean match(X509Certificate cert); }

    @Override public X509Certificate findBySubjectAndPublicKey(final X509Certificate cert) {
        Set<X509Certificate> found = findCerts(cert.getSubjectX500Principal(), ca -> ca.getPublicKey().equals(cert.getPublicKey()), true);
        return found.isEmpty() ? null : found.iterator().next();
    }

    @Override public X509Certificate findByIssuerAndSignature(final X509Certificate cert) {
        Set<X509Certificate> found = findCerts(cert.getIssuerX500Principal(), ca -> signed(cert, ca), true);
        return found.isEmpty() ? null : found.iterator().next();
    }

    @Override public Set<X509Certificate> findAllByIssuerAndSignature(final X509Certificate cert) {
        return findCerts(cert.getIssuerX500Principal(), ca -> signed(cert, ca), false);
    }

    @Override public void handleTrustStorageUpdate() {
        synchronized (mLock) {
            mCertificates = null;
            mRead.clear();
        }
    }

    private static boolean signed(X509Certificate cert, X509Certificate by) {
        try {
            cert.verify(by.getPublicKey());
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private Set<X509Certificate> findCerts(X500Principal subject, CertSelector selector, boolean first) {
        String hash = hash(subject);
        Set<X509Certificate> certs = null;
        for (int index = 0; ; index++) {
            String name = hash + "." + index;
            if (!new File(mDir, name).exists()) break;
            if (isCertMarkedAsRemoved(name)) continue;
            X509Certificate cert = readCertificate(name);
            if (cert == null || !subject.equals(cert.getSubjectX500Principal()) || !selector.match(cert)) continue;
            if (certs == null) certs = new HashSet<>();
            certs.add(cert);
            if (first) break;
        }
        return certs != null ? certs : Collections.<X509Certificate>emptySet();
    }

    /** X509_NAME_hash_old of the subject, as Android's cacerts files are named (conscrypt's NativeCrypto works it out so). */
    private static String hash(X500Principal name) {
        try {
            byte[] md = MessageDigest.getInstance("MD5").digest(name.getEncoded());
            int h = (md[0] & 0xff) | ((md[1] & 0xff) << 8) | ((md[2] & 0xff) << 16) | ((md[3] & 0xff) << 24);
            String hex = Integer.toHexString(h);
            return "00000000".substring(hex.length()) + hex;
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new AssertionError(e);
        }
    }

    private X509Certificate readCertificate(String file) {
        synchronized (mLock) {
            if (mRead.containsKey(file)) return mRead.get(file);
        }
        X509Certificate cert = null;
        try (InputStream in = new BufferedInputStream(new FileInputStream(new File(mDir, file)))) {
            cert = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(in);
        } catch (Exception e) {
            Log.e(TAG, "Failed to read certificate from " + file, e);
        }
        synchronized (mLock) {
            mRead.put(file, cert);
        }
        return cert;
    }
}
