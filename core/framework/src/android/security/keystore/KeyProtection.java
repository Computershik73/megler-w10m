// Started from tools/compat/genstubs.py: a property holder whose Builder keeps what it is given (the alias and purposes too).
package android.security.keystore;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class KeyProtection implements java.security.KeyStore.ProtectionParameter {
    private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
    public java.lang.String[] getBlockModes() { return (java.lang.String[]) megletProps.get("BlockModes"); }
    public long getBoundToSpecificSecureUserId() { return (megletProps.get("BoundToSpecificSecureUserId") instanceof Long ? (Long) megletProps.get("BoundToSpecificSecureUserId") : 0L); }
    public java.lang.String[] getDigests() { return (java.lang.String[]) megletProps.get("Digests"); }
    public java.lang.String[] getEncryptionPaddings() { return (java.lang.String[]) megletProps.get("EncryptionPaddings"); }
    public java.util.Date getKeyValidityForConsumptionEnd() { return (java.util.Date) megletProps.get("KeyValidityForConsumptionEnd"); }
    public java.util.Date getKeyValidityForOriginationEnd() { return (java.util.Date) megletProps.get("KeyValidityForOriginationEnd"); }
    public java.util.Date getKeyValidityStart() { return (java.util.Date) megletProps.get("KeyValidityStart"); }
    public int getMaxUsageCount() { return (megletProps.get("MaxUsageCount") instanceof Integer ? (Integer) megletProps.get("MaxUsageCount") : 0); }
    public java.util.Set getMgf1Digests() { return (megletProps.get("Mgf1Digests") != null ? (java.util.Set) megletProps.get("Mgf1Digests") : new java.util.HashSet()); }
    public int getPurposes() { return (megletProps.get("Purposes") instanceof Integer ? (Integer) megletProps.get("Purposes") : 0); }
    public java.lang.String[] getSignaturePaddings() { return (java.lang.String[]) megletProps.get("SignaturePaddings"); }
    public int getUserAuthenticationType() { return (megletProps.get("UserAuthenticationType") instanceof Integer ? (Integer) megletProps.get("UserAuthenticationType") : 0); }
    public int getUserAuthenticationValidityDurationSeconds() { return (megletProps.get("UserAuthenticationValidityDurationSeconds") instanceof Integer ? (Integer) megletProps.get("UserAuthenticationValidityDurationSeconds") : 0); }
    public boolean isCriticalToDeviceEncryption() { return (megletProps.get("CriticalToDeviceEncryption") instanceof Boolean ? (Boolean) megletProps.get("CriticalToDeviceEncryption") : false); }
    public boolean isDigestsSpecified() { return (megletProps.get("DigestsSpecified") instanceof Boolean ? (Boolean) megletProps.get("DigestsSpecified") : false); }
    public boolean isInvalidatedByBiometricEnrollment() { return (megletProps.get("InvalidatedByBiometricEnrollment") instanceof Boolean ? (Boolean) megletProps.get("InvalidatedByBiometricEnrollment") : false); }
    public boolean isMgf1DigestsSpecified() { return (megletProps.get("Mgf1DigestsSpecified") instanceof Boolean ? (Boolean) megletProps.get("Mgf1DigestsSpecified") : false); }
    public boolean isRandomizedEncryptionRequired() { return (megletProps.get("RandomizedEncryptionRequired") instanceof Boolean ? (Boolean) megletProps.get("RandomizedEncryptionRequired") : false); }
    public boolean isRollbackResistant() { return (megletProps.get("RollbackResistant") instanceof Boolean ? (Boolean) megletProps.get("RollbackResistant") : false); }
    public boolean isStrongBoxBacked() { return (megletProps.get("StrongBoxBacked") instanceof Boolean ? (Boolean) megletProps.get("StrongBoxBacked") : false); }
    public boolean isUnlockedDeviceRequired() { return (megletProps.get("UnlockedDeviceRequired") instanceof Boolean ? (Boolean) megletProps.get("UnlockedDeviceRequired") : false); }
    public boolean isUserAuthenticationRequired() { return (megletProps.get("UserAuthenticationRequired") instanceof Boolean ? (Boolean) megletProps.get("UserAuthenticationRequired") : false); }
    public boolean isUserAuthenticationValidWhileOnBody() { return (megletProps.get("UserAuthenticationValidWhileOnBody") instanceof Boolean ? (Boolean) megletProps.get("UserAuthenticationValidWhileOnBody") : false); }
    public boolean isUserConfirmationRequired() { return (megletProps.get("UserConfirmationRequired") instanceof Boolean ? (Boolean) megletProps.get("UserConfirmationRequired") : false); }
    public boolean isUserPresenceRequired() { return (megletProps.get("UserPresenceRequired") instanceof Boolean ? (Boolean) megletProps.get("UserPresenceRequired") : false); }
    protected KeyProtection() {}
    public static final class Builder {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        public Builder(int p0) { megletProps.put("Purposes", p0); }
        public android.security.keystore.KeyProtection build() { android.security.keystore.KeyProtection x = new android.security.keystore.KeyProtection(); x.megletProps.putAll(megletProps); return x; }
        public android.security.keystore.KeyProtection.Builder setBlockModes(java.lang.String[] p0) { megletProps.put("BlockModes", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setBoundToSpecificSecureUserId(long p0) { megletProps.put("BoundToSpecificSecureUserId", Long.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setCriticalToDeviceEncryption(boolean p0) { megletProps.put("CriticalToDeviceEncryption", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setDigests(java.lang.String[] p0) { megletProps.put("Digests", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setEncryptionPaddings(java.lang.String[] p0) { megletProps.put("EncryptionPaddings", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setInvalidatedByBiometricEnrollment(boolean p0) { megletProps.put("InvalidatedByBiometricEnrollment", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setIsStrongBoxBacked(boolean p0) { megletProps.put("IsStrongBoxBacked", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityEnd(java.util.Date p0) { megletProps.put("KeyValidityEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityForConsumptionEnd(java.util.Date p0) { megletProps.put("KeyValidityForConsumptionEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityForOriginationEnd(java.util.Date p0) { megletProps.put("KeyValidityForOriginationEnd", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setKeyValidityStart(java.util.Date p0) { megletProps.put("KeyValidityStart", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setMaxUsageCount(int p0) { megletProps.put("MaxUsageCount", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setMgf1Digests(java.lang.String[] p0) { megletProps.put("Mgf1Digests", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setRandomizedEncryptionRequired(boolean p0) { megletProps.put("RandomizedEncryptionRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setRollbackResistant(boolean p0) { megletProps.put("RollbackResistant", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setSignaturePaddings(java.lang.String[] p0) { megletProps.put("SignaturePaddings", p0); return this; }
        public android.security.keystore.KeyProtection.Builder setUnlockedDeviceRequired(boolean p0) { megletProps.put("UnlockedDeviceRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationParameters(int p0, int p1) { return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationRequired(boolean p0) { megletProps.put("UserAuthenticationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationValidWhileOnBody(boolean p0) { megletProps.put("UserAuthenticationValidWhileOnBody", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserAuthenticationValidityDurationSeconds(int p0) { megletProps.put("UserAuthenticationValidityDurationSeconds", Integer.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserConfirmationRequired(boolean p0) { megletProps.put("UserConfirmationRequired", Boolean.valueOf(p0)); return this; }
        public android.security.keystore.KeyProtection.Builder setUserPresenceRequired(boolean p0) { megletProps.put("UserPresenceRequired", Boolean.valueOf(p0)); return this; }
        Builder() { this((int) 0); }
    }
}
