package android.security.keystore.recovery;

/**
 * The recoverable key store (Android 9's backup of keys protected by the lock screen), as a phone answers that does not
 * have it for the caller: a Megler phone has no lock settings service. GmsCore asks for it when its key recovery module
 * starts (getRecoverySecretTypes) and leaves the module off on a SecurityException; without this class that start failed.
 */
public class RecoveryController {
    private RecoveryController() {}
    public static RecoveryController getInstance(android.content.Context context) { return new RecoveryController(); }
    public static boolean isRecoverableKeyStoreEnabled(android.content.Context context) { return false; }
    public int[] getRecoverySecretTypes() throws InternalRecoveryServiceException {
        throw new SecurityException("this phone has no recoverable key store");
    }
}
