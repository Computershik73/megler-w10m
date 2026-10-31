// Overlay of core/framework/gen-src/android/hardware/fingerprint/FingerprintManager.java (tools/build-framework.sh), in
// place of the generated stub, which had signatures only: no sensor, no fingerprints, and an abstract CryptoObject. Here the
// phone's check of its user stands for the sensor (megler.Biometrics, Windows Hello): isHardwareDetected and
// hasEnrolledFingerprints say what it can, and authenticate runs its dialog -- Android's has none, its app shows its own
// while the sensor waits -- telling the callback on the handler's thread (else the main thread): succeeded with the
// CryptoObject, or an error (USER_CANCELED, CANCELED, LOCKOUT, HW_UNAVAILABLE). The rest is the stub's.
package android.hardware.fingerprint;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class FingerprintManager implements android.hardware.biometrics.BiometricAuthenticator, android.hardware.biometrics.BiometricFingerprintConstants {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int ENROLL_ENROLL = 2;
    public static final int FINGERPRINT_ERROR_HW_UNAVAILABLE = 1;
    public static final int FINGERPRINT_ERROR_UNABLE_TO_PROCESS = 2;
    public static final int FINGERPRINT_ERROR_TIMEOUT = 3;
    public static final int FINGERPRINT_ERROR_NO_SPACE = 4;
    public static final int FINGERPRINT_ERROR_CANCELED = 5;
    public static final int FINGERPRINT_ERROR_LOCKOUT = 7;
    public static final int FINGERPRINT_ERROR_VENDOR = 8;
    public static final int FINGERPRINT_ERROR_LOCKOUT_PERMANENT = 9;
    public static final int FINGERPRINT_ERROR_USER_CANCELED = 10;
    public static final int FINGERPRINT_ERROR_NO_FINGERPRINTS = 11;
    public static final int FINGERPRINT_ERROR_HW_NOT_PRESENT = 12;
    public static final int ENROLL_FIND_SENSOR = 1;
    public static final int SENSOR_ID_ANY = -1;
    public static final int UDFPS_UI_OVERLAY_SHOWN = 1;
    public static final int UDFPS_UI_READY = 2;
    public FingerprintManager(android.content.Context p0, android.hardware.fingerprint.IFingerprintService p1) {}
    public static java.lang.String getAcquiredString(android.content.Context p0, int p1, int p2) { return null; }
    public static java.lang.String getErrorString(android.content.Context p0, int p1, int p2) { return null; }
    public void addAuthenticatorsRegisteredCallback(android.hardware.fingerprint.IFingerprintAuthenticatorsRegisteredCallback p0) {}
    public void addLockoutResetCallback(android.hardware.fingerprint.FingerprintManager.LockoutResetCallback p0) {}
    public void authenticate(android.hardware.fingerprint.FingerprintManager.CryptoObject p0, android.os.CancellationSignal p1, int p2, android.hardware.fingerprint.FingerprintManager.AuthenticationCallback p3, android.os.Handler p4) { meglerAuthenticate(p0, p1, p3, p4); }
    /* The phone's check of its user (megler.Biometrics), told on the handler's thread, else the main thread. */
    private void meglerAuthenticate(final CryptoObject crypto, final android.os.CancellationSignal cancel, final AuthenticationCallback callback, android.os.Handler handler) {
        if (callback == null) throw new IllegalArgumentException("Must supply an authentication callback");
        final android.os.Handler h = handler != null ? handler : new android.os.Handler(android.os.Looper.getMainLooper());
        if (cancel != null && cancel.isCanceled()) return;
        if (megler.Biometrics.available() != megler.Biometrics.READY) {
            h.post(() -> callback.onAuthenticationError(FINGERPRINT_ERROR_HW_UNAVAILABLE, "Windows Hello is not available"));
            return;
        }
        megler.Biometrics.verify(megler.Biometrics.message(null, null, null), cancel, result -> {
            switch (result) {
            case megler.Biometrics.VERIFIED:
                h.post(() -> callback.onAuthenticationSucceeded(new AuthenticationResult(crypto, null, 0, true)));
                break;
            case megler.Biometrics.CANCELED:
                final boolean byApp = cancel != null && cancel.isCanceled();
                h.post(() -> callback.onAuthenticationError(byApp ? FINGERPRINT_ERROR_CANCELED : FINGERPRINT_ERROR_USER_CANCELED, byApp ? "Canceled" : "Canceled by the user"));
                break;
            case megler.Biometrics.LOCKED:
                h.post(() -> callback.onAuthenticationError(FINGERPRINT_ERROR_LOCKOUT, "Too many attempts. Try again later."));
                break;
            default:
                h.post(() -> callback.onAuthenticationError(FINGERPRINT_ERROR_HW_UNAVAILABLE, "Windows Hello is not available now"));
                break;
            }
        });
    }
    public void authenticate(android.hardware.fingerprint.FingerprintManager.CryptoObject p0, android.os.CancellationSignal p1, android.hardware.fingerprint.FingerprintManager.AuthenticationCallback p2, android.os.Handler p3, int p4) { meglerAuthenticate(p0, p1, p2, p3); }
    public void authenticate(android.hardware.fingerprint.FingerprintManager.CryptoObject p0, android.os.CancellationSignal p1, android.hardware.fingerprint.FingerprintManager.AuthenticationCallback p2, android.os.Handler p3, int p4, int p5, int p6) { meglerAuthenticate(p0, p1, p2, p3); }
    public void authenticate(android.hardware.fingerprint.FingerprintManager.CryptoObject p0, android.os.CancellationSignal p1, android.hardware.fingerprint.FingerprintManager.AuthenticationCallback p2, android.os.Handler p3, android.hardware.fingerprint.FingerprintAuthenticateOptions p4) { meglerAuthenticate(p0, p1, p2, p3); }
    public android.hardware.biometrics.BiometricTestSession createTestSession(int p0) { return null; }
    public void detectFingerprint(android.os.CancellationSignal p0, android.hardware.fingerprint.FingerprintManager.FingerprintDetectionCallback p1, android.hardware.fingerprint.FingerprintAuthenticateOptions p2) {}
    public void enroll(byte[] p0, android.os.CancellationSignal p1, int p2, android.hardware.fingerprint.FingerprintManager.EnrollmentCallback p3, int p4, android.hardware.fingerprint.FingerprintEnrollOptions p5) {}
    public void generateChallenge(int p0, int p1, android.hardware.fingerprint.FingerprintManager.GenerateChallengeCallback p2) {}
    public void generateChallenge(int p0, android.hardware.fingerprint.FingerprintManager.GenerateChallengeCallback p1) {}
    public int getEnrollStageCount() { return (meglerProps.get("EnrollStageCount") instanceof Integer ? (Integer) meglerProps.get("EnrollStageCount") : 0); }
    public float getEnrollStageThreshold(int p0) { return 0f; }
    public java.util.List getEnrolledFingerprints() { return (meglerProps.get("EnrolledFingerprints") != null ? (java.util.List) meglerProps.get("EnrolledFingerprints") : new java.util.ArrayList()); }
    public java.util.List getEnrolledFingerprints(int p0) { return new java.util.ArrayList(); }
    public int getLockoutModeForUser(int p0, int p1) { return 0; }
    public java.util.List getSensorProperties() { return (meglerProps.get("SensorProperties") != null ? (java.util.List) meglerProps.get("SensorProperties") : new java.util.ArrayList()); }
    public java.util.List getSensorPropertiesInternal() { return (meglerProps.get("SensorPropertiesInternal") != null ? (java.util.List) meglerProps.get("SensorPropertiesInternal") : new java.util.ArrayList()); }
    public boolean hasEnrolledFingerprints() { return megler.Biometrics.available() == megler.Biometrics.READY; }
    public boolean hasEnrolledFingerprints(int p0) { return hasEnrolledFingerprints(); }
    public boolean hasEnrolledTemplates() { return hasEnrolledFingerprints(); }
    public boolean hasEnrolledTemplates(int p0) { return hasEnrolledFingerprints(); }
    public boolean isHardwareDetected() { return megler.Biometrics.available() != megler.Biometrics.NONE; }
    public boolean isPowerbuttonFps() { return (meglerProps.get("PowerbuttonFps") instanceof Boolean ? (Boolean) meglerProps.get("PowerbuttonFps") : false); }
    public void onPointerDown(long p0, int p1, int p2, float p3, float p4, float p5, float p6, float p7, long p8, long p9, boolean p10) {}
    public void onPointerDown(long p0, int p1, int p2, int p3, float p4, float p5) {}
    public void onPointerUp(long p0, int p1) {}
    public void onPointerUp(long p0, int p1, int p2, float p3, float p4, float p5, float p6, float p7, long p8, long p9, boolean p10) {}
    public void onPowerPressed() {}
    public void onUdfpsUiEvent(int p0, long p1, int p2) {}
    public void registerBiometricStateListener(android.hardware.biometrics.BiometricStateListener p0) {}
    public void remove(android.hardware.fingerprint.Fingerprint p0, int p1, android.hardware.fingerprint.FingerprintManager.RemovalCallback p2) {}
    public void removeAll(int p0, android.hardware.fingerprint.FingerprintManager.RemovalCallback p1) {}
    public void rename(int p0, int p1, java.lang.String p2) {}
    public void resetLockout(int p0, int p1, byte[] p2) {}
    public void revokeChallenge(int p0, long p1) {}
    public void scheduleWatchdog() {}
    public void setIgnoreDisplayTouches(long p0, int p1, boolean p2) {}
    public void setUdfpsOverlayController(android.hardware.fingerprint.IUdfpsOverlayController p0) { meglerProps.put("UdfpsOverlayController", p0); }
    FingerprintManager() { this((android.content.Context) null, (android.hardware.fingerprint.IFingerprintService) null); }
    public static abstract class AuthenticationCallback extends android.hardware.biometrics.BiometricAuthenticator.AuthenticationCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AuthenticationCallback() { super(); }
        public void onAuthenticationAcquired(int p0) {}
        public void onAuthenticationError(int p0, java.lang.CharSequence p1) {}
        public void onAuthenticationFailed() {}
        public void onAuthenticationHelp(int p0, java.lang.CharSequence p1) {}
        public void onAuthenticationSucceeded(android.hardware.fingerprint.FingerprintManager.AuthenticationResult p0) {}
        public void onUdfpsPointerDown(int p0) {}
        public void onUdfpsPointerUp(int p0) {}
    }
    public static class AuthenticationResult {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AuthenticationResult(android.hardware.fingerprint.FingerprintManager.CryptoObject p0, android.hardware.fingerprint.Fingerprint p1, int p2, boolean p3) { meglerProps.put("CryptoObject", p0); meglerProps.put("Fingerprint", p1); meglerProps.put("UserId", Integer.valueOf(p2)); meglerProps.put("StrongBiometric", Boolean.valueOf(p3)); }
        public android.hardware.fingerprint.FingerprintManager.CryptoObject getCryptoObject() { return (android.hardware.fingerprint.FingerprintManager.CryptoObject) meglerProps.get("CryptoObject"); }
        public android.hardware.fingerprint.Fingerprint getFingerprint() { return (android.hardware.fingerprint.Fingerprint) meglerProps.get("Fingerprint"); }
        public int getUserId() { return (meglerProps.get("UserId") instanceof Integer ? (Integer) meglerProps.get("UserId") : 0); }
        public boolean isStrongBiometric() { return (meglerProps.get("StrongBiometric") instanceof Boolean ? (Boolean) meglerProps.get("StrongBiometric") : false); }
        AuthenticationResult() { this((android.hardware.fingerprint.FingerprintManager.CryptoObject) null, (android.hardware.fingerprint.Fingerprint) null, (int) 0, (boolean) false); }
    }
    public static final class CryptoObject extends android.hardware.biometrics.CryptoObject {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public CryptoObject(java.security.Signature p0) { super(); meglerProps.put("Signature", p0); }
        public CryptoObject(javax.crypto.Cipher p0) { super(); meglerProps.put("Cipher", p0); }
        public CryptoObject(javax.crypto.Mac p0) { super(); meglerProps.put("Mac", p0); }
        public javax.crypto.Cipher getCipher() { return (javax.crypto.Cipher) meglerProps.get("Cipher"); }
        public android.security.identity.IdentityCredential getIdentityCredential() { return (android.security.identity.IdentityCredential) meglerProps.get("IdentityCredential"); }
        public javax.crypto.KeyAgreement getKeyAgreement() { return (javax.crypto.KeyAgreement) meglerProps.get("KeyAgreement"); }
        public javax.crypto.Mac getMac() { return (javax.crypto.Mac) meglerProps.get("Mac"); }
        public android.security.identity.PresentationSession getPresentationSession() { return (android.security.identity.PresentationSession) meglerProps.get("PresentationSession"); }
        public java.security.Signature getSignature() { return (java.security.Signature) meglerProps.get("Signature"); }
        CryptoObject() { this((java.security.Signature) null); }
    }
    public static abstract class EnrollmentCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public EnrollmentCallback() {}
        public void onAcquired(boolean p0) {}
        public void onEnrollmentError(int p0, java.lang.CharSequence p1) {}
        public void onEnrollmentHelp(int p0, java.lang.CharSequence p1) {}
        public void onEnrollmentProgress(int p0) {}
        public void onUdfpsOverlayShown() {}
        public void onUdfpsPointerDown(int p0) {}
        public void onUdfpsPointerUp(int p0) {}
    }
    public interface FingerprintDetectionCallback {
        default void onDetectionError(int p0) {}
        void onFingerprintDetected(int p0, int p1, boolean p2);
    }
    public interface GenerateChallengeCallback {
        void onChallengeGenerated(int p0, int p1, long p2);
    }
    public static abstract class LockoutResetCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public LockoutResetCallback() {}
        public void onLockoutReset(int p0) {}
    }
    public static abstract class RemovalCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public RemovalCallback() {}
        public void onRemovalError(android.hardware.fingerprint.Fingerprint p0, int p1, java.lang.CharSequence p2) {}
        public void onRemovalSucceeded(android.hardware.fingerprint.Fingerprint p0, int p1) {}
    }
}
