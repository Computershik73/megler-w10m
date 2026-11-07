package com.android.org.conscrypt;

/** Android 9's conscrypt (conscrypt.dex): compiled against, not put in the dex. TrustManagerImpl's pins, given as null here. */
public interface CertPinManager {
    void checkChainPinning(String hostname, java.util.List<java.security.cert.X509Certificate> chain) throws java.security.cert.CertificateException;
}
