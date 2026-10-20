// Started from tools/compat/genstubs.py: a property holder whose Builder keeps what it is given (the alias and purposes too).
package android.security.keystore;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class KeyProtection implements java.security.KeyStore.ProtectionParameter {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public java.lang.String[] getBlockModes() { return (java.lang.String[]) meglerProps.get("BlockModes"); }
    public long getBoundToSpecificSecureUserId() { return (meglerProps.get("BoundToSpecificSecureUserId") instanceof Long ? (Long) meglerProps.get("BoundToSpecificSecureUserId") : 0L); }
    public java.lang.String[] getDigests() { return (java.lang.String[]) meglerProps.get("Digests"); }
    public java.lang.String[] getEncryptionPaddings() { return (java.lang.String[]) meglerProps.get("EncryptionPaddings"); }
    public java.util.Date getKeyValidityForConsumptionEnd() { return (java.util.Date) meglerProps.get("KeyValidityForConsumptionEnd"); }
    public java.util.Date getKeyValidityForOriginationEnd() { return (java.util.Date) meglerProps.get("KeyValidityForOriginationEnd"); }
    public java.util.Date getKeyValidityStart() { return (java.util.Date) meglerProps.get("KeyValidityStart"); }
    public int getMaxUsageCount() { return (meglerProps.get("MaxUsageCount") instanceof Integer ? (Integer) meglerProps.get("MaxUsageCount") : 0); }
    public java.util.Set getMgf1Digests() { return (meglerProps.get("Mgf1Digests") != null ? (java.util.Set) meglerProps.get("Mgf1Digests") : new java.util.HashSet()); }
    public int getPurposes() { return (meglerProps.get("Purposes") instanceof Integer ? (Integer) meglerProps.get("Purposes") : 0); }
    public java.lang.String[] getSignaturePaddings() { return (java.lang.String[]) meglerProps.get("SignaturePaddings"); }
    public int getUserAuthenticationType() { return (meglerProps.get("UserAuthenticationType") instanceof Integer ? (Integer) meglerProps.get("UserAuthenticationType") : 0); }
    public int getUserAuthenticationValidityDurationSeconds() { return (meglerProps.get("UserAuthenticationValidityDurationSeconds") instanceof Integer ? (Integer) meglerProps.get("UserAuthenticationValidityDurationSeconds") : 0); }
    public boolean isCriticalToDeviceEncryption() { return (meglerProps.get("CriticalToDeviceEncryption") instanceof Boolean ? (Boolean) meglerProps.get("CriticalToDeviceEncryption") : false); }
    public boolean isDigestsSpecified() { return (meglerProps.get("DigestsSpecified") instanceof Boolean ? (Boolean) meglerProps.get("DigestsSpecified") : false); }
    public boolean isInvalidatedByBiometricEnrollment() { return (meglerProps.get("InvalidatedByBiometricEnrollment") instanceof Boolean ? (Boolean) meglerProps.get("InvalidatedByBiometricEnrollment") : false); }
    public boolean isMgf1DigestsSpecified() { return (meglerProps.get("Mgf1DigestsSpecified") instanceof Boolean ? (Boolean) meglerProps.get("Mgf1DigestsSpecified") : false); }
    public boolean isRandomizedEncryptionRequired() { return (meglerProps.get("RandomizedEncryptionRequired") instanceof Boolean ? (Boolean) meglerProps.get("RandomizedEncryptionRequired") : false); }
    public boolean isRollbackResistant() { return (meglerProps.get("RollbackResistant") instanceof Boolean ? (Boolean) meglerProps.get("RollbackResistant") : false); }
    public boolean isStrongBoxBacked() { return (meglerProps.get("StrongBoxBacked") instanceof Boolean ? (Boolean) meglerProps.get("StrongBoxBacked") : false); }
    public boolean isUnlockedDeviceRequired() { return (meglerProps.get("UnlockedDeviceRequired") instanceof Boolean ? (Boolean) meglerProps.get("UnlockedDeviceRequired") : false); }
    public boolean isUserAuthenticationRequired() { return (meglerProps.get("UserAuthenticationRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserAuthenticationRequired") : false); }
    public boolean isUserAuthenticationValidWhileOnBody() { return (meglerProps.get("UserAuthenticationValidWhileOnBody") instanceof Boolean ? (Boolean) meglerProps.get("UserAuthenticationValidWhileOnBody") : false); }
    public boolean isUserConfirmationRequired() { return (meglerProps.get("UserConfirmationRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserConfirmationRequired") : false); }
    public boolean isUserPresenceRequired() { return (meglerProps.get("UserPresenceRequired") instanceof Boolean ? (Boolean) meglerProps.get("UserPresenceRequired") : false); }
    protected KeyProtection() {}
    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public Builder(int p0) { meglerProps.put("Purposes", p0); }
        public android.security.keystore.KeyProtection build() { android.security.keystore.KeyProtection x = new android.security.keystore.KeyProtection(); x.meglerProps.putAll(meglerProps); return x; }
        public android.security.keystore.KeyProtection.Builder setBlockModes(java.lang.String[] p0) { meglerProps.put("BlockModes", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setBoundToSpecificSecureUserId(long p0) { meglerProps.put("BoundToSpecificSecureUserId", Long.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setCriticalToDeviceEncryption(boolean p0) { meglerProps.put("CriticalToDeviceEncryption", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setDigests(java.lang.String[] p0) { meglerProps.put("Digests", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setEncryptionPaddings(java.lang.String[] p0) { meglerProps.put("EncryptionPaddings", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setInvalidatedByBiometricEnrollment(boolean p0) { meglerProps.put("InvalidatedByBiometricEnrollment", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setIsStrongBoxBacked(boolean p0) { meglerProps.put("IsStrongBoxBacked", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityEnd(java.util.Date p0) { meglerProps.put("KeyValidityEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityForConsumptionEnd(java.util.Date p0) { meglerProps.put("KeyValidityForConsumptionEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityForOriginationEnd(java.util.Date p0) { meglerProps.put("KeyValidityForOriginationEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityStart(java.util.Date p0) { meglerProps.put("KeyValidityStart", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setMaxUsageCount(int p0) { meglerProps.put("MaxUsageCount", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setMgf1Digests(java.lang.String[] p0) { meglerProps.put("Mgf1Digests", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setRandomizedEncryptionRequired(boolean p0) { meglerProps.put("RandomizedEncryptionRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setRollbackResistant(boolean p0) { meglerProps.put("RollbackResistant", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setSignaturePaddings(java.lang.String[] p0) { meglerProps.put("SignaturePaddings", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setUnlockedDeviceRequired(boolean p0) { meglerProps.put("UnlockedDeviceRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationParameters(int p0, int p1) { return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationRequired(boolean p0) { meglerProps.put("UserAuthenticationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationValidWhileOnBody(boolean p0) { meglerProps.put("UserAuthenticationValidWhileOnBody", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationValidityDurationSeconds(int p0) { meglerProps.put("UserAuthenticationValidityDurationSeconds", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserConfirmationRequired(boolean p0) { meglerProps.put("UserConfirmationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserPresenceRequired(boolean p0) { meglerProps.put("UserPresenceRequired", Boolean.valueOf(p0)); return this; }
        Builder() { this((int) 0); }
    }
}
