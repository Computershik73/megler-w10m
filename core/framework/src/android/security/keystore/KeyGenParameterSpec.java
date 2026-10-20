// Started from tools/compat/genstubs.py: a property holder whose Builder keeps what it is given (the alias and purposes too).
package android.security.keystore;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class KeyGenParameterSpec implements java.security.spec.AlgorithmParameterSpec {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public KeyGenParameterSpec(java.lang.String p0, int p1, int p2, java.security.spec.AlgorithmParameterSpec p3, javax.security.auth.x500.X500Principal p4, java.math.BigInteger p5, java.util.Date p6, java.util.Date p7, java.util.Date p8, java.util.Date p9, java.util.Date p10, int p11, java.lang.String[] p12, java.util.Set p13, java.lang.String[] p14, java.lang.String[] p15, java.lang.String[] p16, boolean p17, boolean p18, int p19, int p20, boolean p21, byte[] p22, boolean p23, int[] p24, boolean p25, boolean p26, boolean p27, boolean p28, boolean p29, boolean p30, boolean p31, int p32, java.lang.String p33, long p34) {}
    public java.security.spec.AlgorithmParameterSpec getAlgorithmParameterSpec() { return (java.security.spec.AlgorithmParameterSpec) meglerProps.get("AlgorithmParameterSpec"); }
    public java.lang.String getAttestKeyAlias() { return (java.lang.String) meglerProps.get("AttestKeyAlias"); }
    public byte[] getAttestationChallenge() { return (byte[]) meglerProps.get("AttestationChallenge"); }
    public int[] getAttestationIds() { return (int[]) meglerProps.get("AttestationIds"); }
    public java.lang.String[] getBlockModes() { return (java.lang.String[]) meglerProps.get("BlockModes"); }
    public long getBoundToSpecificSecureUserId() { return (meglerProps.get("BoundToSpecificSecureUserId") instanceof Long ? (Long) meglerProps.get("BoundToSpecificSecureUserId") : 0L); }
    public java.util.Date getCertificateNotAfter() { return (java.util.Date) meglerProps.get("CertificateNotAfter"); }
    public java.util.Date getCertificateNotBefore() { return (java.util.Date) meglerProps.get("CertificateNotBefore"); }
    public java.math.BigInteger getCertificateSerialNumber() { return (java.math.BigInteger) meglerProps.get("CertificateSerialNumber"); }
    public javax.security.auth.x500.X500Principal getCertificateSubject() { return (javax.security.auth.x500.X500Principal) meglerProps.get("CertificateSubject"); }
    public java.lang.String[] getDigests() { return (java.lang.String[]) meglerProps.get("Digests"); }
    public java.lang.String[] getEncryptionPaddings() { return (java.lang.String[]) meglerProps.get("EncryptionPaddings"); }
    public int getKeySize() { return (meglerProps.get("KeySize") instanceof Integer ? (Integer) meglerProps.get("KeySize") : 0); }
    public java.util.Date getKeyValidityForConsumptionEnd() { return (java.util.Date) meglerProps.get("KeyValidityForConsumptionEnd"); }
    public java.util.Date getKeyValidityForOriginationEnd() { return (java.util.Date) meglerProps.get("KeyValidityForOriginationEnd"); }
    public java.util.Date getKeyValidityStart() { return (java.util.Date) meglerProps.get("KeyValidityStart"); }
    public java.lang.String getKeystoreAlias() { return (java.lang.String) meglerProps.get("KeystoreAlias"); }
    public int getMaxUsageCount() { return (meglerProps.get("MaxUsageCount") instanceof Integer ? (Integer) meglerProps.get("MaxUsageCount") : 0); }
    public java.util.Set getMgf1Digests() { return (meglerProps.get("Mgf1Digests") != null ? (java.util.Set) meglerProps.get("Mgf1Digests") : new java.util.HashSet()); }
    public int getNamespace() { return (meglerProps.get("Namespace") instanceof Integer ? (Integer) meglerProps.get("Namespace") : 0); }
    public int getPurposes() { return (meglerProps.get("Purposes") instanceof Integer ? (Integer) meglerProps.get("Purposes") : 0); }
    public java.lang.String[] getSignaturePaddings() { return (java.lang.String[]) meglerProps.get("SignaturePaddings"); }
    public int getUid() { return (meglerProps.get("Uid") instanceof Integer ? (Integer) meglerProps.get("Uid") : 0); }
    public int getUserAuthenticationType() { return (meglerProps.get("UserAuthenticationType") instanceof Integer ? (Integer) meglerProps.get("UserAuthenticationType") : 0); }
    public int getUserAuthenticationValidityDurationSeconds() { return (meglerProps.get("UserAuthenticationValidityDurationSeconds") instanceof Integer ? (Integer) meglerProps.get("UserAuthenticationValidityDurationSeconds") : 0); }
    public boolean isCriticalToDeviceEncryption() { return (meglerProps.get("CriticalToDeviceEncryption") instanceof Boolean ? (Boolean) meglerProps.get("CriticalToDeviceEncryption") : false); }
    public boolean isDevicePropertiesAttestationIncluded() { return (meglerProps.get("DevicePropertiesAttestationIncluded") instanceof Boolean ? (Boolean) meglerProps.get("DevicePropertiesAttestationIncluded") : false); }
    public boolean isDigestsSpecified() { return (meglerProps.get("DigestsSpecified") instanceof Boolean ? (Boolean) meglerProps.get("DigestsSpecified") : false); }
    public boolean isInvalidatedByBiometricEnrollment() { return (meglerProps.get("InvalidatedByBiometricEnrollment") instanceof Boolean ? (Boolean) meglerProps.get("InvalidatedByBiometricEnrollment") : false); }
    public boolean isMgf1DigestsSpecified() { return (meglerProps.get("Mgf1DigestsSpecified") instanceof Boolean ? (Boolean) meglerProps.get("Mgf1DigestsSpecified") : false); }
    public boolean isRandomizedEncryptionRequired() { return (meglerProps.get("RandomizedEncryptionRequired") instanceof Boolean ? (Boolean) meglerProps.get("RandomizedEncryptionRequired") : false); }
    public boolean isStrongBoxBacked() { return (meglerProps.get("StrongBoxBacked") instanceof Boolean ? (Boolean) meglerProps.get("StrongBoxBacked") : false); }
    public boolean isUniqueIdIncluded() { return (meglerProps.get("UniqueIdIncluded") instanceof Boolean ? (Boolean) meglerProps.get("UniqueIdIncluded") : false); }
    public boolean isUnlockedDeviceRequired() { return (meglerProps.get("UnlockedDeviceRequired") instanceof Boolean ? (Boolean) meglerProps.get("UnlockedDeviceRequired") : false); }
    public boolean isUserAuthenticationRequired() { return (meglerProps.get("UserAuthenticationRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserAuthenticationRequired") : false); }
    public boolean isUserAuthenticationValidWhileOnBody() { return (meglerProps.get("UserAuthenticationValidWhileOnBody") instanceof Boolean ? (Boolean) meglerProps.get("UserAuthenticationValidWhileOnBody") : false); }
    public boolean isUserConfirmationRequired() { return (meglerProps.get("UserConfirmationRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserConfirmationRequired") : false); }
    public boolean isUserPresenceRequired() { return (meglerProps.get("UserPresenceRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserPresenceRequired") : false); }
    KeyGenParameterSpec() { this((java.lang.String) null, (int) 0, (int) 0, (java.security.spec.AlgorithmParameterSpec) null, (javax.security.auth.x500.X500Principal) null, (java.math.BigInteger) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (java.util.Date) null, (int) 0, (java.lang.String[]) null, (java.util.Set) null, (java.lang.String[]) null, (java.lang.String[]) null, (java.lang.String[]) null, (boolean) false, (boolean) false, (int) 0, (int) 0, (boolean) false, (byte[]) null, (boolean) false, (int[]) null, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (boolean) false, (int) 0, (java.lang.String) null, (long) 0L); }
    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public Builder(android.security.keystore.KeyGenParameterSpec p0) { meglerProps.putAll(p0.meglerProps); }
        public Builder(java.lang.String p0, int p1) { meglerProps.put("KeystoreAlias", p0); meglerProps.put("Purposes", p1); }
        public android.security.keystore.KeyGenParameterSpec build() { android.security.keystore.KeyGenParameterSpec x = new android.security.keystore.KeyGenParameterSpec(); x.meglerProps.putAll(meglerProps); return x; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAlgorithmParameterSpec(java.security.spec.AlgorithmParameterSpec p0) { meglerProps.put("AlgorithmParameterSpec", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestKeyAlias(java.lang.String p0) { meglerProps.put("AttestKeyAlias", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestationChallenge(byte[] p0) { meglerProps.put("AttestationChallenge", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setAttestationIds(int[] p0) { meglerProps.put("AttestationIds", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setBlockModes(java.lang.String[] p0) { meglerProps.put("BlockModes", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setBoundToSpecificSecureUserId(long p0) { meglerProps.put("BoundToSpecificSecureUserId", Long.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateNotAfter(java.util.Date p0) { meglerProps.put("CertificateNotAfter", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateNotBefore(java.util.Date p0) { meglerProps.put("CertificateNotBefore", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateSerialNumber(java.math.BigInteger p0) { meglerProps.put("CertificateSerialNumber", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCertificateSubject(javax.security.auth.x500.X500Principal p0) { meglerProps.put("CertificateSubject", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setCriticalToDeviceEncryption(boolean p0) { meglerProps.put("CriticalToDeviceEncryption", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setDevicePropertiesAttestationIncluded(boolean p0) { meglerProps.put("DevicePropertiesAttestationIncluded", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setDigests(java.lang.String[] p0) { meglerProps.put("Digests", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setEncryptionPaddings(java.lang.String[] p0) { meglerProps.put("EncryptionPaddings", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setInvalidatedByBiometricEnrollment(boolean p0) { meglerProps.put("InvalidatedByBiometricEnrollment", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setIsStrongBoxBacked(boolean p0) { meglerProps.put("IsStrongBoxBacked", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeySize(int p0) { meglerProps.put("KeySize", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityEnd(java.util.Date p0) { meglerProps.put("KeyValidityEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityForConsumptionEnd(java.util.Date p0) { meglerProps.put("KeyValidityForConsumptionEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityForOriginationEnd(java.util.Date p0) { meglerProps.put("KeyValidityForOriginationEnd", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setKeyValidityStart(java.util.Date p0) { meglerProps.put("KeyValidityStart", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setMaxUsageCount(int p0) { meglerProps.put("MaxUsageCount", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setMgf1Digests(java.lang.String[] p0) { meglerProps.put("Mgf1Digests", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setNamespace(int p0) { meglerProps.put("Namespace", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setRandomizedEncryptionRequired(boolean p0) { meglerProps.put("RandomizedEncryptionRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setSignaturePaddings(java.lang.String[] p0) { meglerProps.put("SignaturePaddings", p0); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUid(int p0) { meglerProps.put("Uid", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUniqueIdIncluded(boolean p0) { meglerProps.put("UniqueIdIncluded", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUnlockedDeviceRequired(boolean p0) { meglerProps.put("UnlockedDeviceRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationParameters(int p0, int p1) { return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationRequired(boolean p0) { meglerProps.put("UserAuthenticationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationValidWhileOnBody(boolean p0) { meglerProps.put("UserAuthenticationValidWhileOnBody", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserAuthenticationValidityDurationSeconds(int p0) { meglerProps.put("UserAuthenticationValidityDurationSeconds", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserConfirmationRequired(boolean p0) { meglerProps.put("UserConfirmationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyGenParameterSpec.Builder setUserPresenceRequired(boolean p0) { meglerProps.put("UserPresenceRequired", Boolean.valueOf(p0)); return this; }
        Builder() { this((android.security.keystore.KeyGenParameterSpec) null); }
    }
}
