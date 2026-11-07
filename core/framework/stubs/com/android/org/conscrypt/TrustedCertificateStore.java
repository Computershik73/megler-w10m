package com.android.org.conscrypt;

import java.io.File;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Date;
import java.util.List;
import java.util.Set;

/** Android 9's conscrypt (conscrypt.dex): compiled against, not put in the dex. Where TrustManagerImpl looks its trust anchors
 *  up; the network security configuration's own takes its place (android.security.net.config.TrustedCertificateStoreAdapter). */
public class TrustedCertificateStore {
    public TrustedCertificateStore() {}
    public X509Certificate getTrustAnchor(X509Certificate c) { return null; }
    public X509Certificate findIssuer(X509Certificate c) { return null; }
    public Set<X509Certificate> findAllIssuers(X509Certificate c) { return null; }
    public boolean isUserAddedCertificate(X509Certificate cert) { return false; }
    public File getCertificateFile(File dir, X509Certificate x) { return null; }
    public Certificate getCertificate(String alias) { return null; }
    public Certificate getCertificate(String alias, boolean includeDeletedSystem) { return null; }
    public Date getCreationDate(String alias) { return null; }
    public Set<String> aliases() { return null; }
    public Set<String> userAliases() { return null; }
    public Set<String> allSystemAliases() { return null; }
    public boolean containsAlias(String alias) { return false; }
    public String getCertificateAlias(Certificate c) { return null; }
    public String getCertificateAlias(Certificate c, boolean includeDeletedSystem) { return null; }
    public List<X509Certificate> getCertificateChain(X509Certificate leaf) throws java.security.cert.CertificateException { return null; }
}
