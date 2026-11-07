// Megler: Android's, a <pin> of a <pin-set> (NetworkSecurityConfigProvider.java says why this package is here).
package android.security.net.config;

import java.util.Arrays;

/** The digest of a certificate's SubjectPublicKeyInfo that a host's chain must have one of. SHA-256 is the one algorithm. */
public final class Pin {
    public final String digestAlgorithm;
    public final byte[] digest;
    private final int mHashCode;

    public Pin(String digestAlgorithm, byte[] digest) {
        this.digestAlgorithm = digestAlgorithm;
        this.digest = digest;
        mHashCode = Arrays.hashCode(digest) ^ digestAlgorithm.hashCode();
    }

    public static boolean isSupportedDigestAlgorithm(String algorithm) {
        return "SHA-256".equalsIgnoreCase(algorithm);
    }

    public static int getDigestLength(String algorithm) {
        if ("SHA-256".equalsIgnoreCase(algorithm)) return 32;
        throw new IllegalArgumentException("Unsupported digest algorithm: " + algorithm);
    }

    @Override public int hashCode() { return mHashCode; }

    @Override public boolean equals(Object other) {
        if (other == this) return true;
        if (!(other instanceof Pin)) return false;
        Pin p = (Pin) other;
        return p.hashCode() == mHashCode && Arrays.equals(digest, p.digest) && digestAlgorithm.equals(p.digestAlgorithm);
    }
}
