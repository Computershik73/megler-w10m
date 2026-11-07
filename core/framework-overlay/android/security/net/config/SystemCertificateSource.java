// Megler: Android's, the certificates of <certificates src="system"/> (NetworkSecurityConfigProvider.java says why this
// package is here). It differs in that none is ever marked removed: Android keeps those the user turned off in Settings in a
// folder of the user's, and there is no such setting here.
package android.security.net.config;

import java.io.File;

/** The authorities Android 9 trusts: the Android root's etc/security/cacerts, which conscrypt's own trust manager reads too. */
public final class SystemCertificateSource extends DirectoryCertificateSource {
    private static final class NoPreloadHolder {
        private static final SystemCertificateSource INSTANCE = new SystemCertificateSource();
    }

    private SystemCertificateSource() {
        super(new File(root() + "/etc/security/cacerts"));
    }

    private static String root() {
        String root = System.getenv("ANDROID_ROOT");
        return root != null ? root : "/system";
    }

    public static SystemCertificateSource getInstance() {
        return NoPreloadHolder.INSTANCE;
    }

    @Override protected boolean isCertMarkedAsRemoved(String caFile) {
        return false;
    }

    @Override public String toString() { return "system"; }
}
