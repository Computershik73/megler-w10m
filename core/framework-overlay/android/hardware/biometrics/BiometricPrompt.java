// Overlay of core/framework/gen-src/android/hardware/biometrics/BiometricPrompt.java (tools/build-framework.sh), in place
// of the generated stub, which had signatures only: its CryptoObject and AuthenticationResult were abstract, and Play
// Store, which makes a CryptoObject before it asks for a fingerprint, ended in an InstantiationError (package 540); its
// authenticate did nothing. Here authenticate is the phone's check of its user (megler.Biometrics: Windows Hello's dialog
// with the prompt's title and text) and tells the callback on the app's executor: succeeded, with the CryptoObject it was
// given, or an error -- USER_CANCELED, or the negative button's listener where the prompt has one, when the user says no;
// CANCELED when the app cancels; LOCKOUT, HW_UNAVAILABLE, HW_NOT_PRESENT or NO_BIOMETRICS when the phone cannot check.
// Its error and acquired constants are the SDK's (the stub's BiometricConstants had none). The rest is the stub's.
package android.hardware.biometrics;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class BiometricPrompt implements android.hardware.biometrics.BiometricAuthenticator, android.hardware.biometrics.BiometricConstants {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int BIOMETRIC_ACQUIRED_GOOD = 0;
    public static final int BIOMETRIC_ACQUIRED_PARTIAL = 1;
    public static final int BIOMETRIC_ACQUIRED_INSUFFICIENT = 2;
    public static final int BIOMETRIC_ACQUIRED_IMAGER_DIRTY = 3;
    public static final int BIOMETRIC_ACQUIRED_TOO_SLOW = 4;
    public static final int BIOMETRIC_ACQUIRED_TOO_FAST = 5;
    public static final int BIOMETRIC_ERROR_HW_UNAVAILABLE = 1;
    public static final int BIOMETRIC_ERROR_UNABLE_TO_PROCESS = 2;
    public static final int BIOMETRIC_ERROR_TIMEOUT = 3;
    public static final int BIOMETRIC_ERROR_NO_SPACE = 4;
    public static final int BIOMETRIC_ERROR_CANCELED = 5;
    public static final int BIOMETRIC_ERROR_LOCKOUT = 7;
    public static final int BIOMETRIC_ERROR_VENDOR = 8;
    public static final int BIOMETRIC_ERROR_LOCKOUT_PERMANENT = 9;
    public static final int BIOMETRIC_ERROR_USER_CANCELED = 10;
    public static final int BIOMETRIC_ERROR_NO_BIOMETRICS = 11;
    public static final int BIOMETRIC_ERROR_HW_NOT_PRESENT = 12;
    public static final int BIOMETRIC_ERROR_NO_DEVICE_CREDENTIAL = 14;
    public static final int BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;
    public static final int AUTHENTICATION_RESULT_TYPE_BIOMETRIC = 2;
    public static final int AUTHENTICATION_RESULT_TYPE_DEVICE_CREDENTIAL = 1;
    public static final int DISMISSED_REASON_BIOMETRIC_CONFIRMED = 1;
    public static final int DISMISSED_REASON_BIOMETRIC_CONFIRM_NOT_REQUIRED = 4;
    public static final int DISMISSED_REASON_CONTENT_VIEW_MORE_OPTIONS = 8;
    public static final int DISMISSED_REASON_CREDENTIAL_CONFIRMED = 7;
    public static final int DISMISSED_REASON_ERROR = 5;
    public static final int DISMISSED_REASON_ERROR_NO_WM = 9;
    public static final int DISMISSED_REASON_FALLBACK_OPTION_BASE = 20;
    public static final int DISMISSED_REASON_FALLBACK_OPTION_MAX = 24;
    public static final int DISMISSED_REASON_NEGATIVE = 2;
    public static final int DISMISSED_REASON_SERVER_REQUESTED = 6;
    public static final int DISMISSED_REASON_USER_CANCEL = 3;
    public static final int HIDE_DIALOG_DELAY = 2000;
    public static int getMaxFallbackOptions() { return 0; }
    public void authenticate(android.hardware.biometrics.BiometricPrompt.CryptoObject p0, android.os.CancellationSignal p1, java.util.concurrent.Executor p2, android.hardware.biometrics.BiometricPrompt.AuthenticationCallback p3) {
        if (p0 == null) throw new IllegalArgumentException("Must supply a crypto object");
        meglerAuthenticate(p0, p1, p2, p3);
    }
    public void authenticate(android.os.CancellationSignal p0, java.util.concurrent.Executor p1, android.hardware.biometrics.BiometricPrompt.AuthenticationCallback p2) { meglerAuthenticate(null, p0, p1, p2); }
    public long authenticateForOperation(android.os.CancellationSignal p0, java.util.concurrent.Executor p1, android.hardware.biometrics.BiometricPrompt.AuthenticationCallback p2, long p3) { meglerAuthenticate(new CryptoObject(p3), p0, p1, p2); return p3; }
    public void authenticateUser(android.os.CancellationSignal p0, java.util.concurrent.Executor p1, android.hardware.biometrics.BiometricPrompt.AuthenticationCallback p2, int p3) { meglerAuthenticate(null, p0, p1, p2); }
    /*
     * The phone's check of its user (megler.Biometrics), told on the app's executor. The user's no is the negative button's
     * where the prompt has one, as Windows' dialog has only its Cancel; else USER_CANCELED.
     */
    private void meglerAuthenticate(final CryptoObject crypto, final android.os.CancellationSignal cancel, final java.util.concurrent.Executor executor, final AuthenticationCallback callback) {
        if (executor == null) throw new IllegalArgumentException("Must supply an executor");
        if (callback == null) throw new IllegalArgumentException("Must supply a callback");
        final int can = megler.Biometrics.available();
        if (can != megler.Biometrics.READY) {
            final boolean notSetUp = can == megler.Biometrics.NOT_SET_UP;
            executor.execute(() -> callback.onAuthenticationError(notSetUp ? BIOMETRIC_ERROR_NO_BIOMETRICS : BIOMETRIC_ERROR_HW_NOT_PRESENT,
                                                                  notSetUp ? "Windows Hello is not set up" : "This phone has no Windows Hello"));
            return;
        }
        final java.util.concurrent.Executor negativeExecutor = (java.util.concurrent.Executor) meglerProps.get("NegativeExecutor");
        final android.content.DialogInterface.OnClickListener negative = (android.content.DialogInterface.OnClickListener) meglerProps.get("NegativeListener");
        megler.Biometrics.verify(megler.Biometrics.message(getTitle(), getSubtitle(), getDescription()), cancel, result -> {
            switch (result) {
            case megler.Biometrics.VERIFIED:
                executor.execute(() -> callback.onAuthenticationSucceeded(new AuthenticationResult(crypto, AUTHENTICATION_RESULT_TYPE_BIOMETRIC)));
                break;
            case megler.Biometrics.CANCELED:
                if (cancel != null && cancel.isCanceled()) executor.execute(() -> callback.onAuthenticationError(BIOMETRIC_ERROR_CANCELED, "Canceled"));
                else if (negative != null && negativeExecutor != null) negativeExecutor.execute(() -> negative.onClick(null, android.content.DialogInterface.BUTTON_NEGATIVE));
                else executor.execute(() -> callback.onAuthenticationError(BIOMETRIC_ERROR_USER_CANCELED, "Canceled by the user"));
                break;
            case megler.Biometrics.LOCKED:
                executor.execute(() -> callback.onAuthenticationError(BIOMETRIC_ERROR_LOCKOUT, "Too many attempts. Try again later."));
                break;
            default:
                executor.execute(() -> callback.onAuthenticationError(BIOMETRIC_ERROR_HW_UNAVAILABLE, "Windows Hello is not available now"));
                break;
            }
        });
    }
    public int getAllowedAuthenticators() { return (meglerProps.get("AllowedAuthenticators") instanceof Integer ? (Integer) meglerProps.get("AllowedAuthenticators") : 0); }
    public java.util.List getAllowedSensorIds() { return (meglerProps.get("AllowedSensorIds") != null ? (java.util.List) meglerProps.get("AllowedSensorIds") : new java.util.ArrayList()); }
    public android.hardware.biometrics.PromptContentView getContentView() { return (android.hardware.biometrics.PromptContentView) meglerProps.get("ContentView"); }
    public java.lang.CharSequence getDescription() { return (java.lang.CharSequence) meglerProps.get("Description"); }
    public java.util.List getFallbackOptions() { return (meglerProps.get("FallbackOptions") != null ? (java.util.List) meglerProps.get("FallbackOptions") : new java.util.ArrayList()); }
    public android.graphics.Bitmap getLogoBitmap() { return (android.graphics.Bitmap) meglerProps.get("LogoBitmap"); }
    public java.lang.String getLogoDescription() { return (java.lang.String) meglerProps.get("LogoDescription"); }
    public int getLogoRes() { return (meglerProps.get("LogoRes") instanceof Integer ? (Integer) meglerProps.get("LogoRes") : 0); }
    public java.lang.CharSequence getNegativeButtonText() { return (java.lang.CharSequence) meglerProps.get("NegativeButtonText"); }
    public java.lang.CharSequence getSubtitle() { return (java.lang.CharSequence) meglerProps.get("Subtitle"); }
    public java.lang.CharSequence getTitle() { return (java.lang.CharSequence) meglerProps.get("Title"); }
    public boolean isAllowBackgroundAuthentication() { return (meglerProps.get("AllowBackgroundAuthentication") instanceof Boolean ? (Boolean) meglerProps.get("AllowBackgroundAuthentication") : false); }
    public boolean isConfirmationRequired() { return (meglerProps.get("ConfirmationRequired") instanceof Boolean ? (Boolean) meglerProps.get("ConfirmationRequired") : false); }
    public boolean shouldUseDefaultSubtitle() { return false; }
    public boolean shouldUseDefaultTitle() { return false; }
    protected BiometricPrompt() {}
    public static abstract class AuthenticationCallback extends android.hardware.biometrics.BiometricAuthenticator.AuthenticationCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AuthenticationCallback() { super(); }
        public void onAuthenticationAcquired(int p0) {}
        public void onAuthenticationError(int p0, java.lang.CharSequence p1) {}
        public void onAuthenticationFailed() {}
        public void onAuthenticationHelp(int p0, java.lang.CharSequence p1) {}
        public void onAuthenticationSucceeded(android.hardware.biometrics.BiometricPrompt.AuthenticationResult p0) {}
        public void onSystemEvent(int p0) {}
    }
    public static class AuthenticationResult extends android.hardware.biometrics.BiometricAuthenticator.AuthenticationResult {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AuthenticationResult(android.hardware.biometrics.BiometricPrompt.CryptoObject p0, int p1) { super(); meglerProps.put("CryptoObject", p0); meglerProps.put("AuthenticationType", Integer.valueOf(p1)); }
        public int getAuthenticationType() { return (meglerProps.get("AuthenticationType") instanceof Integer ? (Integer) meglerProps.get("AuthenticationType") : 0); }
        public android.hardware.biometrics.BiometricPrompt.CryptoObject getCryptoObject() { return (android.hardware.biometrics.BiometricPrompt.CryptoObject) meglerProps.get("CryptoObject"); }
        AuthenticationResult() { this((android.hardware.biometrics.BiometricPrompt.CryptoObject) null, (int) 0); }
    }
    public static class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public Builder(android.content.Context p0) {}
        public android.hardware.biometrics.BiometricPrompt.Builder addFallbackOption(java.lang.CharSequence p0, int p1, java.util.concurrent.Executor p2, android.content.DialogInterface.OnClickListener p3) { return this; }
        public android.hardware.biometrics.BiometricPrompt build() { android.hardware.biometrics.BiometricPrompt x = new android.hardware.biometrics.BiometricPrompt(); x.meglerProps.putAll(meglerProps); return x; }
        public android.hardware.biometrics.BiometricPrompt.Builder setAllowBackgroundAuthentication(boolean p0) { meglerProps.put("AllowBackgroundAuthentication", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setAllowBackgroundAuthentication(boolean p0, boolean p1) { return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setAllowedAuthenticators(int p0) { meglerProps.put("AllowedAuthenticators", Integer.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setAllowedSensorIds(java.util.List p0) { meglerProps.put("AllowedSensorIds", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setConfirmationRequired(boolean p0) { meglerProps.put("ConfirmationRequired", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setContentView(android.hardware.biometrics.PromptContentView p0) { meglerProps.put("ContentView", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setDescription(java.lang.CharSequence p0) { meglerProps.put("Description", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setDeviceCredentialAllowed(boolean p0) { meglerProps.put("DeviceCredentialAllowed", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setDisallowBiometricsIfPolicyExists(boolean p0) { meglerProps.put("DisallowBiometricsIfPolicyExists", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setIgnoreEnrollmentState(boolean p0) { meglerProps.put("IgnoreEnrollmentState", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setIsForLegacyFingerprintManager(int p0) { meglerProps.put("IsForLegacyFingerprintManager", Integer.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setLogoBitmap(android.graphics.Bitmap p0) { meglerProps.put("LogoBitmap", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setLogoDescription(java.lang.String p0) { meglerProps.put("LogoDescription", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setLogoRes(int p0) { meglerProps.put("LogoRes", Integer.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setNegativeButton(java.lang.CharSequence p0, java.util.concurrent.Executor p1, android.content.DialogInterface.OnClickListener p2) {
            meglerProps.put("NegativeButtonText", p0); meglerProps.put("NegativeExecutor", p1); meglerProps.put("NegativeListener", p2); return this;
        }
        public android.hardware.biometrics.BiometricPrompt.Builder setRealCallerForConfirmDeviceCredentialActivity(android.content.ComponentName p0) { meglerProps.put("RealCallerForConfirmDeviceCredentialActivity", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setReceiveSystemEvents(boolean p0) { meglerProps.put("ReceiveSystemEvents", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setService(android.hardware.biometrics.IAuthService p0) { meglerProps.put("Service", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setShowEmergencyCallButton(boolean p0) { meglerProps.put("ShowEmergencyCallButton", Boolean.valueOf(p0)); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setSubtitle(java.lang.CharSequence p0) { meglerProps.put("Subtitle", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setTextForDeviceCredential(java.lang.CharSequence p0, java.lang.CharSequence p1, java.lang.CharSequence p2) { return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setTitle(java.lang.CharSequence p0) { meglerProps.put("Title", p0); return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setUseDefaultSubtitle() { return this; }
        public android.hardware.biometrics.BiometricPrompt.Builder setUseDefaultTitle() { return this; }
        Builder() { this((android.content.Context) null); }
    }
    public static final class CryptoObject extends android.hardware.biometrics.CryptoObject {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public CryptoObject(long p0) { super(); meglerProps.put("OperationHandle", Long.valueOf(p0)); }
        public CryptoObject(android.security.identity.IdentityCredential p0) { super(); meglerProps.put("IdentityCredential", p0); }
        public CryptoObject(android.security.identity.PresentationSession p0) { super(); meglerProps.put("PresentationSession", p0); }
        public CryptoObject(java.security.Signature p0) { super(); meglerProps.put("Signature", p0); }
        public CryptoObject(javax.crypto.Cipher p0) { super(); meglerProps.put("Cipher", p0); }
        public CryptoObject(javax.crypto.KeyAgreement p0) { super(); meglerProps.put("KeyAgreement", p0); }
        public CryptoObject(javax.crypto.Mac p0) { super(); meglerProps.put("Mac", p0); }
        public javax.crypto.Cipher getCipher() { return (javax.crypto.Cipher) meglerProps.get("Cipher"); }
        public android.security.identity.IdentityCredential getIdentityCredential() { return (android.security.identity.IdentityCredential) meglerProps.get("IdentityCredential"); }
        public javax.crypto.KeyAgreement getKeyAgreement() { return (javax.crypto.KeyAgreement) meglerProps.get("KeyAgreement"); }
        public javax.crypto.Mac getMac() { return (javax.crypto.Mac) meglerProps.get("Mac"); }
        public long getOperationHandle() { return (meglerProps.get("OperationHandle") instanceof Long ? (Long) meglerProps.get("OperationHandle") : 0L); }
        public android.security.identity.PresentationSession getPresentationSession() { return (android.security.identity.PresentationSession) meglerProps.get("PresentationSession"); }
        public java.security.Signature getSignature() { return (java.security.Signature) meglerProps.get("Signature"); }
        CryptoObject() { this((long) 0L); }
    }
}
