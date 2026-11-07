// Megler: Android's, one configuration of an app's network security configuration (NetworkSecurityConfigProvider.java says why
// this package is here).
package android.security.net.config;

import android.content.pm.ApplicationInfo;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * What a <base-config> or a <domain-config> says, with what it does not say taken from the configuration around it: whether
 * cleartext is permitted, its trust anchors, its pins. Its trust manager is made at the first use.
 */
public final class NetworkSecurityConfig {
    public static final boolean DEFAULT_CLEARTEXT_TRAFFIC_PERMITTED = true;
    public static final boolean DEFAULT_HSTS_ENFORCED = false;

    private final boolean mCleartextTrafficPermitted;
    private final boolean mHstsEnforced;
    private final PinSet mPins;
    private final List<CertificatesEntryRef> mCertificatesEntryRefs;
    private Set<TrustAnchor> mAnchors;
    private final Object mAnchorsLock = new Object();
    private NetworkSecurityTrustManager mTrustManager;
    private final Object mTrustManagerLock = new Object();

    private NetworkSecurityConfig(boolean cleartextTrafficPermitted, boolean hstsEnforced, PinSet pins, List<CertificatesEntryRef> refs) {
        mCleartextTrafficPermitted = cleartextTrafficPermitted;
        mHstsEnforced = hstsEnforced;
        mPins = pins;
        // the sources that override pins first: a certificate in two of them is then found with overridesPins, as Android does
        mCertificatesEntryRefs = new ArrayList<>(refs);
        Collections.sort(mCertificatesEntryRefs, (a, b) -> a.overridesPins() == b.overridesPins() ? 0 : a.overridesPins() ? -1 : 1);
    }

    /** Every anchor of every source, each certificate once (with overridesPins where one of its sources says so). */
    public Set<TrustAnchor> getTrustAnchors() {
        synchronized (mAnchorsLock) {
            if (mAnchors != null) return mAnchors;
            Map<X509Certificate, TrustAnchor> byCert = new LinkedHashMap<>();
            for (CertificatesEntryRef ref : mCertificatesEntryRefs) {
                for (TrustAnchor anchor : ref.getTrustAnchors()) if (!byCert.containsKey(anchor.certificate)) byCert.put(anchor.certificate, anchor);
            }
            mAnchors = new HashSet<>(byCert.values());
            return mAnchors;
        }
    }

    public boolean isCleartextTrafficPermitted() { return mCleartextTrafficPermitted; }

    public boolean isHstsEnforced() { return mHstsEnforced; }

    public PinSet getPins() { return mPins; }

    public NetworkSecurityTrustManager getTrustManager() {
        synchronized (mTrustManagerLock) {
            if (mTrustManager == null) mTrustManager = new NetworkSecurityTrustManager(this);
            return mTrustManager;
        }
    }

    public TrustAnchor findTrustAnchorBySubjectAndPublicKey(X509Certificate cert) {
        for (CertificatesEntryRef ref : mCertificatesEntryRefs) {
            TrustAnchor anchor = ref.findBySubjectAndPublicKey(cert);
            if (anchor != null) return anchor;
        }
        return null;
    }

    public TrustAnchor findTrustAnchorByIssuerAndSignature(X509Certificate cert) {
        for (CertificatesEntryRef ref : mCertificatesEntryRefs) {
            TrustAnchor anchor = ref.findByIssuerAndSignature(cert);
            if (anchor != null) return anchor;
        }
        return null;
    }

    public Set<X509Certificate> findAllCertificatesByIssuerAndSignature(X509Certificate cert) {
        Set<X509Certificate> certs = new HashSet<>();
        for (CertificatesEntryRef ref : mCertificatesEntryRefs) certs.addAll(ref.findAllCertificatesByIssuerAndSignature(cert));
        return certs;
    }

    public void handleTrustStorageUpdate() {
        synchronized (mAnchorsLock) {
            mAnchors = null;
            for (CertificatesEntryRef ref : mCertificatesEntryRefs) ref.handleTrustStorageUpdate();
        }
        getTrustManager().handleTrustStorageUpdate();
    }

