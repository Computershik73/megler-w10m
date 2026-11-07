package com.android.org.conscrypt;

import java.net.Socket;
import java.security.KeyStore;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.List;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.X509ExtendedTrustManager;

/** Android 9's conscrypt (conscrypt.dex, on the boot class path): compiled against, not put in the dex. The members the
 *  framework's network security configuration calls (android.security.net.config.NetworkSecurityTrustManager). */
public final class TrustManagerImpl extends X509ExtendedTrustManager {
    public TrustManagerImpl(KeyStore keyStore) {}
    public TrustManagerImpl(KeyStore keyStore, CertPinManager manager, TrustedCertificateStore certStore) {}
    public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {}
    public void checkClientTrusted(X509Certificate[] chain, String authType, Socket socket) throws CertificateException {}
    public void checkClientTrusted(X509Certificate[] chain, String authType, SSLEngine engine) throws CertificateException {}
    public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {}
    public void checkServerTrusted(X509Certificate[] chain, String authType, Socket socket) throws CertificateException {}
    public void checkServerTrusted(X509Certificate[] chain, String authType, SSLEngine engine) throws CertificateException {}
    /** The chain as verified, its trust anchor last. */
    public List<X509Certificate> checkServerTrusted(X509Certificate[] chain, String authType, String hostname) throws CertificateException { return null; }
    public List<X509Certificate> getTrustedChainForServer(X509Certificate[] chain, String authType, Socket socket) throws CertificateException { return null; }
    public List<X509Certificate> getTrustedChainForServer(X509Certificate[] chain, String authType, SSLEngine engine) throws CertificateException { return null; }
    public X509Certificate[] getAcceptedIssuers() { return null; }
    public void handleTrustStorageUpdate() {}
}
