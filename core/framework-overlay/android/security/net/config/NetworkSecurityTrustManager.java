// Megler: Android's, the trust manager of one configuration (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import com.android.org.conscrypt.TrustManagerImpl;
import java.net.Socket;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.X509ExtendedTrustManager;

/**
 * conscrypt's TrustManagerImpl over a configuration's trust anchors -- an empty KeyStore, the anchors looked up through
 * TrustedCertificateStoreAdapter as it builds the chain -- then the configuration's pins: before their expiration, a chain
 * that ends in an anchor not overriding pins must have a certificate whose public key's digest is one of them.
 */
public class NetworkSecurityTrustManager extends X509ExtendedTrustManager {
    private final TrustManagerImpl mDelegate;
    private final NetworkSecurityConfig mNetworkSecurityConfig;
    private final Object mIssuersLock = new Object();
    private X509Certificate[] mIssuers;

    public NetworkSecurityTrustManager(NetworkSecurityConfig config) {
        if (config == null) throw new NullPointerException("config must not be null");
        mNetworkSecurityConfig = config;
        try {
            TrustedCertificateStoreAdapter certStore = new TrustedCertificateStoreAdapter(config);
            // TrustManagerImpl takes no null KeyStore: an empty one, the anchors coming from the store
            KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
            store.load(null, null);
            mDelegate = new TrustManagerImpl(store, null, certStore);
        } catch (GeneralSecurityException | java.io.IOException e) {
            throw new RuntimeException(e);
        }
    }

    @Override public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
        mDelegate.checkClientTrusted(chain, authType);
    }

    @Override public void checkClientTrusted(X509Certificate[] certs, String authType, Socket socket) throws CertificateException {
        mDelegate.checkClientTrusted(certs, authType, socket);
    }

    @Override public void checkClientTrusted(X509Certificate[] certs, String authType, SSLEngine engine) throws CertificateException {
        mDelegate.checkClientTrusted(certs, authType, engine);
    }

    @Override public void checkServerTrusted(X509Certificate[] certs, String authType) throws CertificateException {
        checkServerTrusted(certs, authType, (String) null);
    }

    @Override public void checkServerTrusted(X509Certificate[] certs, String authType, Socket socket) throws CertificateException {
        checkPins(mDelegate.getTrustedChainForServer(certs, authType, socket));
    }

    @Override public void checkServerTrusted(X509Certificate[] certs, String authType, SSLEngine engine) throws CertificateException {
        checkPins(mDelegate.getTrustedChainForServer(certs, authType, engine));
    }

    /** The host's check (conscrypt's and X509TrustManagerExtensions' signature): the chain as verified, its anchor last. */
    public List<X509Certificate> checkServerTrusted(X509Certificate[] certs, String authType, String host) throws CertificateException {
        List<X509Certificate> trustedChain = mDelegate.checkServerTrusted(certs, authType, host);
        checkPins(trustedChain);
        return trustedChain;
    }

    private void checkPins(List<X509Certificate> chain) throws CertificateException {
        PinSet pinSet = mNetworkSecurityConfig.getPins();
        if (pinSet.pins.isEmpty() || System.currentTimeMillis() > pinSet.expirationTime || !isPinningEnforced(chain)) return;
        Set<String> pinAlgorithms = pinSet.getPinAlgorithms();
        Map<String, MessageDigest> digests = new HashMap<>();
        for (int i = chain.size() - 1; i >= 0; i--) {
            byte[] spki = chain.get(i).getPublicKey().getEncoded();
            for (String algorithm : pinAlgorithms) {
                MessageDigest md = digests.get(algorithm);
                if (md == null) {
                    try {
                        md = MessageDigest.getInstance(algorithm);
                    } catch (GeneralSecurityException e) {
                        throw new RuntimeException(e);
                    }
                    digests.put(algorithm, md);
                }
                if (pinSet.pins.contains(new Pin(algorithm, md.digest(spki)))) return;
            }
        }
        throw new CertificateException("Pin verification failed");
    }

    private boolean isPinningEnforced(List<X509Certificate> chain) throws CertificateException {
        if (chain.isEmpty()) return false;
        TrustAnchor anchor = mNetworkSecurityConfig.findTrustAnchorBySubjectAndPublicKey(chain.get(chain.size() - 1));
        if (anchor == null) throw new CertificateException("Trusted chain does not end in a TrustAnchor");
        return !anchor.overridesPins;
    }

    /** The configuration's anchors (TrustManagerImpl's own answer would be its empty KeyStore's). */
    @Override public X509Certificate[] getAcceptedIssuers() {
        synchronized (mIssuersLock) {
            if (mIssuers == null) {
                Set<TrustAnchor> anchors = mNetworkSecurityConfig.getTrustAnchors();
                X509Certificate[] issuers = new X509Certificate[anchors.size()];
                int i = 0;
                for (TrustAnchor anchor : anchors) issuers[i++] = anchor.certificate;
                mIssuers = issuers;
            }
            return mIssuers.clone();
        }
    }

    public void handleTrustStorageUpdate() {
        synchronized (mIssuersLock) {
            mIssuers = null;
            mDelegate.handleTrustStorageUpdate();
        }
    }
}