    /** What it says, for the log: "cleartext refused, anchors @raw/x, system, 2 pins until ...". */
    String describe() {
        StringBuilder s = new StringBuilder(mCleartextTrafficPermitted ? "cleartext permitted" : "cleartext refused").append(", anchors");
        if (mCertificatesEntryRefs.isEmpty()) s.append(" none");
        for (int i = 0; i < mCertificatesEntryRefs.size(); i++) s.append(i > 0 ? ", " : " ").append(mCertificatesEntryRefs.get(i).describe());
        if (!mPins.pins.isEmpty()) {
            s.append(", ").append(mPins.pins.size()).append(mPins.pins.size() == 1 ? " pin" : " pins");
            if (mPins.expirationTime != Long.MAX_VALUE)
                s.append(" until ").append(new java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).format(new java.util.Date(mPins.expirationTime)));
        }
        return s.toString();
    }

    /**
     * The platform's default, which an app without a configuration has, and the parent of a configuration's <base-config>:
     * the system's authorities; cleartext permitted to an app written for Android 8.1 or earlier; and for one written for
     * Android 6 or earlier, the authorities the user added as well.
     */
    public static Builder getDefaultBuilder(ApplicationInfo info) {
        Builder builder = new Builder()
                .setHstsEnforced(DEFAULT_HSTS_ENFORCED)
                .addCertificatesEntryRef(new CertificatesEntryRef(SystemCertificateSource.getInstance(), false));
        builder.setCleartextTrafficPermitted(info.targetSdkVersion < 28);
        if (info.targetSdkVersion <= 23) builder.addCertificatesEntryRef(new CertificatesEntryRef(UserCertificateSource.getInstance(), false));
        return builder;
    }

    /** A configuration as it is read: what it says itself, and its parent for the rest. */
    public static final class Builder {
        private List<CertificatesEntryRef> mCertificatesEntryRefs;
        private PinSet mPinSet;
        private boolean mCleartextTrafficPermitted = DEFAULT_CLEARTEXT_TRAFFIC_PERMITTED;
        private boolean mHstsEnforced = DEFAULT_HSTS_ENFORCED;
        private boolean mCleartextTrafficPermittedSet, mHstsEnforcedSet;
        private Builder mParentBuilder;

        public Builder setParent(Builder parent) {
            for (Builder b = parent; b != null; b = b.getParent()) if (b == this) throw new IllegalArgumentException("Loops are not allowed in Builder parents");
            mParentBuilder = parent;
            return this;
        }

        public Builder getParent() { return mParentBuilder; }

        public Builder setPinSet(PinSet pinSet) {
            mPinSet = pinSet;
            return this;
        }

        private PinSet getEffectivePinSet() {
            if (mPinSet != null) return mPinSet;
            if (mParentBuilder != null) return mParentBuilder.getEffectivePinSet();
            return PinSet.EMPTY_PINSET;
        }

        public Builder addCertificatesEntryRef(CertificatesEntryRef ref) {
            if (mCertificatesEntryRefs == null) mCertificatesEntryRefs = new ArrayList<>();
            mCertificatesEntryRefs.add(ref);
            return this;
        }

        public Builder addCertificatesEntryRefs(Collection<? extends CertificatesEntryRef> refs) {
            if (mCertificatesEntryRefs == null) mCertificatesEntryRefs = new ArrayList<>();
            mCertificatesEntryRefs.addAll(refs);
            return this;
        }

        private List<CertificatesEntryRef> getEffectiveCertificatesEntryRefs() {
            if (mCertificatesEntryRefs != null) return mCertificatesEntryRefs;
            if (mParentBuilder != null) return mParentBuilder.getEffectiveCertificatesEntryRefs();
            return Collections.<CertificatesEntryRef>emptyList();
        }

        boolean hasCertificatesEntryRefs() { return mCertificatesEntryRefs != null; }

        List<CertificatesEntryRef> getCertificatesEntryRefs() { return mCertificatesEntryRefs; }

        public Builder setCleartextTrafficPermitted(boolean permitted) {
            mCleartextTrafficPermitted = permitted;
            mCleartextTrafficPermittedSet = true;
            return this;
        }

        private boolean getEffectiveCleartextTrafficPermitted() {
            if (mCleartextTrafficPermittedSet) return mCleartextTrafficPermitted;
            if (mParentBuilder != null) return mParentBuilder.getEffectiveCleartextTrafficPermitted();
            return DEFAULT_CLEARTEXT_TRAFFIC_PERMITTED;
        }

        public Builder setHstsEnforced(boolean enforced) {
            mHstsEnforced = enforced;
            mHstsEnforcedSet = true;
            return this;
        }

        private boolean getEffectiveHstsEnforced() {
            if (mHstsEnforcedSet) return mHstsEnforced;
            if (mParentBuilder != null) return mParentBuilder.getEffectiveHstsEnforced();
            return DEFAULT_HSTS_ENFORCED;
        }

        public NetworkSecurityConfig build() {
            return new NetworkSecurityConfig(getEffectiveCleartextTrafficPermitted(), getEffectiveHstsEnforced(), getEffectivePinSet(),
                    getEffectiveCertificatesEntryRefs());
        }
    }
}
