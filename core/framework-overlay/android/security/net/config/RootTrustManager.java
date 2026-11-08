// Megler: Android's, the app's trust manager (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import java.net.Socket;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.List;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.X509ExtendedTrustManager;

/**
 * The trust manager of an ApplicationConfig: each check of a server goes to the configuration of its host. conscrypt calls
 * checkServerTrusted(chain, authType, socket) and the host is the handshake's peer; X509TrustManagerExtensions (OkHttp's and
 * Cronet's chain checks) calls checkServerTrusted(chain, authType, host). Clients are checked by the base configuration,
 * as only a server's trust is a domain's.
 */
public class RootTrustManager extends X509ExtendedTrustManager {
    private final ApplicationConfig mConfig;

    public RootTrustManager(ApplicationConfig config) {
        if (config == null) throw new NullPointerException("config must not be null");
        mConfig = config;
    }

    @Override public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
        mConfig.getConfigForHostname("").getTrustManager().checkClientTrusted(chain, authType);
    }

    @Override public void checkClientTrusted(X509Certificate[] certs, String authType, Socket socket) throws CertificateException {
        mConfig.getConfigForHostname("").getTrustManager().checkClientTrusted(certs, authType, socket);
    }

    @Override public void checkClientTrusted(X509Certificate[] certs, String authType, SSLEngine engine) throws CertificateException {
        mConfig.getConfigForHostname("").getTrustManager().checkClientTrusted(certs, authType, engine);
    }

    @Override public void checkServerTrusted(X509Certificate[] certs, String authType, Socket socket) throws CertificateException {
        if (!(socket instanceof SSLSocket)) {
            checkServerTrusted(certs, authType);
            return;
        }
        SSLSession session = ((SSLSocket) socket).getHandshakeSession();
        if (session == null) throw new CertificateException("Not in handshake; no session available");
        try {
            mConfig.getConfigForHostname(session.getPeerHost()).getTrustManager().checkServerTrusted(certs, authType, socket);
        } catch (CertificateException e) {
            refused(session.getPeerHost(), certs, e);
            throw e;
        }
    }

    @Override public void checkServerTrusted(X509Certificate[] certs, String authType, SSLEngine engine) throws CertificateException {
        SSLSession session = engine.getHandshakeSession();
        if (session == null) throw new CertificateException("Not in handshake; no session available");
        try {
            mConfig.getConfigForHostname(session.getPeerHost()).getTrustManager().checkServerTrusted(certs, authType, engine);
        } catch (CertificateException e) {
            refused(session.getPeerHost(), certs, e);
            throw e;
        }
    }

    /** With no host there is no configuration to choose: refused where domains have their own, as on Android. */
    @Override public void checkServerTrusted(X509Certificate[] certs, String authType) throws CertificateException {
        if (mConfig.hasPerDomainConfigs()) {
            throw new CertificateException("Domain specific configurations require that hostname aware"
                    + " checkServerTrusted(X509Certificate[], String, String) is used");
        }
        mConfig.getConfigForHostname("").getTrustManager().checkServerTrusted(certs, authType);
    }

    /** The host's check, which conscrypt and X509TrustManagerExtensions call by this signature: the chain as verified, its anchor last. */
    public List<X509Certificate> checkServerTrusted(X509Certificate[] certs, String authType, String hostname) throws CertificateException {
        if (hostname == null && mConfig.hasPerDomainConfigs()) {
            throw new CertificateException("Domain specific configurations require that the hostname be provided");
        }
        try {
            return mConfig.getConfigForHostname(hostname).getTrustManager().checkServerTrusted(certs, authType, hostname);
        } catch (CertificateException e) {
            refused(hostname, certs, e);
            throw e;
        }
    }

    /*
     * Megler: a server's certificates refused, said once a host with why and with the authority they lead to (the issuer of
     * the last one). The system's authorities here are Android 9's, the root's cacerts (137, of 2018), where Android 14 has
     * newer ones: a server whose chain reaches only a newer authority is refused here alone ("Trust anchor for certification
     * path not found"), and the log did not say whose it was -- Transit's WebView was refused eight servers' chains at every
     * start (Chromium's "cr_X509Util: Failed to validate the certificate chain", 0.1.0.675).
     */
    private static final java.util.HashSet<String> sRefusedSaid = new java.util.HashSet<>();
    private static void refused(String host, X509Certificate[] certs, CertificateException e) {
        String name = host == null ? "a server with no name" : host;
        synchronized (sRefusedSaid) {
            if (sRefusedSaid.size() >= 64 || !sRefusedSaid.add(name)) return;
        }
        X509Certificate last = certs != null && certs.length > 0 ? certs[certs.length - 1] : null;
        android.util.Log.w("Megler", "TLS: " + name + "'s certificates refused (" + (certs == null ? 0 : certs.length) + " of them, the last issued by "
                + (last == null ? "no one" : last.getIssuerX500Principal().getName()) + "): " + e.getMessage());
    }

    /** The base configuration's anchors: what a client's certificate may be issued by. */
    @Override public X509Certificate[] getAcceptedIssuers() {
        return mConfig.getConfigForHostname("").getTrustManager().getAcceptedIssuers();
    }

    /** Whether the two hosts have one configuration (X509TrustManagerExtensions.isSameTrustConfiguration). */
    public boolean isSameTrustConfiguration(String hostname1, String hostname2) {
        return mConfig.getConfigForHostname(hostname1).equals(mConfig.getConfigForHostname(hostname2));
    }
}
