// Overlay of core/framework/gen-src/android/hardware/biometrics/BiometricManager.java (tools/build-framework.sh), in place
// of the generated stub, which answered BIOMETRIC_SUCCESS to canAuthenticate: Play Store offered its fingerprint sign-in on
// a phone where nothing would check it. Here it is what the phone can (megler.Biometrics, Windows Hello): SUCCESS,
// NONE_ENROLLED when Windows Hello is not set up, NO_HARDWARE when the phone has none; strong, weak and the device's
// credential alike, as Windows Hello is all three. The rest is the stub's.
package android.hardware.biometrics;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class BiometricManager {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final int BIOMETRIC_ERROR_HW_UNAVAILABLE = 1;
    public static final int BIOMETRIC_ERROR_IDENTITY_CHECK_NOT_ACTIVE = 20;
    public static final int BIOMETRIC_ERROR_LOCKOUT = 7;
    public static final int BIOMETRIC_ERROR_NONE_ENROLLED = 11;
    public static final int BIOMETRIC_ERROR_NOT_ENABLED_FOR_APPS = 21;
    public static final int BIOMETRIC_ERROR_NO_HARDWARE = 12;
    public static final int BIOMETRIC_ERROR_SECURITY_UPDATE_REQUIRED = 15;
    public static final long BIOMETRIC_NO_AUTHENTICATION = -1L;
    public static final int BIOMETRIC_SUCCESS = 0;
    public static final java.lang.String EXTRA_ENROLL_REASON = "enroll_reason";
    public static final int ICON_TYPE_ACCOUNT = 2;
    public static final int ICON_TYPE_GENERIC = 3;
    public static final int ICON_TYPE_PASSWORD = 0;
    public static final int ICON_TYPE_QR_CODE = 1;
    public static final int ICON_TYPE_SETTING = 4;
    public static final int TYPE_FACE = 8;
    public static final int TYPE_FINGERPRINT = 2;
    public BiometricManager(android.content.Context p0, android.hardware.biometrics.IAuthService p1) {}
    public static java.lang.String authenticatorToStr(int p0) { return null; }
    public int canAuthenticate() { return meglerCan(); }
    public int canAuthenticate(int p0) { return p0 == 0 ? BIOMETRIC_ERROR_NO_HARDWARE : meglerCan(); }
    public int canAuthenticate(int p0, int p1) { return p1 == 0 ? BIOMETRIC_ERROR_NO_HARDWARE : meglerCan(); }
    private static int meglerCan() {
        int a = megler.Biometrics.available();
        return a == megler.Biometrics.READY ? BIOMETRIC_SUCCESS : a == megler.Biometrics.NOT_SET_UP ? BIOMETRIC_ERROR_NONE_ENROLLED : BIOMETRIC_ERROR_NO_HARDWARE;
    }
    public android.hardware.biometrics.BiometricTestSession createTestSession(int p0) { return null; }
    public long[] getAuthenticatorIds() { return (long[]) meglerProps.get("AuthenticatorIds"); }
    public long[] getAuthenticatorIds(int p0) { return null; }
    public java.util.Map getEnrollmentStatus() { return (meglerProps.get("EnrollmentStatus") != null ? (java.util.Map) meglerProps.get("EnrollmentStatus") : new java.util.HashMap()); }
    public long getLastAuthenticationTime(int p0) { return 0L; }
    public java.util.List getSensorProperties() { return (meglerProps.get("SensorProperties") != null ? (java.util.List) meglerProps.get("SensorProperties") : new java.util.ArrayList()); }
    public android.hardware.biometrics.BiometricManager.Strings getStrings(int p0) { return new Strings(); }
    public java.lang.String getUiPackage() { return (java.lang.String) meglerProps.get("UiPackage"); }
    public boolean hasEnrolledBiometrics(int p0) { return megler.Biometrics.available() == megler.Biometrics.READY; }
    public void invalidateAuthenticatorIds(int p0, int p1, android.hardware.biometrics.IInvalidationCallback p2) {}
    public void registerAuthenticationStateListener(android.hardware.biometrics.AuthenticationStateListener p0) {}
    public void registerEnabledOnKeyguardCallback(android.hardware.biometrics.IBiometricEnabledOnKeyguardCallback p0) {}
    public void registerIdentityCheckStateListener(android.hardware.biometrics.IIdentityCheckStateListener p0) {}
    public void resetLockout(int p0, byte[] p1) {}
    public void resetLockoutTimeBound(android.os.IBinder p0, java.lang.String p1, int p2, int p3, byte[] p4) {}
    public void setIdentityCheckTestStatus(android.hardware.biometrics.IdentityCheckStatus p0) { meglerProps.put("IdentityCheckTestStatus", p0); }
    public void unregisterAuthenticationStateListener(android.hardware.biometrics.AuthenticationStateListener p0) {}
    public void unregisterIdentityCheckStateListener(android.hardware.biometrics.IIdentityCheckStateListener p0) {}
    BiometricManager() { this((android.content.Context) null, (android.hardware.biometrics.IAuthService) null); }
    public interface Authenticators {
        int BIOMETRIC_CONVENIENCE = 4095;
        int BIOMETRIC_MAX_STRENGTH = 1;
        int BIOMETRIC_MIN_STRENGTH = 32767;
        int BIOMETRIC_STRONG = 15;
        int BIOMETRIC_WEAK = 255;
        int DEVICE_CREDENTIAL = 32768;
        int EMPTY_SET = 0;
        int IDENTITY_CHECK = 65536;
    }
    public static class Strings {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public java.lang.CharSequence getButtonLabel() { return "Use Windows Hello"; }
        public java.lang.CharSequence getPromptMessage() { return "Use Windows Hello to continue"; }
        public java.lang.CharSequence getSettingName() { return "Use Windows Hello"; }
        protected Strings() {}
    }
}
