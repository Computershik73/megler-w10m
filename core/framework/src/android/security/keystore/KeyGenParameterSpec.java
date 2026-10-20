// Started from tools/compat/genstubs.py: a property holder whose Builder keeps what it is given (the alias and purposes too).
package android.security.keystore;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class KeyGenParameterSpec implements java.security.spec.AlgorithmParameterSpec {
    private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
    public KeyGenParameterSpec(java.lang.String p0, int p1, int p2, java.security.spec.AlgorithmParameterSpec p3, javax.security.auth.x500.X500Principal p4, java.math.BigInteger p5, java.util.Date p6, java.util.Date p7, java.util.Date p8, java.util.Date p9, java.util.Date p10, int p11, java.lang.String[] p12, java.util.Set p13, java.lang.String[] p14, java.lang.String[] p15, java.lang.String[] p16, boolean p17, boolean p18, int p19, int p20, boolean p21, byte[] p22, boolean p23, int[] p24, boolean p25, boolean p26, boolean p27, boolean p28, boolean p29, boolean p30, boolean p31, int p32, java.lang.String p33, long p34) {}
    public java.security.spec.AlgorithmParameterSpec getAlgorithmParameterSpec() { return (java.security.spec.AlgorithmParameterSpec) megletProps.get("AlgorithmParameterSpec"); }
    public java.lang.String getAttestKeyAlias() { return (java.lang.String) megletProps.get("AttestKeyAlias"); }
    public byte[] getAttestationChallenge() { return (byte[]) megletProps.get("AttestationChallenge"); }
    public int[] getAttestationIds() { return (int[]) megletProps.get("AttestationIds"); }
    public java.lang.String[] getBlockModes() { return (java.lang.String[]) megletProps.get("BlockModes"); }
    public long getBoundToSpecificSecureUserId() { return (megletProps.get("BoundToSpecificSecureUserId") instanceof Long ? (Long) megletProps.get("BoundToSpecificSecureUserId") : 0L); }
    public java.util.Date getCertificateNotAfter() { return (java.util.Date) megletProps.get("CertificateNotAfter"); }
    public java.util.Date getCertificateNotBefore() { return (java.util.Date) megletProps.get("CertificateNotBefore"); }
    public java.math.BigInteger getCertificateSerialNumber() { return (java.math.BigInteger) megletProps.get("CertificateSerialNumber"); }
    public javax.security.auth.x500.X500Principal getCertificateSubject() { return (javax.security.auth.x500.X500Principal) megletProps.get("CertificateSubject"); }
    public java.lang.String[] getDigests() { return (java.lang.String[]) megletProps.get("Digests"); }
    public java.lang.String[] getEncryptionPaddings() { return (java.lang.String[]) megletProps.get("EncryptionPaddings"); }
    public int getKeySize() { return (megletProps.get("KeySize") instanceof Integer ? (Integer) megletProps.get("KeySize") : 0); }
    public java.util.Date getKeyValidityForConsumptionEnd() { return (java.util.Date) megletProps.get("KeyValidityForConsumptionEnd"); }
    public java.util.Date getKeyValidityForOriginationEnd() { return (java.util.Date) megletProps.get("KeyValidityForOriginationEnd"); }
    public java.util.Date getKeyValidityStart() { return (java.util.Date) megletProps.get("KeyValidityStart"); }
    public java.lang.String getKeystoreAlias() { return (java.lang.String) megletProps.get("KeystoreAlias"); }
    public int getMaxUsageCount() { return (megletProps.get("MaxUsageCount") instanceof Integer ? (Integer) megletProps.get("MaxUsageCount") : 0); }
    public java.util.Set getMgf1Digests() { return (megletProps.get("Mgf1Digests") != null ? (java.util.Set) megletProps.get("Mgf1Digests") : new java.util.HashSet()); }
    public int getNamespace() { return (megletProps.get("Namespace") instanceof Integer ? (Integer) megletProps.get("Namespace") : 0); }
    public int getPurposes() { return (megletProps.get("Purposes") instanceof Integer ? (Integer) megletProps.get("Purposes") : 0); }
    public java.lang.String[] getSignaturePaddings() { return (java.lang.String[]) megletProps.get("SignaturePaddings"); }
    public int getUid() { return (megletProps.get("Uid") instanceof Integer ? (Integer) megletProps.get("Uid") : 0); }
    public int getUserAuthenticationType() { return (megletProps.get("UserAuthenticationType") instanceof Integer ? (Integer) megletProps.get("UserAuthenticationType") : 0); }
    public int getUserAuthenticationValidityDurationSeconds() { return (megletProps.get("UserAuthenticationValidityDurationSeconds") instanceof Integer ? (Integer) megletProps.get("UserAuthenticationValidityDurationSeconds") : 0); }
    public boolean isCriticalToDeviceEncryption() { return (megletProps.get("CriticalToDeviceEncryption") instanceof Boolean ? (Boolean) megletProps.get("CriticalToDeviceEncryption") : false); }
    public boolean isDevicePropertiesAttestationIncluded() { return (megletProps.get("DevicePropertiesAttestationIncluded") instanceof Boolean ? (Boolean) megletProps.get("DevicePropertiesAttestationIncluded") : false); }
    public boolean isDigestsSpecified() { return (megletProps.get("DigestsSpecified") instanceof Boolean ? (Boolean) megletProps.get("DigestsSpecified") : false); }
    public boolean isInvalidatedByBiometricEnrollment() { return (megletProps.get("InvalidatedByBiometricEnrollment") instanceof Boolean ? (Boolean) megletProps.get("InvalidatedByBiometricEnrollment") : false); }
    public boolean isMgf1DigestsSpecified() { return (megletProps.get("Mgf1DigestsSpecified") instanceof Boolean ? (Boolean) megletProps.get("Mgf1DigestsSpecified") : false); }
    public boolean isRandomizedEncryptionRequired() { return (megletProps.get("RandomizedEncryptionRequired") instanceof Boolean ? (Boolean) megletProps.get("RandomizedEncryptionRequired") : false); }
    public boolean isStrongBoxBacked() { return (megletProps.get("StrongBoxBacked") instanceof Boolean ? (Boolean) megletProps.get("StrongBoxBacked") : false); }
    public boolean isUniqueIdIncluded() { return (megletProps.get("UniqueIdIncluded") instanceof Boolean ? (Boolean) megletProps.get("UniqueIdIncluded") : false); }
    public boolean isUnlockedDeviceRequired() { return (megletProps.get("UnlockedDeviceRequired") instanceof Boolean ? (Boolean) megletProps.get("UnlockedDeviceRequired") : false); }
    public boolean isUserAuthenticationRequired() { return (megletProps.get("UserAuthenticationRequired") instanceof Boolean ? (Boolean) megletProps.get("UserAuthenticationRequired") : false); }
    public boolean isUserAuthenticationValidWhileOnBody() { return (megletProps.get("UserAuthenticationValidWhileOnBody") instanceof Boolean ? (Boolean) megletProps.get("UserAuthenticationValidWhileOnBody") : false); }
    public boolean isUserConfirmationRequired() { return (megletProps.get("UserConfirmationRequired") instanceof Boolean ? (Boolean) megletProps.get("UserConfirmationRequired") : false); }
    public boolean isUserPresenceRequired() { return (megletProps.get("UserPresenceRequired") instanceof Boolean ? (Boolean) megletProps.get("UserPresenceRequired") : false); }
    KeyGenParameterSpec() { this((java.lang.String) null, (int) 0, (int) 0, (java.security.spec.AlgorithmParameterSpec) null, (javax.security.auth.x500.X500Principal) null, (java.math.BigInteger) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (int) 0, (java.lang.String[]) null, (java.util.Set) null, (java.lang.String[]) null, (java.lang.String[]) null, (java.lang.String[]) null, (boolean) false, (boolean) false, (int) 0, (int) 0, (boolean) false, (byte[]) null, (boolean) false, (int[]) null, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (int) 0, (java.lang.String) null, (long) 0L); }
    public static final class Builder {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        public Builder(android.security.keystore.KeyGenParameterSpec p0) { megletProps.putAll(p0.megletProps); }
        public Builder(java.lang.String p0, int p1) { megletProps.put("KeystoreAlias", p0); megletProps.put("Purposes", p1); }
        public android.security.keystore.KeyGenParameterSpec build() { android.security.keystore.KeyGenParameterSpec x = new android.security.keystore.KeyGenParameterSpec(); x.megletProps.putAll(megletProps); return x; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAlgorithmParameterSpec(java.security.spec.AlgorithmParameterSpec p0) { megletProps.put("AlgorithmParameterSpec", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestKeyAlias(java.lang.String p0) { megletProps.put("AttestKeyAlias", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestationChallenge(byte[] p0) { megletProps.put("AttestationChallenge", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestationIds(int[] p0) { megletProps.put("AttestationIds", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setBlockModes(java.lang.String[] p0) { megletProps.put("BlockModes", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setBoundToSpecificSecureUserId(long p0) { megletProps.put("BoundToSpecificSecureUserId", Long.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateNotAfter(java.util.Date p0) { megletProps.put("CertificateNotAfter", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateNotBefore(java.util.Date p0) { megletProps.put("CertificateNotBefore", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateSerialNumber(java.math.BigInteger p0) { megletProps.put("CertificateSerialNumber", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateSubject(javax.security.auth.x500.X500Principal p0) { megletProps.put("CertificateSubject", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCriticalToDeviceEncryption(boolean p0) { megletProps.put("CriticalToDeviceEncryption", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setDevicePropertiesAttestationIncluded(boolean p0) { megletProps.put("DevicePropertiesAttestationIncluded", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setDigests(java.lang.String[] p0) { megletProps.put("Digests", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setEncryptionPaddings(java.lang.String[] p0) { megletProps.put("EncryptionPaddings", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setInvalidatedByBiometricEnrollment(boolean p0) { megletProps.put("InvalidatedByBiometricEnrollment", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setIsStrongBoxBacked(boolean p0) { megletProps.put("IsStrongBoxBacked", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeySize(int p0) { megletProps.put("KeySize", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityEnd(java.util.Date p0) { megletProps.put("KeyValidityEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityForConsumptionEnd(java.util.Date p0) { megletProps.put("KeyValidityForConsumptionEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityForOriginationEnd(java.util.Date p0) { megletProps.put("KeyValidityForOriginationEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityStart(java.util.Date p0) { megletProps.put("KeyValidityStart", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setMaxUsageCount(int p0) { megletProps.put("MaxUsageCount", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setMgf1Digests(java.lang.String[] p0) { megletProps.put("Mgf1Digests", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setNamespace(int p0) { megletProps.put("Namespace", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setRandomizedEncryptionRequired(boolean p0) { megletProps.put("RandomizedEncryptionRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setSignaturePaddings(java.lang.String[] p0) { megletProps.put("SignaturePaddings", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUid(int p0) { megletProps.put("Uid", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUniqueIdIncluded(boolean p0) { megletProps.put("UniqueIdIncluded", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUnlockedDeviceRequired(boolean p0) { megletProps.put("UnlockedDeviceRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationParameters(int p0, int p1) { return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationRequired(boolean p0) { megletProps.put("UserAuthenticationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationValidWhileOnBody(boolean p0) { megletProps.put("UserAuthenticationValidWhileOnBody", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationValidityDurationSeconds(int p0) { megletProps.put("UserAuthenticationValidityDurationSeconds", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserConfirmationRequired(boolean p0) { megletProps.put("UserConfirmationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserPresenceRequired(boolean p0) { megletProps.put("UserPresenceRequired", Boolean.valueOf(p0)); return this; }
        Builder() { this((android.security.keystore.KeyGenParameterSpec) null); }
    }
}
